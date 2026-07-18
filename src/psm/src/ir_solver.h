// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2024-2025, The OpenROAD Authors

#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "Eigen/Sparse"
#include "boost/geometry/geometry.hpp"
#include "boost/polygon/polygon.hpp"
#include "connection.h"
#include "debug_gui.h"
#include "ir_network.h"
#include "node.h"
#include "odb/PtrSetMap.h"
#include "odb/db.h"
#include "odb/geom.h"
#include "psm/pdnsim.h"
#include "utl/Logger.h"

namespace sta {
class dbSta;
class Scene;
}  // namespace sta

namespace est {
class EstimateParasitics;
}

namespace psm {
class IRNetwork;

class IRSolver
{
 public:
  using Voltage = double;
  using Current = double;
  using Power = float;

  struct Results
  {
    Voltage net_voltage = 0.0;
    Voltage worst_voltage = 0.0;
    Voltage worst_ir_drop = 0.0;
    Voltage avg_voltage = 0.0;
    Voltage avg_ir_drop = 0.0;
    float max_percent = 0.0;
    Power total_power = 0.0;
  };
  struct EMResults
  {
    Current max_current = 0.0;
    Current avg_current = 0.0;
    std::size_t resistors = 0;
  };
  struct ConnectivityResults
  {
    std::set<Node*, Node::Compare> unconnected_nodes;
    std::set<ITermNode*, Node::Compare> unconnected_iterms;
  };

  // Knobs for a transient (dynamic / di-dt) power-grid analysis.  All defaults
  // reproduce a physically-meaningful vectorless worst case with no external
  // input required; every field is overridable from the Tcl command.
  struct TransientSettings
  {
    double period = 0.0;      // clock period [s] (must be > 0)
    int steps = 100;          // timesteps per clock period
    int num_periods = 1;      // number of clock periods to simulate
    double node_cap = 0.0;    // uniform per-node capacitance to ground [F]
    double total_cap = 0.0;   // total on-die cap distributed across sink nodes
    double decap_cap = 0.0;   // aggregate decap contribution added to total_cap
    double current_duty = 1.0;  // triangular current-pulse duty in (0,1]
    bool phase_spread = false;  // false: worst-case simultaneous switching
    std::string current_profile;  // optional global current-vs-time waveform
    // Vectored (per-instance) refinement.  A file of
    //   <instance_name> <activity_scale> <phase_center> [duty]
    // rows, derived from a VCD/SAIF, giving each instance its own switching
    // phase (and optional activity weight / duty) so switching is no longer
    // assumed simultaneous.  Empty => vectorless worst case (the safe default).
    std::string vectored_profile;
    // When true, per-instance activity_scale weights are renormalized so the
    // total average current is preserved (vectoring redistributes WHERE current
    // peaks in time, it does not inflate total power).
    bool normalize_vectored = true;
    // Lumped package/board parasitics in series with the ideal supply.  Their
    // job is the di/dt inductive-droop ("first droop") term the resistive PDN
    // cannot produce: the whole-die supply rail droops by
    //   package_r * I_total(t) + package_l * dI_total/dt.
    double package_r = 0.0;  // package/board series resistance [ohm]
    double package_l = 0.0;  // package/board series inductance [H]
  };

  // Results of a transient analysis, alongside the static reference so callers
  // can report the dynamic/static droop ratio.
  struct TransientResults
  {
    Voltage net_voltage = 0.0;
    Voltage worst_static_voltage = 0.0;
    Voltage worst_static_ir_drop = 0.0;
    Voltage worst_dynamic_voltage = 0.0;
    Voltage worst_dynamic_ir_drop = 0.0;
    double dynamic_static_ratio = 0.0;
    double worst_time = 0.0;  // [s]
    int worst_step = -1;
    double total_capacitance = 0.0;  // [F] actually applied to the grid
    double timestep = 0.0;           // [s]
    int total_steps = 0;
    bool quasi_static = false;  // true when no capacitance was supplied
    bool vectored = false;      // true when a per-instance profile was applied
    int vectored_insts = 0;     // number of instances matched by the profile
    // Worst instantaneous package/board droop package_r*I + package_l*dI/dt [V].
    double package_droop = 0.0;
  };

  using UserVoltages = odb::PtrMap<odb::dbNet, std::map<sta::Scene*, Voltage>>;
  using UserPowers = odb::PtrMap<odb::dbInst, std::map<sta::Scene*, Power>>;

  IRSolver(odb::dbNet* net,
           bool floorplanning,
           sta::dbSta* sta,
           est::EstimateParasitics* estimate_parasitics,
           utl::Logger* logger,
           const UserVoltages& user_voltages,
           const UserPowers& user_powers,
           const PDNSim::GeneratedSourceSettings& generated_source_settings);

  odb::dbNet* getNet() const { return net_; };

  bool check(bool check_bterms);

  void solve(sta::Scene* corner,
             GeneratedSourceType source_type,
             const std::string& source_file);

  // Transient (dynamic) solve: computes the static DC operating point, then
  // time-steps the RC power grid under a per-clock triangular current model to
  // find the worst dynamic voltage droop.  Leaves the static solution in place
  // (getSolution stays valid) and stores the dynamic envelope separately.
  void solveTransient(sta::Scene* corner,
                      GeneratedSourceType source_type,
                      const std::string& source_file,
                      const TransientSettings& settings);

  void report(sta::Scene* corner) const;
  void reportEM(sta::Scene* corner) const;
  void reportTransient(sta::Scene* corner) const;

  Results getSolution(sta::Scene* corner) const;
  TransientResults getTransientSolution(sta::Scene* corner) const;
  bool hasTransientSolution(sta::Scene* corner) const;
  void writeTransientVoltageFile(const std::string& voltage_file,
                                 sta::Scene* corner) const;
  EMResults getEMSolution(sta::Scene* corner) const;
  PDNSim::IRDropByPoint getIRDrop(odb::dbTechLayer* layer,
                                  sta::Scene* corner) const;
  ConnectivityResults getConnectivityResults() const;

  void enableGui(bool enable);

  void writeErrorFile(const std::string& error_file) const;
  void writeInstanceVoltageFile(const std::string& voltage_file,
                                sta::Scene* corner) const;
  void writeEMFile(const std::string& em_file, sta::Scene* corner) const;
  void writeSpiceFile(GeneratedSourceType source_type,
                      const std::string& spice_file,
                      sta::Scene* corner,
                      const std::string& voltage_source_file) const;

  bool belongsTo(Node* node) const;
  bool belongsTo(Connection* connection) const;

  std::vector<sta::Scene*> getCorners() const;
  bool hasSolution(sta::Scene* corner) const;
  Voltage getNetVoltage(sta::Scene* corner) const;
  std::optional<Voltage> getVoltage(sta::Scene* corner, Node* node) const;

  std::optional<Voltage> getSDCVoltage(sta::Scene* corner,
                                       odb::dbNet* net) const;
  std::optional<Voltage> getPVTVoltage(sta::Scene* corner) const;
  std::optional<Voltage> getUserVoltage(sta::Scene* corner,
                                        odb::dbNet* net) const;
  std::optional<Voltage> getSolutionVoltage(sta::Scene* corner) const;

  odb::dbNet* getPowerNet() const;

  Connection::ResistanceMap getResistanceMap(sta::Scene* corner) const;
  void assertResistanceMap(sta::Scene* corner) const;

  IRNetwork* getNetwork() const { return network_.get(); }

 private:
  template <typename T>
  using ValueNodeMap = std::map<const Node*, T>;

  odb::dbBlock* getBlock() const;
  odb::dbTech* getTech() const;

  bool checkOpen();
  bool checkBTerms() const;
  bool checkShort() const;

  odb::PtrMap<odb::dbInst, Power> getInstancePower(sta::Scene* corner) const;
  Voltage getPowerNetVoltage(sta::Scene* corner) const;

  Connection::ConnectionMap<Current> generateCurrentMap(
      sta::Scene* corner) const;

  Connection::ConnectionMap<Connection::Conductance> generateConductanceMap(
      sta::Scene* corner,
      const Connections& connections) const;
  Voltage generateSourceNodes(GeneratedSourceType source_type,
                              const std::string& source_file,
                              sta::Scene* corner,
                              SourceNodes& sources) const;
  SourceNodes generateSourceNodesFromBTerms() const;
  SourceNodes generateSourceNodesGenericFull() const;
  SourceNodes generateSourceNodesGenericStraps() const;
  SourceNodes generateSourceNodesGenericBumps() const;
  SourceNodes generateSourceNodesFromShapes(
      const std::set<odb::Rect>& shapes) const;
  Voltage generateSourceNodesFromSourceFile(const std::string& source_file,
                                            sta::Scene* corner,
                                            SourceNodes& sources) const;

  void reportUnconnectedNodes() const;
  void reportMissingBTerm() const;

  std::map<Node*, Connection::ConnectionSet> getNodeConnectionMap(
      const Connection::ConnectionMap<Connection::Conductance>& conductance)
      const;
  IRSolver::Power buildNodeCurrentMap(sta::Scene* corner,
                                      ValueNodeMap<Current>& currents) const;
  std::map<Node*, std::size_t> assignNodeIDs(const Node::NodeSet& nodes,
                                             std::size_t start = 0) const;
  std::map<Node*, std::size_t> assignNodeIDs(const SourceNodes& nodes,
                                             std::size_t start = 0) const;
  void buildCondMatrixAndVoltages(
      bool is_ground,
      const std::map<Node*, Connection::ConnectionSet>& node_connections,
      const ValueNodeMap<Current>& currents,
      const Connection::ConnectionMap<Connection::Conductance>& conductance,
      const std::map<Node*, std::size_t>& node_index,
      Eigen::SparseMatrix<Connection::Conductance>& g_matrix,
      Eigen::VectorXd& j_vector) const;
  void addSourcesToMatrixAndVoltages(
      Voltage src_voltage,
      const SourceNodes& sources,
      const std::map<Node*, std::size_t>& node_index,
      Eigen::SparseMatrix<Connection::Conductance>& g_matrix,
      Eigen::VectorXd& j_vector) const;

  // Assembles the linear system (G, J) shared by the static and transient
  // solves.  Fills currents_[corner] with the per-node average current and
  // clears voltages_[corner].  Returns false (and leaves the caches erased)
  // when the net has no nodes.  real_node_index maps only real (non-source-
  // helper) grid nodes back to their matrix row so the caller can extract
  // per-node voltages; the source-helper rows occupy the remaining indices up
  // to g_matrix.rows().
  bool assembleSystem(
      sta::Scene* corner,
      GeneratedSourceType source_type,
      const std::string& source_file,
      Eigen::SparseMatrix<Connection::Conductance>& g_matrix,
      Eigen::VectorXd& j_vector,
      std::map<Node*, std::size_t>& real_node_index,
      Voltage& src_voltage,
      Power& total_power,
      // Optional: receives the matrix-row indices of the ideal-source pins (the
      // rail rows).  Used by the transient path to modulate the rail voltage
      // for the package/board di/dt droop.
      std::set<std::size_t>* source_indices = nullptr);

  // Builds the per-node capacitance-to-ground vector for a transient solve,
  // indexed to match node_index.  Returns the total capacitance applied.
  double buildTransientCapacitance(
      const TransientSettings& settings,
      const std::map<Node*, std::size_t>& real_node_index,
      const ValueNodeMap<Current>& currents,
      std::size_t num_nodes,
      Eigen::VectorXd& cap_diag) const;

  std::string getMetricKey(const std::string& key, sta::Scene* corner) const;

  void dumpVector(const Eigen::VectorXd& vector, const std::string& name) const;
  void dumpMatrix(const Eigen::SparseMatrix<Connection::Conductance>& matrix,
                  const std::string& name) const;
  void dumpConductance(
      const Connection::ConnectionMap<Connection::Conductance>& cond,
      const std::string& name) const;

  odb::dbNet* net_;

  utl::Logger* logger_;
  est::EstimateParasitics* estimate_parasitics_;
  sta::dbSta* sta_;

  std::unique_ptr<IRNetwork> network_;

  std::unique_ptr<DebugGui> gui_;

  const UserVoltages& user_voltages_;
  const UserPowers& user_powers_;
  std::map<sta::Scene*, Voltage> solution_voltages_;
  std::map<sta::Scene*, Power> solution_power_;

  const PDNSim::GeneratedSourceSettings& generated_source_settings_;

  std::optional<bool> connected_;

  std::map<sta::Scene*, ValueNodeMap<Voltage>> voltages_;
  std::map<sta::Scene*, ValueNodeMap<Current>> currents_;

  // Per-node minimum voltage over the transient window, and the summary
  // results, keyed by corner.  Populated only by solveTransient().
  std::map<sta::Scene*, ValueNodeMap<Voltage>> transient_min_voltages_;
  std::map<sta::Scene*, TransientResults> transient_results_;

  static constexpr Current kSpiceFileMinCurrent = 1e-18;
};

}  // namespace psm
