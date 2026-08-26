/*
 * File:        vbi_level_mapper_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for card-capture level estimation and mapping
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * The synthetic lines below are built from the design's timing constants
 * rather than from captured data, so every expectation is traceable to a
 * standard rather than to a sample file.
 */

#include "vbi_level_mapper.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "vbi_output_frame.h"
#include "vbi_source_format.h"
#include "vbi_teletext_service.h"

namespace orc {
namespace {

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

VBIOutputLevels pal_output_levels() {
  VBIOutputFrame frame;
  std::string error;
  EXPECT_TRUE(make_vbi_output_frame(VBITVSystem::kPAL, VBITeletextSystem::kWST,
                                    frame, error))
      << error;
  return frame.levels;
}

// Description of a synthetic WST line in source-domain counts.  The clock
// run-in carries its own pair of levels so that a band-limited source, whose
// run-in has collapsed towards the data midpoint, can be modelled.
struct SyntheticLineSpec {
  double logic0 = 32.0;
  double logic1 = 180.0;
  double cri_low = 32.0;
  double cri_high = 180.0;
  double ripple_counts = 0.0;
  bool carries_teletext = true;
};

// Deterministic sub-count ripple, standing in for capture noise without
// making the test depend on a random generator.
double ripple_at(uint32_t sample_index, double amplitude) {
  if (amplitude <= 0.0) {
    return 0.0;
  }
  const int step = static_cast<int>((sample_index * 37u) % 5u) - 2;
  return (static_cast<double>(step) / 2.0) * amplitude;
}

// The transmitted bit sequence of a WST line: sixteen alternating run-in
// bits, the 0xE4 framing code, then a payload.
std::vector<bool> wst_line_bits(const VBITeletextService& service) {
  std::vector<bool> bits;
  bits.reserve(service.cri_bits + service.frc_bits +
               service.payload_bytes * 8u);

  for (uint32_t bit = 0; bit < service.cri_bits; ++bit) {
    bits.push_back((bit % 2u) == 0u);
  }
  for (uint32_t bit = 0; bit < service.frc_bits; ++bit) {
    const uint32_t shift = service.frc_bits - 1u - bit;
    bits.push_back(((0xE4u >> shift) & 1u) != 0u);
  }
  for (uint32_t bit = 0; bit < service.payload_bytes * 8u; ++bit) {
    // A run-rich payload, so the data region is not a second run-in.
    bits.push_back(((bit / 3u) % 2u) == 0u);
  }
  return bits;
}

std::vector<double> make_synthetic_line(const VBISourceFormat& format,
                                        const VBITeletextService& service,
                                        const SyntheticLineSpec& spec) {
  const double samples_per_bit = service.samples_per_bit(format.sample_rate_hz);
  const double cri_start = service.cri_start_samples(
      format.sample_rate_hz, format.capture_offset_samples);
  const std::vector<bool> bits = wst_line_bits(service);

  std::vector<double> samples;
  samples.reserve(format.valid_samples);
  for (uint32_t index = 0; index < format.valid_samples; ++index) {
    double value = spec.logic0;

    if (spec.carries_teletext) {
      const double bit_position =
          (static_cast<double>(index) - cri_start) / samples_per_bit;
      if (bit_position >= 0.0) {
        const size_t bit_index = static_cast<size_t>(bit_position);
        if (bit_index < bits.size()) {
          const bool is_run_in = bit_index < service.cri_bits;
          const double high = is_run_in ? spec.cri_high : spec.logic1;
          const double low = is_run_in ? spec.cri_low : spec.logic0;
          value = bits[bit_index] ? high : low;
        }
      }
    }

    samples.push_back(value + ripple_at(index, spec.ripple_counts));
  }
  return samples;
}

// Mean of a half-open window of a record after the mapper's levels are applied
// to it, which is what the frame builder does with them.
double mapped_window_mean(const std::vector<double>& samples,
                          const VBILineLevels& levels, uint32_t begin,
                          uint32_t end) {
  const VBISampleMap map = make_vbi_sample_map(levels, pal_output_levels());
  double total = 0.0;
  uint32_t count = 0;
  for (uint32_t index = begin; index < end && index < samples.size(); ++index) {
    total += map.apply(samples[index]);
    ++count;
  }
  return (count > 0) ? (total / count) : 0.0;
}

std::vector<VBILineRecord> make_frame_records(
    const std::vector<std::vector<double>>& lines) {
  std::vector<VBILineRecord> records;
  records.reserve(lines.size());
  for (size_t index = 0; index < lines.size(); ++index) {
    VBILineRecord record;
    record.frame_index = 0;
    record.field_index = (index < lines.size() / 2) ? 0u : 1u;
    record.record_index = static_cast<uint32_t>(
        index % (lines.size() / 2 > 0 ? lines.size() / 2 : 1));
    record.samples = lines[index];
    records.push_back(record);
  }
  return records;
}

// ---------------------------------------------------------------------------
// Service timing
// ---------------------------------------------------------------------------

TEST(VBITeletextServiceTable, WSTTimingMatchesTheServiceTable) {
  const VBITeletextService service = wst_service();

  EXPECT_DOUBLE_EQ(service.t_offset_ns, 10300.0);
  EXPECT_DOUBLE_EQ(service.bit_rate_hz, 6937500.0);
  EXPECT_EQ(service.cri_bits, 16u);
  EXPECT_EQ(service.frc_bits, 8u);
  EXPECT_EQ(service.frc_leading_ones, 3u);
  EXPECT_EQ(service.cri_frc_pattern, 0xAAAAE4u);
  EXPECT_EQ(service.payload_bytes, 42u);

  // 444 x fH, the 625-line frequency being 15 625 Hz.
  EXPECT_DOUBLE_EQ(service.bit_rate_hz, 444.0 * 15625.0);
}

// The record's sample 0 sits capture_offset samples after 0H, so the anchor
// moves back by exactly that much.
TEST(VBITeletextServiceTable, WSTAnchorsRelativeToTheCaptureWindow) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  // 10.3 us at 8 x fsc PAL.
  EXPECT_NEAR(service.cri_start_samples(format.sample_rate_hz, 0.0), 365.33,
              0.01);
  EXPECT_NEAR(service.cri_start_samples(format.sample_rate_hz, 244.0), 121.33,
              0.01);
  EXPECT_NEAR(service.samples_per_bit(format.sample_rate_hz), 5.1126, 0.0005);
}

// ITU-R BT.653 Table 3: System B on 525 lines keeps the 625-line run-in and
// framing code but not its bit rate or packet length, which is why a 625-line
// slicer cannot read a 525-line line.
TEST(VBITeletextServiceTable, WSTOn525LinesCarriesItsOwnTimings) {
  VBITeletextService service;
  std::string error;

  ASSERT_TRUE(vbi_teletext_service(VBITVSystem::kNTSC, VBITeletextSystem::kWST,
                                   service, error))
      << error;

  // Measured on the circulating captures, not taken from libzvbi's table,
  // which disagrees with them by seven bit periods.
  EXPECT_DOUBLE_EQ(service.t_offset_ns, 9300.0);
  EXPECT_DOUBLE_EQ(service.bit_rate_hz, 5727272.0);
  EXPECT_EQ(service.cri_bits, 16u);
  EXPECT_EQ(service.frc_bits, 8u);
  EXPECT_EQ(service.cri_frc_pattern, 0xAAAAE4u);
  EXPECT_EQ(service.payload_bytes, 34u);

  // At 4 x fsc NTSC that is exactly two and a half samples per bit.
  EXPECT_NEAR(service.samples_per_bit(kNtscSampleRate), 2.5, 1e-6);
}

// EIA-516 System C: NABTS shares the 525-line lines and bit rate with System B
// and differs in framing code and packet length — which is why the framing
// code is the only thing that tells the two apart on a capture.
TEST(VBITeletextServiceTable, NABTSCarriesItsOwnFramingCodeAndPacketLength) {
  VBITeletextService wst;
  VBITeletextService nabts;
  std::string error;

  ASSERT_TRUE(vbi_teletext_service(VBITVSystem::kNTSC, VBITeletextSystem::kWST,
                                   wst, error))
      << error;
  ASSERT_TRUE(vbi_teletext_service(VBITVSystem::kNTSC,
                                   VBITeletextSystem::kNABTS, nabts, error))
      << error;

  EXPECT_EQ(nabts.cri_frc_pattern, 0xAAAAE7u);
  EXPECT_EQ(nabts.payload_bytes, 33u);
  // Measured on the ExtraVision captures; libzvbi tabulates 10 480 ns, which
  // leaves the head of the run-in on the edge of the written window.
  EXPECT_DOUBLE_EQ(nabts.t_offset_ns, 10300.0);

  // What the two share.
  EXPECT_DOUBLE_EQ(nabts.bit_rate_hz, wst.bit_rate_hz);
  EXPECT_EQ(nabts.cri_bits, wst.cri_bits);
  EXPECT_EQ(nabts.frc_bits, wst.frc_bits);

  // And what they do not: the framing code is the discriminator.
  EXPECT_NE(nabts.cri_frc_pattern, wst.cri_frc_pattern);
  EXPECT_NE(nabts.payload_bytes, wst.payload_bytes);
}

// NABTS is a 525-line service and has no line list on 625, so the pairing is
// refused rather than placed somewhere invented.
TEST(VBITeletextServiceTable, NABTSIsNotDefinedOn625Lines) {
  VBITeletextService service;
  std::string error;

  EXPECT_FALSE(vbi_teletext_service(VBITVSystem::kPAL,
                                    VBITeletextSystem::kNABTS, service, error));
  EXPECT_FALSE(error.empty());
}

// ---------------------------------------------------------------------------
// Level reading windows
// ---------------------------------------------------------------------------

// The quiet region is the back porch between the start of the record and the
// run-in: samples 0-117 for bt8x8 PAL, about 3.3 us.
TEST(VBILevelMapper, Bt8x8PALWindowsBracketTheRunInAndFramingCode) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBILevelMapperConfig config;

  const VBIRecordWindows windows =
      vbi_record_windows(format, service, config.quiet_guard_samples);

  EXPECT_EQ(windows.quiet_begin, 0u);
  EXPECT_EQ(windows.quiet_end, 118u);

  // The run-in opens at 121.33 samples and runs for sixteen bits.
  EXPECT_EQ(windows.cri_begin, 122u);
  EXPECT_EQ(windows.cri_end, 203u);

  // The logic 1 reference is the centre bit of the framing code's leading
  // run of three ones, which begins where the run-in ends.
  EXPECT_GE(windows.frc_reference_begin, windows.cri_end);
  EXPECT_GT(windows.frc_reference_end, windows.frc_reference_begin);
  EXPECT_LT(windows.frc_reference_end, 219u);
}

TEST(VBILevelMapper, WindowsStayInsideTheValidSamplesOfARecord) {
  VBISourceFormat format = bt8x8_pal_format();
  format.valid_samples = 150;
  const VBITeletextService service = wst_service();

  const VBIRecordWindows windows = vbi_record_windows(format, service, 3.0);

  EXPECT_LE(windows.quiet_end, 150u);
  EXPECT_LE(windows.cri_end, 150u);
  EXPECT_LE(windows.frc_reference_end, 150u);
}

// ---------------------------------------------------------------------------
// Per-line estimation
// ---------------------------------------------------------------------------

TEST(VBILevelMapper, CleanLineLevelsAreReadFromTheRunInAndTheFramingCode) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIRecordWindows windows = vbi_record_windows(format, service, 3.0);

  SyntheticLineSpec spec;
  spec.logic0 = 32.0;
  spec.logic1 = 180.0;
  spec.cri_low = 32.0;
  spec.cri_high = 180.0;

  const VBILineLevels levels = estimate_vbi_line_levels(
      make_synthetic_line(format, service, spec), windows, 8.0);

  EXPECT_TRUE(levels.usable);
  EXPECT_NEAR(levels.logic0, 32.0, 0.5);
  EXPECT_NEAR(levels.logic1, 180.0, 0.5);
}

TEST(VBILevelMapper, ALineWithNoDataServiceIsNotUsable) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIRecordWindows windows = vbi_record_windows(format, service, 3.0);

  SyntheticLineSpec spec;
  spec.carries_teletext = false;
  spec.ripple_counts = 1.0;

  const VBILineLevels levels = estimate_vbi_line_levels(
      make_synthetic_line(format, service, spec), windows, 8.0);

  EXPECT_FALSE(levels.usable);
  EXPECT_LT(levels.amplitude(), 8.0);
}

// ---------------------------------------------------------------------------
// Mapping into the output domain
// ---------------------------------------------------------------------------

TEST(VBILevelMapper, CleanLineMapsOntoTheDerivedPALLogicLevels) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIRecordWindows windows = vbi_record_windows(format, service, 3.0);

  SyntheticLineSpec spec;
  const std::vector<std::vector<double>> lines(
      2, make_synthetic_line(format, service, spec));

  VBILevelMapper mapper(format, service, VBILevelMapperConfig{});
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 2u);
  ASSERT_TRUE(mapped[0].usable);

  // Logic 0 is read from the back porch and logic 1 from the framing code's
  // leading run of ones.
  EXPECT_NEAR(mapped_window_mean(lines[0], mapped[0], 0, windows.quiet_end),
              256.0, 2.0);
  EXPECT_NEAR(
      mapped_window_mean(lines[0], mapped[0], windows.frc_reference_begin,
                         windows.frc_reference_end),
      644.0, 2.0);
}

TEST(VBILevelMapper, MildCaptureNoiseDoesNotShiftTheMappedLogicLevels) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIRecordWindows windows = vbi_record_windows(format, service, 3.0);

  SyntheticLineSpec spec;
  spec.ripple_counts = 1.0;
  const std::vector<std::vector<double>> lines(
      2, make_synthetic_line(format, service, spec));

  VBILevelMapper mapper(format, service, VBILevelMapperConfig{});
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 2u);
  EXPECT_NEAR(mapped_window_mean(lines[0], mapped[0], 0, windows.quiet_end),
              256.0, 5.0);
  EXPECT_NEAR(
      mapped_window_mean(lines[0], mapped[0], windows.frc_reference_begin,
                         windows.frc_reference_end),
      644.0, 5.0);
}

// The regression test for the under-scale failure mode: on the VHS waveform
// measured in design §5.3.6 the run-in has collapsed to 7 counts peak to peak
// about the data midpoint, while the framing code still swings the full 127.
// Normalising to the run-in would emit data at a fraction of the correct
// amplitude; the framing code must win.
TEST(VBILevelMapper, BandLimitedRunInLosesToTheFramingCodeAndKeepsFullScale) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIRecordWindows windows = vbi_record_windows(format, service, 3.0);

  SyntheticLineSpec spec;
  spec.logic0 = 57.0;
  spec.logic1 = 184.0;   // 127 counts peak to peak, midpoint 120.5
  spec.cri_low = 115.6;  // 7 counts peak to peak, mean 119.1
  spec.cri_high = 122.6;

  const std::vector<double> samples =
      make_synthetic_line(format, service, spec);
  const VBILineLevels levels = estimate_vbi_line_levels(samples, windows, 8.0);

  ASSERT_TRUE(levels.usable);
  EXPECT_NEAR(levels.logic0, 57.0, 0.5);

  // The framing code reaches 184 while the run-in has collapsed to 122.6; the
  // larger of the two is what logic 1 is read from.
  EXPECT_NEAR(levels.logic1, 184.0, 0.5);

  const std::vector<std::vector<double>> lines(2, samples);
  VBILevelMapper mapper(format, service, VBILevelMapperConfig{});
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 2u);
  const double mapped_logic0 =
      mapped_window_mean(samples, mapped[0], 0, windows.quiet_end);
  const double mapped_logic1 =
      mapped_window_mean(samples, mapped[0], windows.frc_reference_begin,
                         windows.frc_reference_end);

  EXPECT_NEAR(mapped_logic0, 256.0, 2.0);
  EXPECT_NEAR(mapped_logic1, 644.0, 2.0);

  // Had the run-in been taken as the logic 1 reference, the gain would have
  // been almost six times too large.
  EXPECT_NEAR(mapped_logic1 - mapped_logic0, 388.0, 4.0);
}

// Nothing is done to the samples but a linear map: a blurred waveform must
// arrive downstream still blurred, because the deconvolving slicer recovers
// data by matching it (design §5.3.6).
TEST(VBILevelMapper, MappingIsLinearAndPreservesTheWaveformShape) {
  VBILineLevels levels;
  levels.logic0 = 40.0;
  levels.logic1 = 160.0;
  levels.usable = true;

  const VBISampleMap map = make_vbi_sample_map(levels, pal_output_levels());

  EXPECT_NEAR(map.apply(40.0), 256.0, 1e-9);
  EXPECT_NEAR(map.apply(160.0), 644.0, 1e-9);

  // Midpoints stay midpoints and the map extends outside the logic levels
  // without any knee: overshoot is dealt with by the output clamp, not here.
  EXPECT_NEAR(map.apply(100.0), 450.0, 1e-9);
  EXPECT_NEAR(map.apply(220.0), 838.0, 1e-9);
  EXPECT_NEAR(map.apply(-20.0), 62.0, 1e-9);
}

// A line with no level reference has no honest scale, so its map produces
// blanking whatever it is given (design §5.3.4).
TEST(VBILevelMapper, AnUnusableMeasurementMapsEverythingToBlanking) {
  const VBIOutputLevels output = pal_output_levels();
  const VBISampleMap map = make_vbi_sample_map(VBILineLevels{}, output);

  EXPECT_DOUBLE_EQ(map.apply(0.0), output.blanking);
  EXPECT_DOUBLE_EQ(map.apply(255.0), output.blanking);
}

// ---------------------------------------------------------------------------
// Mode policy
// ---------------------------------------------------------------------------

// A frame of nominal lines with one line whose gain is slightly off.
std::vector<std::vector<double>> frame_with_gain_outlier(
    const VBISourceFormat& format, const VBITeletextService& service,
    size_t outlier_index, double outlier_logic1) {
  std::vector<std::vector<double>> lines;
  for (size_t index = 0; index < 32; ++index) {
    SyntheticLineSpec spec;
    if (index == outlier_index) {
      spec.logic1 = outlier_logic1;
      spec.cri_high = outlier_logic1;
    }
    lines.push_back(make_synthetic_line(format, service, spec));
  }
  return lines;
}

// Per-line normalisation follows every line's own gain, which is what carries
// per-line estimation noise into the output.
TEST(VBILevelMapper, PerLineModeNormalisesEachLineByItsOwnEstimate) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIRecordWindows windows = vbi_record_windows(format, service, 3.0);

  VBILevelMapperConfig config;
  config.mode = VBILevelMode::kPerLine;

  const std::vector<std::vector<double>> lines =
      frame_with_gain_outlier(format, service, 7, 188.0);

  VBILevelMapper mapper(format, service, config);
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 32u);
  EXPECT_NEAR(mapped[7].logic1, 188.0, 0.5);

  // The line's own gain takes it back to the nominal logic 1.
  EXPECT_NEAR(
      mapped_window_mean(lines[7], mapped[7], windows.frc_reference_begin,
                         windows.frc_reference_end),
      644.0, 2.0);
}

// Rolling mode holds a line at the frame's median levels, so a single line's
// gain noise does not reach the output as a gain change.
TEST(VBILevelMapper, RollingModeDoesNotPropagateSingleLineGainNoise) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIRecordWindows windows = vbi_record_windows(format, service, 3.0);

  VBILevelMapperConfig config;
  config.mode = VBILevelMode::kRolling;

  const std::vector<std::vector<double>> lines =
      frame_with_gain_outlier(format, service, 7, 188.0);
  const VBIRecordWindows measured_windows =
      vbi_record_windows(format, service, 3.0);

  // The outlier really is at 188 when it is measured on its own.
  EXPECT_NEAR(estimate_vbi_line_levels(lines[7], measured_windows, 8.0).logic1,
              188.0, 0.5);

  VBILevelMapper mapper(format, service, config);
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 32u);

  // Every line in the frame is mapped through the same gain, so the outlier
  // remains visible as signal rather than being normalised away.
  EXPECT_NEAR(mapped[7].logic1, 180.0, 0.5);
  for (const VBILineLevels& line : mapped) {
    EXPECT_NEAR(line.logic1, mapped[0].logic1, 1e-9);
    EXPECT_NEAR(line.logic0, mapped[0].logic0, 1e-9);
  }
  EXPECT_GT(mapped_window_mean(lines[7], mapped[7], windows.frc_reference_begin,
                               windows.frc_reference_end),
            644.0);
}

// A real gain change is larger than estimation noise and is still tracked.
TEST(VBILevelMapper, RollingModeCorrectsALineThatDeviatesSignificantly) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIRecordWindows windows = vbi_record_windows(format, service, 3.0);

  VBILevelMapperConfig config;
  config.mode = VBILevelMode::kRolling;

  const std::vector<std::vector<double>> lines =
      frame_with_gain_outlier(format, service, 7, 260.0);

  VBILevelMapper mapper(format, service, config);
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 32u);
  EXPECT_NEAR(mapped[7].logic1, 260.0, 0.5);
  EXPECT_NEAR(
      mapped_window_mean(lines[7], mapped[7], windows.frc_reference_begin,
                         windows.frc_reference_end),
      644.0, 2.0);
}

// A weak line inside a good frame carries no data service; its own estimate
// is noise, so the frame's levels are applied instead of a fabricated gain.
TEST(VBILevelMapper, AWeakLineIsMappedThroughTheFrameLevels) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  std::vector<std::vector<double>> lines;
  for (size_t index = 0; index < 32; ++index) {
    SyntheticLineSpec spec;
    if (index == 3) {
      spec.logic1 = 52.0;  // 20 counts of amplitude against a median of 148
      spec.cri_high = 52.0;
    }
    lines.push_back(make_synthetic_line(format, service, spec));
  }

  VBILevelMapper mapper(format, service, VBILevelMapperConfig{});
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 32u);
  EXPECT_TRUE(mapped[3].usable);
  EXPECT_NEAR(mapped[3].logic1, 180.0, 0.5);
}

// With no level reference anywhere in the frame there is no honest scale to
// apply, so the records become ordinary blanking rather than amplified noise.
TEST(VBILevelMapper, AFrameWithNoDataServiceIsEmittedAsBlanking) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  SyntheticLineSpec spec;
  spec.carries_teletext = false;
  spec.ripple_counts = 1.0;
  const std::vector<std::vector<double>> lines(
      4, make_synthetic_line(format, service, spec));

  VBILevelMapper mapper(format, service, VBILevelMapperConfig{});
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 4u);
  for (const VBILineLevels& line : mapped) {
    EXPECT_FALSE(line.usable);
    const VBISampleMap map = make_vbi_sample_map(line, pal_output_levels());
    for (const double sample : lines[0]) {
      EXPECT_DOUBLE_EQ(map.apply(sample), 256.0);
    }
  }
}

// Fixed mode measures nothing and applies the configured levels to every
// line, whatever the lines themselves contain.
TEST(VBILevelMapper, FixedModeAppliesTheConfiguredLevelsToEveryLine) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIRecordWindows windows = vbi_record_windows(format, service, 3.0);

  VBILevelMapperConfig config;
  config.mode = VBILevelMode::kFixed;
  config.fixed_logic0 = 32.0;
  config.fixed_logic1 = 180.0;

  std::vector<std::vector<double>> lines;
  for (size_t index = 0; index < 4; ++index) {
    SyntheticLineSpec spec;
    spec.carries_teletext = (index % 2u) == 0u;
    lines.push_back(make_synthetic_line(format, service, spec));
  }

  VBILevelMapper mapper(format, service, config);
  std::vector<VBILineLevels> mapped;
  mapper.map_frame(make_frame_records(lines), mapped);

  ASSERT_EQ(mapped.size(), 4u);
  for (const VBILineLevels& line : mapped) {
    EXPECT_TRUE(line.usable);
    EXPECT_DOUBLE_EQ(line.logic0, 32.0);
    EXPECT_DOUBLE_EQ(line.logic1, 180.0);
  }

  EXPECT_NEAR(
      mapped_window_mean(lines[0], mapped[0], windows.frc_reference_begin,
                         windows.frc_reference_end),
      644.0, 2.0);
  // A blank line stays at blanking rather than being stretched to fill the
  // data amplitude.
  EXPECT_NEAR(
      mapped_window_mean(lines[1], mapped[1], windows.frc_reference_begin,
                         windows.frc_reference_end),
      256.0, 2.0);
}

// The mapper holds configuration only, so the same frame maps identically
// whatever order frames are asked for — the property that lets frames be built
// lazily.
TEST(VBILevelMapper, MappingAFrameIsIndependentOfWhatWasMappedBefore) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  VBILevelMapperConfig config;
  config.mode = VBILevelMode::kRolling;
  VBILevelMapper mapper(format, service, config);

  SyntheticLineSpec quiet_spec;
  quiet_spec.carries_teletext = false;
  const std::vector<VBILineRecord> quiet_frame =
      make_frame_records(std::vector<std::vector<double>>(
          4, make_synthetic_line(format, service, quiet_spec)));
  const std::vector<VBILineRecord> data_frame =
      make_frame_records(frame_with_gain_outlier(format, service, 7, 188.0));

  std::vector<VBILineLevels> first;
  mapper.map_frame(data_frame, first);

  std::vector<VBILineLevels> discarded;
  mapper.map_frame(quiet_frame, discarded);

  std::vector<VBILineLevels> second;
  mapper.map_frame(data_frame, second);

  ASSERT_EQ(first.size(), second.size());
  for (size_t index = 0; index < first.size(); ++index) {
    EXPECT_DOUBLE_EQ(first[index].logic0, second[index].logic0)
        << "line " << index;
    EXPECT_DOUBLE_EQ(first[index].logic1, second[index].logic1)
        << "line " << index;
    EXPECT_EQ(first[index].usable, second[index].usable) << "line " << index;
  }
}

// A time-base corrected capture's samples are already in the output's
// amplitude domain, scaled by the ld-decode factor, so the whole mapping is a
// division by that factor and nothing is estimated.
TEST(VBILevelMapper, AbsoluteLevelsMapTheDecoderDomainStraightOntoTheOutput) {
  VBIOutputFrame output;
  std::string error;
  ASSERT_TRUE(make_vbi_output_frame(VBITVSystem::kNTSC, VBITeletextSystem::kWST,
                                    output, error))
      << error;

  const VBILevelMapperConfig config = vbi_absolute_level_config(output.levels);
  EXPECT_EQ(config.mode, VBILevelMode::kFixed);
  EXPECT_DOUBLE_EQ(config.fixed_logic0, output.levels.logic0 * 64.0);
  EXPECT_DOUBLE_EQ(config.fixed_logic1, output.levels.logic1 * 64.0);

  VBILineLevels levels;
  levels.logic0 = config.fixed_logic0;
  levels.logic1 = config.fixed_logic1;
  levels.usable = true;
  const VBISampleMap map = make_vbi_sample_map(levels, output.levels);

  // Every level of the domain, not just the two the map was built from.
  for (const double counts :
       {0.0, 15360.0, 18048.0, 39936.0, 51200.0, 65535.0}) {
    EXPECT_NEAR(map.apply(counts), counts / kTBCSampleScale, 1e-9)
        << "source counts " << counts;
  }
}

}  // namespace
}  // namespace orc
