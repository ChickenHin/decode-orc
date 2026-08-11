/*
 * File:        nabts_sink_stage.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     NABTS Sink Stage - recovers North American Basic Teletext
 *              data lines and exports the packet stream
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_SINK_STAGE_H
#define ORC_NABTS_SINK_STAGE_H

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/plugin/orc_stage_tooling.h>
#include <orc/stage/node_type.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/tooling/catalogue_results.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "nabts_catalogue_view.h"
#include "nabts_sink_deps_interface.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {

class IStageServices;

/**
 * @brief NABTS Sink Stage
 *
 * Extracts North American Basic Teletext (CEA-516, ITU-R BT.653 System C) data
 * lines from the VBI of a 525-line source and writes the recovered packets as a
 * flat, headerless packet stream in transmission coding, strictly temporally
 * ordered (frame → field → ascending line): `.t33` for the 33-byte data packet
 * of CEA-516 §3.1.
 *
 * The packets are also reassembled into data groups (§4) and teletext records
 * (§5) as the pass goes, and the records the range carried are catalogued for
 * the host to browse through ICatalogueResults.
 *
 * Separate from the Teletext Sink because the two services share nothing above
 * the packet — see docs-tech/nabts-support-design.md §2.
 *
 * This is a SINK stage - it has inputs but no outputs. All work happens in
 * trigger(); execute() only caches the input so the node can preview before it
 * has been triggered.
 */
class NabtsSinkStage : public DAGStage,
                       public ParameterizedStage,
                       public TriggerableStage,
                       public StageToolProvider,
                       public IStagePreviewCapability,
                       public ICatalogueResults {
 public:
  explicit NabtsSinkStage(IStageServices* stage_services);
  ~NabtsSinkStage() override = default;

  /// Testing seam: inject a pre-built deps instance to substitute concrete
  /// dep creation in trigger().
  void set_deps_override(std::shared_ptr<INabtsSinkStageDeps> deps) {
    deps_override_ = std::move(deps);
  }

  // DAGStage interface
  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD
  NodeTypeInfo get_node_type_info() const override;

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      ObservationContext& observation_context) override;

  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 0; }  // Sink has no outputs

  // ParameterizedStage interface
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;
  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  // TriggerableStage interface
  bool trigger(const std::vector<ArtifactPtr>& inputs,
               const std::map<std::string, ParameterValue>& parameters,
               IObservationContext& observation_context) override;

  std::string get_trigger_status() const override;

  void set_progress_callback(TriggerProgressCallback callback) override {
    progress_callback_ = callback;
  }

  bool is_trigger_in_progress() const override { return is_processing_.load(); }

  void cancel_trigger() override { cancel_requested_.store(true); }

  /// Whether the last trigger left a catalogue behind
  bool has_results() const { return has_results_; }

  /// The raw catalogue the last trigger built. Plugin-private: the host reads
  /// the stage through ICatalogueResults and never sees this type. Kept public
  /// for the stage's own tests, which compile against these sources.
  const NabtsAnalysisDataset& dataset() const { return dataset_; }

  // ICatalogueResults interface. Running every record's presentation code into
  // a display list costs real work and memory, and a run that only exports a
  // packet stream never needs it, so it is built on first ask and cached.
  // Called from the host's render worker, hence the lock.
  const CatalogueDataset& catalogue() const override {
    return catalogue_for(std::string());
  }

  // The same records drawn for a receiver the reader picked in the viewer
  // rather than the one the project's parameter names. Only the display lists
  // differ, so this rebuilds the catalogue and nothing above it: the records
  // themselves were recovered once and are not read again.
  const CatalogueDataset& catalogue(
      const std::string& view_option) const override {
    return catalogue_for(view_option);
  }

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

  std::vector<StageToolDescriptor> get_stage_tools() const override {
    return {StageToolDescriptor{
        "nabts_analysis", "NABTS Records",
        "Decode the NABTS service and browse the records it carried.",
        StageToolKind::CatalogueBrowser, false, kCatalogueBrowserContractId}};
  }

 private:
  /// The catalogue as drawn for the receiver @p view_option names, or for the
  /// project's own where it names none — which an empty string and an
  /// unrecognised one both do. Built if what is held was built for another
  /// receiver. One is held rather than one per receiver: the host copies what
  /// it is given before it asks again, and a page of deposited pixels is a
  /// large thing to keep four copies of for a reader looking at one.
  const CatalogueDataset& catalogue_for(const std::string& view_option) const {
    std::lock_guard<std::mutex> lock(catalogue_mutex_);
    const NaplpsRenderMode mode =
        naplps_render_mode_from_name(view_option, render_mode_);
    if (!catalogue_ || catalogue_mode_ != mode) {
      catalogue_ = std::make_unique<CatalogueDataset>(
          build_nabts_catalogue(dataset_, mode));
      catalogue_mode_ = mode;
    }
    return *catalogue_;
  }

  /// Drop the cached catalogue so the next reader rebuilds it from the dataset
  /// that has just replaced the one it was built from.
  void invalidate_catalogue() {
    std::lock_guard<std::mutex> lock(catalogue_mutex_);
    catalogue_.reset();
  }

  /// Set the receiver resolution the project asks for, which is the one a
  /// reader who picks none in the viewer gets. Shares the catalogue's lock
  /// because it is read on the render worker alongside it; what is cached needs
  /// no dropping, since catalogue_for() rebuilds whenever the receiver asked
  /// for is not the one it holds.
  void set_render_mode(NaplpsRenderMode mode) {
    std::lock_guard<std::mutex> lock(catalogue_mutex_);
    render_mode_ = mode;
  }

  // Parses the parameter set into deps options, converting the 1-based UI line
  // window to the 0-based field lines the slicer uses. Throws
  // std::runtime_error on missing/invalid parameters.
  NabtsSinkOptions parse_config(
      const std::map<std::string, ParameterValue>& parameters) const;

  std::map<std::string, ParameterValue> parameters_;
  std::string trigger_status_{"Idle"};
  TriggerProgressCallback progress_callback_;
  std::atomic<bool> is_processing_{false};
  std::atomic<bool> cancel_requested_{false};
  IStageServices* stage_services_{nullptr};
  std::shared_ptr<INabtsSinkStageDeps> deps_override_;

  // Cached results of the last trigger, resolved into the host-facing
  // catalogue on demand.
  NabtsAnalysisDataset dataset_;
  bool has_results_{false};
  // Built from dataset_ on first catalogue() call; cleared with it.
  mutable std::mutex catalogue_mutex_;
  mutable std::unique_ptr<CatalogueDataset> catalogue_;
  // The receiver catalogue_ was built for, which is the project's unless a
  // reader asked the viewer for another.
  mutable NaplpsRenderMode catalogue_mode_{NaplpsRenderMode::kReference};
  // The receiver the project asks for. Guarded by catalogue_mutex_, which is
  // what makes it safe to read on the render worker.
  NaplpsRenderMode render_mode_{NaplpsRenderMode::kReference};

  mutable std::shared_ptr<const VideoFrameRepresentation> cached_input_;
};

}  // namespace orc

#endif  // ORC_NABTS_SINK_STAGE_H
