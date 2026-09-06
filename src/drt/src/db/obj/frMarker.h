// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2019-2025, The OpenROAD Authors

#pragma once

#include <memory>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "db/obj/frBlockObject.h"
#include "db/obj/frFig.h"
#include "frBaseTypes.h"
#include "odb/dbTransform.h"

namespace drt {
class frConstraint;
class frMarker : public frFig
{
 public:
  // constructors
  frMarker() = default;
  frMarker& operator=(const frMarker&) = default;
  // A hand-written copy constructor used to sit here that copied constraint_,
  // bbox_, layerNum_, srcs_, vioHasDir_ and vioIsH_ and silently dropped
  // victims_ and aggressors_ -- the two rectangles that say what the violation
  // is BETWEEN -- while the copy-assignment above kept them.
  //
  // Every marker that reaches the design is copy-CONSTRUCTED, never assigned:
  // TritonRoute::getDRCMarkers, FlexDRWorker::endAddMarkers and
  // FlexDRWorker::setMarkers all do. So the block's markers, the DRC report and
  // the GUI marker browser carried no victim and no aggressor, and
  // FlexDRWorker::route_queue_update_from_marker's movableAggressorNets loop --
  // reached from route_queue_init_queue, which iterates that copied set --
  // could never fire. Measured: rqi_aggr = 0 in every iteration of
  // gcd_nangate45, sha256 and subservient, while the same run's uncopied path
  // (route_queue_update_queue on the GC worker's own unique_ptrs) reported
  // rq_aggr equal to rq_markers.
  //
  // The two copies also disagreed, and std::vector chooses between them by
  // capacity, so `bestMarkers_ = markers_` could give different contents for
  // the same input.
  frMarker(const frMarker&) = default;
  // setters
  void setConstraint(frConstraint* constraintIn) { constraint_ = constraintIn; }

  void setBBox(const odb::Rect& bboxIn) { bbox_ = bboxIn; }

  void setLayerNum(const frLayerNum& layerNumIn) { layerNum_ = layerNumIn; }

  void setHasDir(const bool& in) { vioHasDir_ = in; }

  void setIsH(const bool& in) { vioIsH_ = in; }

  void addSrc(frBlockObject* srcIn) { srcs_.insert(srcIn); }
  void addAggressor(frBlockObject* obj,
                    const std::tuple<frLayerNum, odb::Rect, bool>& tupleIn)
  {
    aggressors_.emplace_back(obj, tupleIn);
  }
  void addVictim(frBlockObject* obj,
                 const std::tuple<frLayerNum, odb::Rect, bool>& tupleIn)
  {
    victims_.emplace_back(obj, tupleIn);
  }
  // getters

  /* from frFig
   * getBBox
   * move, in .cpp
   * intersects in .cpp
   */

  odb::Rect getBBox() const override { return bbox_; }
  frLayerNum getLayerNum() const { return layerNum_; }

  const frOrderedIdSet<frBlockObject*>& getSrcs() const { return srcs_; }

  void setSrcs(const frOrderedIdSet<frBlockObject*>& srcs) { srcs_ = srcs; }

  std::vector<
      std::pair<frBlockObject*, std::tuple<frLayerNum, odb::Rect, bool>>>&
  getAggressors()
  {
    return aggressors_;
  }

  std::vector<
      std::pair<frBlockObject*, std::tuple<frLayerNum, odb::Rect, bool>>>&
  getVictims()
  {
    return victims_;
  }

  frConstraint* getConstraint() const { return constraint_; }

  bool hasDir() const { return vioHasDir_; }

  bool isH() const { return vioIsH_; }

  void move(const odb::dbTransform& xform) override {}

  bool intersects(const odb::Rect& box) const override { return false; }

  // others
  frBlockObjectEnum typeId() const override { return frcMarker; }

  void setIter(frListIter<std::unique_ptr<frMarker>>& in) { iter_ = in; }
  frListIter<std::unique_ptr<frMarker>> getIter() const { return iter_; }
  void setIndexInOwner(const int& idx) { index_in_owner_ = idx; }
  int getIndexInOwner() const { return index_in_owner_; }

 private:
  frConstraint* constraint_{nullptr};
  odb::Rect bbox_;
  frLayerNum layerNum_{0};
  frOrderedIdSet<frBlockObject*> srcs_;
  std::vector<
      std::pair<frBlockObject*, std::tuple<frLayerNum, odb::Rect, bool>>>
      victims_;  // obj, isFixed
  std::vector<
      std::pair<frBlockObject*, std::tuple<frLayerNum, odb::Rect, bool>>>
      aggressors_;  // obj, isFixed
  frListIter<std::unique_ptr<frMarker>> iter_;
  bool vioHasDir_{false};
  bool vioIsH_{false};
  int index_in_owner_{0};

  template <class Archive>
  void serialize(Archive& ar, unsigned int version);

  friend class boost::serialization::access;
};
}  // namespace drt
