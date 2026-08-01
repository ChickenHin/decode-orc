/*
 * File:        teletext_sink_stage.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Teletext Sink Stage - exports PAL WST teletext as a T42 packet
 *              stream
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_SINK_STAGE_H
#define ORC_TELETEXT_SINK_STAGE_H

#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/node_type.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "teletext_sink_stage_deps_interface.h"

namespace orc {

class IStageServices;

/**
 * @brief Teletext Sink Stage
 *
 * Extracts World System Teletext (ETSI EN 300 706 System B) data lines from
 * the VBI of PAL video and writes the recovered 42-byte packets as a flat
 * `.t42` packet stream — headerless, in transmission coding, strictly
 * temporally ordered (frame → field → ascending line). PAL only.
 *
 * This is a SINK stage - it has inputs but no outputs. All work happens in
 * trigger(); execute() returns no artifacts.
 */
class TeletextSinkStage : public DAGStage,
                          public ParameterizedStage,
                          public TriggerableStage {
 public:
  explicit TeletextSinkStage(IStageServices* stage_services);
  ~TeletextSinkStage() override = default;

  /// Testing seam: inject a pre-built deps instance to substitute concrete
  /// dep creation in trigger().
  void set_deps_override(std::shared_ptr<ITeletextSinkStageDeps> deps) {
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

 private:
  // Parses the §5.2 parameter set into deps options, converting the 1-based
  // UI line window to the 0-based field lines the observation schema uses.
  // Throws std::runtime_error on missing/invalid parameters.
  TeletextSinkOptions parse_config(
      const std::map<std::string, ParameterValue>& parameters) const;

  std::map<std::string, ParameterValue> parameters_;
  std::string trigger_status_{"Idle"};
  TriggerProgressCallback progress_callback_;
  std::atomic<bool> is_processing_{false};
  std::atomic<bool> cancel_requested_{false};
  IStageServices* stage_services_{nullptr};
  std::shared_ptr<ITeletextSinkStageDeps> deps_override_;
};

}  // namespace orc

#endif  // ORC_TELETEXT_SINK_STAGE_H
