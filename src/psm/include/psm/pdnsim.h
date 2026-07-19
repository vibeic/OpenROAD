// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2025, The OpenROAD Authors

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "odb/PtrSetMap.h"
#include "odb/db.h"
#include "odb/dbBlockCallBackObj.h"
#include "psm/em_signoff.h"

namespace odb {
class dbDatabase;
class Point;
class dbNet;
class dbTechLayer;
}  // namespace odb

namespace sta {
class dbSta;
class Scene;
}  // namespace sta
namespace utl {
class Logger;
}
namespace est {
class EstimateParasitics;
}
namespace dpl {
class Opendp;
}
namespace gui {
class HeatMapSourceRegistration;
using HeatMapSourceHandle = std::shared_ptr<HeatMapSourceRegistration>;
}  // namespace gui

namespace psm {
class IRDropDataSource;
class IRSolver;

enum class GeneratedSourceType
{
  kFull,
  kStraps,
  kBumps
};

class PDNSim : public odb::dbBlockCallBackObj
{
 public:
  struct GeneratedSourceSettings
  {
    // Bumps
    int bump_dx = 140;
    int bump_dy = 140;
    int bump_size = 70;
    int bump_interval = 3;

    // Straps
    int strap_track_pitch = 10;

    // Source resistance
    float resistance = 0.0;  // Ohms
  };

  using IRDropByPoint = std::map<odb::Point, double>;
  using IRDropByLayer = odb::PtrMap<odb::dbTechLayer, IRDropByPoint>;

  PDNSim(utl::Logger* logger,
         odb::dbDatabase* db,
         sta::dbSta* sta,
         est::EstimateParasitics* estimate_parasitics,
         dpl::Opendp* opendp);
  ~PDNSim() override;

  void setNetVoltage(odb::dbNet* net, sta::Scene* corner, double voltage);
  void setInstPower(odb::dbInst* inst, sta::Scene* corner, float power);
  void analyzePowerGrid(odb::dbNet* net,
                        sta::Scene* corner,
                        GeneratedSourceType source_type,
                        const std::string& voltage_file,
                        bool use_prev_solution,
                        bool enable_em,
                        const std::string& em_file,
                        const std::string& error_file,
                        const std::string& voltage_source_file);
  // Transient / dynamic (di-dt) power-grid analysis.  Computes the static DC
  // operating point and then time-steps the RC grid under a vectorless
  // per-clock current model to report the worst dynamic voltage droop.  The
  // static path (analyzePowerGrid) is unchanged.
  void analyzePowerGridDynamic(odb::dbNet* net,
                               sta::Scene* corner,
                               GeneratedSourceType source_type,
                               const std::string& voltage_file,
                               const std::string& error_file,
                               const std::string& voltage_source_file,
                               double period,
                               int steps,
                               int num_periods,
                               double node_cap,
                               double total_cap,
                               double decap_cap,
                               double current_duty,
                               bool phase_spread,
                               const std::string& current_profile,
                               const std::string& vectored_profile,
                               double package_r,
                               double package_l);
  // EM current-density signoff (EM3): flag every current-carrying segment whose
  // DC current density J = I/A exceeds a per-layer limit.  Reuses (or computes)
  // the static power-grid solution, then runs the pure J = I/A rule engine.
  // default_limit / limits_file supply the per-layer J-limits [A/um^2]; the
  // limits_file (when non-empty) is parsed as "<layer_name> <A_per_um2>" rows.
  EMSignoffResult checkCurrentDensity(odb::dbNet* net,
                                      sta::Scene* corner,
                                      GeneratedSourceType source_type,
                                      const std::string& voltage_source_file,
                                      bool use_prev_solution,
                                      double default_limit,
                                      const std::string& limits_file,
                                      const std::string& report_file);
  void writeSpiceNetwork(odb::dbNet* net,
                         sta::Scene* corner,
                         GeneratedSourceType source_type,
                         const std::string& spice_file,
                         const std::string& voltage_source_file);
  void getIRDropForLayer(odb::dbNet* net,
                         sta::Scene* corner,
                         odb::dbTechLayer* layer,
                         IRDropByPoint& ir_drop) const;
  bool checkConnectivity(odb::dbNet* net,
                         bool floorplanning,
                         const std::string& error_file,
                         bool require_bterm);
  void setDebugGui(bool enable);

  void clearSolvers();

  void setGeneratedSourceSettings(const GeneratedSourceSettings& settings);

  // from dbBlockCallBackObj
  void inDbPostMoveInst(odb::dbInst*) override;
  void inDbNetDestroy(odb::dbNet*) override;
  void inDbBTermPostConnect(odb::dbBTerm*) override;
  void inDbBTermPostDisConnect(odb::dbBTerm*, odb::dbNet*) override;
  void inDbBPinCreate(odb::dbBPin*) override;
  void inDbBPinAddBox(odb::dbBox*) override;
  void inDbBPinRemoveBox(odb::dbBox*) override;
  void inDbBPinDestroy(odb::dbBPin*) override;
  void inDbSWireAddSBox(odb::dbSBox*) override;
  void inDbSWireRemoveSBox(odb::dbSBox*) override;
  void inDbSWirePostDestroySBoxes(odb::dbSWire*) override;

  void getIRDropForLayer(odb::dbNet* net,
                         odb::dbTechLayer* layer,
                         IRDropByPoint& ir_drop) const;

  // Worst (largest) static IR drop [V] measured across every routing layer of
  // `net` by the most recent analyze_power_grid solve.  Returns 0.0 when the
  // net has not been solved.  This is the single measured droop number the
  // analysis-driven sizing engine (pdn_sizing.h) is calibrated against.
  double getWorstIRDrop(odb::dbNet* net) const;

  // Functions of decap cells
  void addDecapMaster(odb::dbMaster* decap_master, double decap_cap);
  void insertDecapCells(double target, const char* net_name);

  odb::dbNet* getLastAnalyzedNet() const { return last_net_; }
  sta::Scene* getLastAnalyzedCorner() const { return last_corner_; }

 private:
  // Functions of decap cells
  odb::dbTechLayer* getLowestLayer(odb::dbNet* db_net);
  odb::dbNet* findPowerNet(const char* net_name);

  IRSolver* getIRSolver(odb::dbNet* net, bool floorplanning);

  odb::dbDatabase* db_ = nullptr;
  sta::dbSta* sta_ = nullptr;
  est::EstimateParasitics* estimate_parasitics_ = nullptr;
  dpl::Opendp* opendp_ = nullptr;
  utl::Logger* logger_ = nullptr;

  gui::HeatMapSourceHandle heatmap_source_;

  bool debug_gui_enabled_ = false;

  GeneratedSourceSettings generated_source_settings_;

  odb::PtrMap<odb::dbNet, std::unique_ptr<IRSolver>> solvers_;
  odb::PtrMap<odb::dbNet, std::map<sta::Scene*, double>> user_voltages_;
  odb::PtrMap<odb::dbInst, std::map<sta::Scene*, float>> user_powers_;

  odb::dbNet* last_net_ = nullptr;
  sta::Scene* last_corner_ = nullptr;
};
}  // namespace psm
