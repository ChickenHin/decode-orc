/*
 * File:        vbi_source_stage.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Raw VBI capture source stage: places VBI records into CVBS
 * frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_source_stage.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/error_types.h>
#include <orc/support/logging.h>
#include <orc/support/lru_cache.h>
#include <orc/support/preview_helpers.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <utility>

#include "vbi_frame_builder.h"
#include "vbi_frame_index.h"
#include "vbi_level_mapper.h"
#include "vbi_line_reader.h"
#include "vbi_offset_calibration.h"
#include "vbi_source_validation.h"
#include "vbi_teletext_service.h"
#include "vbi_transport.h"

namespace orc {

namespace {

// ---------------------------------------------------------------------------
// Stage identity
// ---------------------------------------------------------------------------

constexpr const char* kStageName = "vbi_source";
constexpr const char* kDisplayName = "VBI Capture Source";
constexpr const char* kDescription =
    "Raw VBI capture source - places the teletext line records of card and TBC "
    "VBI captures onto CVBS frames";

// The namespace every observation this stage writes belongs to.
constexpr const char* kObservationNamespace = "vbi_source";

// ---------------------------------------------------------------------------
// Parameter names
// ---------------------------------------------------------------------------

constexpr const char* kParamInputPath = "input_path";
constexpr const char* kParamFormat = "format";
constexpr const char* kParamSampleRate = "container_sample_rate_hz";
constexpr const char* kParamLineLength = "container_line_length";
constexpr const char* kParamValidSamples = "container_valid_samples";
constexpr const char* kParamSampleFormat = "container_sample_format";
constexpr const char* kParamFieldLines = "container_field_lines";
constexpr const char* kParamFirstRecord = "container_first_record";
constexpr const char* kParamLastRecord = "container_last_record";
constexpr const char* kParamFrameTrailerBytes = "container_frame_trailer_bytes";
constexpr const char* kParamContainerTvSystem = "container_tv_system";
constexpr const char* kParamTeletextSystem = "teletext_system";
constexpr const char* kParamCaptureOffsetMode = "capture_offset_mode";
constexpr const char* kParamCaptureOffsetSamples = "capture_offset_samples";
constexpr const char* kParamLevels = "levels";
constexpr const char* kParamFixedLogic0 = "fixed_logic0";
constexpr const char* kParamFixedLogic1 = "fixed_logic1";
constexpr const char* kParamFirstField = "first_field";
constexpr const char* kParamDrops = "drops";

// The value of "format" that spells the container out field by field.
constexpr const char* kCustomPreset = "custom";

// Values of the capture-offset mode.
constexpr const char* kCaptureOffsetAuto = "auto";
constexpr const char* kCaptureOffsetManual = "manual";

// Values of the level mode.
constexpr const char* kLevelsPerLine = "per-line";
constexpr const char* kLevelsRolling = "rolling";
constexpr const char* kLevelsFixed = "fixed";

// Values of the drop policy.
constexpr const char* kDropsPreserve = "preserve";
constexpr const char* kDropsPad = "pad";

// Spellings of the data services and television systems the parameter surface
// offers.  A system the stage cannot yet synthesise is deliberately absent
// rather than offered and then refused.
constexpr const char* kTeletextWST = "WST";
constexpr const char* kTeletextNABTS = "NABTS";
constexpr const char* kTvSystemPAL = "PAL";

// Built frames held against a repeat request.  Each PAL frame is 1,4 MB, so the
// window is a memory-against-rework trade: it covers a preview scrubbing back
// and forth and the observers that re-read a frame they have just been handed,
// without holding a decode's worth of frames resident.
//
// It has to be comfortably larger than the number of readers, or a window the
// size of the reader pool is evicted before its frames are used: every reader
// then misses, rebuilds what another reader just built, and evicts a third
// reader's frame doing it. The background observation pool runs one worker per
// two cores and the preview render is another reader on top, so thirty-two
// frames (45 MB) covers the pool on any machine this runs on with room for the
// prefetch window either side of the preview.
constexpr size_t kFrameCacheSize = 32;

// Stored frames calibration samples across the capture.
constexpr uint32_t kCalibrationSampleFrames = 16;

// ---------------------------------------------------------------------------
// Parameter map access
// ---------------------------------------------------------------------------

std::string string_param(const std::map<std::string, ParameterValue>& params,
                         const char* key, const std::string& fallback) {
  const auto it = params.find(key);
  if (it == params.end()) return fallback;
  const auto* value = std::get_if<std::string>(&it->second);
  return value ? *value : fallback;
}

double double_param(const std::map<std::string, ParameterValue>& params,
                    const char* key, double fallback) {
  const auto it = params.find(key);
  if (it == params.end()) return fallback;
  if (const auto* value = std::get_if<double>(&it->second)) return *value;
  if (const auto* value = std::get_if<int32_t>(&it->second)) {
    return static_cast<double>(*value);
  }
  if (const auto* value = std::get_if<uint32_t>(&it->second)) {
    return static_cast<double>(*value);
  }
  return fallback;
}

uint32_t uint_param(const std::map<std::string, ParameterValue>& params,
                    const char* key, uint32_t fallback) {
  const auto it = params.find(key);
  if (it == params.end()) return fallback;
  if (const auto* value = std::get_if<uint32_t>(&it->second)) return *value;
  if (const auto* value = std::get_if<int32_t>(&it->second)) {
    return (*value >= 0) ? static_cast<uint32_t>(*value) : fallback;
  }
  return fallback;
}

// ---------------------------------------------------------------------------
// Production dependencies
// ---------------------------------------------------------------------------

class VBISourceStageDeps final : public IVBISourceStageDeps {
 public:
  bool validate_input_file(const std::string& input_path,
                           std::string& error_message) const override {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(input_path, ec)) {
      error_message = "VBI capture '" + input_path + "' does not exist.";
      return false;
    }
    if (!fs::is_regular_file(input_path, ec)) {
      error_message = "VBI capture '" + input_path + "' is not a regular file.";
      return false;
    }
    return true;
  }

  std::unique_ptr<IVBIByteSource> open_byte_source(
      const std::string& input_path,
      std::string& error_message) const override {
    return open_vbi_byte_source(input_path, error_message);
  }
};

// ---------------------------------------------------------------------------
// VBIFrameRepresentation
// ---------------------------------------------------------------------------

// The stage's output: CVBS frames built on the frame a consumer asks for, from
// the line records of the capture that frame came from.
//
// Inherits VideoFrameRepresentation (the read contract every downstream stage
// uses) and Artifact (so it can be returned from execute()), and implements the
// frame index's counter source over its own reader.
//
// Thread safety: two locks, deliberately narrow.  io_mutex_ covers reads
// through the byte source (which holds one stream position, so they are
// strictly serial) and cache_mutex_ covers the frame cache.  Building the frame
// from the records that read produced holds neither: the builder is const and
// carries only configuration, so building is reentrant.  Holding one lock
// across all of it made every reader in the process queue behind every other,
// and because std::mutex is not fair, a pool of background observation workers
// could starve the interactive preview render.  Nothing that is not stream
// state is locked here.
class VBIFrameRepresentation final : public VideoFrameRepresentation,
                                     public Artifact,
                                     private IVBIFrameCounterSource {
 public:
  // Everything a run is configured by, gathered so the factory signature does
  // not run to a dozen positional arguments.
  struct Setup {
    VBISourceFormat format;
    VBILevelMapperConfig levels;
    VBIFrameSequenceConfig sequence;
    double capture_offset_samples = 0.0;
    std::string input_path;
  };

  // Build the representation over an opened capture.  Returns nullptr with an
  // error message for any part of the configuration the stage cannot place, or
  // when the capture's frame counter could not be read.
  static std::shared_ptr<VBIFrameRepresentation> create(
      std::unique_ptr<IVBIByteSource> byte_source, Setup setup,
      ArtifactID artifact_id, Provenance provenance,
      std::string& error_message) {
    if (byte_source == nullptr) {
      error_message = "The VBI capture was not opened.";
      return nullptr;
    }

    auto representation =
        std::shared_ptr<VBIFrameRepresentation>(new VBIFrameRepresentation(
            std::move(byte_source), std::move(setup), std::move(artifact_id),
            std::move(provenance)));

    if (!representation->initialise(error_message)) {
      return nullptr;
    }
    return representation;
  }

  // --------------------------------------------------------------------------
  // Artifact
  // --------------------------------------------------------------------------
  std::string type_name() const override { return "VBIFrameRepresentation"; }

  // --------------------------------------------------------------------------
  // What the run made of the capture
  // --------------------------------------------------------------------------
  const VBIFrameIndex& frame_index() const { return index_; }

  // --------------------------------------------------------------------------
  // Navigation
  // --------------------------------------------------------------------------
  FrameIDRange frame_range() const override {
    if (frame_count_ == 0) return FrameIDRange{1, 0};  // empty: last < first
    return FrameIDRange{0, static_cast<FrameID>(frame_count_ - 1)};
  }

  size_t frame_count() const override { return frame_count_; }

  bool has_frame(FrameID id) const override {
    return id < static_cast<FrameID>(frame_count_);
  }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    const VBIOutputFrame& output = builder_.output_frame();
    FrameDescriptor desc;
    desc.frame_id = id;
    desc.system = output.system;
    desc.height = static_cast<size_t>(output.lines_per_frame);
    desc.samples_total = static_cast<size_t>(output.samples_per_frame);
    desc.samples_per_line_nominal =
        static_cast<size_t>(output.samples_per_line_nominal);
    return desc;
  }

  // --------------------------------------------------------------------------
  // Flat sample access
  // --------------------------------------------------------------------------
  //
  // Line access is left to the base class, which reads the flat frame through
  // frame_line_util.h — the same lattice the frames are written on, so a caller
  // asking for a line gets the line the builder wrote.
  const sample_type* get_frame(FrameID id) const override {
    if (!has_frame(id)) return nullptr;
    const std::vector<sample_type>* frame = ensure_frame_cached(id);
    return frame ? frame->data() : nullptr;
  }

  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    if (!has_frame(id)) return {};
    ensure_frame_cached(id);
    // Copied with the cache lock held so an eviction on another thread cannot
    // free the entry mid-copy.
    std::lock_guard<std::mutex> lock(cache_mutex_);
    const std::vector<sample_type>* frame = frame_cache_.get_ptr(id);
    return frame != nullptr ? *frame : std::vector<sample_type>{};
  }

  // --------------------------------------------------------------------------
  // Hints
  // --------------------------------------------------------------------------
  std::optional<SourceParameters> get_video_parameters() const override {
    return video_params_;
  }

 private:
  VBIFrameRepresentation(std::unique_ptr<IVBIByteSource> byte_source,
                         Setup setup, ArtifactID artifact_id,
                         Provenance provenance)
      : Artifact(std::move(artifact_id), std::move(provenance)),
        byte_source_(std::move(byte_source)),
        setup_(std::move(setup)) {}

  bool initialise(std::string& error_message) {
    reader_ = std::make_unique<VBILineReader>(setup_.format, *byte_source_);

    if (!make_vbi_frame_builder(setup_.format, setup_.levels,
                                setup_.capture_offset_samples, builder_,
                                error_message)) {
      return false;
    }

    const std::optional<uint64_t> stored_frames = reader_->frame_count();
    if (!stored_frames.has_value()) {
      error_message =
          "The capture's length could not be established, so the number of "
          "frames it holds is unknown.";
      return false;
    }

    if (!VBIFrameIndex::build(setup_.format, setup_.sequence, *stored_frames,
                              *this, index_, error_message)) {
      return false;
    }
    frame_count_ = static_cast<size_t>(index_.output_frame_count());

    build_video_parameters();
    return true;
  }

  // EBU Tech. 3280-E level and geometry constants for the emitted frames.  The
  // active picture bounds are the standard's, so that a consumer reading them
  // sees the frame the stage claims to produce rather than the empty raster it
  // happens to hold.
  void build_video_parameters() {
    video_params_ = SourceParameters{};
    video_params_.system = VideoSystem::PAL;
    video_params_.frame_width_nominal = kPalSamplesPerLineNominal;
    video_params_.frame_height = kPalFrameLines;
    video_params_.number_of_sequential_frames =
        static_cast<int32_t>(frame_count_);
    video_params_.sync_tip_level = kPalSyncTip;
    video_params_.blanking_level = kPalBlanking;
    video_params_.black_level = kPalBlack;
    video_params_.white_level = kPalWhite;
    video_params_.peak_level = kPalPeak;
    video_params_.active_video_start = kPalActiveVideoStart;
    video_params_.active_video_end = kPalActiveVideoEnd;
    video_params_.first_active_frame_line = kPalFirstActiveFrameLine;
    video_params_.last_active_frame_line = kPalLastActiveFrameLine;
  }

  // IVBIFrameCounterSource.  Called by the frame index, which is only ever
  // reached with io_mutex_ already held.
  bool frame_counter(uint64_t stored_frame_index,
                     std::optional<uint32_t>& out_counter,
                     std::string& error_message) const override {
    return reader_->read_frame_counter(stored_frame_index, out_counter,
                                       error_message);
  }

  // Caller holds no lock.
  const std::vector<sample_type>* ensure_frame_cached(FrameID id) const {
    {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      if (frame_cache_.contains(id)) {
        return frame_cache_.get_ptr(id);
      }
    }

    // Built with no lock held. Two threads racing the same frame build it twice
    // and the loser's copy is dropped, which costs one frame of duplicated work
    // — far less than the alternative of holding every other reader off for the
    // duration (see the class comment).
    std::vector<sample_type> built = build_frame(id);

    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (!frame_cache_.contains(id)) {
      frame_cache_.put(id, std::move(built));
    }
    return frame_cache_.get_ptr(id);
  }

  // Caller holds no lock: this takes io_mutex_ for the parts that read the
  // capture and holds nothing for the rest.  Throws when the frame cannot be
  // built: a frame that cannot be built is a broken configuration or a broken
  // capture, and returning blanking for it would hide both.
  std::vector<sample_type> build_frame(FrameID id) const {
    std::string error;

    VBIOutputFramePlan plan;
    VBIFrameRecords records;
    {
      // Everything that reads through the byte source, and nothing else. The
      // frame index resolves a plan by reading frame counters through the same
      // reader, so it belongs inside the same critical section.
      std::lock_guard<std::mutex> lock(io_mutex_);

      if (!index_.frame_plan(static_cast<uint64_t>(id), plan, error)) {
        throw std::runtime_error("VBI source '" + setup_.input_path +
                                 "': " + error);
      }
      if (!plan.padding &&
          !reader_->read_frame(plan.source_frame_index, records, error)) {
        throw std::runtime_error("VBI source '" + setup_.input_path +
                                 "': " + error);
      }
    }

    std::vector<sample_type> samples;
    if (plan.padding) {
      builder_.build_blank_frame(samples);
      return samples;
    }

    uint32_t data_lines = 0;
    if (!builder_.build_frame(records.lines, samples, data_lines, error)) {
      throw std::runtime_error("VBI source '" + setup_.input_path +
                               "': " + error);
    }
    return samples;
  }

  std::unique_ptr<IVBIByteSource> byte_source_;
  Setup setup_;

  // Held by pointer because it cannot be built until the capture is open and
  // it is not default-constructible.
  std::unique_ptr<VBILineReader> reader_;

  VBIFrameBuilder builder_;
  VBIFrameIndex index_;

  size_t frame_count_ = 0;
  SourceParameters video_params_;

  // The capture: one stream position, so every read through it is serial.
  mutable std::mutex io_mutex_;

  // The cache, held only across a lookup or an insertion — never across a
  // build, and never across a read of the capture.
  mutable std::mutex cache_mutex_;
  mutable LRUCache<FrameID, std::vector<sample_type>> frame_cache_{
      kFrameCacheSize};
};

// ---------------------------------------------------------------------------
// Configuration helpers
// ---------------------------------------------------------------------------

VBILevelMode parse_level_mode(const std::string& name) {
  if (name == kLevelsRolling) return VBILevelMode::kRolling;
  if (name == kLevelsFixed) return VBILevelMode::kFixed;
  return VBILevelMode::kPerLine;
}

VBIDropPolicy parse_drop_policy(const std::string& name) {
  return (name == kDropsPad) ? VBIDropPolicy::kPad : VBIDropPolicy::kPreserve;
}

// Join a list of messages into one, so a wrong configuration is reported in
// full rather than one field at a time.
std::string join_messages(const std::vector<std::string>& messages) {
  std::string joined;
  for (const std::string& message : messages) {
    if (!joined.empty()) joined += " ";
    joined += message;
  }
  return joined;
}

// Fit the capture offset from the clock run-in.
//
// The stage does not proceed with a bad fit: the offset is applied globally to
// a capture that may run for hours, so a wrong one silently mis-places every
// line of it (design §5.3.4).
double calibrate_capture_offset(const VBILineReader& reader,
                                const VBITeletextService& service,
                                const std::string& input_path,
                                ObservationContext& observation_context) {
  VBICalibrationConfig calibration_config;
  calibration_config.sample_frames = kCalibrationSampleFrames;

  VBIOffsetCalibration calibration;
  std::string error;
  if (!calibrate_vbi_capture_offset(reader, service, calibration_config,
                                    calibration, error)) {
    throw UserDataError("The capture offset of '" + input_path +
                        "' could not be measured: " + error);
  }

  if (!calibration.converged) {
    throw UserDataError(
        "The capture offset of '" + input_path + "' could not be trusted: " +
        join_messages(calibration.diagnostics) + " " + calibration.summary);
  }

  observation_context.set(FieldID(0), kObservationNamespace, "calibration",
                          calibration.summary);
  observation_context.set(FieldID(0), kObservationNamespace, "capture_offset",
                          calibration.capture_offset_samples);
  observation_context.set(FieldID(0), kObservationNamespace,
                          "capture_offset_spread", calibration.spread_samples);
  observation_context.set(FieldID(0), kObservationNamespace,
                          "capture_offset_acceptance",
                          calibration.acceptance_fraction);

  ORC_LOG_INFO("{}: {}", kStageName, calibration.summary);
  for (const std::string& warning : calibration.warnings) {
    ORC_LOG_WARN("{}: {}", kStageName, warning);
  }

  return calibration.capture_offset_samples;
}

}  // namespace

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

std::string VBISourceStage::Configuration::cache_key() const {
  std::string key = input_path;
  key += "|" + format_preset;
  key += "|" + std::to_string(container_sample_rate_hz);
  key += "|" + std::to_string(container_line_length);
  key += "|" + std::to_string(container_valid_samples);
  key += "|" + container_sample_format;
  key += "|" + std::to_string(container_field_lines);
  key += "|" + std::to_string(container_first_record);
  key += "|" + std::to_string(container_last_record);
  key += "|" + std::to_string(container_frame_trailer_bytes);
  key += "|" + container_tv_system;
  key += "|" + teletext_system;
  key += "|" + capture_offset_mode;
  key += "|" + std::to_string(capture_offset_samples);
  key += "|" + levels;
  key += "|" + std::to_string(fixed_logic0);
  key += "|" + std::to_string(fixed_logic1);
  key += "|" + std::to_string(first_field);
  key += "|" + drops;
  return key;
}

bool VBISourceStage::make_source_format(const Configuration& configuration,
                                        VBISourceFormat& out_format,
                                        std::string& error_message) {
  if (!expand_vbi_source_preset(configuration.format_preset, out_format,
                                error_message)) {
    return false;
  }

  if (configuration.format_preset == kCustomPreset) {
    if (!parse_vbi_sample_format(configuration.container_sample_format,
                                 out_format.sample_format)) {
      error_message = "Container sample format '" +
                      configuration.container_sample_format +
                      "' is not one of 'u8', 'u16le' or 's16le'.";
      return false;
    }
    if (configuration.container_tv_system != kTvSystemPAL) {
      error_message = "Television system '" +
                      configuration.container_tv_system +
                      "' is not implemented yet; only PAL frames can "
                      "currently be synthesised.";
      return false;
    }

    out_format.sample_rate_hz = configuration.container_sample_rate_hz;
    out_format.line_length = configuration.container_line_length;
    out_format.valid_samples = configuration.container_valid_samples;
    out_format.field_lines = configuration.container_field_lines;
    out_format.field_range = VBIFieldRange{configuration.container_first_record,
                                           configuration.container_last_record};
    out_format.frame_trailer_bytes =
        configuration.container_frame_trailer_bytes;
    out_format.frame_trailer_is_counter =
        configuration.container_frame_trailer_bytes >= 4u;
    out_format.tv_system = VBITVSystem::kPAL;
  }

  // The data service is configured rather than taken from the preset, because
  // it is a property of the broadcast the capture was made from, not of the
  // card that made it.
  if (configuration.teletext_system == kTeletextWST) {
    out_format.tt_system = VBITeletextSystem::kWST;
  } else if (configuration.teletext_system == kTeletextNABTS) {
    out_format.tt_system = VBITeletextSystem::kNABTS;
    error_message =
        "Teletext system 'NABTS' is not implemented yet; only WST (System B) "
        "can currently be placed. The configured system is carried on the "
        "stage's output so a downstream decoder sees it rather than "
        "silently decoding the wrong service.";
    return false;
  } else {
    error_message = "Teletext system '" + configuration.teletext_system +
                    "' is not recognised; it must be 'WST' or 'NABTS'.";
    return false;
  }

  if (configuration.first_field != 1u && configuration.first_field != 2u) {
    error_message =
        "The first stored field must be television field 1 or 2, "
        "not " +
        std::to_string(configuration.first_field) + ".";
    return false;
  }
  out_format.first_field = configuration.first_field;

  if (configuration.capture_offset_mode == kCaptureOffsetManual) {
    out_format.capture_offset_samples = configuration.capture_offset_samples;
    out_format.capture_offset_is_auto = false;
  } else if (configuration.capture_offset_mode != kCaptureOffsetAuto) {
    error_message = "Capture offset mode '" +
                    configuration.capture_offset_mode +
                    "' is not recognised; it must be 'auto' or 'manual'.";
    return false;
  }

  return true;
}

VBISourceStage::Configuration VBISourceStage::configuration_from(
    const std::map<std::string, ParameterValue>& parameters) const {
  Configuration configuration = configuration_;

  configuration.input_path =
      string_param(parameters, kParamInputPath, configuration.input_path);
  configuration.format_preset =
      string_param(parameters, kParamFormat, configuration.format_preset);
  configuration.container_sample_rate_hz = double_param(
      parameters, kParamSampleRate, configuration.container_sample_rate_hz);
  configuration.container_line_length = uint_param(
      parameters, kParamLineLength, configuration.container_line_length);
  configuration.container_valid_samples = uint_param(
      parameters, kParamValidSamples, configuration.container_valid_samples);
  configuration.container_sample_format = string_param(
      parameters, kParamSampleFormat, configuration.container_sample_format);
  configuration.container_field_lines = uint_param(
      parameters, kParamFieldLines, configuration.container_field_lines);
  configuration.container_first_record = uint_param(
      parameters, kParamFirstRecord, configuration.container_first_record);
  configuration.container_last_record = uint_param(
      parameters, kParamLastRecord, configuration.container_last_record);
  configuration.container_frame_trailer_bytes =
      uint_param(parameters, kParamFrameTrailerBytes,
                 configuration.container_frame_trailer_bytes);
  configuration.container_tv_system = string_param(
      parameters, kParamContainerTvSystem, configuration.container_tv_system);
  configuration.teletext_system = string_param(parameters, kParamTeletextSystem,
                                               configuration.teletext_system);
  configuration.capture_offset_mode = string_param(
      parameters, kParamCaptureOffsetMode, configuration.capture_offset_mode);
  configuration.capture_offset_samples =
      double_param(parameters, kParamCaptureOffsetSamples,
                   configuration.capture_offset_samples);
  configuration.levels =
      string_param(parameters, kParamLevels, configuration.levels);
  configuration.fixed_logic0 =
      double_param(parameters, kParamFixedLogic0, configuration.fixed_logic0);
  configuration.fixed_logic1 =
      double_param(parameters, kParamFixedLogic1, configuration.fixed_logic1);
  configuration.first_field =
      uint_param(parameters, kParamFirstField, configuration.first_field);
  configuration.drops =
      string_param(parameters, kParamDrops, configuration.drops);

  return configuration;
}

// ---------------------------------------------------------------------------
// VBISourceStage
// ---------------------------------------------------------------------------

VBISourceStage::VBISourceStage(std::shared_ptr<IVBISourceStageDeps> deps)
    : deps_(deps ? std::move(deps) : std::make_shared<VBISourceStageDeps>()) {
  set_configuration_status(ConfigurationStatus::Red);
}

NodeTypeInfo VBISourceStage::get_node_type_info() const {
  return NodeTypeInfo{NodeType::SOURCE, kStageName, kDisplayName, kDescription,
                      0, 0, 1, UINT32_MAX,
                      // Only 625-line frames can be synthesised so far; the
                      // 525-line systems arrive with their geometry.
                      VideoFormatCompatibility::PAL_ONLY};
}

std::vector<ArtifactPtr> VBISourceStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  std::lock_guard<std::mutex> lock(execute_mutex_);

  if (!inputs.empty()) {
    throw std::runtime_error(std::string(kStageName) +
                             ": source stage expects no inputs");
  }

  const Configuration configuration = configuration_from(parameters);

  if (configuration.input_path.empty()) {
    ORC_LOG_DEBUG("{}: no input configured", kStageName);
    return {};
  }

  const std::string cache_key = configuration.cache_key();
  if (cached_representation_ && cached_key_ == cache_key) {
    return {cached_representation_};
  }

  // --- Container descriptor ---
  VBISourceFormat format;
  std::string error;
  if (!make_source_format(configuration, format, error)) {
    throw UserDataError(error);
  }

  // --- Input file ---
  if (!deps_->validate_input_file(configuration.input_path, error)) {
    throw UserDataError(error);
  }

  std::unique_ptr<IVBIByteSource> byte_source =
      deps_->open_byte_source(configuration.input_path, error);
  if (byte_source == nullptr) {
    throw UserDataError(error);
  }

  // --- Configuration validation against the stream ---
  VBITransportHints hints;
  hints.bits_per_sample = byte_source->declared_bits_per_sample();
  const std::vector<std::string> violations =
      validate_vbi_source_config(format, byte_source->size_bytes(), hints);
  if (!violations.empty()) {
    throw UserDataError(
        "VBI capture '" + configuration.input_path +
        "' does not match its configuration: " + join_messages(violations));
  }

  VBITeletextService service;
  if (!vbi_teletext_service(format.tv_system, format.tt_system, service,
                            error)) {
    throw UserDataError(error);
  }

  VBILineReader reader(format, *byte_source);
  const std::optional<uint64_t> stored_frames = reader.frame_count();
  if (!stored_frames.has_value() || *stored_frames == 0) {
    throw UserDataError("VBI capture '" + configuration.input_path +
                        "' is too short to hold one complete frame of the "
                        "configured container.");
  }

  // --- Capture offset ---
  double capture_offset_samples = format.capture_offset_samples;
  if (format.capture_offset_is_auto) {
    capture_offset_samples = calibrate_capture_offset(
        reader, service, configuration.input_path, observation_context);
  } else {
    ORC_LOG_INFO(
        "{}: capture offset held at the configured {:.2f} samples; no "
        "calibration was run",
        kStageName, capture_offset_samples);
  }

  // The fitted figure replaces the descriptor's starting hint from here on, so
  // that everything derived from the record's own geometry — in particular the
  // windows the level mapper reads its logic levels from — sits where the
  // run-in actually is rather than where the preset's folklore predicted it.
  format.capture_offset_samples = capture_offset_samples;
  format.capture_offset_is_auto = false;

  // --- Output representation ---
  VBIFrameRepresentation::Setup setup;
  setup.format = format;
  setup.levels.mode = parse_level_mode(configuration.levels);
  setup.levels.fixed_logic0 = configuration.fixed_logic0;
  setup.levels.fixed_logic1 = configuration.fixed_logic1;
  setup.sequence.drops = parse_drop_policy(configuration.drops);
  setup.capture_offset_samples = capture_offset_samples;
  setup.input_path = configuration.input_path;

  Provenance provenance;
  provenance.stage_name = kStageName;
  provenance.stage_version = version();
  provenance.parameters = {
      {kParamInputPath, configuration.input_path},
      {kParamFormat, configuration.format_preset},
      // Carried so a downstream decoder can see which data service the
      // capture holds rather than assuming one (design §4).
      {kParamTeletextSystem, configuration.teletext_system},
      {"video_system", "PAL"},
      {"sample_encoding", "CVBS_U10_4FSC"},
      {kParamCaptureOffsetSamples, std::to_string(capture_offset_samples)},
      {kParamDrops, configuration.drops},
  };

  auto representation = VBIFrameRepresentation::create(
      std::move(byte_source), std::move(setup),
      ArtifactID(std::string(kStageName) + ":" + cache_key),
      std::move(provenance), error);
  if (representation == nullptr) {
    throw UserDataError(error);
  }

  const VBIFrameIndex& index = representation->frame_index();
  const std::string sequence_summary = index.summary();
  observation_context.set(FieldID(0), kObservationNamespace, "frame_sequence",
                          sequence_summary);
  observation_context.set(FieldID(0), kObservationNamespace, "teletext_system",
                          configuration.teletext_system);

  ORC_LOG_INFO(
      "{}: loaded '{}' — {} format, {} stored frames, {} output frames. {}",
      kStageName, configuration.input_path, configuration.format_preset,
      index.stored_frame_count(), index.output_frame_count(), sequence_summary);

  cached_representation_ = representation;
  cached_key_ = cache_key;
  return {representation};
}

std::vector<ParameterDescriptor> VBISourceStage::get_parameter_descriptors(
    VideoSystem /*project_format*/, SourceType /*source_type*/) const {
  std::vector<ParameterDescriptor> descriptors;

  // Shown only when the container is spelled out field by field.
  const ParameterDependency custom_only{kParamFormat, {kCustomPreset}, true};

  {
    ParameterDescriptor pd;
    pd.name = kParamInputPath;
    pd.display_name = "VBI Capture Path";
    pd.description =
        "Path to the raw VBI capture. FLAC-wrapped captures (.vbi.flac) are "
        "unwrapped transparently; the wrapper's declared sample rate is "
        "ignored.";
    pd.type = ParameterType::FILE_PATH;
    pd.constraints.required = false;
    pd.constraints.default_value = std::string("");
    pd.file_extension_hint = ".flac";
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamFormat;
    pd.display_name = "Capture Format";
    pd.description =
        "Named container preset describing how the capture stores its line "
        "records. 'custom' spells the container out field by field instead.";
    pd.type = ParameterType::STRING;
    pd.constraints.required = true;
    pd.constraints.default_value = std::string("bt8x8-pal");
    pd.constraints.allowed_strings = vbi_source_preset_names();
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamSampleRate;
    pd.display_name = "Sample Rate (Hz)";
    pd.description =
        "Exact sampling rate of the capture. Never taken from a FLAC "
        "wrapper's header, which carries a placeholder.";
    pd.type = ParameterType::DOUBLE;
    pd.constraints.default_value = 0.0;
    pd.constraints.min_value = 0.0;
    pd.constraints.depends_on = custom_only;
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamLineLength;
    pd.display_name = "Record Stride (samples)";
    pd.description =
        "Stored samples per line record, hardware padding included.";
    pd.type = ParameterType::UINT32;
    pd.constraints.default_value = uint32_t{0};
    pd.constraints.depends_on = custom_only;
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamValidSamples;
    pd.display_name = "Valid Samples per Record";
    pd.description =
        "Real samples at the start of each record. Samples beyond this are "
        "hardware padding and are never resampled or measured.";
    pd.type = ParameterType::UINT32;
    pd.constraints.default_value = uint32_t{0};
    pd.constraints.depends_on = custom_only;
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamSampleFormat;
    pd.display_name = "Sample Format";
    pd.description =
        "Sample word of the stored records. Nothing in the container declares "
        "it, so it is always configuration.";
    pd.type = ParameterType::STRING;
    pd.constraints.default_value = std::string("u8");
    pd.constraints.allowed_strings = {"u8", "u16le", "s16le"};
    pd.constraints.depends_on = custom_only;
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamFieldLines;
    pd.display_name = "Records per Field";
    pd.description = "Stored line records per field: the field stride.";
    pd.type = ParameterType::UINT32;
    pd.constraints.default_value = uint32_t{0};
    pd.constraints.depends_on = custom_only;
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamFirstRecord;
    pd.display_name = "First Data Record";
    pd.description =
        "First stored record of each field that carries the data service "
        "(0-based).";
    pd.type = ParameterType::UINT32;
    pd.constraints.default_value = uint32_t{0};
    pd.constraints.depends_on = custom_only;
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamLastRecord;
    pd.display_name = "Last Data Record";
    pd.description =
        "Last stored record of each field that carries the data service "
        "(0-based, inclusive).";
    pd.type = ParameterType::UINT32;
    pd.constraints.default_value = uint32_t{0};
    pd.constraints.depends_on = custom_only;
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamFrameTrailerBytes;
    pd.display_name = "Frame Trailer Bytes";
    pd.description =
        "Trailing bytes of each stored frame that are not sample data. Four "
        "for the bt8x8 frame counter, which is what makes dropped frames "
        "detectable; zero for formats without one.";
    pd.type = ParameterType::UINT32;
    pd.constraints.default_value = uint32_t{0};
    pd.constraints.depends_on = custom_only;
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamContainerTvSystem;
    pd.display_name = "Television System";
    pd.description =
        "Television system the capture was made from. Fixes the output frame "
        "geometry and amplitude domain.";
    pd.type = ParameterType::STRING;
    pd.constraints.default_value = std::string(kTvSystemPAL);
    pd.constraints.allowed_strings = {kTvSystemPAL};
    pd.constraints.depends_on = custom_only;
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamTeletextSystem;
    pd.display_name = "Teletext System";
    pd.description =
        "Data service the captured lines carry. Only WST (System B, 625 "
        "lines) can currently be placed; NABTS is refused with a clear error "
        "rather than decoded as the wrong service.";
    pd.type = ParameterType::STRING;
    pd.constraints.default_value = std::string(kTeletextWST);
    pd.constraints.allowed_strings = {kTeletextWST, kTeletextNABTS};
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamCaptureOffsetMode;
    pd.display_name = "Capture Offset";
    pd.description =
        "How the time from 0H to sample 0 of each record is established. "
        "'auto' fits it from the clock run-in of the captured lines, which is "
        "what a card capture needs: its documented offset is unreliable "
        "hardware folklore. 'manual' applies a configured figure unchanged.";
    pd.type = ParameterType::STRING;
    pd.constraints.default_value = std::string(kCaptureOffsetAuto);
    pd.constraints.allowed_strings = {kCaptureOffsetAuto, kCaptureOffsetManual};
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamCaptureOffsetSamples;
    pd.display_name = "Capture Offset (samples)";
    pd.description =
        "Time from 0H to sample 0 of each record, in source samples. Applied "
        "globally, never per line.";
    pd.type = ParameterType::DOUBLE;
    pd.constraints.default_value = 0.0;
    pd.constraints.depends_on = ParameterDependency{
        kParamCaptureOffsetMode, {kCaptureOffsetManual}, true};
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamLevels;
    pd.display_name = "Level Mapping";
    pd.description =
        "How the logic levels of a card capture are established. 'per-line' "
        "follows fast gain changes; 'rolling' holds lines at the frame's "
        "median except where they deviate significantly; 'fixed' applies "
        "configured levels and measures nothing.";
    pd.type = ParameterType::STRING;
    pd.constraints.default_value = std::string(kLevelsPerLine);
    pd.constraints.allowed_strings = {kLevelsPerLine, kLevelsRolling,
                                      kLevelsFixed};
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamFixedLogic0;
    pd.display_name = "Fixed Logic 0 Level";
    pd.description = "Source-domain level taken as logic 0 in 'fixed' mode.";
    pd.type = ParameterType::DOUBLE;
    pd.constraints.default_value = 0.0;
    pd.constraints.depends_on =
        ParameterDependency{kParamLevels, {kLevelsFixed}, true};
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamFixedLogic1;
    pd.display_name = "Fixed Logic 1 Level";
    pd.description = "Source-domain level taken as logic 1 in 'fixed' mode.";
    pd.type = ParameterType::DOUBLE;
    pd.constraints.default_value = 255.0;
    pd.constraints.depends_on =
        ParameterDependency{kParamLevels, {kLevelsFixed}, true};
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamFirstField;
    pd.display_name = "First Stored Field";
    pd.description =
        "Television field the first stored field of each frame carries. A "
        "driver convention rather than recorded information, so it is "
        "configuration; getting it wrong swaps the two line ranges.";
    pd.type = ParameterType::UINT32;
    pd.constraints.default_value = uint32_t{1};
    pd.constraints.min_value = uint32_t{1};
    pd.constraints.max_value = uint32_t{2};
    descriptors.push_back(pd);
  }

  {
    ParameterDescriptor pd;
    pd.name = kParamDrops;
    pd.display_name = "Dropped Frames";
    pd.description =
        "What to do about frames the capture dropped, which the bt8x8 frame "
        "counter makes visible. 'preserve' emits only the frames present; "
        "'pad' synthesises blank frames so output frame n stays aligned with "
        "source frame n and the colour sequence survives.";
    pd.type = ParameterType::STRING;
    pd.constraints.default_value = std::string(kDropsPreserve);
    pd.constraints.allowed_strings = {kDropsPreserve, kDropsPad};
    descriptors.push_back(pd);
  }

  return descriptors;
}

std::map<std::string, ParameterValue> VBISourceStage::get_parameters() const {
  return {
      {kParamInputPath, configuration_.input_path},
      {kParamFormat, configuration_.format_preset},
      {kParamSampleRate, configuration_.container_sample_rate_hz},
      {kParamLineLength, configuration_.container_line_length},
      {kParamValidSamples, configuration_.container_valid_samples},
      {kParamSampleFormat, configuration_.container_sample_format},
      {kParamFieldLines, configuration_.container_field_lines},
      {kParamFirstRecord, configuration_.container_first_record},
      {kParamLastRecord, configuration_.container_last_record},
      {kParamFrameTrailerBytes, configuration_.container_frame_trailer_bytes},
      {kParamContainerTvSystem, configuration_.container_tv_system},
      {kParamTeletextSystem, configuration_.teletext_system},
      {kParamCaptureOffsetMode, configuration_.capture_offset_mode},
      {kParamCaptureOffsetSamples, configuration_.capture_offset_samples},
      {kParamLevels, configuration_.levels},
      {kParamFixedLogic0, configuration_.fixed_logic0},
      {kParamFixedLogic1, configuration_.fixed_logic1},
      {kParamFirstField, configuration_.first_field},
      {kParamDrops, configuration_.drops},
  };
}

bool VBISourceStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& [key, value] : params) {
    if (key == kParamInputPath || key == kParamFormat ||
        key == kParamSampleFormat || key == kParamContainerTvSystem ||
        key == kParamTeletextSystem || key == kParamCaptureOffsetMode ||
        key == kParamLevels || key == kParamDrops) {
      if (!std::holds_alternative<std::string>(value)) {
        ORC_LOG_WARN("{}: parameter '{}' expects a string", kStageName, key);
        return false;
      }
    } else if (key == kParamSampleRate || key == kParamCaptureOffsetSamples ||
               key == kParamFixedLogic0 || key == kParamFixedLogic1) {
      if (!std::holds_alternative<double>(value)) {
        ORC_LOG_WARN("{}: parameter '{}' expects a number", kStageName, key);
        return false;
      }
    } else if (key == kParamLineLength || key == kParamValidSamples ||
               key == kParamFieldLines || key == kParamFirstRecord ||
               key == kParamLastRecord || key == kParamFrameTrailerBytes ||
               key == kParamFirstField) {
      if (!std::holds_alternative<uint32_t>(value) &&
          !std::holds_alternative<int32_t>(value)) {
        ORC_LOG_WARN("{}: parameter '{}' expects a whole number", kStageName,
                     key);
        return false;
      }
    } else {
      ORC_LOG_WARN("{}: unknown parameter '{}'", kStageName, key);
      return false;
    }
  }

  configuration_ = configuration_from(params);

  if (configuration_.input_path.empty()) {
    set_configuration_status(ConfigurationStatus::Red);
    return true;
  }

  std::string error;
  if (!deps_->validate_input_file(configuration_.input_path, error)) {
    ORC_LOG_WARN("{}: capture not accessible: {}", kStageName, error);
    set_configuration_status(ConfigurationStatus::Red);
    return true;
  }

  // Everything that can be judged without reading the capture: the preset, the
  // data service, and the internal consistency of the container fields.  The
  // stream itself is checked against them at execution.
  VBISourceFormat format;
  if (!make_source_format(configuration_, format, error)) {
    ORC_LOG_WARN("{}: {}", kStageName, error);
    set_configuration_status(ConfigurationStatus::Red);
    return true;
  }

  const std::vector<std::string> violations =
      validate_vbi_source_config(format, std::nullopt, VBITransportHints{});
  if (!violations.empty()) {
    ORC_LOG_WARN("{}: {}", kStageName, join_messages(violations));
    set_configuration_status(ConfigurationStatus::Red);
    return true;
  }

  set_configuration_status(ConfigurationStatus::Green);
  return true;
}

StagePreviewCapability VBISourceStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(
      std::dynamic_pointer_cast<const VideoFrameRepresentation>(
          cached_representation_));
}

}  // namespace orc
