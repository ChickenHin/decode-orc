/*
 * File:        vbi_line_placement_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for horizontal placement of records on frame lines
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_line_placement.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "vbi_frame_geometry.h"
#include "vbi_line_mapping.h"
#include "vbi_resampler.h"
#include "vbi_source_format.h"
#include "vbi_teletext_service.h"

namespace orc {
namespace {

constexpr double kOutputRateHz = 17734475.0;  // 4 x fsc PAL
constexpr double kPi = 3.14159265358979323846;

// The frame lines at which the 4 x fsc PAL lattice takes up its accumulated
// fraction and holds 1136 samples instead of 1135 (design §2.3).  Placement
// has to be right on these as much as on any other line.
constexpr uint32_t kPALLongLines[] = {0, 156, 312, 468};

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

VBIFrameGeometry pal_geometry() {
  VBIFrameGeometry geometry;
  std::string error;
  EXPECT_TRUE(make_vbi_frame_geometry(VBITVSystem::kPAL, geometry, error))
      << error;
  return geometry;
}

// Every frame line the stage places WST data on, in stored frame-line terms.
std::vector<uint32_t> teletext_frame_lines() {
  VBITeletextLineMap line_map;
  std::string error;
  EXPECT_TRUE(make_vbi_teletext_line_map(
      VBITVSystem::kPAL, VBITeletextSystem::kWST, line_map, error))
      << error;

  std::vector<uint32_t> lines = line_map.field1;
  lines.insert(lines.end(), line_map.field2.begin(), line_map.field2.end());
  return lines;
}

// A record holding one symmetric raised-cosine pulse centred on the given
// source coordinate, and nothing else.  A symmetric pulse is the cleanest
// position marker there is: a linear-phase resampler preserves its centroid
// exactly, so the centroid of the output measures placement directly.
std::vector<double> record_with_marker_at(uint32_t valid_samples,
                                          double centre_position) {
  constexpr double kMarkerHalfWidth = 24.0;

  std::vector<double> record(valid_samples, 0.0);
  for (uint32_t index = 0; index < valid_samples; ++index) {
    const double offset = static_cast<double>(index) - centre_position;
    if (std::abs(offset) < kMarkerHalfWidth) {
      record[index] = 0.5 * (1.0 + std::cos(kPi * offset / kMarkerHalfWidth));
    }
  }
  return record;
}

// Centroid of a resampled line, in output line sample indices.
double marker_position(const std::vector<double>& out_samples,
                       uint32_t output_begin) {
  double weighted_sum = 0.0;
  double weight_sum = 0.0;
  for (size_t index = 0; index < out_samples.size(); ++index) {
    const double weight = out_samples[index];
    weighted_sum += weight * static_cast<double>(output_begin +
                                                 static_cast<uint32_t>(index));
    weight_sum += weight;
  }
  return (weight_sum != 0.0) ? (weighted_sum / weight_sum) : 0.0;
}

TEST(VBILinePlacement, TheRatioIsTheSourceRateOverTheOutputLatticeRate) {
  const VBISourceFormat format = bt8x8_pal_format();
  VBILinePlacement placement;
  std::string error;

  ASSERT_TRUE(make_vbi_line_placement(format, wst_service(), pal_geometry(),
                                      format.capture_offset_samples, 6,
                                      placement, error))
      << error;

  // bt8x8 PAL samples at 8 x fsc onto a 4 x fsc lattice.
  EXPECT_DOUBLE_EQ(placement.source_samples_per_output_sample, 2.0);
}

// The data region begins at the service's 0H offset less the line's own
// sub-sample lattice phase (design §5.2).
TEST(VBILinePlacement, TheDataStartIsTheServiceOffsetLessTheLinePhase) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIFrameGeometry geometry = pal_geometry();

  const double nominal_start = service.t_offset_ns * 1e-9 * kOutputRateHz;
  EXPECT_NEAR(nominal_start, 182.665, 0.001);

  for (const uint32_t frame_line : teletext_frame_lines()) {
    VBILinePlacement placement;
    std::string error;
    ASSERT_TRUE(make_vbi_line_placement(format, service, geometry,
                                        format.capture_offset_samples,
                                        frame_line, placement, error))
        << error;

    EXPECT_NEAR(placement.data_start_samples,
                nominal_start - geometry.line_phase(frame_line), 1e-9)
        << "frame line " << frame_line;
  }
}

// The acceptance criterion for the phase: a marker sitting at the service's
// anchor in the record lands within a tenth of a sample of the anchor's
// output position, on every teletext line.
TEST(VBILinePlacement, MarkedDataLandsAtTheServiceAnchorOnEveryTeletextLine) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIFrameGeometry geometry = pal_geometry();
  const VBIBandLimitedResampler resampler(format.sample_rate_hz /
                                          kOutputRateHz);

  // Source coordinate of the first clock run-in one bit, given the record's
  // own capture offset.
  const double marker_source_position = service.cri_start_samples(
      format.sample_rate_hz, format.capture_offset_samples);
  const std::vector<double> record =
      record_with_marker_at(format.valid_samples, marker_source_position);

  for (const uint32_t frame_line : teletext_frame_lines()) {
    VBILinePlacement placement;
    std::string error;
    ASSERT_TRUE(make_vbi_line_placement(format, service, geometry,
                                        format.capture_offset_samples,
                                        frame_line, placement, error))
        << error;

    std::vector<double> line;
    resample_vbi_line(resampler, record, placement, line);

    EXPECT_NEAR(marker_position(line, placement.output_begin),
                placement.data_start_samples, 0.1)
        << "frame line " << frame_line;
  }
}

// The four lines that hold an extra sample are the ones a constant-stride
// assumption gets wrong, so they are checked explicitly rather than left to
// the teletext line list, which happens not to include any of them.
TEST(VBILinePlacement, MarkedDataLandsAtTheServiceAnchorOnTheLongLines) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIFrameGeometry geometry = pal_geometry();
  const VBIBandLimitedResampler resampler(format.sample_rate_hz /
                                          kOutputRateHz);

  const double marker_source_position = service.cri_start_samples(
      format.sample_rate_hz, format.capture_offset_samples);
  const std::vector<double> record =
      record_with_marker_at(format.valid_samples, marker_source_position);

  for (const uint32_t frame_line : kPALLongLines) {
    ASSERT_EQ(geometry.line_length(frame_line), 1136u)
        << "frame line " << frame_line;

    VBILinePlacement placement;
    std::string error;
    ASSERT_TRUE(make_vbi_line_placement(format, service, geometry,
                                        format.capture_offset_samples,
                                        frame_line, placement, error))
        << error;

    std::vector<double> line;
    resample_vbi_line(resampler, record, placement, line);

    EXPECT_NEAR(marker_position(line, placement.output_begin),
                placement.data_start_samples, 0.1)
        << "frame line " << frame_line;
  }
}

// The calibrated offset is what makes the placement right: a record captured
// with a different offset than the one supplied lands displaced by exactly
// that difference, scaled into output samples.
TEST(VBILinePlacement, AMisstatedCaptureOffsetDisplacesTheDataItPlaces) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIFrameGeometry geometry = pal_geometry();
  const VBIBandLimitedResampler resampler(format.sample_rate_hz /
                                          kOutputRateHz);
  constexpr uint32_t kFrameLine = 6;
  constexpr double kOffsetError = 10.0;

  const double marker_source_position = service.cri_start_samples(
      format.sample_rate_hz, format.capture_offset_samples);
  const std::vector<double> record =
      record_with_marker_at(format.valid_samples, marker_source_position);

  VBILinePlacement placement;
  std::string error;
  ASSERT_TRUE(make_vbi_line_placement(
      format, service, geometry, format.capture_offset_samples + kOffsetError,
      kFrameLine, placement, error))
      << error;

  std::vector<double> line;
  resample_vbi_line(resampler, record, placement, line);

  // Ten source samples is five output samples at 2:1, and believing sample 0
  // of the record sits later after 0H than it does carries everything in the
  // record the same distance later along the line.
  EXPECT_NEAR(marker_position(line, placement.output_begin),
              placement.data_start_samples + kOffsetError / 2.0, 0.1);
}

// Resampling is a linear filter and nothing else.  A source that is already
// band-limited comes back as itself, which is the property the deconvolving
// slicer downstream depends on (design §5.3.6).
TEST(VBILinePlacement, ABandLimitedWaveformIsReproducedRatherThanConditioned) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBIBandLimitedResampler resampler(format.sample_rate_hz /
                                          kOutputRateHz);

  // Three components, the highest at about 4.4 MHz, all comfortably inside
  // the decimation passband.
  const double frequencies[] = {0.021, 0.061, 0.124};  // cycles per sample
  const double amplitudes[] = {1.0, 0.45, 0.2};
  const double phases[] = {0.3, 1.9, 2.7};

  const auto waveform = [&](double position) {
    double value = 0.0;
    for (size_t term = 0; term < 3; ++term) {
      value +=
          amplitudes[term] *
          std::sin(2.0 * kPi * frequencies[term] * position + phases[term]);
    }
    return value;
  };

  std::vector<double> record(format.valid_samples, 0.0);
  for (uint32_t index = 0; index < format.valid_samples; ++index) {
    record[index] = waveform(static_cast<double>(index));
  }

  VBILinePlacement placement;
  std::string error;
  ASSERT_TRUE(make_vbi_line_placement(format, wst_service(), pal_geometry(),
                                      format.capture_offset_samples, 6,
                                      placement, error))
      << error;

  std::vector<double> line;
  resample_vbi_line(resampler, record, placement, line);
  ASSERT_FALSE(line.empty());

  // Skip the ends of the window, where the kernel runs off the record and
  // constant extension takes over.
  constexpr uint32_t kEdgeGuard = 48;
  ASSERT_GT(line.size(), 2u * kEdgeGuard);
  for (size_t index = kEdgeGuard; index + kEdgeGuard < line.size(); ++index) {
    const double position = placement.source_position(
        static_cast<double>(placement.output_begin + index));
    EXPECT_NEAR(line[index], waveform(position), 2e-3) << "sample " << index;
  }
}

// A band-limited source stays band-limited: a clock run-in that reaches the
// stage at a few per cent of full amplitude must come out at a few per cent
// of full amplitude, not be restored, sliced or sharpened (design §5.3.6).
TEST(VBILinePlacement, ABlurredClockRunInKeepsItsCollapsedAmplitude) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIBandLimitedResampler resampler(format.sample_rate_hz /
                                          kOutputRateHz);

  // The VHS waveform measured in design §5.3.6: the run-in has collapsed to
  // about 5.5 % of the data amplitude, leaving only its fundamental at half
  // the bit rate.
  constexpr double kCollapsedAmplitude = 0.055;
  const double cycles_per_sample =
      service.bit_rate_hz / (2.0 * format.sample_rate_hz);

  std::vector<double> record(format.valid_samples, 0.5);
  for (uint32_t index = 0; index < format.valid_samples; ++index) {
    record[index] +=
        kCollapsedAmplitude *
        std::sin(2.0 * kPi * cycles_per_sample * static_cast<double>(index));
  }

  VBILinePlacement placement;
  std::string error;
  ASSERT_TRUE(make_vbi_line_placement(format, service, pal_geometry(),
                                      format.capture_offset_samples, 6,
                                      placement, error))
      << error;

  std::vector<double> line;
  resample_vbi_line(resampler, record, placement, line);
  ASSERT_FALSE(line.empty());

  constexpr uint32_t kEdgeGuard = 48;
  ASSERT_GT(line.size(), 2u * kEdgeGuard);
  double lowest = line[kEdgeGuard];
  double highest = line[kEdgeGuard];
  for (size_t index = kEdgeGuard; index + kEdgeGuard < line.size(); ++index) {
    lowest = std::min(lowest, line[index]);
    highest = std::max(highest, line[index]);
  }

  EXPECT_NEAR(highest - lowest, 2.0 * kCollapsedAmplitude, 2e-3);
}

// The output window is bounded by the stored record at one end and by the
// frame line at the other; neither may be overrun.
TEST(VBILinePlacement, TheOutputWindowIsBoundedByTheRecordAndTheFrameLine) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBIFrameGeometry geometry = pal_geometry();
  constexpr uint32_t kFrameLine = 6;

  VBILinePlacement placement;
  std::string error;
  ASSERT_TRUE(make_vbi_line_placement(format, wst_service(), geometry,
                                      format.capture_offset_samples, kFrameLine,
                                      placement, error))
      << error;

  EXPECT_LE(placement.output_end, geometry.line_length(kFrameLine));
  EXPECT_LT(placement.output_begin, placement.output_end);

  // The record starts 244 source samples after 0H, so the first 122 output
  // samples of the line have no stored data behind them.
  EXPECT_GE(
      placement.source_position(static_cast<double>(placement.output_begin)),
      0.0);
  EXPECT_LT(placement.source_position(
                static_cast<double>(placement.output_begin - 1u)),
            0.0);
  EXPECT_LE(
      placement.source_position(static_cast<double>(placement.output_end - 1u)),
      static_cast<double>(format.valid_samples - 1u));

  // The data region begins after the window opens and ends inside the line.
  EXPECT_GT(placement.data_start_samples,
            static_cast<double>(placement.output_begin));
  EXPECT_LT(placement.data_end_samples,
            static_cast<double>(geometry.line_length(kFrameLine)));
}

// WST carries 360 transmitted bits, which is 51.892 us and just over 920
// output samples (design §5.2).
TEST(VBILinePlacement, TheDataEndFollowsTheServicePayloadLength) {
  const VBISourceFormat format = bt8x8_pal_format();
  VBILinePlacement placement;
  std::string error;

  ASSERT_TRUE(make_vbi_line_placement(format, wst_service(), pal_geometry(),
                                      format.capture_offset_samples, 6,
                                      placement, error))
      << error;

  EXPECT_NEAR(placement.data_end_samples - placement.data_start_samples, 920.31,
              0.05);
}

TEST(VBILinePlacement, TheSourcePositionMapIsInvertible) {
  const VBISourceFormat format = bt8x8_pal_format();
  VBILinePlacement placement;
  std::string error;

  ASSERT_TRUE(make_vbi_line_placement(format, wst_service(), pal_geometry(),
                                      format.capture_offset_samples, 319,
                                      placement, error))
      << error;

  for (const double index : {0.0, 122.5, 500.0, 1134.0}) {
    EXPECT_NEAR(placement.output_index(placement.source_position(index)), index,
                1e-9);
  }
}

TEST(VBILinePlacement, AFrameLineOutsideTheGeometryIsRejected) {
  const VBISourceFormat format = bt8x8_pal_format();
  VBILinePlacement placement;
  std::string error;

  EXPECT_FALSE(make_vbi_line_placement(format, wst_service(), pal_geometry(),
                                       format.capture_offset_samples, 625,
                                       placement, error));
  EXPECT_NE(error.find("625"), std::string::npos) << error;
}

TEST(VBILinePlacement, AnUnusableSourceSamplingRateIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.sample_rate_hz = 0.0;

  VBILinePlacement placement;
  std::string error;
  EXPECT_FALSE(make_vbi_line_placement(format, wst_service(), pal_geometry(),
                                       244.0, 6, placement, error));
  EXPECT_NE(error.find("sampling rate"), std::string::npos) << error;
}

TEST(VBILinePlacement, ARecordWithNoValidSamplesIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.valid_samples = 0;

  VBILinePlacement placement;
  std::string error;
  EXPECT_FALSE(make_vbi_line_placement(format, wst_service(), pal_geometry(),
                                       244.0, 6, placement, error));
  EXPECT_NE(error.find("valid samples"), std::string::npos) << error;
}

TEST(VBILinePlacement, ANonFiniteCaptureOffsetIsRejected) {
  const VBISourceFormat format = bt8x8_pal_format();
  VBILinePlacement placement;
  std::string error;

  EXPECT_FALSE(make_vbi_line_placement(format, wst_service(), pal_geometry(),
                                       std::nan(""), 6, placement, error));
  EXPECT_NE(error.find("capture offset"), std::string::npos) << error;
}

TEST(VBILinePlacement, FiveHundredTwentyFiveLineSystemsAreNotYetPlaced) {
  VBISourceFormat format = bt8x8_pal_format();
  format.tv_system = VBITVSystem::kNTSC;

  VBILinePlacement placement;
  std::string error;
  EXPECT_FALSE(make_vbi_line_placement(format, wst_service(), pal_geometry(),
                                       244.0, 6, placement, error));
  EXPECT_FALSE(error.empty());
}

TEST(VBILinePlacement, AnEmptyRecordProducesAnEmptyLine) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBIBandLimitedResampler resampler(2.0);
  VBILinePlacement placement;
  std::string error;

  ASSERT_TRUE(make_vbi_line_placement(format, wst_service(), pal_geometry(),
                                      format.capture_offset_samples, 6,
                                      placement, error))
      << error;

  std::vector<double> line;
  resample_vbi_line(resampler, {}, placement, line);

  ASSERT_EQ(line.size(), placement.output_count());
  for (const double sample : line) {
    EXPECT_DOUBLE_EQ(sample, 0.0);
  }
}

}  // namespace
}  // namespace orc
