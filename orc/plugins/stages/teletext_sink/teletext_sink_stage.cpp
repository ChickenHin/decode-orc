/*
 * File:        teletext_sink_stage.cpp
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Teletext Sink Stage - exports PAL WST teletext as a T42 packet
 *              stream
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_sink_stage.h"

#include <orc/abi/orc_plugin_services.h>
#include <orc/support/logging.h>

#include <memory>
#include <stdexcept>
#include <variant>

#include "teletext_sink_stage_deps.h"

namespace orc {

namespace {

// ETSI EN 300 706 §4.1: broadcast lines 6-22 (field 1) / 318-335 (field 2)
// may carry teletext; expressed as 1-based field lines the window is 6-22 in
// both fields. These bound the UI parameters.
constexpr int32_t kFirstAllowedUiLine = 1;
constexpr int32_t kLastAllowedUiLine = 22;

int32_t get_int32_or(const std::map<std::string, ParameterValue>& parameters,
                     const std::string& name, int32_t fallback) {
  const auto it = parameters.find(name);
  if (it == parameters.end() || !std::holds_alternative<int32_t>(it->second)) {
    return fallback;
  }
  return std::get<int32_t>(it->second);
}

bool get_bool_or(const std::map<std::string, ParameterValue>& parameters,
                 const std::string& name, bool fallback) {
  const auto it = parameters.find(name);
  if (it == parameters.end() || !std::holds_alternative<bool>(it->second)) {
    return fallback;
  }
  return std::get<bool>(it->second);
}

}  // namespace

TeletextSinkStage::TeletextSinkStage(IStageServices* stage_services)
    : stage_services_(stage_services) {
  set_configuration_status(orc::ConfigurationStatus::Red);
}

NodeTypeInfo TeletextSinkStage::get_node_type_info() const {
  return NodeTypeInfo{
      NodeType::SINK,
      "teletext_sink",
      "Teletext Sink",
      "Extracts teletext from the VBI and exports a T42 packet stream",
      1,
      1,  // One input
      0,
      0,  // No outputs (sink)
      VideoFormatCompatibility::PAL_ONLY};
}

std::vector<ArtifactPtr> TeletextSinkStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  // Sink stages don't produce outputs in execute(); the actual work happens
  // in trigger().
  (void)inputs;
  (void)parameters;
  (void)observation_context;
  return {};
}

std::vector<ParameterDescriptor> TeletextSinkStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  (void)project_format;
  (void)source_type;
  std::vector<ParameterDescriptor> descriptors;

  {
    ParameterDescriptor desc;
    desc.name = "output_path";
    desc.display_name = "Output File";
    desc.description = "Path to the output T42 packet stream";
    desc.type = ParameterType::FILE_PATH;
    desc.constraints.required = true;
    desc.constraints.default_value = std::string("");
    desc.file_extension_hint = ".t42";
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "first_vbi_line";
    desc.display_name = "First VBI Line";
    desc.description =
        "First candidate field line probed for teletext (1-based, both "
        "fields)";
    desc.type = ParameterType::INT32;
    desc.constraints.min_value = kFirstAllowedUiLine;
    desc.constraints.max_value = kLastAllowedUiLine;
    desc.constraints.default_value = int32_t{6};
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "last_vbi_line";
    desc.display_name = "Last VBI Line";
    desc.description =
        "Last candidate field line probed for teletext (1-based, both fields)";
    desc.type = ParameterType::INT32;
    desc.constraints.min_value = kFirstAllowedUiLine;
    desc.constraints.max_value = kLastAllowedUiLine;
    desc.constraints.default_value = int32_t{22};
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "keep_empty_packets";
    desc.display_name = "Keep Empty Packets";
    desc.description =
        "Emit 42 zero bytes for every candidate line with no data so packet "
        "position maps 1:1 to (frame, field, line)";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "tolerant_framing";
    desc.display_name = "Tolerant Framing";
    desc.description =
        "Accept framing codes with one bit error (raises the false-positive "
        "rate on noisy sources)";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "require_valid_mrag";
    desc.display_name = "Require Valid MRAG";
    desc.description =
        "Drop packets whose magazine/row address fails Hamming 8/4 "
        "correction (suppresses false locks on noise)";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = true;
    descriptors.push_back(desc);
  }

  return descriptors;
}

std::map<std::string, ParameterValue> TeletextSinkStage::get_parameters()
    const {
  return parameters_;
}

bool TeletextSinkStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  parameters_ = params;

  const auto it = params.find("output_path");
  const bool has_path =
      (it != params.end() && std::holds_alternative<std::string>(it->second) &&
       !std::get<std::string>(it->second).empty());

  set_configuration_status(has_path ? orc::ConfigurationStatus::Green
                                    : orc::ConfigurationStatus::Red);
  return true;
}

TeletextSinkOptions TeletextSinkStage::parse_config(
    const std::map<std::string, ParameterValue>& parameters) const {
  TeletextSinkOptions options;

  const auto path_it = parameters.find("output_path");
  if (path_it == parameters.end() ||
      !std::holds_alternative<std::string>(path_it->second)) {
    throw std::runtime_error("No output path specified");
  }
  options.output_path = std::get<std::string>(path_it->second);
  if (options.output_path.empty()) {
    throw std::runtime_error("Output path is empty");
  }

  // UI lines are 1-based (frame_numbering presentation convention); the
  // observation schema keys t42_<n> use 0-based field lines.
  const int32_t first_ui = get_int32_or(parameters, "first_vbi_line", 6);
  const int32_t last_ui = get_int32_or(parameters, "last_vbi_line", 22);
  if (first_ui < kFirstAllowedUiLine || last_ui > kLastAllowedUiLine ||
      first_ui > last_ui) {
    throw std::runtime_error("Invalid VBI line window");
  }
  options.first_field_line = first_ui - 1;
  options.last_field_line = last_ui - 1;

  options.keep_empty_packets =
      get_bool_or(parameters, "keep_empty_packets", false);
  options.tolerant_framing = get_bool_or(parameters, "tolerant_framing", false);
  options.require_valid_mrag =
      get_bool_or(parameters, "require_valid_mrag", true);

  return options;
}

bool TeletextSinkStage::trigger(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    IObservationContext& observation_context) {
  trigger_status_ = "Starting teletext export...";
  is_processing_.store(true);
  cancel_requested_.store(false);

  const auto fail_trigger = [this](const std::string& status) {
    trigger_status_ = status;
    is_processing_.store(false);
    return false;
  };

  try {
    if (inputs.empty()) {
      return fail_trigger("Error: No input connected");
    }

    auto representation =
        std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
    if (!representation) {
      return fail_trigger("Error: Input is not a video frame representation");
    }

    TeletextSinkOptions options;
    try {
      options = parse_config(parameters);
    } catch (const std::exception& e) {
      return fail_trigger(std::string("Error: ") + e.what());
    }

    std::shared_ptr<ITeletextSinkStageDeps> deps = deps_override_;
    if (!deps) {
      deps = std::make_shared<TeletextSinkStageDeps>(
          stage_services_, orc::plugin::get_observation_service());
    }

    deps->init(progress_callback_, &cancel_requested_);

    const TeletextSinkResult result =
        deps->export_t42(representation.get(), observation_context, options);

    is_processing_.store(false);

    if (!result.success) {
      trigger_status_ = "Error: " + (result.message.empty()
                                         ? std::string("Teletext export failed")
                                         : result.message);
      ORC_LOG_ERROR("TeletextSink: {}", trigger_status_);
      return false;
    }

    trigger_status_ = "Exported " + std::to_string(result.packets_written) +
                      " teletext packets (" +
                      std::to_string(result.fields_with_data) +
                      " fields with data) to " + result.output_path;
    ORC_LOG_INFO("TeletextSink: {}", trigger_status_);
    return true;

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("TeletextSink: {}", e.what());
    return fail_trigger(std::string("Error: ") + e.what());
  }
}

std::string TeletextSinkStage::get_trigger_status() const {
  return trigger_status_;
}

}  // namespace orc
