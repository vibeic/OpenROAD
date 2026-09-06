// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2025, The OpenROAD Authors

#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/synchronization/mutex.h"
#include "boost/asio/thread_pool.hpp"
#include "drt/PinAccessService.h"
#include "odb/geom.h"

namespace odb {
class dbDatabase;
class dbInst;
class dbBTerm;
class dbNet;
class dbWire;
}  // namespace odb

namespace utl {
class Logger;
class ServiceRegistry;
}  // namespace utl

namespace stt {
class SteinerTreeBuilder;
}

namespace dst {
class Distributed;
}

namespace drt {

class frDesign;
class frInst;
class DesignCallBack;
class FlexDR;
class FlexPA;
class FlexTA;
class FlexDRWorker;
class drUpdate;
struct frDebugSettings;
struct FlexDRViaData;
class frMarker;
struct RouterConfiguration;
class AbstractGraphicsFactory;
class frViaDef;

struct ParamStruct
{
  std::string outputMazeFile;
  std::string outputDrcFile;
  int drcReportIterStep = 0;
  std::string outputGuideCoverageFile;
  std::string dbProcessNode;
  bool enableViaGen = false;
  int drouteEndIter = -1;
  std::string viaInPinBottomLayer;
  std::string viaInPinTopLayer;
  std::string viaAccessLayer;
  int orSeed = 0;
  double orK = 0;
  int verbose = 1;
  bool cleanPatches = false;
  bool doPa = false;
  bool singleStepDR = false;
  int minAccessPoints = -1;
  bool saveGuideUpdates = false;
  std::string repairPDNLayerName;
  int num_threads = 1;
  // vibeic fork: -report_unowned_gc_objects. Declared LAST so the positional
  // brace initialisation in TritonRoute.i keeps its existing order.
  bool reportUnownedGcObjects = false;
  bool gcVerifyCheckNdr = false;
  bool gcInloopNoCheckNdr = false;
  std::string gcDumpShapesBox;
};

class TritonRoute : public PinAccessService
{
 public:
  TritonRoute(odb::dbDatabase* db,
              utl::Logger* logger,
              utl::ServiceRegistry* service_registry,
              dst::Distributed* dist,
              stt::SteinerTreeBuilder* stt_builder);
  ~TritonRoute() override;

  void updateDirtyPinAccess() override;

  void initGraphics(std::unique_ptr<AbstractGraphicsFactory> graphics_factory);

  frDesign* getDesign() const { return design_.get(); }
  utl::Logger* getLogger() const { return logger_; }
  RouterConfiguration* getRouterConfiguration() const
  {
    return router_cfg_.get();
  }

  int main();
  void endFR();
  void pinAccess(const std::vector<odb::dbInst*>& target_insts
                 = std::vector<odb::dbInst*>());
  void stepDR(int size,
              int offset,
              int mazeEndIter,
              unsigned int workerDRCCost,
              unsigned int workerMarkerCost,
              unsigned int workerFixedShapeCost,
              float workerMarkerDecay,
              int ripupMode,
              bool followGuide);

  int getNumDRVs() const;

  void setDebugDR(bool on = true);
  void setDebugDumpDR(bool on, const std::string& dumpDir);
  void setDebugSnapshotDir(const std::string& snapshotDir);
  void setDebugMaze(bool on = true);
  void setDebugPA(bool on = true);
  void setDebugTA(bool on = true);
  void setDebugWriteNetTracks(bool on = true);
  void setDebugNetName(const char* name);  // for DR
  void setDebugPinName(const char* name);  // for PA
  void setDebugBox(int x1, int y1, int x2, int y2);
  void setDebugIter(int iter);
  void setDebugPaMarkers(bool on = true);
  void setDumpLastWorker(bool on = true);
  void setDebugWorkerParams(int mazeEndIter,
                            int drcCost,
                            int markerCost,
                            int fixedShapeCost,
                            float markerDecay,
                            int ripupMode,
                            int followGuide);
  void setDistributed(bool on = true);
  void setWorkerIpPort(const char* ip, unsigned short port);
  void setSharedVolume(const std::string& vol);
  void setCloudSize(unsigned int cloud_sz) { cloud_sz_ = cloud_sz; }
  unsigned int getCloudSize() const { return cloud_sz_; }
  void setDebugPaEdge(bool on = true);
  void setDebugPaCommit(bool on = true);
  void reportConstraints();

  void setParams(const ParamStruct& params);
  void addUserSelectedVia(const std::string& viaName);
  void setUnidirectionalLayer(const std::string& layerName);
  frDebugSettings* getDebugSettings() const { return debug_.get(); }
  // This runs a serialized worker from file_name.  It is intended
  // for debugging and not general usage.
  std::string runDRWorker(const std::string& workerStr, FlexDRViaData* viaData);
  void debugSingleWorker(const std::string& dumpDir, const std::string& drcRpt);
  void updateGlobals(const char* file_name);
  void resetDb(const char* file_name);
  void clearDesign();
  void updateDesign(const std::vector<std::string>& updates, int num_threads);
  void updateDesign(const std::string& path, int num_threads);
  void addWorkerResults(
      const std::vector<std::pair<int, std::string>>& results);
  bool getWorkerResults(std::vector<std::pair<int, std::string>>& results);
  int getWorkerResultsSize();
  void sendDesignDist();
  bool writeGlobals(const std::string& name);
  void sendDesignUpdates(const std::string& router_cfg_path, int num_threads);
  void sendGlobalsUpdates(const std::string& router_cfg_path,
                          const std::string& serializedViaData);
  void reportDRC(const std::string& file_name,
                 const std::list<std::unique_ptr<frMarker>>& markers,
                 const std::string& marker_name,
                 odb::Rect drcBox = odb::Rect(0, 0, 0, 0)) const;
  std::vector<int> routeLayerLengths(odb::dbWire* wire) const;
  void checkDRC(const char* filename,
                int x1,
                int y1,
                int x2,
                int y2,
                const std::string& marker_name,
                int num_threads);
  bool initGuide();
  void prep();
  // vibeic fork: post-route additive min-area repair. After detailed_route
  // fully converges, grow any residual isolated routing polygon that is below
  // the layer min-area (e.g. an isolated via landing pad) into a spacing-safe
  // rectangle >= min-area. Runs OUTSIDE the ripup loop, so it never re-enters
  // the maze router. Returns the number of polygons patched.
  int patchMinAreaViolations();
  int patchNonSufficientMetalViolations();
  // vibeic fork: what the NS-Metal repair pass was handed and what became of it.
  //
  // The pass used to report only the number it PATCHED. That is not the number it
  // CLEARED, and the difference is not academic: a patch that lands without
  // satisfying the rule it repairs manufactures new markers, and a report built
  // from the patch count calls that a success. Measured on a deliberately broken
  // build: "widened 1 ... 0 left unresolved" while the design went 1 -> 3.
  //
  // So the pass records what it was handed and why each junction it did not
  // widen was refused, and verifyRoute() -- which recomputes the whole-design
  // marker set immediately afterwards anyway, at no extra cost -- supplies the
  // only honest "cleared" number and contradicts the pass if it made things worse.
  struct NsMetalRepairStats
  {
    bool ran{false};
    int handed{0};        // NS-Metal markers the pass was given
    int patched{0};       // junctions it widened
    int no_room{0};       // could not reach a full MINWIDTH on some side
    int degenerate{0};    // grown box empty/undersized/outside the die
    int not_a_neck{0};    // marker already >= MINWIDTH on both axes
    int no_owner{0};      // no signal net to add metal to (fixed/special/none)
    int not_routing{0};   // marker not on a routing layer
    int before{0};        // NS-Metal count the pass saw on entry
  };
  const NsMetalRepairStats& getNsMetalRepairStats() const
  {
    return ns_metal_repair_stats_;
  }
  // Emit DRT-0706 (and DRT-0707 if the repair made things worse) from a
  // whole-design NS-Metal count taken AFTER the pass. Shared by verifyRoute()
  // and by the test entry point so both report the same way and the warning is
  // reachable from a regression test.
  void reportNsMetalRepairOutcome(int ns_metal_after);
  // vibeic fork: run the post-route NS-Metal repair on an ALREADY ROUTED
  // design and re-verify, without re-entering detailed_route. This is the
  // only way patchNonSufficientMetalViolations can be regression-tested:
  // it otherwise runs solely from inside TritonRoute::main(), which needs a
  // full route. Mirrors checkDRC's setup exactly, then reports before/after
  // marker counts and writes the SURVIVING markers to `filename`, so a pass
  // that silently repairs nothing and a pass that manufactures new violations
  // are both visible in the same output.
  void repairNonSufficientMetal(const char* filename, int num_threads);
  // vibeic fork: post-route whole-design DRC VERIFICATION.
  //
  // The number detailed_route publishes today is the residual in-loop marker
  // set: the union of what the per-worker, per-iteration GC happened to raise,
  // minus whatever a later worker cleared over its own box.  That is a
  // CONVERGENCE counter for the ripup loop, not a statement about the finished
  // route -- and two things run AFTER FlexDR::end() has already written the
  // report (this fork's own additive min-area repair, and any caller-side
  // reroute), so the published verdict can describe geometry that no longer
  // exists.  This re-runs the SAME GC engine over the whole die once routing is
  // final, replaces the block's marker set with the result, and reports it, so
  // "0 violations" means "verified clean", not "the loop stopped finding
  // things".  Returns the verified violation count.
  int verifyRoute();
  // vibeic fork, MEASUREMENT ONLY, `-debug_level DRT verifysplit 1`.
  // The whole-design marker count as of BEFORE the post-route repair passes.
  void reportPreRepairDrc();
  odb::dbDatabase* getDb() const { return db_; }
  void fixMaxSpacing(int num_threads);
  void deleteInstancePAData(frInst* inst, bool delete_inst = false);
  void addInstancePAData(frInst* inst);
  void addAvoidViaDefPA(const frViaDef* via_def);
  void updateDirtyPAData();

 private:
  NsMetalRepairStats ns_metal_repair_stats_;
  std::unique_ptr<frDesign> design_;
  std::unique_ptr<frDebugSettings> debug_;
  std::unique_ptr<DesignCallBack> db_callback_;
  std::unique_ptr<RouterConfiguration> router_cfg_;
  odb::dbDatabase* db_{nullptr};
  utl::Logger* logger_{nullptr};
  utl::ServiceRegistry* service_registry_{nullptr};
  std::unique_ptr<FlexDR> dr_;  // kept for single stepping
  stt::SteinerTreeBuilder* stt_builder_{nullptr};
  int num_drvs_{-1};
  dst::Distributed* dist_{nullptr};
  bool distributed_{false};
  std::string dist_ip_;
  uint16_t dist_port_{0};
  std::string shared_volume_;
  std::vector<std::pair<int, std::string>> workers_results_;
  absl::Mutex results_mutex_;
  int results_sz_{0};
  unsigned int cloud_sz_{0};
  std::optional<boost::asio::thread_pool> dist_pool_;
  std::unique_ptr<FlexPA> pa_{nullptr};
  std::unique_ptr<AbstractGraphicsFactory> graphics_factory_{nullptr};

  void initDesign();
  void initGraphics();
  void ta();
  void dr();
  void applyUpdates(const std::vector<std::vector<drUpdate>>& updates);
  void getDRCMarkers(std::list<std::unique_ptr<frMarker>>& markers,
                     const odb::Rect& requiredDrcBox);
  void repairPDNVias();
  friend class FlexDR;
};

}  // namespace drt
