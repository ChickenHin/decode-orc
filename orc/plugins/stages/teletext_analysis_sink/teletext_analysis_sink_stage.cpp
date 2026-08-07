/*
 * File:        teletext_analysis_sink_stage.cpp
 * Module:      orc-stage-plugin-teletext_analysis_sink
 * Purpose:     Teletext Analysis Sink Stage implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_analysis_sink_stage.h"

#include <orc/abi/orc_plugin_services.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>
#include <orc/support/teletext_page_decoder.h>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>

#include "teletext_analysis_sink_deps.h"
#include "teletext_frame_slicer.h"

namespace orc {

namespace {

// The candidate VBI windows, as the 1-based field lines the UI presents.
// 625 lines — ETSI EN 300 706 §4.1: broadcast lines 6-22 (field 1) / 318-335
// (field 2), i.e. 1-based field lines 6-22 in both fields.
// 525 lines — ITU-R BT.653 §2: broadcast lines 10-21 / 273-284, i.e. 1-based
// field lines 10-21.
constexpr int32_t kFirstUiLine625 = kTeletextFirstFieldLine625 + 1;
constexpr int32_t kLastUiLine625 = kTeletextLastFieldLine625 + 1;
constexpr int32_t kFirstUiLine525 = kTeletextFirstFieldLine525 + 1;
constexpr int32_t kLastUiLine525 = kTeletextLastFieldLine525 + 1;

// The widest window either service uses bounds the parameters, so a project
// whose format is not yet known can still be configured.
constexpr int32_t kFirstAllowedUiLine = 1;
constexpr int32_t kLastAllowedUiLine = 22;

// Whether |project_format| is a 525-line system. Unknown is treated as 625:
// it is the service the stage was written for, and the parameters it produces
// are a superset of the 525-line window.
bool is_525_line(VideoSystem project_format) {
  return project_format == VideoSystem::NTSC ||
         project_format == VideoSystem::PAL_M;
}

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

std::string get_string_or(
    const std::map<std::string, ParameterValue>& parameters,
    const std::string& name, const std::string& fallback) {
  const auto it = parameters.find(name);
  if (it == parameters.end() ||
      !std::holds_alternative<std::string>(it->second)) {
    return fallback;
  }
  return std::get<std::string>(it->second);
}

}  // namespace

TeletextAnalysisSinkStage::TeletextAnalysisSinkStage(
    IStageServices* stage_services)
    : stage_services_(stage_services) {
  set_configuration_status(orc::ConfigurationStatus::Red);
}

NodeTypeInfo TeletextAnalysisSinkStage::get_node_type_info() const {
  return NodeTypeInfo{
      NodeType::ANALYSIS_SINK,
      "teletext_analysis_sink",
      "Teletext Analysis Sink",
      "Recovers teletext from the VBI, exports the packet stream and browses "
      "the pages. Trigger to update the page catalogue.",
      1,
      1,  // One input
      0,
      0,  // No outputs (sink)
      VideoFormatCompatibility::ALL};
}

std::vector<ArtifactPtr> TeletextAnalysisSinkStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  // Sink stages don't produce outputs in execute(); the actual work happens
  // in trigger(). The input is cached so the preview surface has something to
  // show before the node has been triggered.
  (void)parameters;
  (void)observation_context;
  cached_input_ = nullptr;
  if (!inputs.empty()) {
    cached_input_ =
        std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  }
  return {};
}

StagePreviewCapability TeletextAnalysisSinkStage::get_preview_capability()
    const {
  return PreviewHelpers::make_signal_preview_capability(cached_input_);
}

std::vector<ParameterDescriptor>
TeletextAnalysisSinkStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  (void)source_type;
  const bool line_525 = is_525_line(project_format);
  std::vector<ParameterDescriptor> descriptors;

  {
    ParameterDescriptor desc;
    desc.name = "output_path";
    desc.display_name = "Output File";
    desc.description =
        line_525
            ? "Path to the output T34 packet stream (34-byte 525-line packets)"
            : "Path to the output T42 packet stream (42-byte 625-line packets)";
    desc.type = ParameterType::FILE_PATH;
    desc.constraints.required = true;
    desc.constraints.default_value = std::string("");
    desc.file_extension_hint = line_525 ? ".t34" : ".t42";
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
    desc.constraints.default_value =
        line_525 ? int32_t{kFirstUiLine525} : int32_t{kFirstUiLine625};
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
    desc.constraints.default_value =
        line_525 ? int32_t{kLastUiLine525} : int32_t{kLastUiLine625};
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "keep_empty_packets";
    desc.display_name = "Keep Empty Packets";
    desc.description =
        "Emit a whole zero packet for every candidate line with no data so "
        "packet position maps 1:1 to (frame, field, line)";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "detector";
    desc.display_name = "Bit Detector";
    desc.description =
        "How data bits are recovered from each line. Threshold slices at bit "
        "centres and suits discs and direct captures, which pass the whole "
        "data band. MLSE fits the recording's frequency response to the known "
        "start of each line and is what recovers teletext from tape, where "
        "the limited bandwidth smears bits into their neighbours. Automatic "
        "tries Threshold first and falls back to MLSE only where it fails";
    desc.type = ParameterType::STRING;
    desc.constraints.allowed_strings = {"Automatic", "Threshold", "MLSE"};
    desc.constraints.default_value = std::string("Automatic");
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

  {
    ParameterDescriptor desc;
    desc.name = "repair_damaged_bytes";
    desc.display_name = "Repair Damaged Bytes";
    desc.description =
        "Every display byte carries a parity bit, so a byte that fails its "
        "parity check is known to be damaged. Restore it by flipping the bit "
        "the MLSE detector came closest to reading the other way. Recovers "
        "characters a difficult tape would otherwise lose, at the cost of the "
        "repaired bytes becoming indistinguishable from undamaged ones — a "
        "repair that guessed wrong is no longer marked as damage. Applies to "
        "the MLSE detector only, so a disc or direct capture is unaffected";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = true;
    desc.constraints.depends_on =
        ParameterDependency{"detector", {"Automatic", "MLSE"}};
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "squash_repeated_rows";
    desc.display_name = "Combine Repeated Rows";
    desc.description =
        "Teletext pages are transmitted on a loop, so a recording holds "
        "several copies of every row damaged in different places. Combine "
        "them byte by byte, preferring values that pass their parity check, "
        "and write the combined rows. Needs a second pass over the recovered "
        "packets, held in memory";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = true;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "write_report";
    desc.display_name = "Write Report File";
    desc.description =
        "Write the run's diagnostic report next to the packet stream, named "
        "after it with a .txt extension (mydata.t42 gives mydata.t42.txt). "
        "The report says how many candidate lines yielded packets, how the "
        "odd-parity failures of the recovered packets are spread across the "
        "display-byte positions, and what combining repeated rows changed. "
        "The same report is always written to the log at debug level";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  // Subtitle export is 625-line only: the cue timing derives from the 50
  // fields per second of ITU-R BT.1700 Annex 1 Part B Table 1 item 2.
  if (!line_525) {
    {
      ParameterDescriptor desc;
      desc.name = "export_subtitles";
      desc.display_name = "Export Subtitles";
      desc.description =
          "Decode the subtitle page (C6-flagged, conventionally 888) and "
          "write timed subtitle cues next to the T42 output";
      desc.type = ParameterType::BOOL;
      desc.constraints.default_value = false;
      descriptors.push_back(desc);
    }

    {
      ParameterDescriptor desc;
      desc.name = "subtitle_page";
      desc.display_name = "Subtitle Page";
      desc.description =
          "Teletext page carrying the subtitles: magazine digit (1-8) "
          "followed by two hexadecimal page digits, e.g. 888";
      desc.type = ParameterType::STRING;
      desc.constraints.default_value = std::string("888");
      desc.constraints.depends_on =
          ParameterDependency{"export_subtitles", {"true"}};
      descriptors.push_back(desc);
    }

    {
      ParameterDescriptor desc;
      desc.name = "subtitle_format";
      desc.display_name = "Subtitle Format";
      desc.description =
          "Subtitle output format (SubRip .srt; colour and positioning are "
          "dropped at this level)";
      desc.type = ParameterType::STRING;
      desc.constraints.allowed_strings = {"SRT"};
      desc.constraints.default_value = std::string("SRT");
      desc.constraints.depends_on =
          ParameterDependency{"export_subtitles", {"true"}};
      descriptors.push_back(desc);
    }
  }

  return descriptors;
}

std::map<std::string, ParameterValue>
TeletextAnalysisSinkStage::get_parameters() const {
  return parameters_;
}

bool TeletextAnalysisSinkStage::set_parameters(
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

TeletextAnalysisSinkOptions TeletextAnalysisSinkStage::parse_config(
    const std::map<std::string, ParameterValue>& parameters) const {
  TeletextAnalysisSinkOptions options;

  const auto path_it = parameters.find("output_path");
  if (path_it == parameters.end() ||
      !std::holds_alternative<std::string>(path_it->second)) {
    throw std::runtime_error("No output path specified");
  }
  options.output_path = std::get<std::string>(path_it->second);
  if (options.output_path.empty()) {
    throw std::runtime_error("Output path is empty");
  }

  // UI lines are 1-based (frame_numbering presentation convention); the slicer
  // window is 0-based field lines.
  const int32_t first_ui =
      get_int32_or(parameters, "first_vbi_line", kFirstUiLine625);
  const int32_t last_ui =
      get_int32_or(parameters, "last_vbi_line", kLastUiLine625);
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
  options.parity_repair = get_bool_or(parameters, "repair_damaged_bytes", true);

  const std::string detector =
      get_string_or(parameters, "detector", "Automatic");
  if (detector == "Automatic") {
    options.detector = TeletextDetector::kAuto;
  } else if (detector == "Threshold") {
    options.detector = TeletextDetector::kThreshold;
  } else if (detector == "MLSE") {
    options.detector = TeletextDetector::kMlse;
  } else {
    throw std::runtime_error("Unknown bit detector: " + detector);
  }

  options.squash_repeated_rows =
      get_bool_or(parameters, "squash_repeated_rows", true);
  options.write_report = get_bool_or(parameters, "write_report", false);
  options.export_subtitles = get_bool_or(parameters, "export_subtitles", false);
  if (options.export_subtitles) {
    options.subtitle_page = get_string_or(parameters, "subtitle_page", "888");
    if (!TeletextPageDecoder::parse_page_number(options.subtitle_page)
             .has_value()) {
      throw std::runtime_error(
          "Invalid subtitle page \"" + options.subtitle_page +
          "\" (expected magazine digit 1-8 plus two hex digits, e.g. 888)");
    }
    const std::string format =
        get_string_or(parameters, "subtitle_format", "SRT");
    if (format != "SRT") {
      throw std::runtime_error("Unsupported subtitle format: " + format);
    }
  }

  return options;
}

bool TeletextAnalysisSinkStage::trigger(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    IObservationContext& observation_context) {
  // The stage owns its decoding end to end; nothing is read from or written to
  // the observation store.
  (void)observation_context;

  trigger_status_ = "Starting teletext analysis...";
  is_processing_.store(true);
  cancel_requested_.store(false);
  has_results_ = false;
  dataset_ = TeletextAnalysisDataset{};

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

    TeletextAnalysisSinkOptions options;
    try {
      options = parse_config(parameters);
    } catch (const std::exception& e) {
      return fail_trigger(std::string("Error: ") + e.what());
    }

    std::shared_ptr<ITeletextAnalysisSinkStageDeps> deps = deps_override_;
    if (!deps) {
      deps = std::make_shared<TeletextAnalysisSinkDeps>(stage_services_);
    }

    deps->init(progress_callback_, &cancel_requested_);

    const TeletextAnalysisSinkResult result =
        deps->analyse(representation.get(), options);

    is_processing_.store(false);

    // The catalogue is kept whether or not the run finished: a cancelled run
    // still recovered the pages it got to, and the viewer is the reason the
    // user triggered it.
    dataset_ = result.dataset;
    has_results_ = result.success;

    // Diagnostic report of the run: recovery profile plus what combining
    // repeated rows changed. Reported for a run that was cancelled part-way as
    // well as one that finished — how it was going is exactly the question a
    // cancelled run leaves — at debug level, because it is many lines and the
    // answer most runs need is the one-line status below.
    if (!result.report.empty()) {
      ORC_LOG_DEBUG("TeletextAnalysisSink:\n{}", result.report);
    }

    if (!result.success) {
      trigger_status_ =
          "Error: " + (result.message.empty()
                           ? std::string("Teletext analysis failed")
                           : result.message);
      ORC_LOG_ERROR("TeletextAnalysisSink: {}", trigger_status_);
      return false;
    }

    trigger_status_ = "Recovered " + std::to_string(result.packets_written) +
                      " teletext packets (" +
                      std::to_string(result.fields_with_data) +
                      " fields with data) to " + result.output_path;
    trigger_status_ += "; " + std::to_string(result.dataset.pages.size()) +
                       (result.dataset.pages.size() == 1 ? " page" : " pages");
    if (result.bytes_repaired > 0) {
      trigger_status_ += "; repaired " + std::to_string(result.bytes_repaired) +
                         " damaged bytes";
    }
    if (result.packets_corrected > 0) {
      trigger_status_ += "; combined repeated rows corrected " +
                         std::to_string(result.packets_corrected) + " packets";
    }
    // The result in one figure, on the same terms as the report's headline:
    // characters that still fail their parity check are characters the reader
    // will see damaged.
    if (result.characters_written > 0) {
      char loss[32];
      std::snprintf(loss, sizeof(loss), "%.2f",
                    100.0 * static_cast<double>(result.characters_damaged) /
                        static_cast<double>(result.characters_written));
      trigger_status_ += "; data loss " + std::string(loss) + "% (" +
                         std::to_string(result.characters_damaged) + " of " +
                         std::to_string(result.characters_written) +
                         " characters damaged)";
    }
    if (!result.subtitle_path.empty()) {
      trigger_status_ += "; " + std::to_string(result.subtitle_cues_written) +
                         " subtitle cues to " + result.subtitle_path;
    }
    if (!result.report_path.empty()) {
      trigger_status_ += "; report to " + result.report_path;
    }
    ORC_LOG_INFO("TeletextAnalysisSink: {}", trigger_status_);
    return true;

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("TeletextAnalysisSink: {}", e.what());
    return fail_trigger(std::string("Error: ") + e.what());
  }
}

std::string TeletextAnalysisSinkStage::get_trigger_status() const {
  return trigger_status_;
}

}  // namespace orc
