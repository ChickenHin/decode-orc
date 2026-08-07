/*
 * File:        teletext_analysis_sink_stage.h
 * Module:      orc-stage-plugin-teletext_analysis_sink
 * Purpose:     Teletext Analysis Sink Stage - recovers WST teletext, exports
 *              the packet stream and presents a page viewer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_ANALYSIS_SINK_STAGE_H
#define ORC_TELETEXT_ANALYSIS_SINK_STAGE_H

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/plugin/orc_stage_tooling.h>
#include <orc/stage/analysis_sink_results.h>
#include <orc/stage/node_type.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "teletext_analysis_sink_deps_interface.h"

namespace orc {

class IStageServices;

/**
 * @brief Teletext Analysis Sink Stage
 *
 * Extracts World System Teletext (ITU-R BT.653 System B) data lines from the
 * VBI and writes the recovered packets as a flat, headerless packet stream in
 * transmission coding, strictly temporally ordered (frame → field → ascending
 * line): `.t42` for the 42-byte 625-line service (ETSI EN 300 706) and `.t34`
 * for the 34-byte 525-line one (BT.653 Table 1b).
 *
 * The same pass builds a catalogue of every page the range carried, which the
 * host reads through ITeletextAnalysisResults and presents as the stage's
 * batch-analysis tool.
 *
 * This is an ANALYSIS_SINK stage - it has inputs but no outputs. All work
 * happens in trigger(); execute() only caches the input.
 */
class TeletextAnalysisSinkStage : public DAGStage,
                                  public ParameterizedStage,
                                  public TriggerableStage,
                                  public StageToolProvider,
                                  public IStagePreviewCapability,
                                  public ITeletextAnalysisResults {
 public:
  explicit TeletextAnalysisSinkStage(IStageServices* stage_services);
  ~TeletextAnalysisSinkStage() override = default;

  /// Testing seam: inject a pre-built deps instance to substitute concrete
  /// dep creation in trigger().
  void set_deps_override(std::shared_ptr<ITeletextAnalysisSinkStageDeps> deps) {
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

  // ITeletextAnalysisResults interface
  bool has_results() const override { return has_results_; }
  const TeletextAnalysisDataset& dataset() const override { return dataset_; }

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

  std::vector<StageToolDescriptor> get_stage_tools() const override {
    return {StageToolDescriptor{
        "teletext_analysis", "Teletext Pages",
        "Decode the teletext service and browse the pages it carried.",
        StageToolKind::BatchAnalysis, false,
        "decode-orc.stage-tools.teletext-analysis.v1"}};
  }

 private:
  // Parses the parameter set into deps options, converting the 1-based UI line
  // window to the 0-based field lines the slicer uses. Throws
  // std::runtime_error on missing/invalid parameters.
  TeletextAnalysisSinkOptions parse_config(
      const std::map<std::string, ParameterValue>& parameters) const;

  std::map<std::string, ParameterValue> parameters_;
  std::string trigger_status_{"Idle"};
  TriggerProgressCallback progress_callback_;
  std::atomic<bool> is_processing_{false};
  std::atomic<bool> cancel_requested_{false};
  IStageServices* stage_services_{nullptr};
  std::shared_ptr<ITeletextAnalysisSinkStageDeps> deps_override_;

  // Cached results of the last trigger, read by the host through
  // ITeletextAnalysisResults.
  TeletextAnalysisDataset dataset_;
  bool has_results_{false};
  mutable std::shared_ptr<const VideoFrameRepresentation> cached_input_;
};

}  // namespace orc

#endif  // ORC_TELETEXT_ANALYSIS_SINK_STAGE_H
