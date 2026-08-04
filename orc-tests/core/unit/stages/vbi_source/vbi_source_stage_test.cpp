/*
 * File:        vbi_source_stage_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the raw VBI capture source stage
 *
 * Every capture is synthesised in memory and reaches the stage through the
 * injected dependency interface, so nothing here touches the filesystem.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_source_stage.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/error_types.h>
#include <orc/stage/node_type.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/params/parameter_types.h>
#include <orc/stage/video_frame_representation.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "vbi_frame_geometry.h"
#include "vbi_output_levels.h"
#include "vbi_source_format.h"
#include "vbi_synthetic_line.h"
#include "vbi_teletext_service.h"

namespace orc {
namespace {

using orc::testing::render_synthetic_vbi_line;
using orc::testing::SyntheticVBILine;

// The capture offset every synthetic capture below is built at: the figure
// measured on the reference bt8x8 sample, so calibration has something
// realistic to find.
constexpr double kTruthOffsetSamples = 261.6;

constexpr const char* kCapturePath = "/captures/synthetic.vbi";

VBISourceFormat bt8x8_pal_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(expand_vbi_source_preset("bt8x8-pal", format, error)) << error;
  return format;
}

VBITeletextService wst_service() {
  VBITeletextService service;
  std::string error;
  EXPECT_TRUE(vbi_teletext_service(VBITVSystem::kPAL, VBITeletextSystem::kWST,
                                   service, error))
      << error;
  return service;
}

// ---------------------------------------------------------------------------
// A synthetic capture
// ---------------------------------------------------------------------------

// Build a bt8x8 PAL capture whose teletext lines all sit at one true capture
// offset, with the driver's frame counter written into each frame's trailer.
std::vector<uint8_t> make_synthetic_capture(
    const VBISourceFormat& format, uint64_t frame_count,
    double capture_offset_samples = kTruthOffsetSamples,
    uint32_t first_counter = 399598u, uint64_t dropped_after_frame = UINT64_MAX,
    uint32_t dropped_frames = 0) {
  const VBITeletextService service = wst_service();
  std::vector<uint8_t> bytes(
      static_cast<size_t>(format.bytes_per_frame() * frame_count), 0);

  const double position =
      service.cri_start_samples(format.sample_rate_hz, capture_offset_samples);

  uint32_t counter = first_counter;
  for (uint64_t frame = 0; frame < frame_count; ++frame) {
    for (uint32_t field = 0; field < 2u; ++field) {
      for (uint32_t index = 0; index < format.field_lines; ++index) {
        SyntheticVBILine line;
        line.sample_rate_hz = format.sample_rate_hz;
        line.valid_samples = format.valid_samples;
        line.anchor_position_samples = position;
        line.seed =
            static_cast<uint32_t>(frame * 64u + field * 32u + index + 1u);
        line.noise_amplitude = 0.5;

        const std::vector<double> samples = render_synthetic_vbi_line(line);
        const uint64_t offset = frame * format.bytes_per_frame() +
                                field * format.bytes_per_field() +
                                index * format.bytes_per_record();
        for (uint32_t sample = 0; sample < format.valid_samples; ++sample) {
          const double value =
              std::clamp(std::round(samples[sample]), 0.0, 255.0);
          bytes[static_cast<size_t>(offset + sample)] =
              static_cast<uint8_t>(value);
        }
      }
    }

    // The driver's frame sequence number occupies the last four bytes of the
    // frame, which on PAL are the padding of its final record.
    const size_t trailer =
        static_cast<size_t>((frame + 1u) * format.bytes_per_frame() - 4u);
    for (uint32_t byte = 0; byte < 4u; ++byte) {
      bytes[trailer + byte] =
          static_cast<uint8_t>((counter >> (8u * byte)) & 0xFFu);
    }

    ++counter;
    if (frame == dropped_after_frame) counter += dropped_frames;
  }
  return bytes;
}

// In-memory capture: unit tests never touch the filesystem, and the transport
// presents identical bytes whatever the container.
class FakeByteSource : public IVBIByteSource {
 public:
  explicit FakeByteSource(std::vector<uint8_t> bytes)
      : bytes_(std::move(bytes)) {}

  std::optional<uint64_t> size_bytes() const override {
    return static_cast<uint64_t>(bytes_.size());
  }

  size_t read_at(uint64_t byte_offset, size_t count, uint8_t* out_buffer,
                 std::string& /*error_message*/) override {
    if (byte_offset >= bytes_.size()) return 0;
    const size_t available = bytes_.size() - static_cast<size_t>(byte_offset);
    const size_t produced = std::min(count, available);
    std::memcpy(out_buffer, bytes_.data() + byte_offset, produced);
    return produced;
  }

  std::optional<uint32_t> declared_bits_per_sample() const override {
    return declared_bits;
  }

  std::optional<uint32_t> declared_bits = 8u;

 private:
  std::vector<uint8_t> bytes_;
};

class FakeDeps final : public IVBISourceStageDeps {
 public:
  explicit FakeDeps(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}

  bool validate_input_file(const std::string& /*input_path*/,
                           std::string& error_message) const override {
    if (!file_valid) {
      error_message = "VBI capture is not accessible.";
      return false;
    }
    return true;
  }

  std::unique_ptr<IVBIByteSource> open_byte_source(
      const std::string& /*input_path*/,
      std::string& error_message) const override {
    if (!open_succeeds) {
      error_message = "VBI capture could not be opened.";
      return nullptr;
    }
    ++opens;
    return std::make_unique<FakeByteSource>(bytes_);
  }

  bool file_valid = true;
  bool open_succeeds = true;
  mutable uint32_t opens = 0;

 private:
  std::vector<uint8_t> bytes_;
};

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

std::map<std::string, ParameterValue> default_parameters() {
  return {{"input_path", std::string(kCapturePath)}};
}

// Run the stage over a synthetic capture and return its output.
ArtifactPtr run_stage(VBISourceStage& stage,
                      const std::map<std::string, ParameterValue>& parameters) {
  ObservationContext observations;
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, parameters, observations);
  return outputs.empty() ? nullptr : outputs.front();
}

std::shared_ptr<VideoFrameRepresentation> representation_of(
    const ArtifactPtr& artifact) {
  return std::dynamic_pointer_cast<VideoFrameRepresentation>(artifact);
}

// ---------------------------------------------------------------------------
// Identity and contract
// ---------------------------------------------------------------------------

TEST(VBISourceStageIdentity, IsAPALSourceWithNoInputsAndOneOutput) {
  VBISourceStage stage;
  const NodeTypeInfo info = stage.get_node_type_info();

  EXPECT_EQ(info.stage_name, "vbi_source");
  EXPECT_EQ(info.type, NodeType::SOURCE);
  EXPECT_EQ(info.compatible_formats, VideoFormatCompatibility::PAL_ONLY);
  EXPECT_EQ(stage.required_input_count(), 0u);
  EXPECT_EQ(stage.output_count(), 1u);
}

TEST(VBISourceStageIdentity, RejectsInputArtifacts) {
  VBISourceStage stage;
  ObservationContext observations;
  const std::vector<ArtifactPtr> inputs = {nullptr};
  EXPECT_THROW(stage.execute(inputs, default_parameters(), observations),
               std::runtime_error);
}

TEST(VBISourceStageIdentity, ShipsItsOwnInstructions) {
  VBISourceStage stage;
  // The instructions are read from alongside the plugin at runtime, which a
  // test binary linking the stage directly cannot do; the accessor must still
  // answer rather than crash.
  EXPECT_NO_THROW((void)stage.get_instructions());
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

TEST(VBISourceStageParameters, EveryRuntimeParameterHasADescriptor) {
  VBISourceStage stage;
  const auto descriptors = stage.get_parameter_descriptors();
  const auto parameters = stage.get_parameters();

  for (const auto& entry : parameters) {
    const std::string& name = entry.first;
    const auto it = std::find_if(
        descriptors.begin(), descriptors.end(),
        [&name](const ParameterDescriptor& d) { return d.name == name; });
    EXPECT_NE(it, descriptors.end()) << "no descriptor for '" << name << "'";
  }
}

TEST(VBISourceStageParameters, DefaultsMatchTheDescriptorDefaults) {
  VBISourceStage stage;
  for (const ParameterDescriptor& descriptor :
       stage.get_parameter_descriptors()) {
    ASSERT_TRUE(descriptor.constraints.default_value.has_value())
        << descriptor.name << " has no default";
    VBISourceStage fresh;
    ASSERT_TRUE(fresh.set_parameters(
        {{descriptor.name, *descriptor.constraints.default_value}}))
        << "rejected its own default for '" << descriptor.name << "'";
    const auto applied = fresh.get_parameters();
    const auto it = applied.find(descriptor.name);
    ASSERT_NE(it, applied.end()) << descriptor.name;
    EXPECT_EQ(it->second, *descriptor.constraints.default_value)
        << descriptor.name;
  }
}

TEST(VBISourceStageParameters, OffersEveryKnownContainerPreset) {
  VBISourceStage stage;
  const auto descriptors = stage.get_parameter_descriptors();
  const auto it = std::find_if(
      descriptors.begin(), descriptors.end(),
      [](const ParameterDescriptor& d) { return d.name == "format"; });
  ASSERT_NE(it, descriptors.end());
  EXPECT_EQ(it->constraints.allowed_strings, vbi_source_preset_names());
}

TEST(VBISourceStageParameters, ContainerFieldsDependOnTheCustomPreset) {
  VBISourceStage stage;
  for (const ParameterDescriptor& descriptor :
       stage.get_parameter_descriptors()) {
    if (descriptor.name.rfind("container_", 0) != 0) continue;
    ASSERT_TRUE(descriptor.constraints.depends_on.has_value())
        << descriptor.name;
    EXPECT_EQ(descriptor.constraints.depends_on->parameter_name, "format");
    EXPECT_EQ(descriptor.constraints.depends_on->required_values,
              std::vector<std::string>{"custom"});
  }
}

TEST(VBISourceStageParameters, RejectsAnUnknownParameter) {
  VBISourceStage stage;
  EXPECT_FALSE(stage.set_parameters({{"nonsense", std::string("value")}}));
}

TEST(VBISourceStageParameters, RoundTripsTheParametersItIsGiven) {
  VBISourceStage stage;
  ASSERT_TRUE(stage.set_parameters({
      {"input_path", std::string(kCapturePath)},
      {"drops", std::string("pad")},
      {"synthesise_burst", false},
      {"first_field", uint32_t{2}},
  }));

  const auto parameters = stage.get_parameters();
  EXPECT_EQ(std::get<std::string>(parameters.at("drops")), "pad");
  EXPECT_FALSE(std::get<bool>(parameters.at("synthesise_burst")));
  EXPECT_EQ(std::get<uint32_t>(parameters.at("first_field")), 2u);
}

// ---------------------------------------------------------------------------
// Configuration status
// ---------------------------------------------------------------------------

TEST(VBISourceStageStatus, IsRedUntilACaptureIsConfigured) {
  VBISourceStage stage;
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Red);
}

TEST(VBISourceStageStatus, IsGreenForAnAccessibleCaptureAndAKnownPreset) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 2));
  VBISourceStage stage(deps);

  ASSERT_TRUE(stage.set_parameters(default_parameters()));
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Green);
}

TEST(VBISourceStageStatus, IsRedWhenTheCaptureIsNotAccessible) {
  auto deps = std::make_shared<FakeDeps>(std::vector<uint8_t>{});
  deps->file_valid = false;
  VBISourceStage stage(deps);

  ASSERT_TRUE(stage.set_parameters(default_parameters()));
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Red);
}

TEST(VBISourceStageStatus, IsRedForAnIncompletelySpelledCustomContainer) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 2));
  VBISourceStage stage(deps);

  ASSERT_TRUE(stage.set_parameters({
      {"input_path", std::string(kCapturePath)},
      {"format", std::string("custom")},
  }));
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Red);
}

TEST(VBISourceStageStatus, IsRedForADataServiceThatCannotBePlaced) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 2));
  VBISourceStage stage(deps);

  ASSERT_TRUE(stage.set_parameters({
      {"input_path", std::string(kCapturePath)},
      {"teletext_system", std::string("NABTS")},
  }));
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Red);
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

TEST(VBISourceStageExecution, ProducesNoOutputUntilACaptureIsConfigured) {
  VBISourceStage stage(std::make_shared<FakeDeps>(std::vector<uint8_t>{}));
  ObservationContext observations;
  EXPECT_TRUE(stage.execute({}, {}, observations).empty());
}

TEST(VBISourceStageExecution, SynthesisesOneOutputFramePerStoredFrame) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 3));
  VBISourceStage stage(deps);

  const auto representation =
      representation_of(run_stage(stage, default_parameters()));
  ASSERT_NE(representation, nullptr);

  EXPECT_EQ(representation->frame_count(), 3u);
  EXPECT_TRUE(representation->has_frame(2));
  EXPECT_FALSE(representation->has_frame(3));
}

TEST(VBISourceStageExecution, FramesAreNormativelySizedPALCVBS) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 1));
  VBISourceStage stage(deps);

  const auto representation =
      representation_of(run_stage(stage, default_parameters()));
  ASSERT_NE(representation, nullptr);

  const auto parameters = representation->get_video_parameters();
  ASSERT_TRUE(parameters.has_value());
  EXPECT_EQ(parameters->system, VideoSystem::PAL);
  EXPECT_EQ(parameters->frame_height, kPalFrameLines);
  EXPECT_EQ(parameters->frame_width_nominal, kPalSamplesPerLineNominal);
  EXPECT_EQ(parameters->blanking_level, kPalBlanking);

  const auto descriptor = representation->get_frame_descriptor(0);
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->samples_total, static_cast<size_t>(kPalFrameSamples));

  EXPECT_EQ(representation->get_frame_copy(0).size(),
            static_cast<size_t>(kPalFrameSamples));
}

// The sample encoding reserves the extremes of the 10-bit word; nothing the
// stage writes may land in either protected range (design §2.2).
TEST(VBISourceStageExecution, EverySampleIsInsideTheLegalRange) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 1));
  VBISourceStage stage(deps);

  const auto representation =
      representation_of(run_stage(stage, default_parameters()));
  ASSERT_NE(representation, nullptr);

  const std::vector<int16_t> frame = representation->get_frame_copy(0);
  ASSERT_FALSE(frame.empty());
  const auto bounds = std::minmax_element(frame.begin(), frame.end());
  EXPECT_GE(*bounds.first, kVBIOutputSampleMin);
  EXPECT_LE(*bounds.second, kVBIOutputSampleMax);
}

// The teletext lines of a 625-line WST capture are broadcast frame lines 7-22
// and 320-335, which are stored frame lines 6-21 and 319-334 (design §5.1).
TEST(VBISourceStageExecution, DataLandsOnTheTeletextLinesAndNowhereElse) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 1));
  VBISourceStage stage(deps);

  const auto representation =
      representation_of(run_stage(stage, default_parameters()));
  ASSERT_NE(representation, nullptr);

  VBIOutputLevels levels;
  std::string error;
  ASSERT_TRUE(vbi_output_levels(VBITVSystem::kPAL, levels, error)) << error;

  // How far above blanking a line's data region rises: a teletext line swings
  // to the logic 1 level, a blank one stays at blanking.
  const auto line_peak = [&](size_t line) {
    const std::vector<int16_t> samples =
        representation->get_line_samples(0, line);
    EXPECT_FALSE(samples.empty()) << "line " << line;
    // Skip the sync and burst regions at the head of the line.
    const size_t data_begin = std::min<size_t>(300, samples.size());
    return *std::max_element(samples.begin() + data_begin, samples.end());
  };

  const int16_t threshold =
      static_cast<int16_t>((levels.blanking + levels.logic1) / 2);

  for (const size_t line : {size_t{6}, size_t{21}, size_t{319}, size_t{334}}) {
    EXPECT_GT(line_peak(line), threshold) << "teletext line " << line;
  }
  for (const size_t line : {size_t{5}, size_t{22}, size_t{318}, size_t{335},
                            size_t{100}, size_t{400}}) {
    EXPECT_LT(line_peak(line), threshold) << "non-teletext line " << line;
  }
}

TEST(VBISourceStageExecution, SwitchingTheBurstOffLeavesTheBurstWindowBlank) {
  const auto capture = make_synthetic_capture(bt8x8_pal_format(), 1);

  VBISourceStage with_burst(std::make_shared<FakeDeps>(capture));
  auto parameters = default_parameters();
  const auto burst_frame =
      representation_of(run_stage(with_burst, parameters))->get_frame_copy(0);

  VBISourceStage without_burst(std::make_shared<FakeDeps>(capture));
  parameters["synthesise_burst"] = false;
  const auto blank_frame =
      representation_of(run_stage(without_burst, parameters))
          ->get_frame_copy(0);

  ASSERT_EQ(burst_frame.size(), blank_frame.size());
  EXPECT_NE(burst_frame, blank_frame);
}

TEST(VBISourceStageExecution, RepeatedExecutionReusesTheOpenedCapture) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 2));
  VBISourceStage stage(deps);

  const ArtifactPtr first = run_stage(stage, default_parameters());
  const ArtifactPtr second = run_stage(stage, default_parameters());
  EXPECT_EQ(first, second);
  EXPECT_EQ(deps->opens, 1u);

  // A changed configuration is a different capture and is re-read.
  auto parameters = default_parameters();
  parameters["synthesise_burst"] = false;
  const ArtifactPtr third = run_stage(stage, parameters);
  EXPECT_NE(first, third);
  EXPECT_EQ(deps->opens, 2u);
}

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------

TEST(VBISourceStageCalibration, FitsTheCapturesOwnOffsetAndReportsIt) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 4));
  VBISourceStage stage(deps);

  ObservationContext observations;
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, default_parameters(), observations);
  ASSERT_FALSE(outputs.empty());

  const auto offset =
      observations.get(FieldID(0), "vbi_source", "capture_offset");
  ASSERT_TRUE(offset.has_value());
  EXPECT_NEAR(std::get<double>(*offset), kTruthOffsetSamples, 0.5);
}

TEST(VBISourceStageCalibration, AConfiguredOffsetIsAppliedUnchanged) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 2));
  VBISourceStage stage(deps);

  auto parameters = default_parameters();
  parameters["capture_offset_mode"] = std::string("manual");
  parameters["capture_offset_samples"] = kTruthOffsetSamples;

  ObservationContext observations;
  ASSERT_FALSE(stage.execute({}, parameters, observations).empty());

  // Nothing was measured, so nothing is reported.
  EXPECT_FALSE(
      observations.get(FieldID(0), "vbi_source", "capture_offset").has_value());
}

// A capture whose teletext cannot be located is not decoded with a guessed
// offset: it is refused (design §5.3.4).
TEST(VBISourceStageCalibration, RefusesACaptureItCannotLockTo) {
  const VBISourceFormat format = bt8x8_pal_format();
  auto deps = std::make_shared<FakeDeps>(std::vector<uint8_t>(
      static_cast<size_t>(format.bytes_per_frame() * 2u), 0u));
  VBISourceStage stage(deps);

  ObservationContext observations;
  EXPECT_THROW(stage.execute({}, default_parameters(), observations),
               UserDataError);
}

// ---------------------------------------------------------------------------
// Frame sequence
// ---------------------------------------------------------------------------

TEST(VBISourceStageSequence, AContinuousCounterGivesALockedSignalState) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 4));
  VBISourceStage stage(deps);

  ObservationContext observations;
  ASSERT_FALSE(stage.execute({}, default_parameters(), observations).empty());

  const auto state = observations.get(FieldID(0), "vbi_source", "signal_state");
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(std::get<std::string>(*state), "STANDARD_TBC_LOCKED");
}

TEST(VBISourceStageSequence, PreservingDroppedFramesDowngradesTheSignalState) {
  auto deps = std::make_shared<FakeDeps>(make_synthetic_capture(
      bt8x8_pal_format(), 4, kTruthOffsetSamples, 399598u, 1u, 2u));
  VBISourceStage stage(deps);

  ObservationContext observations;
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, default_parameters(), observations);
  ASSERT_FALSE(outputs.empty());

  const auto state = observations.get(FieldID(0), "vbi_source", "signal_state");
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(std::get<std::string>(*state), "STANDARD_TBC_UNLOCKED");
  EXPECT_EQ(representation_of(outputs.front())->frame_count(), 4u);
}

TEST(VBISourceStageSequence, PaddingKeepsTheOutputAlignedWithTheSource) {
  auto deps = std::make_shared<FakeDeps>(make_synthetic_capture(
      bt8x8_pal_format(), 4, kTruthOffsetSamples, 399598u, 1u, 2u));
  VBISourceStage stage(deps);

  auto parameters = default_parameters();
  parameters["drops"] = std::string("pad");

  ObservationContext observations;
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, parameters, observations);
  ASSERT_FALSE(outputs.empty());

  const auto representation = representation_of(outputs.front());
  ASSERT_NE(representation, nullptr);
  EXPECT_EQ(representation->frame_count(), 6u);

  // The padded frames carry no data, and every frame is still a structurally
  // valid CVBS frame.
  const std::vector<int16_t> padded = representation->get_frame_copy(2);
  EXPECT_EQ(padded.size(), static_cast<size_t>(kPalFrameSamples));

  const auto bounds = std::minmax_element(padded.begin(), padded.end());
  EXPECT_GE(*bounds.first, kVBIOutputSampleMin);
  EXPECT_LE(*bounds.second, kVBIOutputSampleMax);

  const auto state = observations.get(FieldID(0), "vbi_source", "signal_state");
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(std::get<std::string>(*state), "STANDARD_TBC_LOCKED");
}

// ---------------------------------------------------------------------------
// Rejected configurations
// ---------------------------------------------------------------------------

TEST(VBISourceStageValidation, RefusesADataServiceItCannotPlace) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 1));
  VBISourceStage stage(deps);

  auto parameters = default_parameters();
  parameters["teletext_system"] = std::string("NABTS");

  ObservationContext observations;
  try {
    stage.execute({}, parameters, observations);
    FAIL() << "NABTS was accepted";
  } catch (const UserDataError& error) {
    EXPECT_NE(std::string(error.what()).find("WST"), std::string::npos)
        << error.what();
  }
}

TEST(VBISourceStageValidation, RefusesACaptureThatIsNotAWholeNumberOfFrames) {
  const VBISourceFormat format = bt8x8_pal_format();
  std::vector<uint8_t> capture = make_synthetic_capture(format, 2);
  capture.resize(capture.size() - 16u);

  VBISourceStage stage(std::make_shared<FakeDeps>(capture));
  ObservationContext observations;
  EXPECT_THROW(stage.execute({}, default_parameters(), observations),
               UserDataError);
}

TEST(VBISourceStageValidation, RefusesACaptureItCannotOpen) {
  auto deps = std::make_shared<FakeDeps>(std::vector<uint8_t>{});
  deps->open_succeeds = false;
  VBISourceStage stage(deps);

  ObservationContext observations;
  EXPECT_THROW(stage.execute({}, default_parameters(), observations),
               UserDataError);
}

TEST(VBISourceStageValidation, RefusesAnUnknownFormatPreset) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 1));
  VBISourceStage stage(deps);

  auto parameters = default_parameters();
  parameters["format"] = std::string("bt8x8-ntsc");

  ObservationContext observations;
  EXPECT_THROW(stage.execute({}, parameters, observations), UserDataError);
}

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------

TEST(VBISourceStagePreview, AdvertisesNoPreviewUntilACaptureIsLoaded) {
  VBISourceStage stage;
  EXPECT_FALSE(stage.get_preview_capability().is_valid());
}

TEST(VBISourceStagePreview, AdvertisesTheSynthesisedFramesOnceLoaded) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 3));
  VBISourceStage stage(deps);
  ASSERT_NE(run_stage(stage, default_parameters()), nullptr);

  const StagePreviewCapability capability = stage.get_preview_capability();
  ASSERT_TRUE(capability.is_valid());
  EXPECT_GT(capability.navigation_extent.item_count, 0u);
}

}  // namespace
}  // namespace orc
