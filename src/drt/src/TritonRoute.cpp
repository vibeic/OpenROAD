// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2025, The OpenROAD Authors

#include "drt/TritonRoute.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "AbstractGraphicsFactory.h"
#include "DesignCallBack.h"
#include "absl/synchronization/mutex.h"
#include "boost/asio/post.hpp"
#include "boost/bind/bind.hpp"
#include "boost/geometry/geometry.hpp"
#include "boost/polygon/polygon.hpp"
#include "db/infra/frSegStyle.h"
#include "db/tech/frConstraint.h"
#include "db/obj/frShape.h"
#include "db/obj/frVia.h"
#include "db/tech/frLayer.h"
#include "db/tech/frTechObject.h"
#include "db/tech/frViaDef.h"
#include "distributed/PinAccessJobDescription.h"
#include "distributed/RoutingCallBack.h"
#include "distributed/RoutingJobDescription.h"
#include "distributed/drUpdate.h"
#include "distributed/frArchive.h"
#include "dr/AbstractDRGraphics.h"
#include "dr/FlexDR.h"
#include "drt-global.h"
#include "drt/PinAccessService.h"
#include "dst/Distributed.h"
#include "dst/JobMessage.h"
#include "frBaseTypes.h"
#include "frDesign.h"
#include "frProfileTask.h"
#include "frRTree.h"
#include "gc/FlexGC.h"
#include "io/GuideProcessor.h"
#include "io/io.h"
#include "odb/db.h"
#include "odb/dbId.h"
#include "odb/dbShape.h"
#include "odb/dbTypes.h"
#include "omp.h"
#include "pa/AbstractPAGraphics.h"
#include "pa/FlexPA.h"
#include "rp/FlexRP.h"
#include "serialization.h"
#include "stt/SteinerTreeBuilder.h"
#include "ta/AbstractTAGraphics.h"
#include "ta/FlexTA.h"
#include "utl/Logger.h"
#include "utl/ScopedTemporaryFile.h"
#include "utl/ServiceRegistry.h"
#include "utl/timer.h"

using odb::dbTechLayerType;

namespace drt {
TritonRoute::TritonRoute(odb::dbDatabase* db,
                         utl::Logger* logger,
                         utl::ServiceRegistry* service_registry,
                         dst::Distributed* dist,
                         stt::SteinerTreeBuilder* stt_builder)
    : debug_(std::make_unique<frDebugSettings>()),
      db_callback_(std::make_unique<DesignCallBack>(this)),
      router_cfg_(std::make_unique<RouterConfiguration>())
{
  if (distributed_) {
    dist_pool_.emplace(1);
  }
  db_ = db;
  logger_ = logger;
  service_registry_ = service_registry;
  dist_ = dist;
  stt_builder_ = stt_builder;
  design_ = std::make_unique<frDesign>(logger_, router_cfg_.get());
  dist->addCallBack(new RoutingCallBack(this, dist, logger));
  service_registry_->provide<PinAccessService>(this);
}

TritonRoute::~TritonRoute()
{
  service_registry_->withdraw<PinAccessService>(this);
}

void TritonRoute::updateDirtyPinAccess()
{
  if (design_ == nullptr || design_->getTopBlock() == nullptr) {
    return;
  }
  updateDirtyPAData();
}

void TritonRoute::initGraphics(
    std::unique_ptr<AbstractGraphicsFactory> graphics_factory)
{
  graphics_factory_ = std::move(graphics_factory);
}

void TritonRoute::setDebugDR(bool on)
{
  debug_->debugDR = on;
}

void TritonRoute::setDebugDumpDR(bool on, const std::string& dumpDir)
{
  debug_->debugDumpDR = on;
  debug_->dumpDir = dumpDir;
}

void TritonRoute::setDebugSnapshotDir(const std::string& snapshotDir)
{
  debug_->snapshotDir = snapshotDir;
}

void TritonRoute::setDebugMaze(bool on)
{
  debug_->debugMaze = on;
}

void TritonRoute::setDebugPA(bool on)
{
  debug_->debugPA = on;
}

void TritonRoute::setDebugTA(bool on)
{
  debug_->debugTA = on;
}

void TritonRoute::setDistributed(bool on)
{
  distributed_ = on;
  if (distributed_ && !dist_pool_.has_value()) {
    dist_pool_.emplace(1);
  }
}

void TritonRoute::setDebugWriteNetTracks(bool on)
{
  debug_->writeNetTracks = on;
}

void TritonRoute::setDumpLastWorker(bool on)
{
  debug_->dumpLastWorker = on;
}

void TritonRoute::setWorkerIpPort(const char* ip, unsigned short port)
{
  dist_ip_ = ip;
  dist_port_ = port;
}

void TritonRoute::setSharedVolume(const std::string& vol)
{
  shared_volume_ = vol;
  if (!shared_volume_.empty() && shared_volume_.back() != '/') {
    shared_volume_ += '/';
  }
}

void TritonRoute::setDebugNetName(const char* name)
{
  debug_->netName = name;
}

void TritonRoute::setDebugPinName(const char* name)
{
  debug_->pinName = name;
}

void TritonRoute::setDebugBox(int x1, int y1, int x2, int y2)
{
  debug_->box.init(x1, y1, x2, y2);
}

void TritonRoute::setDebugIter(int iter)
{
  debug_->iter = iter;
}

void TritonRoute::setDebugPaMarkers(bool on)
{
  debug_->paMarkers = on;
}

void TritonRoute::setDebugPaEdge(bool on)
{
  debug_->paEdge = on;
}

void TritonRoute::setDebugPaCommit(bool on)
{
  debug_->paCommit = on;
}

RipUpMode getMode(int ripupMode)
{
  switch (ripupMode) {
    case 0:
      return RipUpMode::DRC;
    case 1:
      return RipUpMode::ALL;
    case 2:
      return RipUpMode::NEARDRC;
    default:
      return RipUpMode::INCR;
  }
}

void TritonRoute::setDebugWorkerParams(int mazeEndIter,
                                       int drcCost,
                                       int markerCost,
                                       int fixedShapeCost,
                                       float markerDecay,
                                       int ripupMode,
                                       int followGuide)
{
  debug_->mazeEndIter = mazeEndIter;
  debug_->drcCost = drcCost;
  debug_->markerCost = markerCost;
  debug_->fixedShapeCost = fixedShapeCost;
  debug_->markerDecay = markerDecay;
  debug_->ripupMode = ripupMode;
  debug_->followGuide = followGuide;
}

int TritonRoute::getNumDRVs() const
{
  if (num_drvs_ < 0) {
    logger_->error(DRT, 2, "Detailed routing has not been run yet.");
  }
  return num_drvs_;
}

std::string TritonRoute::runDRWorker(const std::string& workerStr,
                                     FlexDRViaData* viaData)
{
  auto worker = FlexDRWorker::load(
      workerStr, viaData, design_.get(), logger_, router_cfg_.get());
  worker->setSharedVolume(shared_volume_);
  worker->setDebugSettings(debug_.get());
  if (graphics_factory_->guiActive() && debug_->debugDR) {
    std::unique_ptr<AbstractDRGraphics> dr_graphics
        = graphics_factory_->makeUniqueDRGraphics();
    worker->setGraphics(dr_graphics.get());
    dr_graphics->startIter(worker->getDRIter(), router_cfg_.get());
  }
  std::string result = worker->reloadedMain();
  return result;
}

void TritonRoute::debugSingleWorker(const std::string& dumpDir,
                                    const std::string& drcRpt)
{
  {
    io::Writer writer(getDesign(), logger_);
    writer.updateTrackAssignment(db_->getChip()->getBlock());
  }
  FlexDRViaData viaData;
  std::ifstream viaDataFile(fmt::format("{}/viadata.bin", dumpDir),
                            std::ios::binary);
  frIArchive ar(viaDataFile);
  ar >> viaData;

  std::ifstream workerFile(fmt::format("{}/worker.bin", dumpDir),
                           std::ios::binary);
  std::string workerStr((std::istreambuf_iterator<char>(workerFile)),
                        std::istreambuf_iterator<char>());
  workerFile.close();
  auto worker = FlexDRWorker::load(
      workerStr, &viaData, design_.get(), logger_, router_cfg_.get());
  std::unique_ptr<AbstractDRGraphics> graphics
      = debug_->debugDR ? graphics_factory_->makeUniqueDRGraphics() : nullptr;
  worker->setGraphics(graphics.get());
  if (debug_->mazeEndIter != -1) {
    worker->setMazeEndIter(debug_->mazeEndIter);
  }
  if (debug_->markerCost != -1) {
    worker->setMarkerCost(debug_->markerCost);
  }
  if (debug_->drcCost != -1) {
    worker->setDrcCost(debug_->drcCost);
  }
  if (debug_->fixedShapeCost != -1) {
    worker->setFixedShapeCost(debug_->fixedShapeCost);
  }
  if (debug_->markerDecay != -1) {
    worker->setMarkerDecay(debug_->markerDecay);
  }
  if (debug_->ripupMode != -1) {
    worker->setRipupMode(getMode(debug_->ripupMode));
  }
  if (debug_->followGuide != -1) {
    worker->setFollowGuide((debug_->followGuide == 1));
  }
  worker->setSharedVolume(shared_volume_);
  worker->setDebugSettings(debug_.get());
  if (graphics) {
    graphics->startIter(worker->getDRIter(), router_cfg_.get());
  }
  worker->reloadedMain();
  bool updated = worker->end(design_.get());
  debugPrint(logger_,
             utl::DRT,
             "autotuner",
             1,
             "End number of markers {}. Updated={}",
             worker->getBestNumMarkers(),
             updated);
  if (updated) {
    reportDRC(drcRpt,
              design_->getTopBlock()->getMarkers(),
              "DRC - debug single worker",
              worker->getDrcBox());
  }
}

void TritonRoute::updateGlobals(const char* file_name)
{
  std::ifstream file(file_name);
  if (!file.good()) {
    return;
  }
  frIArchive ar(file);
  registerTypes(ar);
  serializeGlobals(ar, router_cfg_.get());
  file.close();
}

void TritonRoute::resetDb(const char* file_name)
{
  std::ifstream stream;
  stream.open(file_name, std::ios::binary);
  try {
    if (db_->getChip() && db_->getChip()->getBlock()) {
      logger_->error(
          DRT,
          9947,
          "You can't load a new db file as the db is already populated");
    }

    stream.exceptions(std::ifstream::failbit | std::ifstream::badbit
                      | std::ios::eofbit);

    db_->read(stream);
  } catch (const std::ios_base::failure& f) {
    logger_->error(
        DRT, 9954, "odb file {} is invalid: {}", file_name, f.what());
  }
  design_ = std::make_unique<frDesign>(logger_, router_cfg_.get());
  initDesign();
  if (!db_->getChip()->getBlock()->getAccessPoints().empty()) {
    initGuide();
    prep();
    design_->getRegionQuery()->initDRObj();
  }
}

void TritonRoute::clearDesign()
{
  design_ = std::make_unique<frDesign>(logger_, router_cfg_.get());
}

static void deserializeUpdate(frDesign* design,
                              const std::string& updateStr,
                              std::vector<drUpdate>& updates)
{
  std::ifstream file(updateStr.c_str());
  frIArchive ar(file);
  ar.setDesign(design);
  registerTypes(ar);
  ar >> updates;
  file.close();
}

static void deserializeUpdates(frDesign* design,
                               const std::string& updateStr,
                               std::vector<std::vector<drUpdate>>& updates)
{
  std::ifstream file(updateStr.c_str());
  frIArchive ar(file);
  ar.setDesign(design);
  registerTypes(ar);
  ar >> updates;
  file.close();
}

void TritonRoute::updateDesign(const std::vector<std::string>& updatesStrs,
                               int num_threads)
{
  omp_set_num_threads(num_threads);
  std::vector<std::vector<drUpdate>> updates(updatesStrs.size());
#pragma omp parallel for schedule(dynamic)
  for (int i = 0; i < updatesStrs.size(); i++) {
    deserializeUpdate(design_.get(), updatesStrs.at(i), updates[i]);
  }
  applyUpdates(updates);
}

void TritonRoute::updateDesign(const std::string& path, int num_threads)
{
  omp_set_num_threads(num_threads);
  std::vector<std::vector<drUpdate>> updates;
  deserializeUpdates(design_.get(), path, updates);
  applyUpdates(updates);
}

void TritonRoute::applyUpdates(
    const std::vector<std::vector<drUpdate>>& updates)
{
  auto topBlock = design_->getTopBlock();
  auto regionQuery = design_->getRegionQuery();
  const auto maxSz = updates[0].size();
  for (int j = 0; j < maxSz; j++) {
    for (const auto& update_batch : updates) {
      if (update_batch.size() <= j) {
        continue;
      }
      const auto& update = update_batch[j];
      switch (update.getType()) {
        case drUpdate::REMOVE_FROM_BLOCK: {
          auto id = update.getIndexInOwner();
          auto marker = design_->getTopBlock()->getMarker(id);
          regionQuery->removeMarker(marker);
          topBlock->removeMarker(marker);
          break;
        }
        case drUpdate::REMOVE_FROM_NET:
        case drUpdate::REMOVE_FROM_RQ: {
          auto net = update.getNet();
          auto id = update.getIndexInOwner();
          auto pinfig = net->getPinFig(id);
          switch (pinfig->typeId()) {
            case frcPathSeg: {
              auto seg = static_cast<frPathSeg*>(pinfig);
              regionQuery->removeDRObj(seg);
              if (update.getType() == drUpdate::REMOVE_FROM_NET) {
                net->removeShape(seg);
              }
              break;
            }
            case frcPatchWire: {
              auto pwire = static_cast<frPatchWire*>(pinfig);
              regionQuery->removeDRObj(pwire);
              if (update.getType() == drUpdate::REMOVE_FROM_NET) {
                net->removePatchWire(pwire);
              }
              break;
            }
            case frcVia: {
              auto via = static_cast<frVia*>(pinfig);
              regionQuery->removeDRObj(via);
              if (update.getType() == drUpdate::REMOVE_FROM_NET) {
                net->removeVia(via);
              }
              break;
            }
            default:
              logger_->error(
                  DRT, 9999, "unknown update type {}", pinfig->typeId());
              break;
          }
          break;
        }
        case drUpdate::ADD_SHAPE:
        case drUpdate::ADD_SHAPE_NET_ONLY: {
          switch (update.getObjTypeId()) {
            case frcPathSeg: {
              auto net = update.getNet();
              frPathSeg seg = update.getPathSeg();
              std::unique_ptr<frShape> uShape
                  = std::make_unique<frPathSeg>(seg);
              auto sptr = uShape.get();
              net->addShape(std::move(uShape));
              if (update.getType() == drUpdate::ADD_SHAPE) {
                regionQuery->addDRObj(sptr);
              }
              break;
            }
            case frcPatchWire: {
              auto net = update.getNet();
              frPatchWire pwire = update.getPatchWire();
              std::unique_ptr<frShape> uShape
                  = std::make_unique<frPatchWire>(pwire);
              auto sptr = uShape.get();
              net->addPatchWire(std::move(uShape));
              if (update.getType() == drUpdate::ADD_SHAPE) {
                regionQuery->addDRObj(sptr);
              }
              break;
            }
            case frcVia: {
              auto net = update.getNet();
              frVia via = update.getVia();
              auto uVia = std::make_unique<frVia>(via);
              auto sptr = uVia.get();
              net->addVia(std::move(uVia));
              if (update.getType() == drUpdate::ADD_SHAPE) {
                regionQuery->addDRObj(sptr);
              }
              break;
            }
            default: {
              frMarker marker = update.getMarker();
              auto uMarker = std::make_unique<frMarker>(marker);
              auto sptr = uMarker.get();
              topBlock->addMarker(std::move(uMarker));
              regionQuery->addMarker(sptr);
              break;
            }
          }
          break;
        }
        case drUpdate::ADD_GUIDE: {
          frPathSeg seg = update.getPathSeg();
          std::unique_ptr<frPathSeg> uSeg = std::make_unique<frPathSeg>(seg);
          auto net = update.getNet();
          uSeg->addToNet(net);
          std::vector<std::unique_ptr<frConnFig>> tmp;
          tmp.push_back(std::move(uSeg));
          auto idx = update.getIndexInOwner();
          if (idx < 0 || idx >= net->getGuides().size()) {
            logger_->error(DRT,
                           9199,
                           "Guide {} out of range {}",
                           idx,
                           net->getGuides().size());
          }
          const auto& guide = net->getGuides().at(idx);
          guide->setRoutes(tmp);
          break;
        }
        case drUpdate::UPDATE_SHAPE: {
          auto net = update.getNet();
          auto id = update.getIndexInOwner();
          auto pinfig = net->getPinFig(id);
          switch (pinfig->typeId()) {
            case frcPathSeg: {
              auto seg = static_cast<frPathSeg*>(pinfig);
              frPathSeg updatedSeg = update.getPathSeg();
              seg->setPoints(updatedSeg.getBeginPoint(),
                             updatedSeg.getEndPoint());
              frSegStyle style = updatedSeg.getStyle();
              seg->setStyle(style);
              regionQuery->addDRObj(seg);
              break;
            }
            case frcVia: {
              auto via = static_cast<frVia*>(pinfig);
              frVia updatedVia = update.getVia();
              via->setBottomConnected(updatedVia.isBottomConnected());
              via->setTopConnected(updatedVia.isTopConnected());
              break;
            }
            default:
              break;
          }
        }
      }
    }
  }
}

bool TritonRoute::initGuide()
{
  io::GuideProcessor guide_processor(
      getDesign(), db_, logger_, router_cfg_.get());
  bool guideOk = guide_processor.readGuides();
  guide_processor.processGuides();
  return guideOk;
}
void TritonRoute::initDesign()
{
  if (db_ == nullptr || db_->getChip() == nullptr
      || db_->getChip()->getBlock() == nullptr) {
    logger_->error(utl::DRT, 151, "Database, chip or block not initialized.");
  }
  const bool design_exists = getDesign()->getTopBlock() != nullptr;
  io::Parser parser(db_, getDesign(), logger_, router_cfg_.get());
  if (design_exists) {
    parser.updateDesign();
  } else {
    parser.readTechAndLibs(db_);
    parser.readDesign(db_);
  }
  auto tech = getDesign()->getTech();

  if (!router_cfg_->VIAINPIN_BOTTOMLAYER_NAME.empty()) {
    frLayer* layer = tech->getLayer(router_cfg_->VIAINPIN_BOTTOMLAYER_NAME);
    if (layer) {
      router_cfg_->VIAINPIN_BOTTOMLAYERNUM = layer->getLayerNum();
    } else {
      logger_->warn(utl::DRT,
                    606,
                    "via in pin bottom layer {} not found.",
                    router_cfg_->VIAINPIN_BOTTOMLAYER_NAME);
    }
  }

  if (!router_cfg_->VIAINPIN_TOPLAYER_NAME.empty()) {
    frLayer* layer = tech->getLayer(router_cfg_->VIAINPIN_TOPLAYER_NAME);
    if (layer) {
      router_cfg_->VIAINPIN_TOPLAYERNUM = layer->getLayerNum();
    } else {
      logger_->warn(utl::DRT,
                    607,
                    "via in pin top layer {} not found.",
                    router_cfg_->VIAINPIN_TOPLAYER_NAME);
    }
  }

  if (!router_cfg_->VIA_ACCESS_LAYER_NAME.empty()) {
    frLayer* layer = tech->getLayer(router_cfg_->VIA_ACCESS_LAYER_NAME);
    if (layer) {
      router_cfg_->VIA_ACCESS_LAYERNUM = layer->getLayerNum();
    } else {
      logger_->warn(utl::DRT,
                    609,
                    "via access layer {} not found.",
                    router_cfg_->VIA_ACCESS_LAYER_NAME);
    }
  }

  if (!router_cfg_->REPAIR_PDN_LAYER_NAME.empty()) {
    frLayer* layer = tech->getLayer(router_cfg_->REPAIR_PDN_LAYER_NAME);
    if (layer) {
      router_cfg_->GC_IGNORE_PDN_LAYER_NUM = layer->getLayerNum();
    } else {
      logger_->warn(utl::DRT,
                    617,
                    "PDN layer {} not found.",
                    router_cfg_->REPAIR_PDN_LAYER_NAME);
    }
  }
  if (!design_exists) {
    parser.postProcess();
    db_callback_->addOwner(db_->getChip()->getBlock());
    initGraphics();
  }
}

void TritonRoute::initGraphics()
{
  graphics_factory_->reset(
      debug_.get(), design_.get(), db_, logger_, router_cfg_.get());
}

void TritonRoute::prep()
{
  FlexRP rp(getDesign(), logger_, router_cfg_.get());
  rp.main();
}

void TritonRoute::ta()
{
  std::unique_ptr<FlexTA> ta = std::make_unique<FlexTA>(
      getDesign(), logger_, router_cfg_.get(), distributed_);
  if (debug_->debugTA) {
    ta->setDebug(graphics_factory_->makeUniqueTAGraphics());
  }
  ta->main();
  if (debug_->writeNetTracks) {
    io::Writer writer(getDesign(), logger_);
    writer.updateTrackAssignment(db_->getChip()->getBlock());
  }
}

void TritonRoute::dr()
{
  num_drvs_ = -1;
  dr_ = std::make_unique<FlexDR>(
      this, getDesign(), logger_, db_, router_cfg_.get());
  if (debug_->debugDR) {
    dr_->setDebug(graphics_factory_->makeUniqueDRGraphics());
  }
  if (distributed_) {
    dr_->setDistributed(dist_, dist_ip_, dist_port_, shared_volume_);
  }
  if (router_cfg_->SINGLE_STEP_DR) {
    dr_->init();
  } else {
    dr_->main();
  }
}

void TritonRoute::stepDR(int size,
                         int offset,
                         int mazeEndIter,
                         frUInt4 workerDRCCost,
                         frUInt4 workerMarkerCost,
                         frUInt4 workerFixedShapeCost,
                         float workerMarkerDecay,
                         int ripupMode,
                         bool followGuide)
{
  FlexDR::SearchRepairArgs args = {.size = size,
                                   .offset = offset,
                                   .mazeEndIter = mazeEndIter,
                                   .workerDRCCost = workerDRCCost,
                                   .workerMarkerCost = workerMarkerCost,
                                   .workerFixedShapeCost = workerFixedShapeCost,
                                   .workerMarkerDecay = workerMarkerDecay,
                                   .ripupMode = getMode(ripupMode),
                                   .followGuide = followGuide};
  dr_->searchRepair(args);
  dr_->incIter();
  num_drvs_ = design_->getTopBlock()->getNumMarkers();
}
void TritonRoute::endFR()
{
  if (router_cfg_->SINGLE_STEP_DR) {
    dr_->end(/* done */ true);
  }
  dr_.reset();
  io::Writer writer(getDesign(), logger_);
  writer.updateDb(db_, router_cfg_.get());

  num_drvs_ = design_->getTopBlock()->getNumMarkers();

  repairPDNVias();
}

void TritonRoute::repairPDNVias()
{
  if (router_cfg_->REPAIR_PDN_LAYER_NAME.empty()) {
    return;
  }

  auto dbBlock = db_->getChip()->getBlock();
  auto pdnLayer
      = design_->getTech()->getLayer(router_cfg_->REPAIR_PDN_LAYER_NAME);
  frLayerNum pdnLayerNum = pdnLayer->getLayerNum();
  frList<std::unique_ptr<frMarker>> markers;
  auto blockBox = design_->getTopBlock()->getBBox();
  router_cfg_->REPAIR_PDN_LAYER_NUM = pdnLayerNum;
  router_cfg_->GC_IGNORE_PDN_LAYER_NUM = -1;
  getDRCMarkers(markers, blockBox);
  markers.erase(std::remove_if(markers.begin(),
                               markers.end(),
                               [pdnLayerNum](const auto& marker) {
                                 if (marker->getLayerNum() != pdnLayerNum) {
                                   return true;
                                 }
                                 for (auto src : marker->getSrcs()) {
                                   if (src->typeId() == frcNet) {
                                     frNet* net = static_cast<frNet*>(src);
                                     if (net->getType().isSupply()) {
                                       return false;
                                     }
                                   }
                                 }
                                 return true;
                               }),
                markers.end());

  if (markers.empty()) {
    // nothing to do
    return;
  }

  std::vector<std::pair<odb::Rect, odb::dbId<odb::dbSBox>>> all_vias;
  std::vector<std::pair<odb::Rect, odb::dbId<odb::dbSBox>>> block_vias;
  for (auto* net : dbBlock->getNets()) {
    if (!net->getSigType().isSupply()) {
      continue;
    }
    for (auto* swire : net->getSWires()) {
      for (auto* wire : swire->getWires()) {
        if (!wire->isVia()) {
          continue;
        }
        //
        std::vector<odb::dbShape> via_boxes;
        wire->getViaBoxes(via_boxes);
        for (const auto& via_box : via_boxes) {
          auto* layer = via_box.getTechLayer();
          if (layer != pdnLayer->getDbLayer()) {
            continue;
          }

          if (wire->getTechVia() != nullptr) {
            all_vias.emplace_back(via_box.getBox(), wire->getId());
          } else {
            block_vias.emplace_back(via_box.getBox(), wire->getId());
          }
        }
      }
    }
  }

  const RTree<odb::dbId<odb::dbSBox>> pdnBlockViaTree(block_vias);
  std::set<odb::dbId<odb::dbSBox>> removedBoxes;
  for (const auto& marker : markers) {
    odb::Rect queryBox;
    marker->getBBox().bloat(1, queryBox);
    std::vector<rq_box_value_t<odb::dbId<odb::dbSBox>>> results;
    pdnBlockViaTree.query(bgi::intersects(queryBox), back_inserter(results));
    for (auto& [rect, bid] : results) {
      if (removedBoxes.find(bid) == removedBoxes.end()) {
        removedBoxes.insert(bid);
        auto boxPtr = odb::dbSBox::getSBox(dbBlock, bid);

        const auto new_vias = boxPtr->smashVia();
        for (auto* new_via : new_vias) {
          std::vector<odb::dbShape> via_boxes;
          new_via->getViaBoxes(via_boxes);
          for (const auto& via_box : via_boxes) {
            auto* layer = via_box.getTechLayer();
            if (layer != pdnLayer->getDbLayer()) {
              continue;
            }
            all_vias.emplace_back(via_box.getBox(), new_via->getId());
          }
        }

        if (!new_vias.empty()) {
          odb::dbSBox::destroy(boxPtr);
        }
      }
    }
  }
  removedBoxes.clear();

  const RTree<odb::dbId<odb::dbSBox>> pdnTree(all_vias);
  for (const auto& marker : markers) {
    odb::Rect queryBox;
    marker->getBBox().bloat(1, queryBox);
    std::vector<rq_box_value_t<odb::dbId<odb::dbSBox>>> results;
    pdnTree.query(bgi::intersects(queryBox), back_inserter(results));
    for (auto& [rect, bid] : results) {
      if (removedBoxes.find(bid) == removedBoxes.end()) {
        removedBoxes.insert(bid);
        odb::dbSBox::destroy(odb::dbSBox::getSBox(dbBlock, bid));
      }
    }
  }
  logger_->report("Removed {} pdn vias on layer {}",
                  removedBoxes.size(),
                  pdnLayer->getName());
}

void TritonRoute::reportConstraints()
{
  getDesign()->getTech()->printAllConstraints(logger_);
}

bool TritonRoute::writeGlobals(const std::string& name)
{
  std::ofstream file(name);
  if (!file.good()) {
    return false;
  }
  frOArchive ar(file);
  registerTypes(ar);
  serializeGlobals(ar, router_cfg_.get());
  file.close();
  return true;
}

void TritonRoute::sendDesignDist()
{
  if (distributed_) {
    std::string design_path = fmt::format("{}DESIGN.db", shared_volume_);
    std::string router_cfg_path
        = fmt::format("{}DESIGN.router_cfg", shared_volume_);

    db_->write(utl::OutStreamHandler(design_path.c_str(), true).getStream());
    writeGlobals(router_cfg_path);
    dst::JobMessage msg(dst::JobMessage::kUpdateDesign,
                        dst::JobMessage::kBroadcast),
        result(dst::JobMessage::kNone);
    std::unique_ptr<dst::JobDescription> desc
        = std::make_unique<RoutingJobDescription>();
    RoutingJobDescription* rjd
        = static_cast<RoutingJobDescription*>(desc.get());
    rjd->setDesignPath(design_path);
    rjd->setSharedDir(shared_volume_);
    rjd->setGlobalsPath(router_cfg_path);
    rjd->setDesignUpdate(false);
    msg.setJobDescription(std::move(desc));
    bool ok = dist_->sendJob(msg, dist_ip_.c_str(), dist_port_, result);
    if (!ok) {
      logger_->error(DRT, 12304, "Updating design remotely failed");
    }
  }
  design_->clearUpdates();
}
static void serializeUpdatesBatch(const std::vector<drUpdate>& batch,
                                  const std::string& file_name)
{
  std::ofstream file(file_name.c_str());
  frOArchive ar(file);
  registerTypes(ar);
  ar << batch;
  file.close();
}

void TritonRoute::sendGlobalsUpdates(const std::string& router_cfg_path,
                                     const std::string& serializedViaData)
{
  if (!distributed_) {
    return;
  }
  ProfileTask task("DIST: SENDING GLOBALS");
  dst::JobMessage msg(dst::JobMessage::kUpdateDesign,
                      dst::JobMessage::kBroadcast),
      result(dst::JobMessage::kNone);
  std::unique_ptr<dst::JobDescription> desc
      = std::make_unique<RoutingJobDescription>();
  RoutingJobDescription* rjd = static_cast<RoutingJobDescription*>(desc.get());
  rjd->setGlobalsPath(router_cfg_path);
  rjd->setSharedDir(shared_volume_);
  rjd->setViaData(serializedViaData);
  msg.setJobDescription(std::move(desc));
  bool ok = dist_->sendJob(msg, dist_ip_.c_str(), dist_port_, result);
  if (!ok) {
    logger_->error(DRT, 9504, "Updating router_cfg remotely failed");
  }
}

void TritonRoute::sendDesignUpdates(const std::string& router_cfg_path,
                                    int num_threads)
{
  if (!distributed_) {
    return;
  }
  if (!design_->hasUpdates()) {
    return;
  }
  std::unique_ptr<ProfileTask> serializeTask;
  if (design_->getVersion() == 0) {
    serializeTask = std::make_unique<ProfileTask>("DIST: SERIALIZE_TA");
  } else {
    serializeTask = std::make_unique<ProfileTask>("DIST: SERIALIZE_UPDATES");
  }
  const auto& designUpdates = design_->getUpdates();
  omp_set_num_threads(num_threads);
  std::vector<std::string> updates(designUpdates.size());
#pragma omp parallel for schedule(dynamic)
  for (int i = 0; i < designUpdates.size(); i++) {
    updates[i] = fmt::format("{}updates_{}.bin", shared_volume_, i);
    serializeUpdatesBatch(designUpdates.at(i), updates[i]);
  }
  serializeTask->done();
  std::unique_ptr<ProfileTask> task;
  if (design_->getVersion() == 0) {
    task = std::make_unique<ProfileTask>("DIST: SENDING_TA");
  } else {
    task = std::make_unique<ProfileTask>("DIST: SENDING_UDPATES");
  }
  dst::JobMessage msg(dst::JobMessage::kUpdateDesign,
                      dst::JobMessage::kBroadcast),
      result(dst::JobMessage::kNone);
  std::unique_ptr<dst::JobDescription> desc
      = std::make_unique<RoutingJobDescription>();
  RoutingJobDescription* rjd = static_cast<RoutingJobDescription*>(desc.get());
  rjd->setUpdates(updates);
  rjd->setGlobalsPath(router_cfg_path);
  rjd->setSharedDir(shared_volume_);
  rjd->setDesignUpdate(true);
  msg.setJobDescription(std::move(desc));
  bool ok = dist_->sendJob(msg, dist_ip_.c_str(), dist_port_, result);
  if (!ok) {
    logger_->error(DRT, 304, "Updating design remotely failed");
  }
  task->done();
  design_->clearUpdates();
  design_->incrementVersion();
}

namespace {
namespace gtl = boost::polygon;

// Metal rectangle on the repair layer that the growth must respect.
//  - hard  (different net / blockage / supply): the grown rectangle must keep
//           >= spc to it.
//  - soft  (same net as the pad): may be merged into (overlap), and any
//           pre-existing sub-spc relationship (two shapes connected through
//           std-cell metal in the sign-off spacing view, hence DRC-clean in the
//           baseline) must be PRESERVED, never made tighter.
struct MetalObs
{
  odb::Rect box;
  bool hard;
};

// Squared euclidean edge-to-edge distance between two rectangles (0 if they
// overlap or touch). Matches the euclidian metric the sign-off deck measures.
int64_t distSq(const odb::Rect& a, const odb::Rect& b)
{
  const frCoord dx = std::max<frCoord>(0, std::max(b.xMin() - a.xMax(), a.xMin() - b.xMax()));
  const frCoord dy = std::max<frCoord>(0, std::max(b.yMin() - a.yMax(), a.yMin() - b.yMax()));
  return static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
}

// True iff the grown candidate `cand` (which contains `pad`) is spacing-legal.
// For every obstacle the candidate must not come CLOSER than the smaller of the
// min spacing and the obstacle's pre-existing euclidean distance to the pad.
// Different-net metal is always >= spc away in a clean design, so this reduces
// to the ordinary >= spc rule for it; a same-net shape that is already inside
// spc (connected through excluded metal, hence baseline-clean) may be kept at
// that distance or merged, but never approached more tightly -- so the repair
// can never introduce a new external-spacing violation.
bool patchSpacingSafe(const odb::Rect& cand,
                      const odb::Rect& pad,
                      frCoord spc,
                      const std::vector<MetalObs>& obstacles)
{
  const int64_t spc_sq = static_cast<int64_t>(spc) * spc;
  for (const auto& obs : obstacles) {
    const int64_t dc = distSq(cand, obs.box);
    if (dc >= spc_sq) {
      continue;  // comfortably clear
    }
    if (obs.hard) {
      return false;  // never within spc of a different net (overlap included)
    }
    if (cand.intersects(obs.box)) {
      continue;  // merged into same-net metal: one polygon, no spacing rule
    }
    const int64_t d0 = distSq(pad, obs.box);  // pre-existing clean distance
    if (dc < std::min(spc_sq, d0)) {
      return false;  // approached same-net metal more tightly than the baseline
    }
  }
  return true;
}

// Smallest length, snapped up to the manufacturing grid, so that
// length * other_dim >= min_area (never shorter than keep_dim).
frCoord neededLen(frArea min_area, frCoord other_dim, frCoord keep_dim, frCoord mgrid)
{
  const double raw
      = static_cast<double>(min_area) / static_cast<double>(std::max<frCoord>(1, other_dim));
  frCoord snapped = static_cast<frCoord>(std::ceil(raw / mgrid - 1e-9)) * mgrid;
  return std::max(snapped, keep_dim);
}

// Resolve the owning signal net of a queried metal object, or nullptr for
// blockages / supply / floating metal (which are always treated as hard).
frNet* obstacleNet(frBlockObject* obj)
{
  if (obj == nullptr) {
    return nullptr;
  }
  switch (obj->typeId()) {
    case frcInstTerm: {
      auto* t = static_cast<frInstTerm*>(obj);
      return t->hasNet() ? t->getNet() : nullptr;
    }
    case frcBTerm: {
      auto* t = static_cast<frBTerm*>(obj);
      return t->hasNet() ? t->getNet() : nullptr;
    }
    case frcPathSeg:
    case frcVia:
    case frcPatchWire: {
      auto* s = static_cast<frPinFig*>(obj);
      return s->hasNet() ? s->getNet() : nullptr;
    }
    default:
      return nullptr;  // blockage / obstruction: hard
  }
}

// Grow the undersized rectangular pad into a legal rectangle whose area is
// >= min_area. Computes the real room available on each side (bounded by
// different-net metal only; same-net metal may be merged into), then tries
// single-axis growth (partial + opposite fill) and, if that is not enough, a
// 2-axis expansion. Every emitted rectangle is re-verified against the full
// obstacle set so diagonal/corner cases can never slip through. On success
// writes the grown rectangle to `out` and returns true.
bool findMinAreaPatch(const odb::Rect& pad,
                      frNet* pad_net,
                      frArea min_area,
                      frCoord spc,
                      frCoord mgrid,
                      const odb::Rect& die,
                      frLayerNum lNum,
                      frRegionQuery* rq,
                      const std::vector<std::pair<odb::Rect, frNet*>>& added_patches,
                      odb::Rect& out)
{
  const frCoord w = pad.dx();
  const frCoord h = pad.dy();
  if (w <= 0 || h <= 0) {
    return false;
  }

  // Gather every OTHER metal rect on lNum in a generous window around the pad,
  // classified same-net (soft, mergeable) vs different-net/blockage (hard).
  const frCoord reach = neededLen(min_area, std::min(w, h), std::max(w, h), mgrid)
                        + 2 * spc + 2 * mgrid;
  const odb::Rect win(pad.xMin() - reach,
                      pad.yMin() - reach,
                      pad.xMax() + reach,
                      pad.yMax() + reach);
  std::vector<MetalObs> obstacles;
  frRegionQuery::Objects<frBlockObject> dr_objs;
  frRegionQuery::Objects<frBlockObject> fixed_objs;
  rq->queryDRObj(win, lNum, dr_objs);
  rq->query(win, lNum, fixed_objs);
  auto add_obs = [&](const odb::Rect& box, frBlockObject* obj) {
    if (box.overlaps(pad)) {
      return;  // part of the pad polygon being grown (same net)
    }
    const bool hard = (pad_net == nullptr) || (obstacleNet(obj) != pad_net);
    obstacles.push_back({box, hard});
  };
  for (const auto& [box, obj] : dr_objs) {
    add_obs(box, obj);
  }
  for (const auto& [box, obj] : fixed_objs) {
    add_obs(box, obj);
  }
  for (const auto& [r, r_net] : added_patches) {
    if (!r.overlaps(pad) && r.intersects(win)) {
      // A patch already added for the SAME net may be merged into (soft);
      // a different net's patch must keep spacing (hard).
      const bool hard = (pad_net == nullptr) || (r_net != pad_net);
      obstacles.push_back({r, hard});
    }
  }

  auto snap_down = [&](frCoord v) -> frCoord {
    return v <= 0 ? 0 : (v / mgrid) * mgrid;
  };

  // Spacing-safe room available extending each side, keeping the perpendicular
  // dimension equal to the pad. Only HARD (different-net) metal limits the
  // extension; same-net metal can be grown through (merged). A hard obstacle
  // offset `off` along the perpendicular axis only demands sqrt(spc^2-off^2) of
  // clearance along the growth axis (euclidean), so the sized candidate stays
  // >= spc from diagonally-offset different-net metal and will pass the verify.
  auto perp_clear = [&](frCoord off) -> frCoord {
    if (off >= spc) {
      return -1;  // far enough perpendicular: never constrains this axis
    }
    if (off <= 0) {
      return spc;
    }
    const double c = std::sqrt(static_cast<double>(spc) * spc
                               - static_cast<double>(off) * off);
    return static_cast<frCoord>(std::ceil(c - 1e-9));
  };
  frCoord eR = die.xMax() - pad.xMax();
  frCoord eL = pad.xMin() - die.xMin();
  frCoord eU = die.yMax() - pad.yMax();
  frCoord eD = pad.yMin() - die.yMin();
  for (const auto& obs : obstacles) {
    if (!obs.hard) {
      continue;
    }
    const odb::Rect& o = obs.box;
    const frCoord off_x = std::max<frCoord>(
        0, std::max(o.xMin() - pad.xMax(), pad.xMin() - o.xMax()));
    const frCoord off_y = std::max<frCoord>(
        0, std::max(o.yMin() - pad.yMax(), pad.yMin() - o.yMax()));
    const frCoord clr_v = perp_clear(off_x);  // +/- y growth (keep pad x-range)
    if (clr_v >= 0) {
      if (o.yMin() >= pad.yMax()) {
        eU = std::min(eU, o.yMin() - clr_v - pad.yMax());
      }
      if (o.yMax() <= pad.yMin()) {
        eD = std::min(eD, pad.yMin() - clr_v - o.yMax());
      }
    }
    const frCoord clr_h = perp_clear(off_y);  // +/- x growth (keep pad y-range)
    if (clr_h >= 0) {
      if (o.xMin() >= pad.xMax()) {
        eR = std::min(eR, o.xMin() - clr_h - pad.xMax());
      }
      if (o.xMax() <= pad.xMin()) {
        eL = std::min(eL, pad.xMin() - clr_h - o.xMax());
      }
    }
  }
  eR = snap_down(eR);
  eL = snap_down(eL);
  eU = snap_down(eU);
  eD = snap_down(eD);

  const frCoord need_w = neededLen(min_area, h, w, mgrid);  // keep height h
  const frCoord need_h = neededLen(min_area, w, h, mgrid);  // keep width w

  // Build the build-order candidate list.
  auto gen_cands = [&](std::vector<odb::Rect>& cands) {
    // Single-axis X growth (prefer filling right, then left), keep height.
    if (w + eL + eR >= need_w) {
      const frCoord grow = need_w - w;
      const frCoord gr = std::min(eR, grow);
      cands.emplace_back(pad.xMin() - (grow - gr), pad.yMin(), pad.xMax() + gr, pad.yMax());
      const frCoord gl = std::min(eL, grow);
      cands.emplace_back(pad.xMin() - gl, pad.yMin(), pad.xMax() + (grow - gl), pad.yMax());
    }
    // Single-axis Y growth (prefer up, then down), keep width.
    if (h + eU + eD >= need_h) {
      const frCoord grow = need_h - h;
      const frCoord gu = std::min(eU, grow);
      cands.emplace_back(pad.xMin(), pad.yMin() - (grow - gu), pad.xMax(), pad.yMax() + gu);
      const frCoord gd = std::min(eD, grow);
      cands.emplace_back(pad.xMin(), pad.yMin() - gd, pad.xMax(), pad.yMax() + (grow - gd));
    }
    // 2-axis: use full free width, fill the remaining area in height.
    {
      const frCoord new_w = w + eL + eR;
      const frCoord nh = neededLen(min_area, new_w, h, mgrid);
      if (h + eU + eD >= nh) {
        const frCoord grow = nh - h;
        const frCoord gu = std::min(eU, grow);
        cands.emplace_back(
            pad.xMin() - eL, pad.yMin() - (grow - gu), pad.xMax() + eR, pad.yMax() + gu);
      }
    }
    // 2-axis: use full free height, fill the remaining area in width.
    {
      const frCoord new_h = h + eU + eD;
      const frCoord nw = neededLen(min_area, new_h, w, mgrid);
      if (w + eL + eR >= nw) {
        const frCoord grow = nw - w;
        const frCoord gr = std::min(eR, grow);
        cands.emplace_back(
            pad.xMin() - (grow - gr), pad.yMin() - eD, pad.xMax() + gr, pad.yMax() + eU);
      }
    }
  };

  std::vector<odb::Rect> cands;
  gen_cands(cands);

  for (const auto& c : cands) {
    if (!die.contains(c)) {
      continue;  // never grow outside the die/core area
    }
    if (static_cast<frArea>(c.dx()) * static_cast<frArea>(c.dy()) < min_area) {
      continue;  // must actually satisfy min-area
    }
    if (patchSpacingSafe(c, pad, spc, obstacles)) {
      out = c;
      return true;
    }
  }
  return false;
}
}  // namespace

// Widen a same-net junction that the GC engine reports as NON-SUFFICIENT METAL.
//
// An NS Metal marker says two same-net shapes overlap by less than the layer's
// MINWIDTH: the metal IS connected, but only through a neck narrower than the
// process can reliably print. Measured on gf180mcuD, a Via1 Metal1 pad clipping
// a cell pin at the corner leaves an 80 x 110 dbu junction against a MINWIDTH of
// 460 -- a real neck, correctly reported.
//
// The routing loop does not see these. `FlexGCWorker::Impl::initDesign` returns
// early for a DR worker before it loads the design's DR objects, so an in-loop
// worker is paired only against the nets its own worker is modifying; a net
// finished in an earlier iteration is never re-checked against the fixed cell
// metal beside it. The loop therefore converges to 0 with these standing, and
// the whole-design pass in verifyRoute() is the first thing to see them -- which
// is exactly what DRT-0701 reports.
//
// Repair follows patchMinAreaViolations' established shape: run AFTER routing
// has converged so the ripup loop is never re-entered, and add metal only --
// a patch on the owning net, never a rip or a reroute. Growing the junction to
// MINWIDTH in both axes is the physically correct repair: it makes the
// connection printable without moving anything the router decided.
//
// Returns the number of junctions patched.
int TritonRoute::patchNonSufficientMetalViolations()
{
  frDesign* design = getDesign();
  if (design == nullptr) {
    return 0;
  }
  frTechObject* tech = design->getTech();
  frBlock* block = design->getTopBlock();
  frRegionQuery* rq = design->getRegionQuery();
  if (tech == nullptr || block == nullptr || rq == nullptr) {
    return 0;
  }
  if (block->getGCellPatterns().size() < 2) {
    return 0;  // no gcell grid: getDRCMarkers cannot tile
  }

  // See what a whole-design pass sees -- the same view verifyRoute uses, and the
  // only one in which these markers exist at all.
  rq->initDRObj();
  frList<std::unique_ptr<frMarker>> markers;
  getDRCMarkers(markers, block->getBBox());

  const odb::Rect die = block->getBBox();
  const frCoord mgrid = std::max<frCoord>(1, tech->getManufacturingGrid());

  std::vector<std::pair<odb::Rect, frNet*>> added_patches;
  int patched = 0;
  int unresolved = 0;

  for (const auto& marker : markers) {
    auto* con = marker->getConstraint();
    if (con == nullptr
        || con->typeId()
               != frConstraintTypeEnum::frcNonSufficientMetalConstraint) {
      continue;
    }
    const frLayerNum lNum = marker->getLayerNum();
    frLayer* layer = tech->getLayer(lNum);
    if (layer == nullptr || layer->getType() != dbTechLayerType::ROUTING) {
      continue;
    }
    const frCoord min_width = std::max<frCoord>(1, layer->getMinWidth());
    frCoord spc = layer->getMinSpacingValue(min_width, min_width, 0, false);
    if (spc <= 0) {
      spc = min_width;
    }

    // NSMetal is same-net by construction, so every src names the one owner.
    frNet* net = nullptr;
    for (auto src : marker->getSrcs()) {
      if (src != nullptr && src->typeId() == frcNet) {
        net = static_cast<frNet*>(src);
        break;
      }
    }
    if (net == nullptr || net->isFixed() || net->isSpecial()) {
      continue;  // nothing this pass may add metal to
    }

    const odb::Rect neck = marker->getBBox();
    if (neck.dx() >= min_width && neck.dy() >= min_width) {
      continue;  // not a neck we can explain; leave it reported
    }

    // Hard obstacles only: different-net metal and blockages. Same-net metal is
    // what we are trying to merge into, so it never limits the patch.
    const frCoord reach = min_width + 2 * spc + 2 * mgrid;
    const odb::Rect win(neck.xMin() - reach, neck.yMin() - reach,
                        neck.xMax() + reach, neck.yMax() + reach);
    std::vector<odb::Rect> hard;
    frRegionQuery::Objects<frBlockObject> dr_objs, fixed_objs;
    rq->queryDRObj(win, lNum, dr_objs);
    rq->query(win, lNum, fixed_objs);
    auto consider = [&](const odb::Rect& box, frBlockObject* obj) {
      if (box.xMin() >= box.xMax() || box.yMin() >= box.yMax()) {
        return;
      }
      if (obstacleNet(obj) == net) {
        return;  // same net: soft, mergeable
      }
      hard.push_back(box);
    };
    for (const auto& [box, obj] : dr_objs) {
      consider(box, obj);
    }
    for (const auto& [box, obj] : fixed_objs) {
      consider(box, obj);
    }
    for (const auto& [r, r_net] : added_patches) {
      if (r_net != net && r.intersects(win)) {
        hard.push_back(r);
      }
    }

    // Grow the neck outward until both axes reach MINWIDTH, stopping spc short
    // of any hard obstacle and inside the die. Symmetric growth keeps the patch
    // centred on the junction rather than biased into one neighbour.
    auto room = [&](int axis, int dir) -> frCoord {
      frCoord limit = (axis == 0)
                          ? (dir < 0 ? neck.xMin() - die.xMin()
                                     : die.xMax() - neck.xMax())
                          : (dir < 0 ? neck.yMin() - die.yMin()
                                     : die.yMax() - neck.yMax());
      for (const auto& o : hard) {
        if (axis == 0) {
          if (o.yMax() <= neck.yMin() - spc || o.yMin() >= neck.yMax() + spc) {
            continue;
          }
          if (dir < 0 && o.xMax() <= neck.xMin()) {
            limit = std::min(limit, neck.xMin() - o.xMax() - spc);
          } else if (dir > 0 && o.xMin() >= neck.xMax()) {
            limit = std::min(limit, o.xMin() - neck.xMax() - spc);
          }
        } else {
          if (o.xMax() <= neck.xMin() - spc || o.xMin() >= neck.xMax() + spc) {
            continue;
          }
          if (dir < 0 && o.yMax() <= neck.yMin()) {
            limit = std::min(limit, neck.yMin() - o.yMax() - spc);
          } else if (dir > 0 && o.yMin() >= neck.yMax()) {
            limit = std::min(limit, o.yMin() - neck.yMax() - spc);
          }
        }
      }
      limit = std::max<frCoord>(0, limit);
      return (limit / mgrid) * mgrid;
    };

    // The patch must extend a FULL MINWIDTH beyond the junction on every
    // side, not merely make the junction rectangle minWidth wide. The same-net
    // skip logic accepts a bridging shape only when its intersection with EACH
    // neighbour satisfies x^2 + y^2 >= minWidth^2 -- and the first version of
    // this pass proved why by failing it: a minWidth x minWidth patch centred
    // on the neck overlapped each neighbour by only ~0.135 x 0.145 um and
    // MANUFACTURED two new NS Metal markers where it landed (measured:
    // violations went 2 -> 3). Extending minWidth past the junction guarantees
    // the intersection with any >= minWidth neighbour is >= minWidth on both
    // axes: a neighbour shorter than neck + minWidth is covered entirely, and
    // a longer one yields exactly minWidth of overlap.
    //
    // A side that cannot reach the full extension (hard metal or the die edge
    // in the way) makes the repair unprovable, so the junction is left
    // reported rather than patched into a new violation.
    auto up_to_grid = [&](frCoord v) -> frCoord {
      if (v <= 0) {
        return 0;
      }
      return ((v + mgrid - 1) / mgrid) * mgrid;
    };
    const frCoord want = up_to_grid(min_width);
    const frCoord dxl = std::min(room(0, -1), want);
    const frCoord dxh = std::min(room(0, 1), want);
    const frCoord dyl = std::min(room(1, -1), want);
    const frCoord dyh = std::min(room(1, 1), want);
    if (dxl < want || dxh < want || dyl < want || dyh < want) {
      ++unresolved;
      continue;
    }
    const odb::Rect grown(neck.xMin() - dxl, neck.yMin() - dyl,
                          neck.xMax() + dxh, neck.yMax() + dyh);
    // Refuse anything degenerate rather than hand it to the database: an
    // inverted or empty patch box aborts the next GC init, which is a worse
    // failure than the marker this pass exists to remove.
    if (grown.xMin() >= grown.xMax() || grown.yMin() >= grown.yMax()
        || grown.dx() < min_width || grown.dy() < min_width
        || !die.contains(grown)) {
      ++unresolved;
      continue;
    }
    auto pwire = std::make_unique<frPatchWire>();
    // setLayerNum and setOrigin are not optional. initNetsFromDesign computes
    // `z = pwire->getLayerNum() / 2 - 1` and indexes getNonTaperedRects(z); a
    // patch left on the default layer 0 gives z = -1 and aborts the next GC
    // init. Same three calls, same order, as patchMinAreaViolations.
    pwire->setLayerNum(lNum);
    pwire->setOrigin(odb::Point(0, 0));
    pwire->setOffsetBox(grown);
    net->addPatchWire(std::move(pwire));
    added_patches.emplace_back(grown, net);
    ++patched;
  }

  if (patched > 0 || unresolved > 0) {
    logger_->info(DRT,
                  703,
                  "Post-route non-sufficient-metal repair: widened {} "
                  "same-net junction(s) to MINWIDTH, {} left unresolved.",
                  patched,
                  unresolved);
  }
  return patched;
}

int TritonRoute::patchMinAreaViolations()
{
  frDesign* design = getDesign();
  if (design == nullptr) {
    return 0;
  }
  frTechObject* tech = design->getTech();
  frBlock* block = design->getTopBlock();
  frRegionQuery* rq = design->getRegionQuery();
  if (tech == nullptr || block == nullptr || rq == nullptr) {
    return 0;
  }

  // Rebuild the DR-object region query so the spacing check sees every shape as
  // finally committed by detailed routing.
  rq->initDRObj();

  const odb::Rect die = block->getBBox();
  const frCoord mgrid = std::max<frCoord>(1, tech->getManufacturingGrid());

  int total_patched = 0;
  int total_unresolved = 0;

  for (const auto& ulayer : tech->getLayers()) {
    frLayer* layer = ulayer.get();
    if (layer->getType() != dbTechLayerType::ROUTING) {
      continue;
    }
    frAreaConstraint* area_con = layer->getAreaConstraint();
    if (area_con == nullptr) {
      continue;
    }
    const frArea min_area = area_con->getMinArea();
    if (min_area <= 0) {
      continue;
    }
    const frLayerNum lNum = layer->getLayerNum();
    const frCoord min_width = std::max<frCoord>(1, layer->getMinWidth());
    frCoord spc = layer->getMinSpacingValue(min_width, min_width, 0, false);
    if (spc <= 0) {
      spc = min_width;  // conservative fallback
    }

    // Collect every undersized routing polygon on this layer.
    std::vector<std::pair<frNet*, odb::Rect>> pads;
    for (const auto& unet : block->getNets()) {
      frNet* net = unet.get();
      if (net == nullptr || net->isFixed() || net->isSpecial()) {
        continue;
      }

      // Merge all of this net's metal on lNum into connected polygons.
      gtl::polygon_90_set_data<frCoord> net_set;
      bool any = false;
      auto insert_rect = [&](const odb::Rect& r) {
        if (r.xMin() >= r.xMax() || r.yMin() >= r.yMax()) {
          return;
        }
        net_set.insert(
            gtl::rectangle_data<frCoord>(r.xMin(), r.yMin(), r.xMax(), r.yMax()));
        any = true;
      };
      for (const auto& shape : net->getShapes()) {
        if (shape->getLayerNum() == lNum) {
          insert_rect(shape->getBBox());
        }
      }
      for (const auto& via : net->getVias()) {
        const frViaDef* vd = via->getViaDef();
        if (vd == nullptr) {
          continue;
        }
        if (vd->getLayer1Num() == lNum) {
          insert_rect(via->getLayer1BBox());
        } else if (vd->getLayer2Num() == lNum) {
          insert_rect(via->getLayer2BBox());
        }
      }
      for (const auto& pwire : net->getPatchWires()) {
        if (pwire->getLayerNum() == lNum) {
          insert_rect(pwire->getBBox());
        }
      }
      if (!any) {
        continue;
      }

      std::vector<gtl::polygon_90_data<frCoord>> polys;
      net_set.get(polys);

      // Fast path: skip the net entirely unless it has at least one routing
      // polygon on this layer below min-area (the common case is none, and we
      // then avoid the placed-cell fixed-metal region query below).
      bool any_undersized = false;
      for (const auto& poly : polys) {
        if (static_cast<frArea>(gtl::area(poly)) < min_area) {
          any_undersized = true;
          break;
        }
      }
      if (!any_undersized) {
        continue;
      }

      // Placed-cell-geometry-aware detection.  Min-area is a property of the
      // FULL PHYSICAL connected metal on the layer, not of the net's routing in
      // isolation.  On a std-cell pin layer (sky130 li1, and a commercial-PDK met1 alike)
      // a routed stub/via-pad abuts the cell's own pin: the pin is fixed metal
      // the router cannot grow but which contributes real area, so sign-off DRC
      // (and the GC engine's own checkMetalShape_minArea) measure the connected
      // routing+pin polygon.  If that polygon already meets min-area the shape
      // is DRC-clean and must NOT be patched -- measuring the routing alone
      // flags a phantom and grows a patch into neighbouring cells, injecting new
      // spacing violations (the sky130 regression).
      //
      // Reproduce the DRC's connected-metal semantics: merge the net's routing
      // with every placed-cell PIN and OBSTRUCTION shape on this layer that
      // physically abuts it (from the SAME frRegionQuery the router uses --
      // rq->query returns the fixed instTerm/instBlockage/blockage/bTerm metal
      // that init() indexed from the placed cells), then flag a routing polygon
      // ONLY when its full connected component is STILL below min-area.  A
      // routing via pad whose full component (routing + the small cell metal it
      // touches) is below min-area is a genuine violation and is grown by its
      // own routing extent; a pad that abuts a pin large enough to already
      // satisfy min-area is left untouched.  This is a true no-op wherever the
      // router already left DRC-clean connected metal, on ANY PDK.
      gtl::rectangle_data<frCoord> net_bbox;
      gtl::extents(net_bbox, net_set);
      const odb::Rect net_win(gtl::xl(net_bbox) - mgrid,
                              gtl::yl(net_bbox) - mgrid,
                              gtl::xh(net_bbox) + mgrid,
                              gtl::yh(net_bbox) + mgrid);
      frRegionQuery::Objects<frBlockObject> fixed_objs;
      rq->query(net_win, lNum, fixed_objs);
      gtl::polygon_90_set_data<frCoord> full_set(net_set);
      for (const auto& [box, obj] : fixed_objs) {
        if (obj == nullptr) {
          continue;
        }
        switch (obj->typeId()) {
          case frcInstTerm:      // placed std-cell pin metal
          case frcInstBlockage:  // placed std-cell obstruction metal
          case frcBlockage:      // fixed routing obstruction
          case frcBTerm:         // IO pin metal
            break;
          default:
            continue;  // signal/special-net routing: not fixed cell geometry
        }
        if (box.xMin() >= box.xMax() || box.yMin() >= box.yMax()) {
          continue;
        }
        full_set.insert(gtl::rectangle_data<frCoord>(
            box.xMin(), box.yMin(), box.xMax(), box.yMax()));
      }

      std::vector<gtl::polygon_90_data<frCoord>> full_polys;
      full_set.get(full_polys);

      std::ofstream diag_ofs;
      if (const char* diag = std::getenv("VIBE_MINAREA_DIAG")) {
        diag_ofs.open(diag, std::ios::app);
      }

      // Area of the connected component of the full (routing + cell) metal that
      // contains a given routing polygon -- what the physical min-area DRC
      // measures for that shape.
      auto component_area = [&](const gtl::polygon_90_data<frCoord>& rpoly,
                                frArea fallback) -> frArea {
        for (const auto& fp : full_polys) {
          gtl::polygon_90_set_data<frCoord> inter;
          {
            using boost::polygon::operators::operator+=;
            using boost::polygon::operators::operator&=;
            inter += fp;
            inter &= rpoly;
          }
          if (gtl::area(inter) == 0) {
            continue;  // not the component holding this routing polygon
          }
          return static_cast<frArea>(gtl::area(fp));
        }
        return fallback;
      };

      // Flag a routing polygon ONLY when its full physical connected component
      // (routing + abutting placed-cell pin/obstruction metal) is still below
      // min-area; grow it by its own routing extent.  A pad that abuts fixed
      // cell metal large enough to already satisfy min-area is DRC-clean and
      // left untouched -- a true no-op on any PDK whose router left connected
      // metal that meets the rule.
      for (const auto& rpoly : polys) {
        const frArea route_area = static_cast<frArea>(gtl::area(rpoly));
        if (route_area >= min_area) {
          continue;  // routing alone already satisfies min-area
        }
        const frArea comp_area = component_area(rpoly, route_area);
        if (diag_ofs.is_open()) {
          diag_ofs << "DIAG lNum=" << lNum << " min_area=" << min_area
                   << " route_area=" << route_area << " comp_area=" << comp_area
                   << " comp_ge_min=" << (comp_area >= min_area ? 1 : 0) << "\n";
        }
        if (comp_area >= min_area) {
          continue;  // full connected metal is DRC-clean: leave it (no-op)
        }
        gtl::rectangle_data<frCoord> ext;
        gtl::extents(ext, rpoly);
        pads.emplace_back(
            net, odb::Rect(gtl::xl(ext), gtl::yl(ext), gtl::xh(ext), gtl::yh(ext)));
      }
    }

    // Tightness = flat spacing-safe bounding area available around the pad
    // (hard, different-net metal only; no repair patches yet). Repair the most
    // boxed-in pads FIRST so they claim their scarce room before easier pads
    // consume it — this keeps the sequential-greedy repair convergent.
    auto tightness = [&](frNet* pnet, const odb::Rect& pad) -> int64_t {
      const frCoord w = pad.dx();
      const frCoord h = pad.dy();
      const frCoord reach
          = neededLen(min_area, std::min(w, h), std::max(w, h), mgrid) + 2 * spc + 2 * mgrid;
      const odb::Rect win(pad.xMin() - reach, pad.yMin() - reach,
                          pad.xMax() + reach, pad.yMax() + reach);
      frRegionQuery::Objects<frBlockObject> dr_objs, fixed_objs;
      rq->queryDRObj(win, lNum, dr_objs);
      rq->query(win, lNum, fixed_objs);
      frCoord eR = die.xMax() - pad.xMax(), eL = pad.xMin() - die.xMin();
      frCoord eU = die.yMax() - pad.yMax(), eD = pad.yMin() - die.yMin();
      auto lim = [&](const odb::Rect& o, frBlockObject* ob) {
        if (o.overlaps(pad)) {
          return;
        }
        if (pnet != nullptr && obstacleNet(ob) == pnet) {
          return;  // same net: mergeable, does not constrain
        }
        if ((o.yMax() > pad.yMin() - spc) && (o.yMin() < pad.yMax() + spc)) {
          if (o.xMin() >= pad.xMax())
            eR = std::min(eR, o.xMin() - spc - pad.xMax());
          if (o.xMax() <= pad.xMin())
            eL = std::min(eL, pad.xMin() - spc - o.xMax());
        }
        if ((o.xMax() > pad.xMin() - spc) && (o.xMin() < pad.xMax() + spc)) {
          if (o.yMin() >= pad.yMax())
            eU = std::min(eU, o.yMin() - spc - pad.yMax());
          if (o.yMax() <= pad.yMin())
            eD = std::min(eD, pad.yMin() - spc - o.yMax());
        }
      };
      for (const auto& [b, o] : dr_objs) {
        lim(b, o);
      }
      for (const auto& [b, o] : fixed_objs) {
        lim(b, o);
      }
      eR = std::max<frCoord>(0, eR);
      eL = std::max<frCoord>(0, eL);
      eU = std::max<frCoord>(0, eU);
      eD = std::max<frCoord>(0, eD);
      return static_cast<int64_t>(w + eL + eR) * static_cast<int64_t>(h + eU + eD);
    };

    std::vector<int64_t> keys(pads.size());
    for (size_t i = 0; i < pads.size(); ++i) {
      keys[i] = tightness(pads[i].first, pads[i].second);
    }
    std::vector<size_t> order(pads.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return keys[a] < keys[b]; });

    // Sequential-greedy: each patch we add becomes an obstacle for later pads
    // so neighbouring repairs never collide.
    std::vector<std::pair<odb::Rect, frNet*>> added_patches;
    for (size_t idx : order) {
      frNet* net = pads[idx].first;
      const odb::Rect& pad = pads[idx].second;
      odb::Rect grown;
      if (findMinAreaPatch(pad, net, min_area, spc, mgrid, die, lNum, rq,
                           added_patches, grown)) {
        auto pwire = std::make_unique<frPatchWire>();
        pwire->setLayerNum(lNum);
        pwire->setOrigin(odb::Point(0, 0));
        pwire->setOffsetBox(grown);
        net->addPatchWire(std::move(pwire));
        added_patches.emplace_back(grown, net);
        ++total_patched;
      } else {
        ++total_unresolved;
        if (const char* dbg = std::getenv("VIBE_MINAREA_DEBUG")) {
          std::ofstream ofs(dbg, std::ios::app);
          if (ofs.is_open()) {
            ofs << "UNRESOLVED net=" << net->getName() << " pad=(" << pad.xMin()
                << "," << pad.yMin() << "," << pad.xMax() << "," << pad.yMax()
                << ") wxh=" << pad.dx() << "x" << pad.dy()
                << " tightness=" << keys[idx] << "\n";
          }
        }
      }
    }
  }

  if (total_patched > 0 || total_unresolved > 0) {
    logger_->info(DRT,
                  700,
                  "Post-route min-area repair: patched {} isolated undersized "
                  "polygon(s); {} unresolved.",
                  total_patched,
                  total_unresolved);
  }
  return total_patched;
}

int TritonRoute::verifyRoute()
{
  frDesign* design = getDesign();
  if (design == nullptr) {
    return -1;
  }
  frBlock* block = design->getTopBlock();
  frRegionQuery* rq = design->getRegionQuery();
  if (block == nullptr || rq == nullptr) {
    return -1;
  }
  // getDRCMarkers() tiles the die by gcell; without a gcell grid there is
  // nothing to tile and verification cannot run.
  if (block->getGCellPatterns().size() < 2) {
    return -1;
  }

  // What the ripup loop last said about itself.
  const int in_loop = block->getNumMarkers();

  // The post-route repair passes commit geometry after the last DR worker ran.
  // Re-index the DR-object region query so the GC engine checks what is
  // actually about to be written out.
  rq->initDRObj();

  frList<std::unique_ptr<frMarker>> markers;
  getDRCMarkers(markers, block->getBBox());
  const int verified = static_cast<int>(markers.size());

  // Replace the block's marker set with the verified one so
  // detailed_route_num_drvs, the GUI marker browser and the DRC report all
  // quote the same number, and that number describes the finished route.
  {
    std::vector<frMarker*> stale;
    stale.reserve(block->getNumMarkers());
    for (const auto& marker : block->getMarkers()) {
      stale.push_back(marker.get());
    }
    for (frMarker* marker : stale) {
      rq->removeMarker(marker);
      block->removeMarker(marker);
    }
    for (auto& marker : markers) {
      frMarker* ptr = marker.get();
      rq->addMarker(ptr);
      block->addMarker(std::move(marker));
    }
  }

  if (verified > in_loop) {
    logger_->warn(DRT,
                  701,
                  "Post-route verification found {} violation(s) that the "
                  "routing loop did not report ({} in-loop). The published "
                  "result is the verified one.",
                  verified,
                  in_loop);
  } else if (router_cfg_->VERBOSE > 0) {
    logger_->info(
        DRT, 702, "Post-route verification: {} violation(s).", verified);
  }

  // Re-emit under the same category the routing loop used, so the verified set
  // REPLACES the in-loop residual rather than sitting beside it.
  reportDRC(router_cfg_->DRC_RPT_FILE, block->getMarkers(), "DRC");
  return verified;
}

int TritonRoute::main()
{
  utl::Timer timer;
  // Just to verify that OMP support is compiled in correctly.
  omp_set_num_threads(2);
#pragma omp parallel
  {
    if (omp_get_num_threads() != 2) {
      logger_->error(DRT, 623, "OMP threading is not working.");
    }
  }

  if (router_cfg_->DBPROCESSNODE == "GF14_13M_3Mx_2Cx_4Kx_2Hx_2Gx_LB") {
    router_cfg_->USENONPREFTRACKS = false;
  }
  std::unique_ptr<std::thread> pa_thread;

  if (debug_->debugDumpDR) {
    std::string router_cfg_path
        = fmt::format("{}/init_router_cfg.bin", debug_->dumpDir);
    writeGlobals(router_cfg_path);
  }
  if (distributed_) {
    if (router_cfg_->DO_PA) {
      pa_thread = std::make_unique<std::thread>([this]() {
        sendDesignDist();
        dst::JobMessage msg(dst::JobMessage::kPinAccess,
                            dst::JobMessage::kBroadcast),
            result;
        auto uDesc = std::make_unique<PinAccessJobDescription>();
        uDesc->setType(PinAccessJobDescription::INIT_PA);
        msg.setJobDescription(std::move(uDesc));
        dist_->sendJob(msg, dist_ip_.c_str(), dist_port_, result);
      });
    } else {
      asio::post(*dist_pool_, boost::bind(&TritonRoute::sendDesignDist, this));
    }
  }
  initDesign();
  bool has_routable_nets = false;
  bool has_grt_guides = false;
  for (auto* net : db_->getChip()->getBlock()->getNets()) {
    auto iterms = net->getITerms();
    auto bterms = net->getBTerms();
    if (!has_routable_nets
        && (iterms.hasMoreThan(1) || bterms.hasMoreThan(1)
            || (!iterms.empty() && !bterms.empty()))) {
      has_routable_nets = true;
    }
    if (!has_grt_guides && !net->getGuides().empty()) {
      has_grt_guides = true;
    }
    if (has_routable_nets && has_grt_guides) {
      break;
    }
  }
  if (!has_routable_nets) {
    logger_->warn(DRT,
                  40,
                  "Design does not have any routable net "
                  "(with at least 2 terms)");
    return 0;
  }
  if (!has_grt_guides) {
    logger_->error(DRT,
                   47,
                   "Design has no global routing guides. Global routing must "
                   "be run before detailed routing.");
  }
  if (router_cfg_->DO_PA) {
    pa_ = std::make_unique<FlexPA>(
        getDesign(), logger_, dist_, router_cfg_.get());
    pa_->setDistributed(dist_ip_, dist_port_, shared_volume_, cloud_sz_);
    if (debug_->debugPA) {
      pa_->setDebug(graphics_factory_->makeUniquePAGraphics());
    }
    if (pa_thread) {
      pa_thread->join();
    }
    pa_->main();
    /// bookmark
    if (distributed_ || debug_->debugDR || debug_->debugDumpDR) {
      io::Writer writer(getDesign(), logger_);
      writer.updateDb(db_, router_cfg_.get(), true);
    }
    if (distributed_) {
      asio::post(*dist_pool_, [this]() {
        dst::JobMessage msg(dst::JobMessage::kGrdrInit,
                            dst::JobMessage::kBroadcast),
            result;
        dist_->sendJob(msg, dist_ip_.c_str(), dist_port_, result);
      });
    }
  }
  if (debug_->debugDumpDR) {
    db_->write(utl::OutStreamHandler(
                   fmt::format("{}/design.odb", debug_->dumpDir).c_str(), true)
                   .getStream());
  }
  if (!initGuide()) {
    logger_->error(DRT, 626, "Guide loading failed.");
  }
  prep();
  ta();
  if (distributed_) {
    asio::post(*dist_pool_,
               [this] { sendDesignUpdates("", router_cfg_->MAX_THREADS); });
  }
  dr();
  // vibeic fork: additive post-route min-area repair. detailed_route can leave
  // isolated routing polygons (e.g. via landing pads) below the layer min-area
  // that its maze-integrated patcher never sees. Grow them into spacing-safe
  // rectangles here, AFTER routing has fully converged, so we never re-enter
  // the ripup loop. Purely additive metal on the owning signal net.
  if (!router_cfg_->SINGLE_STEP_DR) {
    patchMinAreaViolations();
    // Same shape, same moment: additive, post-convergence, never re-entering
    // the ripup loop. verifyRoute() below re-runs the whole-design check and so
    // is the measurement of whether this repair worked.
    patchNonSufficientMetalViolations();
    // vibeic fork: verify the FINISHED route before writing it out. Until this
    // ran, the number detailed_route published was the ripup loop's own
    // residual -- measured to disagree with a whole-design pass of the same GC
    // engine on the same DEF -- and it was written before the min-area repair
    // above had even added its patches.
    verifyRoute();
    endFR();
  }
  logger_->info(DRT, 501, "Runtime: {:.2f}s", timer.elapsed());
  return 0;
}

void TritonRoute::pinAccess(const std::vector<odb::dbInst*>& target_insts)
{
  if (router_cfg_->DBPROCESSNODE == "GF14_13M_3Mx_2Cx_4Kx_2Hx_2Gx_LB") {
    router_cfg_->USENONPREFTRACKS = false;
  }
  if (distributed_) {
    asio::post(*dist_pool_, [this]() {
      sendDesignDist();
      dst::JobMessage msg(dst::JobMessage::kPinAccess,
                          dst::JobMessage::kBroadcast),
          result;
      auto uDesc = std::make_unique<PinAccessJobDescription>();
      uDesc->setType(PinAccessJobDescription::INIT_PA);
      msg.setJobDescription(std::move(uDesc));
      dist_->sendJob(msg, dist_ip_.c_str(), dist_port_, result);
    });
  }
  clearDesign();
  router_cfg_->ENABLE_VIA_GEN = true;
  initDesign();
  pa_ = std::make_unique<FlexPA>(
      getDesign(), logger_, dist_, router_cfg_.get());
  pa_->setTargetInstances(target_insts);
  if (debug_->debugPA) {
    pa_->setDebug(graphics_factory_->makeUniquePAGraphics());
  }
  if (distributed_) {
    pa_->setDistributed(dist_ip_, dist_port_, shared_volume_, cloud_sz_);
    dist_pool_->join();
  }
  pa_->main();
  io::Writer writer(getDesign(), logger_);
  writer.updateDb(db_, router_cfg_.get(), true);
}

void TritonRoute::deleteInstancePAData(frInst* inst, bool delete_inst)
{
  if (pa_) {
    pa_->removeFromInstsSet(inst);
    if (delete_inst) {
      pa_->deleteInst(inst);
    }
  }
}

void TritonRoute::addInstancePAData(frInst* inst)
{
  if (pa_) {
    pa_->addDirtyInst(inst);
  }
}

void TritonRoute::addAvoidViaDefPA(const frViaDef* via_def)
{
  if (pa_) {
    pa_->addAvoidViaDef(via_def);
  }
}
void TritonRoute::updateDirtyPAData()
{
  if (pa_) {
    design_->getTopBlock()->removeDeletedObjects();
    pa_->updateDirtyInsts();
    io::Writer writer(getDesign(), logger_);
    writer.updateDb(getDb(), getRouterConfiguration(), true);
  }
}

void TritonRoute::fixMaxSpacing(int num_threads)
{
  initDesign();
  initGuide();
  prep();
  router_cfg_->MAX_THREADS = num_threads;
  dr_ = std::make_unique<FlexDR>(
      this, getDesign(), logger_, db_, router_cfg_.get());
  dr_->init();
  dr_->fixMaxSpacing();
  io::Writer writer(getDesign(), logger_);
  writer.updateDb(db_, router_cfg_.get());
}

void TritonRoute::getDRCMarkers(frList<std::unique_ptr<frMarker>>& markers,
                                const odb::Rect& requiredDrcBox)
{
  std::vector<std::vector<std::unique_ptr<FlexGCWorker>>> workersBatches(1);
  auto size = 7;
  auto offset = 0;
  auto gCellPatterns = design_->getTopBlock()->getGCellPatterns();
  auto& xgp = gCellPatterns.at(0);
  auto& ygp = gCellPatterns.at(1);
  for (int i = offset; i < (int) xgp.getCount(); i += size) {
    for (int j = offset; j < (int) ygp.getCount(); j += size) {
      odb::Rect routeBox1
          = design_->getTopBlock()->getGCellBox(odb::Point(i, j));
      const int max_i = std::min((int) xgp.getCount() - 1, i + size - 1);
      const int max_j = std::min((int) ygp.getCount(), j + size - 1);
      odb::Rect routeBox2
          = design_->getTopBlock()->getGCellBox(odb::Point(max_i, max_j));
      odb::Rect routeBox(routeBox1.xMin(),
                         routeBox1.yMin(),
                         routeBox2.xMax(),
                         routeBox2.yMax());
      odb::Rect extBox;
      odb::Rect drcBox;
      routeBox.bloat(router_cfg_->DRCSAFEDIST, drcBox);
      routeBox.bloat(router_cfg_->MTSAFEDIST, extBox);
      if (!drcBox.intersects(requiredDrcBox)) {
        continue;
      }
      auto gcWorker = std::make_unique<FlexGCWorker>(
          design_->getTech(), logger_, router_cfg_.get());
      gcWorker->setDrcBox(drcBox);
      gcWorker->setExtBox(extBox);
      if (workersBatches.back().size() >= router_cfg_->BATCHSIZE) {
        workersBatches.emplace_back();
      }
      workersBatches.back().push_back(std::move(gcWorker));
    }
  }
  std::map<MarkerId, frMarker*> mapMarkers;
  omp_set_num_threads(router_cfg_->MAX_THREADS);
  for (auto& workers : workersBatches) {
#pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < workers.size(); i++) {  // NOLINT
      workers[i]->init(design_.get());
      workers[i]->main();
    }
    for (const auto& worker : workers) {
      for (auto& marker : worker->getMarkers()) {
        odb::Rect bbox = marker->getBBox();
        if (!bbox.intersects(requiredDrcBox)) {
          continue;
        }
        auto layerNum = marker->getLayerNum();
        auto con = marker->getConstraint();
        if (mapMarkers.find({bbox, layerNum, con, marker->getSrcs()})
            != mapMarkers.end()) {
          continue;
        }
        markers.push_back(std::make_unique<frMarker>(*marker));
        mapMarkers[{bbox, layerNum, con, marker->getSrcs()}]
            = markers.back().get();
      }
    }
    workers.clear();
  }
}

void TritonRoute::checkDRC(const char* filename,
                           int x1,
                           int y1,
                           int x2,
                           int y2,
                           const std::string& marker_name,
                           int num_threads)
{
  router_cfg_->GC_IGNORE_PDN_LAYER_NUM = -1;
  router_cfg_->REPAIR_PDN_LAYER_NUM = -1;
  router_cfg_->MAX_THREADS = num_threads;
  initDesign();
  auto gcellGrid = db_->getChip()->getBlock()->getGCellGrid();
  if (gcellGrid != nullptr && gcellGrid->getNumGridPatternsX() == 1
      && gcellGrid->getNumGridPatternsY() == 1) {
    io::GuideProcessor guide_processor(
        getDesign(), db_, logger_, router_cfg_.get());
    guide_processor.readGuides();
    guide_processor.buildGCellPatterns();
  } else if (!initGuide()) {
    logger_->error(DRT, 1, "GCELLGRID is undefined");
  }
  odb::Rect requiredDrcBox(x1, y1, x2, y2);
  if (requiredDrcBox.area() == 0) {
    requiredDrcBox = design_->getTopBlock()->getBBox();
  }
  frList<std::unique_ptr<frMarker>> markers;
  getDRCMarkers(markers, requiredDrcBox);
  reportDRC(filename, markers, marker_name, requiredDrcBox);
}

// vibeic fork: TEST/DIAGNOSTIC ENTRY POINT for the post-route NS-Metal repair.
//
// patchNonSufficientMetalViolations() runs only from inside TritonRoute::main(),
// after the ripup loop converges. That makes it untestable except through a
// full detailed_route, and it landed (vibeic/OpenROAD#13) with no regression
// test at all. This entry point runs the SAME production function against an
// already-routed design, using checkDRC's setup verbatim, and reports:
//
//   before   NS-Metal markers a whole-design pass finds on the input
//   patched  what the repair says it fixed
//   after    NS-Metal markers a whole-design pass finds on the RESULT
//
// Reporting all three in one line is deliberate. A repair pass that always
// claims success is the defect one level up, and `patched` alone cannot tell
// "cleared it" from "moved it": only `after`, measured by the checker rather
// than by the repair, can. `after > before` means the pass manufactured
// violations -- which is exactly the v2 failure #13's own commit message
// records (2 -> 3), and which no test existed to catch.
//
// The surviving markers are written to `filename` in the same format checkDRC
// uses, so a junction the pass correctly REFUSES to repair stays visible and
// diffable rather than disappearing into a count.
void TritonRoute::repairNonSufficientMetal(const char* filename,
                                           int num_threads)
{
  router_cfg_->GC_IGNORE_PDN_LAYER_NUM = -1;
  router_cfg_->REPAIR_PDN_LAYER_NUM = -1;
  router_cfg_->MAX_THREADS = num_threads;
  initDesign();
  auto gcellGrid = db_->getChip()->getBlock()->getGCellGrid();
  if (gcellGrid != nullptr && gcellGrid->getNumGridPatternsX() == 1
      && gcellGrid->getNumGridPatternsY() == 1) {
    io::GuideProcessor guide_processor(
        getDesign(), db_, logger_, router_cfg_.get());
    guide_processor.readGuides();
    guide_processor.buildGCellPatterns();
  } else if (!initGuide()) {
    logger_->error(DRT, 705, "GCELLGRID is undefined");
  }

  const odb::Rect box = design_->getTopBlock()->getBBox();

  auto count_ns_metal = [](const frList<std::unique_ptr<frMarker>>& ms) {
    int n = 0;
    for (const auto& m : ms) {
      auto* con = m->getConstraint();
      if (con != nullptr
          && con->typeId()
                 == frConstraintTypeEnum::frcNonSufficientMetalConstraint) {
        ++n;
      }
    }
    return n;
  };

  design_->getRegionQuery()->initDRObj();
  frList<std::unique_ptr<frMarker>> before;
  getDRCMarkers(before, box);
  const int n_before = count_ns_metal(before);

  const int patched = patchNonSufficientMetalViolations();

  design_->getRegionQuery()->initDRObj();
  frList<std::unique_ptr<frMarker>> after;
  getDRCMarkers(after, box);
  const int n_after = count_ns_metal(after);

  logger_->info(DRT,
                704,
                "NS-Metal repair check: before={} patched={} after={}.",
                n_before,
                patched,
                n_after);

  reportDRC(filename, after, "DRC", box);
}

void TritonRoute::addUserSelectedVia(const std::string& viaName)
{
  if (db_->getChip() == nullptr || db_->getChip()->getBlock() == nullptr
      || db_->getTech() == nullptr) {
    logger_->error(DRT, 610, "Load design before setting default vias");
  }
  auto block = db_->getChip()->getBlock();
  auto tech = db_->getTech();
  if (tech->findVia(viaName.c_str()) == nullptr
      && block->findVia(viaName.c_str()) == nullptr) {
    logger_->error(utl::DRT, 611, "Via {} not found", viaName);
  } else {
    design_->addUserSelectedVia(viaName);
  }
}

void TritonRoute::setUnidirectionalLayer(const std::string& layerName)
{
  if (db_->getTech() == nullptr) {
    logger_->error(DRT, 615, "Load tech before setting unidirectional layers");
  }
  auto tech = db_->getTech();
  auto dbLayer = tech->findLayer(layerName.c_str());
  if (dbLayer == nullptr) {
    logger_->error(utl::DRT, 616, "Layer {} not found", layerName);
  }
  if (dbLayer->getType() != dbTechLayerType::ROUTING) {
    logger_->error(utl::DRT,
                   618,
                   "Non-routing layer {} can't be set unidirectional",
                   layerName);
  }
  router_cfg_->unidirectional_layer_names_.insert(layerName);
}

void TritonRoute::setParams(const ParamStruct& params)
{
  router_cfg_->OUT_MAZE_FILE = params.outputMazeFile;
  router_cfg_->DRC_RPT_FILE = params.outputDrcFile;
  router_cfg_->DRC_RPT_ITER_STEP = params.drcReportIterStep;
  router_cfg_->GUIDE_REPORT_FILE = params.outputGuideCoverageFile;
  router_cfg_->VERBOSE = params.verbose;
  router_cfg_->ENABLE_VIA_GEN = params.enableViaGen;
  router_cfg_->DBPROCESSNODE = params.dbProcessNode;
  router_cfg_->CLEAN_PATCHES = params.cleanPatches;
  router_cfg_->DO_PA = params.doPa;
  router_cfg_->SINGLE_STEP_DR = params.singleStepDR;
  if (!params.viaInPinBottomLayer.empty()) {
    router_cfg_->VIAINPIN_BOTTOMLAYER_NAME = params.viaInPinBottomLayer;
  }
  if (!params.viaInPinTopLayer.empty()) {
    router_cfg_->VIAINPIN_TOPLAYER_NAME = params.viaInPinTopLayer;
  }
  if (!params.viaAccessLayer.empty()) {
    router_cfg_->VIA_ACCESS_LAYER_NAME = params.viaAccessLayer;
  }
  if (params.drouteEndIter >= 0) {
    router_cfg_->END_ITERATION = params.drouteEndIter;
  }
  router_cfg_->OR_SEED = params.orSeed;
  router_cfg_->OR_K = params.orK;
  if (params.minAccessPoints > 0) {
    router_cfg_->MINNUMACCESSPOINT_STDCELLPIN = params.minAccessPoints;
    router_cfg_->MINNUMACCESSPOINT_MACROCELLPIN = params.minAccessPoints;
  }
  router_cfg_->SAVE_GUIDE_UPDATES = params.saveGuideUpdates;
  router_cfg_->REPAIR_PDN_LAYER_NAME = params.repairPDNLayerName;
  router_cfg_->MAX_THREADS = params.num_threads;
}

void TritonRoute::addWorkerResults(
    const std::vector<std::pair<int, std::string>>& results)
{
  absl::MutexLock lock(&results_mutex_);
  workers_results_.insert(
      workers_results_.end(), results.begin(), results.end());
  results_sz_ = workers_results_.size();
}

bool TritonRoute::getWorkerResults(
    std::vector<std::pair<int, std::string>>& results)
{
  absl::MutexLock lock(&results_mutex_);
  if (workers_results_.empty()) {
    return false;
  }
  results = workers_results_;
  workers_results_.clear();
  results_sz_ = 0;
  return true;
}

int TritonRoute::getWorkerResultsSize()
{
  return results_sz_;
}

void TritonRoute::reportDRC(const std::string& file_name,
                            const frList<std::unique_ptr<frMarker>>& markers,
                            const std::string& marker_name,
                            odb::Rect drcBox) const
{
  odb::dbBlock* block = db_->getChip()->getBlock();
  odb::dbMarkerCategory* tool_category
      = odb::dbMarkerCategory::createOrReplace(block, marker_name.c_str());
  tool_category->setSource("DRT");

  // Obstructions Rtree
  std::vector<std::pair<odb::Rect, odb::dbObstruction*>> obstructions;
  for (odb::dbObstruction* obs : block->getObstructions()) {
    obstructions.emplace_back(obs->getBBox()->getBox(), obs);
  }
  const boost::geometry::index::rtree<std::pair<odb::Rect, odb::dbObstruction*>,
                                      boost::geometry::index::quadratic<16UL>>
      obs_rtree(obstructions.begin(), obstructions.end());

  std::vector<const frMarker*> sorted_markers;
  sorted_markers.reserve(std::distance(markers.begin(), markers.end()));
  for (const auto& marker : markers) {
    sorted_markers.push_back(marker.get());
  }
  auto marker_sort_key = [this](const frMarker* marker) {
    const odb::Rect bbox = marker->getBBox();
    auto tech = getDesign()->getTech();
    auto layer = tech->getLayer(marker->getLayerNum());
    auto con = marker->getConstraint();
    std::string viol_name = "unknown";
    if (con) {
      if (con->typeId() == frConstraintTypeEnum::frcShortConstraint
          && layer->getType() == dbTechLayerType::CUT) {
        viol_name = "Cut Short";
      } else {
        viol_name = con->getViolName();
      }
    }
    std::vector<std::string> sources;
    for (auto src : marker->getSrcs()) {
      if (!src) {
        continue;
      }
      switch (src->typeId()) {
        case frcNet:
          sources.push_back(static_cast<frNet*>(src)->getName());
          break;
        case frcInstTerm: {
          auto* inst_term = static_cast<frInstTerm*>(src);
          sources.push_back(inst_term->getInst()->getName() + "/"
                            + inst_term->getTerm()->getName());
          break;
        }
        case frcBTerm:
          sources.push_back(static_cast<frBTerm*>(src)->getName());
          break;
        default:
          sources.push_back(std::to_string(src->typeId()) + ":"
                            + std::to_string(src->getId()));
          break;
      }
    }
    std::ranges::sort(sources);
    return std::make_tuple(marker->getLayerNum(),
                           bbox.xMin(),
                           bbox.yMin(),
                           bbox.xMax(),
                           bbox.yMax(),
                           viol_name,
                           sources);
  };
  std::ranges::sort(sorted_markers,
                    [&](const frMarker* lhs, const frMarker* rhs) {
                      return marker_sort_key(lhs) < marker_sort_key(rhs);
                    });

  for (const frMarker* marker : sorted_markers) {
    // get violation bbox
    odb::Rect bbox = marker->getBBox();
    if (drcBox != odb::Rect() && !drcBox.intersects(bbox)) {
      continue;
    }
    auto tech = getDesign()->getTech();
    auto layer = tech->getLayer(marker->getLayerNum());
    auto layerType = layer->getType();

    auto con = marker->getConstraint();
    std::string violName;
    if (con) {
      if (con->typeId() == frConstraintTypeEnum::frcShortConstraint
          && layerType == dbTechLayerType::CUT) {
        violName = "Cut Short";
      } else {
        violName = con->getViolName();
      }
    } else {
      violName = "unknown";
    }

    odb::dbMarkerCategory* category
        = odb::dbMarkerCategory::createOrGet(tool_category, violName.c_str());

    odb::dbMarker* db_marker = odb::dbMarker::create(category);
    if (db_marker == nullptr) {
      continue;
    }

    // get source(s) of violation
    for (auto src : marker->getSrcs()) {
      if (src) {
        switch (src->typeId()) {
          case frcNet:
            db_marker->addSource(
                block->findNet(static_cast<frNet*>(src)->getName().c_str()));
            break;
          case frcInstTerm: {
            frInstTerm* instTerm = (static_cast<frInstTerm*>(src));
            std::string iterm_name;
            iterm_name += instTerm->getInst()->getName();
            iterm_name += "/";
            iterm_name += instTerm->getTerm()->getName();
            db_marker->addSource(block->findITerm(iterm_name.c_str()));
            break;
          }
          case frcBTerm: {
            frBTerm* bterm = static_cast<frBTerm*>(src);
            db_marker->addSource(block->findBTerm(bterm->getName().c_str()));
            break;
          }
          case frcInstBlockage: {
            frInst* inst = (static_cast<frInstBlockage*>(src))->getInst();
            db_marker->addSource(block->findInst(inst->getName().c_str()));
            break;
          }
          case frcInst: {
            frInst* inst = (static_cast<frInst*>(src));
            db_marker->addSource(block->findInst(inst->getName().c_str()));
            break;
          }
          case frcBlockage: {
            for (auto itr
                 = obs_rtree.qbegin(boost::geometry::index::intersects(bbox));
                 itr != obs_rtree.qend();
                 itr++) {
              db_marker->addSource(itr->second);
            }
            break;
          }
          default:
            logger_->error(DRT,
                           291,
                           "Unexpected source type in marker: {}",
                           src->typeId());
        }
      }
    }
    db_marker->addShape(bbox);
    db_marker->setTechLayer(
        block->getTech()->findLayer(layer->getName().c_str()));
  }

  if (file_name.empty()) {
    if (router_cfg_->VERBOSE > 0) {
      logger_->warn(
          DRT,
          290,
          "Warning: no DRC report specified, skipped writing DRC report");
    }
    return;
  }

  tool_category->writeTR(file_name);
}

std::vector<int> TritonRoute::routeLayerLengths(odb::dbWire* wire) const
{
  std::vector<int> lengths;
  lengths.resize(db_->getTech()->getLayerCount());
  odb::dbWireShapeItr shapes;
  odb::dbShape s;

  for (shapes.begin(wire); shapes.next(s);) {
    if (!s.isVia()) {
      lengths[s.getTechLayer()->getNumber()] += s.getLength();
    } else {
      if (s.getTechVia()) {
        lengths[s.getTechVia()->getBottomLayer()->getNumber() + 1] += 1;
      }
    }
  }

  return lengths;
}

}  // namespace drt
