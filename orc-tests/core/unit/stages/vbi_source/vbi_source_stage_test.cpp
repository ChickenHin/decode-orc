/*
 * File:        vbi_source_stage_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the raw VBI capture source stage
 *
 * Every capture is rendered in memory and reaches the stage through the
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
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "vbi-services/teletext_slicer.h"
#include "vbi_output_frame.h"
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
  EXPECT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", format, error))
      << error;
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
    auto source = std::make_unique<FakeByteSource>(bytes_);
    source->declared_bits = declared_bits;
    return source;
  }

  bool file_valid = true;
  bool open_succeeds = true;
  // What the transport declares: the one FLAC header field that is meaningful.
  uint32_t declared_bits = 8u;
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

TEST(VBISourceStageIdentity, IsASourceForEitherSystemWithNoInputsAndOneOutput) {
  VBISourceStage stage;
  const NodeTypeInfo info = stage.get_node_type_info();

  EXPECT_EQ(info.stage_name, "vbi_source");
  EXPECT_EQ(info.type, NodeType::SOURCE);
  EXPECT_EQ(info.compatible_formats, VideoFormatCompatibility::ALL);
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

// A capture's television system fixes the geometry of the frames it is placed
// on, so a project is offered the formats of its own system and nothing else.
// That is the whole of the choice: a PAL project has one, an NTSC project two.
TEST(VBISourceStageParameters, TheFormatsOfferedFollowTheProjectSystem) {
  VBISourceStage stage;

  const auto format_descriptor = [&stage](VideoSystem system) {
    const auto descriptors =
        stage.get_parameter_descriptors(system, SourceType::Unknown);
    const auto it = std::find_if(
        descriptors.begin(), descriptors.end(),
        [](const ParameterDescriptor& d) { return d.name == "format"; });
    EXPECT_NE(it, descriptors.end());
    return *it;
  };

  const ParameterDescriptor pal = format_descriptor(VideoSystem::PAL);
  EXPECT_EQ(pal.constraints.allowed_strings,
            vbi_source_preset_names(VBITVSystem::kPAL));
  EXPECT_EQ(pal.constraints.default_value,
            ParameterValue(std::string("bt8x8 card dump, 8-bit (WST)")));

  const ParameterDescriptor ntsc = format_descriptor(VideoSystem::NTSC);
  EXPECT_EQ(ntsc.constraints.allowed_strings,
            vbi_source_preset_names(VBITVSystem::kNTSC));
  EXPECT_EQ(ntsc.constraints.default_value,
            ParameterValue(std::string(".tbc VBI crop, 16-bit (WST)")));
}

// The preset carries the whole configuration, so there is nothing else for the
// surface to offer: the file, the format and one policy about dropped frames.
TEST(VBISourceStageParameters, TheSurfaceIsThreeParameters) {
  VBISourceStage stage;
  const auto descriptors = stage.get_parameter_descriptors();

  std::vector<std::string> names;
  for (const ParameterDescriptor& descriptor : descriptors) {
    names.push_back(descriptor.name);
    // Nothing is conditional on anything else any more.
    EXPECT_FALSE(descriptor.constraints.depends_on.has_value())
        << descriptor.name;
  }
  EXPECT_EQ(names, (std::vector<std::string>{"input_path", "format", "drops"}));
}

TEST(VBISourceStageParameters, RejectsAnUnknownParameter) {
  VBISourceStage stage;
  EXPECT_FALSE(stage.set_parameters({{"nonsense", std::string("value")}}));
}

TEST(VBISourceStageParameters, RoundTripsTheParametersItIsGiven) {
  VBISourceStage stage;
  ASSERT_TRUE(stage.set_parameters({
      {"input_path", std::string(kCapturePath)},
      {"format", std::string(".tbc VBI crop, 16-bit (NABTS)")},
      {"drops", std::string("pad")},
  }));

  const auto parameters = stage.get_parameters();
  EXPECT_EQ(std::get<std::string>(parameters.at("input_path")), kCapturePath);
  EXPECT_EQ(std::get<std::string>(parameters.at("format")),
            ".tbc VBI crop, 16-bit (NABTS)");
  EXPECT_EQ(std::get<std::string>(parameters.at("drops")), "pad");
}

// Every parameter is a string now, so a caller handing over a number is
// handing over the wrong thing and is told rather than quietly ignored.
TEST(VBISourceStageParameters, RejectsAParameterOfTheWrongType) {
  VBISourceStage stage;
  EXPECT_FALSE(stage.set_parameters({{"format", uint32_t{2}}}));
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

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

TEST(VBISourceStageExecution, ProducesNoOutputUntilACaptureIsConfigured) {
  VBISourceStage stage(std::make_shared<FakeDeps>(std::vector<uint8_t>{}));
  ObservationContext observations;
  EXPECT_TRUE(stage.execute({}, {}, observations).empty());
}

TEST(VBISourceStageExecution, EmitsOneOutputFramePerStoredFrame) {
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

  VBIOutputFrame output;
  std::string error;
  ASSERT_TRUE(make_vbi_output_frame(VBITVSystem::kPAL, VBITeletextSystem::kWST,
                                    output, error))
      << error;
  const VBIOutputLevels& levels = output.levels;

  // How far above blanking a line's data region rises: a teletext line swings
  // to the logic 1 level, a blank one stays at blanking.
  const auto line_peak = [&](size_t line) {
    const std::vector<int16_t> samples =
        representation->get_line_samples(0, line);
    EXPECT_FALSE(samples.empty()) << "line " << line;
    // Start inside the data region, past the back porch.
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

// The stage places data and manufactures nothing: every sample outside the
// data region of a teletext line is exactly the blanking level, with no sync,
// no vertical interval and no colour burst anywhere in the frame.
TEST(VBISourceStageExecution, EverythingOutsideTheDataRegionIsBlanking) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 1));
  VBISourceStage stage(deps);

  const auto representation =
      representation_of(run_stage(stage, default_parameters()));
  ASSERT_NE(representation, nullptr);

  VBIOutputFrame output;
  std::string error;
  ASSERT_TRUE(make_vbi_output_frame(VBITVSystem::kPAL, VBITeletextSystem::kWST,
                                    output, error))
      << error;
  const auto blanking = static_cast<int16_t>(output.levels.blanking);

  const std::vector<int16_t> frame = representation->get_frame_copy(0);
  ASSERT_EQ(frame.size(), static_cast<size_t>(kPalFrameSamples));

  // Every line that carries no data service, sample for sample.
  for (uint32_t line = 0; line < output.lines_per_frame; ++line) {
    const bool data_line =
        (line >= 6u && line <= 21u) || (line >= 319u && line <= 334u);
    if (data_line) continue;
    const size_t begin = output.line_offset(line);
    const size_t length = output.line_length(line);
    for (size_t sample = 0; sample < length; ++sample) {
      ASSERT_EQ(frame[begin + sample], blanking)
          << "line " << line << " sample " << sample;
    }
  }

  // And the head of a data line, ahead of everything the record covers.
  const size_t data_line_begin = output.line_offset(6);
  for (size_t sample = 0; sample < 120u; ++sample) {
    EXPECT_EQ(frame[data_line_begin + sample], blanking) << "sample " << sample;
  }
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
  parameters["drops"] = std::string("pad");
  const ArtifactPtr third = run_stage(stage, parameters);
  EXPECT_NE(first, third);
  EXPECT_EQ(deps->opens, 2u);
}

// ---------------------------------------------------------------------------
// The whole point: what the teletext decoders read back
// ---------------------------------------------------------------------------

// A recognisable T42 packet: magazine 1 row 0, then 40 display bytes carrying
// odd parity, as ETSI EN 300 706 §7.1.2 and §8.1 define them.
std::array<uint8_t, kTeletextPacketBytes> reference_packet() {
  std::array<uint8_t, kTeletextPacketBytes> bytes{};
  bytes[0] = teletext_hamming84_encode(1);  // magazine 1, row 0 low bit
  bytes[1] = teletext_hamming84_encode(0);
  for (size_t index = 2; index < bytes.size(); ++index) {
    auto value = static_cast<uint8_t>(0x20u + ((index * 7u) % 0x5Fu));
    int ones = 0;
    for (int bit = 0; bit < 7; ++bit) {
      if (((value >> bit) & 1u) != 0u) ++ones;
    }
    if ((ones % 2) == 0) value |= 0x80u;  // odd parity over the whole byte
    bytes[index] = value;
  }
  return bytes;
}

// The packet's bits in transmission order: each byte least significant bit
// first (ETSI EN 300 706 §7.1).
std::vector<bool> packet_bits(
    const std::array<uint8_t, kTeletextPacketBytes>& bytes) {
  std::vector<bool> bits;
  bits.reserve(bytes.size() * 8u);
  for (const uint8_t byte : bytes) {
    for (int bit = 0; bit < 8; ++bit) {
      bits.push_back(((byte >> bit) & 1u) != 0u);
    }
  }
  return bits;
}

// A capture every one of whose lines carries |bytes|.
std::vector<uint8_t> make_packet_capture(
    const VBISourceFormat& format,
    const std::array<uint8_t, kTeletextPacketBytes>& bytes) {
  const VBITeletextService service = wst_service();
  std::vector<uint8_t> capture(static_cast<size_t>(format.bytes_per_frame()),
                               0);

  SyntheticVBILine line;
  line.sample_rate_hz = format.sample_rate_hz;
  line.valid_samples = format.valid_samples;
  line.anchor_position_samples =
      service.cri_start_samples(format.sample_rate_hz, kTruthOffsetSamples);
  line.payload = packet_bits(bytes);
  line.noise_amplitude = 0.5;
  const std::vector<double> samples = render_synthetic_vbi_line(line);

  for (uint32_t field = 0; field < 2u; ++field) {
    for (uint32_t index = 0; index < format.field_lines; ++index) {
      const uint64_t offset =
          field * format.bytes_per_field() + index * format.bytes_per_record();
      for (uint32_t sample = 0; sample < format.valid_samples; ++sample) {
        capture[static_cast<size_t>(offset + sample)] = static_cast<uint8_t>(
            std::clamp(std::round(samples[sample]), 0.0, 255.0));
      }
    }
  }
  return capture;
}

// The stage exists so that the teletext decoders see what they would see on a
// native decode.  This is that claim, end to end: a packet put into a raw VBI
// record comes back out of the slicer byte for byte, off the frame the stage
// built, with no sync, no burst and no vertical interval anywhere in it.
TEST(VBISourceStageDecoding, TheTeletextSlicerRecoversThePacketsThatWentIn) {
  const VBISourceFormat format = bt8x8_pal_format();
  const std::array<uint8_t, kTeletextPacketBytes> expected = reference_packet();

  VBISourceStage stage(
      std::make_shared<FakeDeps>(make_packet_capture(format, expected)));
  const auto representation =
      representation_of(run_stage(stage, default_parameters()));
  ASSERT_NE(representation, nullptr);

  // EBU Tech. 3280-E §1.1.1: 4FSC PAL; the bit rate is fixed by ETSI EN
  // 300 706 §5.3.  These are the observer's own slicer settings.
  const TeletextSlicer slicer(kPalSampleRate, kTeletextBitRate);

  for (const size_t line : {size_t{6}, size_t{21}, size_t{319}, size_t{334}}) {
    const std::vector<int16_t> samples =
        representation->get_line_samples(0, line);
    ASSERT_FALSE(samples.empty()) << "line " << line;

    const TeletextLineResult result =
        slicer.slice(samples.data(), samples.size(), kPalBlack, kPalWhite);
    ASSERT_TRUE(result.valid)
        << "line " << line
        << " rejected: " << teletext_reject_reason_name(result.reject_reason);
    EXPECT_EQ(result.bytes, expected) << "line " << line;
  }

  // And a line the capture never carried data on yields nothing, rather than
  // the slicer finding a packet in manufactured signal.
  const std::vector<int16_t> blank = representation->get_line_samples(0, 100);
  ASSERT_FALSE(blank.empty());
  EXPECT_FALSE(
      slicer.slice(blank.data(), blank.size(), kPalBlack, kPalWhite).valid);
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

TEST(VBISourceStageSequence, AContinuousCounterIsReported) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 4));
  VBISourceStage stage(deps);

  ObservationContext observations;
  ASSERT_FALSE(stage.execute({}, default_parameters(), observations).empty());

  const auto summary =
      observations.get(FieldID(0), "vbi_source", "frame_sequence");
  ASSERT_TRUE(summary.has_value());
  EXPECT_NE(std::get<std::string>(*summary).find("continuous"),
            std::string::npos)
      << std::get<std::string>(*summary);
}

TEST(VBISourceStageSequence, PreservingDroppedFramesEmitsOnlyWhatIsPresent) {
  auto deps = std::make_shared<FakeDeps>(make_synthetic_capture(
      bt8x8_pal_format(), 4, kTruthOffsetSamples, 399598u, 1u, 2u));
  VBISourceStage stage(deps);

  ObservationContext observations;
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, default_parameters(), observations);
  ASSERT_FALSE(outputs.empty());

  EXPECT_EQ(representation_of(outputs.front())->frame_count(), 4u);

  const auto summary =
      observations.get(FieldID(0), "vbi_source", "frame_sequence");
  ASSERT_TRUE(summary.has_value());
  EXPECT_NE(std::get<std::string>(*summary).find("2 frame(s) missing"),
            std::string::npos)
      << std::get<std::string>(*summary);
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

  // A padded frame carries no data at all: it is blanking throughout.
  const std::vector<int16_t> padded = representation->get_frame_copy(2);
  ASSERT_EQ(padded.size(), static_cast<size_t>(kPalFrameSamples));
  const auto bounds = std::minmax_element(padded.begin(), padded.end());
  EXPECT_EQ(*bounds.first, kPalBlanking);
  EXPECT_EQ(*bounds.second, kPalBlanking);
}

// ---------------------------------------------------------------------------
// Rejected configurations
// ---------------------------------------------------------------------------

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
  parameters["format"] = std::string("bt8x8-secam");

  ObservationContext observations;
  EXPECT_THROW(stage.execute({}, parameters, observations), UserDataError);
}

// ---------------------------------------------------------------------------
// 525-line captures
// ---------------------------------------------------------------------------

VBISourceFormat tbc_vbi_ntsc_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(
      expand_vbi_source_preset(".tbc VBI crop, 16-bit (WST)", format, error))
      << error;
  return format;
}

// A capture in the shape of the circulating NTSC VBI-only crops: 16-bit
// little-endian words in the decoder's amplitude domain, sixteen whole .tbc
// lines per field, records starting at 0H so the offset is zero.
std::vector<uint8_t> make_synthetic_ntsc_capture(const VBISourceFormat& format,
                                                 uint64_t frame_count) {
  VBITeletextService service;
  std::string error;
  EXPECT_TRUE(vbi_teletext_service(VBITVSystem::kNTSC, VBITeletextSystem::kWST,
                                   service, error))
      << error;

  std::vector<uint8_t> bytes(
      static_cast<size_t>(format.bytes_per_frame() * frame_count), 0);
  const double position = service.cri_start_samples(format.sample_rate_hz, 0.0);

  for (uint64_t frame = 0; frame < frame_count; ++frame) {
    for (uint32_t field = 0; field < 2u; ++field) {
      for (uint32_t index = 0; index < format.field_lines; ++index) {
        SyntheticVBILine line;
        line.sample_rate_hz = format.sample_rate_hz;
        line.valid_samples = format.valid_samples;
        line.bit_rate_hz = service.bit_rate_hz;
        line.payload_bits = service.payload_bytes * 8u;
        line.anchor_position_samples = position;
        line.logic0 = static_cast<double>(kNtscBlack) * 64.0;
        line.logic1 = 624.0 * 64.0;
        line.noise_amplitude = 32.0;
        line.seed =
            static_cast<uint32_t>(frame * 64u + field * 32u + index + 1u);
        // Records outside the data range are the equalising line and the
        // active picture, neither of which carries a data service.
        line.carries_data = index >= format.field_range.start &&
                            index <= format.field_range.end;

        const std::vector<double> samples = render_synthetic_vbi_line(line);
        const uint64_t offset = frame * format.bytes_per_frame() +
                                field * format.bytes_per_field() +
                                index * format.bytes_per_record();
        for (uint32_t sample = 0; sample < format.valid_samples; ++sample) {
          const auto value = static_cast<uint32_t>(
              std::clamp(std::round(samples[sample]), 0.0, 65535.0));
          const size_t at = static_cast<size_t>(offset) + sample * 2u;
          bytes[at] = static_cast<uint8_t>(value & 0xFFu);
          bytes[at + 1u] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
        }
      }
    }
  }
  return bytes;
}

std::map<std::string, ParameterValue> ntsc_parameters() {
  auto parameters = default_parameters();
  parameters["format"] = std::string(".tbc VBI crop, 16-bit (WST)");
  return parameters;
}

TEST(VBISourceStageNTSC, FramesAreNormativelySizedNTSCCVBS) {
  const VBISourceFormat format = tbc_vbi_ntsc_format();
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_ntsc_capture(format, 3));
  deps->declared_bits = 16u;
  VBISourceStage stage(deps);

  const auto representation =
      representation_of(run_stage(stage, ntsc_parameters()));
  ASSERT_NE(representation, nullptr);
  EXPECT_EQ(representation->frame_count(), 3u);

  const auto params = representation->get_video_parameters();
  ASSERT_TRUE(params.has_value());
  EXPECT_EQ(params->system, VideoSystem::NTSC);
  EXPECT_EQ(params->frame_width_nominal, kNtscSamplesPerLine);
  EXPECT_EQ(params->frame_height, kNtscFrameLines);
  EXPECT_EQ(params->blanking_level, kNtscBlanking);

  const auto descriptor = representation->get_frame_descriptor(0);
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->system, VideoSystem::NTSC);
  EXPECT_EQ(descriptor->samples_total, static_cast<size_t>(kNtscFrameSamples));
  EXPECT_EQ(descriptor->samples_per_line_nominal, 910u);
}

// The whole 525-line path: data on broadcast lines 10-21 and 273-284 and
// blanking everywhere else, with every sample inside the legal range.
TEST(VBISourceStageNTSC, DataLandsOnTheStandardLinesAndNowhereElse) {
  const VBISourceFormat format = tbc_vbi_ntsc_format();
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_ntsc_capture(format, 2));
  deps->declared_bits = 16u;
  VBISourceStage stage(deps);

  const auto representation =
      representation_of(run_stage(stage, ntsc_parameters()));
  ASSERT_NE(representation, nullptr);

  VBIOutputFrame output;
  std::string error;
  ASSERT_TRUE(make_vbi_output_frame(VBITVSystem::kNTSC, VBITeletextSystem::kWST,
                                    output, error))
      << error;

  const int16_t* frame = representation->get_frame(0);
  ASSERT_NE(frame, nullptr);

  for (uint32_t line = 0; line < output.lines_per_frame; ++line) {
    const bool data =
        (line >= 9u && line <= 20u) || (line >= 272u && line <= 283u);
    const int16_t* samples = frame + output.line_offset(line);
    bool anything_written = false;
    for (size_t sample = 0; sample < output.line_length(line); ++sample) {
      EXPECT_GE(samples[sample], kVBIOutputSampleMin);
      EXPECT_LE(samples[sample], kVBIOutputSampleMax);
      if (samples[sample] != static_cast<int16_t>(output.levels.blanking)) {
        anything_written = true;
      }
    }
    EXPECT_EQ(anything_written, data) << "line " << line;
  }
}

// Nothing in a capture records which of the two 525-line services it carries,
// so the service is configuration — and choosing it changes the framing code
// that is placed, the packet length, and the amplitude the data sits at.
TEST(VBISourceStageNTSC, NABTSIsPlacedAsItsOwnService) {
  const VBISourceFormat format = tbc_vbi_ntsc_format();
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_ntsc_capture(format, 2));
  deps->declared_bits = 16u;
  VBISourceStage stage(deps);

  auto parameters = ntsc_parameters();
  parameters["format"] = std::string(".tbc VBI crop, 16-bit (NABTS)");

  ObservationContext observations;
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, parameters, observations);
  ASSERT_FALSE(outputs.empty());
  const auto representation = representation_of(outputs.front());
  ASSERT_NE(representation, nullptr);
  EXPECT_EQ(representation->frame_count(), 2u);

  // The configured service reaches the output, so a downstream decoder sees
  // which one it is being handed rather than assuming.
  const auto system =
      observations.get(FieldID(0), "vbi_source", "teletext_system");
  ASSERT_TRUE(system.has_value());
  EXPECT_EQ(std::get<std::string>(*system), "NABTS");

  // NABTS puts logic 0 at blanking where WST puts it at black, so the two
  // place the same records at measurably different amplitudes.
  VBIOutputFrame nabts_frame;
  VBIOutputFrame wst_frame;
  std::string error;
  ASSERT_TRUE(make_vbi_output_frame(
      VBITVSystem::kNTSC, VBITeletextSystem::kNABTS, nabts_frame, error))
      << error;
  ASSERT_TRUE(make_vbi_output_frame(VBITVSystem::kNTSC, VBITeletextSystem::kWST,
                                    wst_frame, error))
      << error;
  EXPECT_NE(nabts_frame.levels.logic0, wst_frame.levels.logic0);

  // And the data still lands on the twelve lines the standard gives it.
  const int16_t* frame = representation->get_frame(0);
  ASSERT_NE(frame, nullptr);
  for (uint32_t line = 0; line < nabts_frame.lines_per_frame; ++line) {
    const bool data =
        (line >= 9u && line <= 20u) || (line >= 272u && line <= 283u);
    const int16_t* samples = frame + nabts_frame.line_offset(line);
    const auto bounds =
        std::minmax_element(samples, samples + nabts_frame.line_length(line));
    const bool written = *bounds.second != nabts_frame.levels.blanking ||
                         *bounds.first != nabts_frame.levels.blanking;
    EXPECT_EQ(written, data) << "line " << line;
  }
}

// A capture ending on an odd field is ordinary; the trailing field is one
// short of a frame and is simply not emitted.
TEST(VBISourceStageNTSC, ACaptureEndingOnAnOddFieldLoadsAndDropsThatField) {
  const VBISourceFormat format = tbc_vbi_ntsc_format();
  std::vector<uint8_t> capture = make_synthetic_ntsc_capture(format, 3);
  capture.resize(capture.size() -
                 static_cast<size_t>(format.bytes_per_field()));

  auto deps = std::make_shared<FakeDeps>(capture);
  deps->declared_bits = 16u;
  VBISourceStage stage(deps);
  const auto representation =
      representation_of(run_stage(stage, ntsc_parameters()));
  ASSERT_NE(representation, nullptr);
  EXPECT_EQ(representation->frame_count(), 2u);
}

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------

TEST(VBISourceStagePreview, AdvertisesNoPreviewUntilACaptureIsLoaded) {
  VBISourceStage stage;
  EXPECT_FALSE(stage.get_preview_capability().is_valid());
}

TEST(VBISourceStagePreview, AdvertisesTheBuiltFramesOnceLoaded) {
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
