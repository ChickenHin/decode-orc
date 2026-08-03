/*
 * File:        teletext_observer_test.cpp
 * Module:      orc-tests/core/unit/observers
 * Purpose:     Unit tests for TeletextObserver
 *
 * Covers: observation schema keys and hex payload correctness, key absence
 * for empty lines, PAL-only gating, luma-path selection for YC sources, and
 * statelessness. Frame data is synthesised in memory; no I/O is performed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/teletext_slicer.h>
#include <teletext_observer.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "../support/teletext_line_synthesizer.h"

namespace orc {
namespace tests {
namespace {

// ---------------------------------------------------------------------------
// Minimal in-memory VFR backed by flat sample buffers (composite + optional
// separate luma channel)
// ---------------------------------------------------------------------------

class FlatBufferVFR : public VideoFrameRepresentation {
 public:
  FlatBufferVFR(std::vector<int16_t> composite, SourceParameters params)
      : composite_(std::move(composite)), params_(std::move(params)) {}

  void set_luma(std::vector<int16_t> luma) { luma_ = std::move(luma); }

  FrameIDRange frame_range() const override { return {0, 1}; }
  size_t frame_count() const override { return 1; }
  bool has_frame(FrameID id) const override { return id == FrameID{0}; }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (id != FrameID{0}) return std::nullopt;
    FrameDescriptor d;
    d.frame_id = id;
    d.system = params_.system;
    d.height = static_cast<size_t>(params_.frame_height);
    d.samples_total = composite_.size();
    d.samples_per_line_nominal =
        static_cast<size_t>(params_.frame_width_nominal);
    return d;
  }

  const sample_type* get_frame(FrameID id) const override {
    return (id == FrameID{0}) ? composite_.data() : nullptr;
  }

  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    return (id == FrameID{0}) ? composite_ : std::vector<sample_type>{};
  }

  bool has_separate_channels() const override { return !luma_.empty(); }

  const sample_type* get_line_luma(FrameID id, size_t line) const override {
    if (id != FrameID{0} || luma_.empty()) return nullptr;
    if (line >= static_cast<size_t>(params_.frame_height)) return nullptr;
    return luma_.data() + frame_line_sample_offset(
                              params_.system,
                              static_cast<size_t>(params_.frame_width_nominal),
                              line);
  }

  std::optional<SourceParameters> get_video_parameters() const override {
    return params_;
  }

 private:
  std::vector<int16_t> composite_;
  std::vector<int16_t> luma_;
  SourceParameters params_;
};

SourceParameters make_pal_params() {
  SourceParameters p{};
  p.system = VideoSystem::PAL;
  p.frame_width_nominal = kPalSamplesPerLineNominal;
  p.frame_height = kPalFrameLines;
  p.black_level = kPalBlack;
  p.white_level = kPalWhite;
  return p;
}

SourceParameters make_ntsc_params() {
  SourceParameters p{};
  p.system = VideoSystem::NTSC;
  p.frame_width_nominal = kNtscSamplesPerLine;
  p.frame_height = kNtscFrameLines;
  p.black_level = kNtscBlack;
  p.white_level = kNtscWhite;
  return p;
}

std::vector<int16_t> make_black_pal_frame() {
  return std::vector<int16_t>(static_cast<size_t>(kPalFrameSamples),
                              static_cast<int16_t>(kPalBlack));
}

// Copy a synthesized teletext line into frame-flat line |flat_line|.
void inject_line(std::vector<int16_t>& frame, size_t flat_line,
                 const std::vector<int16_t>& line) {
  const size_t offset = frame_line_sample_offset(
      VideoSystem::PAL, static_cast<size_t>(kPalSamplesPerLineNominal),
      flat_line);
  std::copy(line.begin(), line.end(), frame.begin() + offset);
}

std::optional<std::string> get_string(const ObservationContext& context,
                                      FieldID fid, const std::string& key) {
  const auto value = context.get(fid, "teletext", key);
  if (!value || !std::holds_alternative<std::string>(*value)) {
    return std::nullopt;
  }
  return std::get<std::string>(*value);
}

// ---------------------------------------------------------------------------
// Schema declaration
// ---------------------------------------------------------------------------

TEST(TeletextObserver, ProvidedObservations_DeclareFullCandidateWindow) {
  TeletextObserver observer;
  const auto keys = observer.get_provided_observations();

  std::set<std::string> names;
  for (const auto& key : keys) {
    EXPECT_EQ(key.namespace_, "teletext");
    names.insert(key.name);
  }

  // present + line_count + one t42_<n> per candidate field line 5-21
  // (EN 300 706 §4.1: broadcast lines 6-22 / 318-335).
  EXPECT_EQ(keys.size(), 2u + 17u);
  EXPECT_TRUE(names.count("present"));
  EXPECT_TRUE(names.count("line_count"));
  for (int line = 5; line <= 21; ++line) {
    EXPECT_TRUE(names.count("t42_" + std::to_string(line))) << "line " << line;
  }
}

// ---------------------------------------------------------------------------
// Packet recovery and schema population
// ---------------------------------------------------------------------------

TEST(TeletextObserver, RecoversPacketsOnSelectedLines_BothFields) {
  auto frame = make_black_pal_frame();
  const auto payload_f1 = make_test_payload();
  auto payload_f2 = make_test_payload();
  payload_f2[2] ^= 0xFF;  // distinguishable field-2 payload

  // Field 1, 0-based field line 7 → frame-flat line 7.
  inject_line(frame, 7, synthesize_teletext_line(payload_f1));
  // Field 2, 0-based field line 9 → frame-flat line 313 + 9 = 322.
  inject_line(frame, static_cast<size_t>(kPalField1Lines) + 9,
              synthesize_teletext_line(payload_f2));

  FlatBufferVFR vfr(std::move(frame), make_pal_params());
  ObservationContext context;
  TeletextObserver observer;
  observer.process_frame(vfr, FrameID(0), context);

  // Field 1 (FieldID 0)
  const FieldID f0(0);
  EXPECT_EQ(context.get(f0, "teletext", "present"),
            std::optional<ObservationValue>(true));
  EXPECT_EQ(context.get(f0, "teletext", "line_count"),
            std::optional<ObservationValue>(int32_t{1}));
  const auto hex_f1 = get_string(context, f0, "t42_7");
  ASSERT_TRUE(hex_f1.has_value());
  const auto bytes_f1 = teletext_hex_to_packet(*hex_f1);
  ASSERT_TRUE(bytes_f1.has_value());
  EXPECT_EQ(*bytes_f1, payload_f1);

  // Field 2 (FieldID 1)
  const FieldID f1(1);
  EXPECT_EQ(context.get(f1, "teletext", "present"),
            std::optional<ObservationValue>(true));
  EXPECT_EQ(context.get(f1, "teletext", "line_count"),
            std::optional<ObservationValue>(int32_t{1}));
  const auto hex_f2 = get_string(context, f1, "t42_9");
  ASSERT_TRUE(hex_f2.has_value());
  const auto bytes_f2 = teletext_hex_to_packet(*hex_f2);
  ASSERT_TRUE(bytes_f2.has_value());
  EXPECT_EQ(*bytes_f2, payload_f2);

  // Empty candidate lines yield no t42_<n> keys.
  EXPECT_FALSE(context.has(f0, "teletext", "t42_9"));
  EXPECT_FALSE(context.has(f1, "teletext", "t42_7"));
  for (int line = 5; line <= 21; ++line) {
    if (line == 7) continue;
    EXPECT_FALSE(context.has(f0, "teletext", "t42_" + std::to_string(line)))
        << "line " << line;
  }
}

TEST(TeletextObserver, EmptyFrame_ReportsNotPresentWithZeroCount) {
  FlatBufferVFR vfr(make_black_pal_frame(), make_pal_params());
  ObservationContext context;
  TeletextObserver observer;
  observer.process_frame(vfr, FrameID(0), context);

  for (const uint64_t field : {0u, 1u}) {
    const FieldID fid(field);
    EXPECT_EQ(context.get(fid, "teletext", "present"),
              std::optional<ObservationValue>(false));
    EXPECT_EQ(context.get(fid, "teletext", "line_count"),
              std::optional<ObservationValue>(int32_t{0}));
    EXPECT_EQ(context.get_keys(fid, "teletext").size(), 2u);
  }
}

// ---------------------------------------------------------------------------
// PAL-only gating
// ---------------------------------------------------------------------------

TEST(TeletextObserver, NonPalFrame_ProducesNoObservations) {
  std::vector<int16_t> frame(static_cast<size_t>(kNtscFrameSamples),
                             static_cast<int16_t>(kNtscBlack));
  FlatBufferVFR vfr(std::move(frame), make_ntsc_params());
  ObservationContext context;
  TeletextObserver observer;
  observer.process_frame(vfr, FrameID(0), context);

  for (const uint64_t field : {0u, 1u}) {
    const FieldID fid(field);
    EXPECT_FALSE(context.has(fid, "teletext", "present"));
    EXPECT_TRUE(context.get_namespaces(fid).empty());
  }
}

TEST(TeletextObserver, MissingVideoParameters_ProducesNoObservations) {
  class NoParamsVFR : public FlatBufferVFR {
   public:
    using FlatBufferVFR::FlatBufferVFR;
    std::optional<SourceParameters> get_video_parameters() const override {
      return std::nullopt;
    }
  };
  NoParamsVFR vfr(make_black_pal_frame(), make_pal_params());
  ObservationContext context;
  TeletextObserver observer;
  observer.process_frame(vfr, FrameID(0), context);
  EXPECT_TRUE(context.get_namespaces(FieldID(0)).empty());
}

// ---------------------------------------------------------------------------
// Luma-path selection (YC sources)
// ---------------------------------------------------------------------------

TEST(TeletextObserver, YcSource_SlicesLumaChannel) {
  // Teletext lives only in the separate luma plane; the composite buffer is
  // black. Recovery proves the observer selects get_line_luma() when
  // has_separate_channels() is true.
  const auto payload = make_test_payload();
  auto luma = make_black_pal_frame();
  inject_line(luma, 12, synthesize_teletext_line(payload));

  FlatBufferVFR vfr(make_black_pal_frame(), make_pal_params());
  vfr.set_luma(std::move(luma));

  ObservationContext context;
  TeletextObserver observer;
  observer.process_frame(vfr, FrameID(0), context);

  const auto hex = get_string(context, FieldID(0), "t42_12");
  ASSERT_TRUE(hex.has_value());
  const auto bytes = teletext_hex_to_packet(*hex);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(*bytes, payload);
}

TEST(TeletextObserver, YcSource_IgnoresCompositeChannel) {
  // Teletext in the composite buffer only: a YC source must slice luma (all
  // black) and find nothing.
  auto composite = make_black_pal_frame();
  inject_line(composite, 12, synthesize_teletext_line(make_test_payload()));

  FlatBufferVFR vfr(std::move(composite), make_pal_params());
  vfr.set_luma(make_black_pal_frame());

  ObservationContext context;
  TeletextObserver observer;
  observer.process_frame(vfr, FrameID(0), context);

  EXPECT_EQ(context.get(FieldID(0), "teletext", "present"),
            std::optional<ObservationValue>(false));
  EXPECT_FALSE(context.has(FieldID(0), "teletext", "t42_12"));
}

TEST(TeletextObserver, BandLimitedYcSource_RecoversViaMlseFallback) {
  // The tape case that motivated the automatic detector: a Y/C VHS source
  // whose luma cannot pass the clock run-in. The observer takes no
  // configuration, so this only works if its slicer is set to fall back.
  const auto payload = make_parity_coded_payload();
  TeletextLineSynthOptions synth;
  synth.low_pass_cutoff_hz = 2.8e6;  // consumer VHS luma bandwidth

  auto luma = make_black_pal_frame();
  inject_line(luma, 12, synthesize_teletext_line(payload, synth));

  FlatBufferVFR vfr(make_black_pal_frame(), make_pal_params());
  vfr.set_luma(std::move(luma));

  ObservationContext context;
  TeletextObserver observer;
  observer.process_frame(vfr, FrameID(0), context);

  const auto hex = get_string(context, FieldID(0), "t42_12");
  ASSERT_TRUE(hex.has_value());
  const auto bytes = teletext_hex_to_packet(*hex);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(*bytes, payload);
}

TEST(TeletextObserver, MlseRecoveredPacket_CarriesPerByteConfidence) {
  // The stored observation carries how sure the detector was of each byte, so
  // a consumer combining repeated copies of a row can weight this one
  // (orc/support/teletext_row_squasher.h).
  const auto payload = make_parity_coded_payload();
  TeletextLineSynthOptions synth;
  synth.low_pass_cutoff_hz = 2.8e6;

  auto luma = make_black_pal_frame();
  inject_line(luma, 12, synthesize_teletext_line(payload, synth));

  FlatBufferVFR vfr(make_black_pal_frame(), make_pal_params());
  vfr.set_luma(std::move(luma));

  ObservationContext context;
  TeletextObserver observer;
  observer.process_frame(vfr, FrameID(0), context);

  const auto hex = get_string(context, FieldID(0), "t42_12");
  ASSERT_TRUE(hex.has_value());
  const auto observed = teletext_hex_to_observed_packet(*hex);
  ASSERT_TRUE(observed.has_value());
  EXPECT_EQ(observed->bytes, payload);
  ASSERT_TRUE(observed->has_confidence);
  for (const float value : observed->confidence) {
    EXPECT_GT(value, 0.0F);
    EXPECT_LE(value, 1.0F);
  }
}

TEST(TeletextObserver, DamagedDisplayBytesAreRepairedFromConfidence) {
  // The observer's slicer repairs parity failures by flipping the bit it was
  // least sure of. The fixture is pinned to a noise seed whose unrepaired
  // decode carries four damaged bytes — asserted below, so the test says so
  // rather than passing vacuously if the fixture ever stops being damaged.
  const auto payload = make_parity_coded_payload();
  TeletextLineSynthOptions synth;
  synth.low_pass_cutoff_hz = 2.8e6;
  synth.noise_amplitude = 90;
  synth.noise_seed = 16;
  const auto line = synthesize_teletext_line(payload, synth);

  TeletextSlicerOptions unrepaired;
  unrepaired.detector = TeletextDetector::kAuto;
  const auto as_read =
      TeletextSlicer(kPalSampleRate, kTeletextBitRate, unrepaired)
          .slice(line.data(), line.size(), static_cast<int16_t>(kPalBlack),
                 static_cast<int16_t>(kPalWhite));
  ASSERT_TRUE(as_read.valid);
  int damaged = 0;
  for (size_t i = 2; i < kTeletextPacketBytes; ++i) {
    damaged += teletext_odd_parity_valid(as_read.bytes[i]) ? 0 : 1;
  }
  ASSERT_GT(damaged, 0) << "fixture is no longer damaged";

  auto luma = make_black_pal_frame();
  inject_line(luma, 12, line);
  FlatBufferVFR vfr(make_black_pal_frame(), make_pal_params());
  vfr.set_luma(std::move(luma));

  ObservationContext context;
  TeletextObserver observer;
  observer.process_frame(vfr, FrameID(0), context);

  const auto hex = get_string(context, FieldID(0), "t42_12");
  ASSERT_TRUE(hex.has_value());
  const auto bytes = teletext_hex_to_packet(*hex);
  ASSERT_TRUE(bytes.has_value());
  // Every repaired byte satisfies parity again, and on this line the repairs
  // restore what was transmitted.
  for (size_t i = 2; i < kTeletextPacketBytes; ++i) {
    EXPECT_TRUE(teletext_odd_parity_valid((*bytes)[i])) << "byte " << i;
  }
  EXPECT_EQ(*bytes, payload);
}

TEST(TeletextObserver, ThresholdRecoveredPacket_CarriesNoConfidenceSuffix) {
  // A source the threshold detector handles is decided one sample per bit,
  // with no path metric to measure: the observation says nothing about
  // confidence rather than inventing it, and stays the 84 characters every
  // build has written.
  const auto payload = make_test_payload();
  auto frame = make_black_pal_frame();
  inject_line(frame, 8, synthesize_teletext_line(payload));

  FlatBufferVFR vfr(std::move(frame), make_pal_params());
  ObservationContext context;
  TeletextObserver observer;
  observer.process_frame(vfr, FrameID(0), context);

  const auto hex = get_string(context, FieldID(0), "t42_8");
  ASSERT_TRUE(hex.has_value());
  EXPECT_EQ(hex->size(), kTeletextPacketBytes * 2);
  const auto observed = teletext_hex_to_observed_packet(*hex);
  ASSERT_TRUE(observed.has_value());
  EXPECT_EQ(observed->bytes, payload);
  EXPECT_FALSE(observed->has_confidence);
}

// ---------------------------------------------------------------------------
// Statelessness
// ---------------------------------------------------------------------------

TEST(TeletextObserver, RepeatedProcessing_ProducesIdenticalObservations) {
  auto frame = make_black_pal_frame();
  inject_line(frame, 8, synthesize_teletext_line(make_test_payload()));
  FlatBufferVFR vfr(std::move(frame), make_pal_params());

  TeletextObserver observer;
  ObservationContext first;
  ObservationContext second;
  observer.process_frame(vfr, FrameID(0), first);
  observer.process_frame(vfr, FrameID(0), second);
  // A third pass on the same instance must also match (no accumulated state).
  ObservationContext third;
  observer.process_frame(vfr, FrameID(0), third);

  for (const uint64_t field : {0u, 1u}) {
    const FieldID fid(field);
    EXPECT_EQ(first.get_all_observations(fid),
              second.get_all_observations(fid));
    EXPECT_EQ(first.get_all_observations(fid), third.get_all_observations(fid));
  }
}

}  // namespace
}  // namespace tests
}  // namespace orc
