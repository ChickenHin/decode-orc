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

#include "vbi_output_frame.h"
#include "vbi_resampler.h"
#include "vbi_source_format.h"
#include "vbi_teletext_service.h"

namespace orc {
namespace {

constexpr double kOutputRateHz = 17734475.0;  // 4 x fsc PAL
constexpr double kPi = 3.14159265358979323846;

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

VBIOutputFrame pal_output_frame() {
  VBIOutputFrame frame;
  std::string error;
  EXPECT_TRUE(make_vbi_output_frame(VBITVSystem::kPAL, frame, error)) << error;
  return frame;
}

VBIDataPlacement placement_at(const VBISourceFormat& format,
                              double capture_offset_samples) {
  VBIDataPlacement placement;
  std::string error;
  EXPECT_TRUE(make_vbi_data_placement(format, wst_service(), pal_output_frame(),
                                      capture_offset_samples, placement, error))
      << error;
  return placement;
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

TEST(VBIDataPlacement, TheRatioIsTheSourceRateOverTheOutputLatticeRate) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBIDataPlacement placement =
      placement_at(format, format.capture_offset_samples);

  // bt8x8 PAL samples at 8 x fsc onto a 4 x fsc lattice.
  EXPECT_DOUBLE_EQ(placement.source_samples_per_output_sample, 2.0);
}

// Sample zero of a stored output line is that line's 0H, so the data region
// begins at the service's own 0H offset (design §5.2).
TEST(VBIDataPlacement, TheDataStartIsTheServiceOffsetOnTheOutputLattice) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  const double nominal_start = service.t_offset_ns * 1e-9 * kOutputRateHz;
  EXPECT_NEAR(nominal_start, 182.665, 0.001);

  const VBIDataPlacement placement =
      placement_at(format, format.capture_offset_samples);
  EXPECT_NEAR(placement.data_start_samples, nominal_start, 1e-9);
}

// The acceptance criterion for the phase: a marker in the record lands within
// a tenth of a sample of where the placement says it should.  One placement
// serves every data line, so this is the whole of the horizontal accuracy
// claim.
//
// The marker sits a little way into the packet rather than on the run-in
// itself, so that the whole of its symmetric shape is inside the window the
// record is clipped to and its centroid measures position rather than the
// clip.
TEST(VBIDataPlacement, MarkedDataLandsWhereThePlacementSaysItShould) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  constexpr double kSamplesIntoThePacket = 400.0;

  // Source coordinate of the first clock run-in one bit, given the record's
  // own capture offset, and of the marker a little after it.
  const double anchor_source_position = service.cri_start_samples(
      format.sample_rate_hz, format.capture_offset_samples);
  const double marker_source_position =
      anchor_source_position + kSamplesIntoThePacket;
  const std::vector<double> record =
      record_with_marker_at(format.valid_samples, marker_source_position);

  const VBIDataPlacement placement =
      placement_at(format, format.capture_offset_samples);
  const VBIRecordResampler resampler(placement, format.valid_samples);

  std::vector<double> line;
  resampler.resample(record, line);

  const double expected =
      placement.data_start_samples +
      kSamplesIntoThePacket / placement.source_samples_per_output_sample;
  EXPECT_NEAR(marker_position(line, placement.output_begin), expected, 0.1);
}

// The clip keeps a bit period of guard at each end, so the leading edge of the
// first run-in bit and the trailing edge of the last payload bit both survive
// it (design §5.6).
TEST(VBIDataPlacement, TheGuardKeepsTheEdgesOfThePacketInsideTheWindow) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIDataPlacement placement =
      placement_at(format, format.capture_offset_samples);

  const double guard = service.samples_per_bit(kOutputRateHz);
  EXPECT_LE(static_cast<double>(placement.output_begin),
            placement.data_start_samples - guard);
  EXPECT_GE(static_cast<double>(placement.output_end),
            placement.data_end_samples + guard);
}

// The calibrated offset is what makes the placement right: a record captured
// with a different offset than the one supplied lands displaced by exactly
// that difference, scaled into output samples.
TEST(VBIDataPlacement, AMisstatedCaptureOffsetDisplacesTheDataItPlaces) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  constexpr double kOffsetError = 10.0;

  constexpr double kSamplesIntoThePacket = 400.0;
  const double marker_source_position =
      service.cri_start_samples(format.sample_rate_hz,
                                format.capture_offset_samples) +
      kSamplesIntoThePacket;
  const std::vector<double> record =
      record_with_marker_at(format.valid_samples, marker_source_position);

  const VBIDataPlacement placement =
      placement_at(format, format.capture_offset_samples + kOffsetError);
  const VBIRecordResampler resampler(placement, format.valid_samples);

  std::vector<double> line;
  resampler.resample(record, line);

  // Ten source samples is five output samples at 2:1, and believing sample 0
  // of the record sits later after 0H than it does carries everything in the
  // record the same distance later along the line.
  const double expected =
      placement.data_start_samples +
      kSamplesIntoThePacket / placement.source_samples_per_output_sample +
      kOffsetError / 2.0;
  EXPECT_NEAR(marker_position(line, placement.output_begin), expected, 0.1);
}

// The output window is bounded by the stored record at one end and by the data
// region at the other; neither may be overrun.
TEST(VBIDataPlacement, TheOutputWindowIsBoundedByTheRecordAndTheDataRegion) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBIOutputFrame output = pal_output_frame();
  const VBIDataPlacement placement =
      placement_at(format, format.capture_offset_samples);

  EXPECT_LT(placement.output_begin, placement.output_end);
  EXPECT_LE(placement.output_end, output.samples_per_line_nominal);

  // Nothing is read from before the start of the record.
  EXPECT_GE(
      placement.source_position(static_cast<double>(placement.output_begin)),
      0.0);
  EXPECT_LE(
      placement.source_position(static_cast<double>(placement.output_end - 1u)),
      static_cast<double>(format.valid_samples - 1u));

  // A record covers more of the line than the data region does at both ends,
  // and the clip is what stops the rest of the line being written over
  // (design §5.6).
  const double guard = wst_service().samples_per_bit(kOutputRateHz);
  EXPECT_GE(static_cast<double>(placement.output_begin),
            placement.data_start_samples - guard - 1.0);
  EXPECT_LE(static_cast<double>(placement.output_end),
            placement.data_end_samples + guard + 1.0);
}

// WST carries 360 transmitted bits, which is 51.892 us and just over 920
// output samples (design §5.2).
TEST(VBIDataPlacement, TheDataEndFollowsTheServicePayloadLength) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBIDataPlacement placement =
      placement_at(format, format.capture_offset_samples);

  EXPECT_NEAR(placement.data_end_samples - placement.data_start_samples, 920.31,
              0.05);
}

TEST(VBIDataPlacement, AnUnusableSourceSamplingRateIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.sample_rate_hz = 0.0;

  VBIDataPlacement placement;
  std::string error;
  EXPECT_FALSE(make_vbi_data_placement(
      format, wst_service(), pal_output_frame(), 244.0, placement, error));
  EXPECT_NE(error.find("sampling rate"), std::string::npos) << error;
}

TEST(VBIDataPlacement, ARecordWithNoValidSamplesIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.valid_samples = 0;

  VBIDataPlacement placement;
  std::string error;
  EXPECT_FALSE(make_vbi_data_placement(
      format, wst_service(), pal_output_frame(), 244.0, placement, error));
  EXPECT_NE(error.find("valid samples"), std::string::npos) << error;
}

TEST(VBIDataPlacement, ANonFiniteCaptureOffsetIsRejected) {
  const VBISourceFormat format = bt8x8_pal_format();

  VBIDataPlacement placement;
  std::string error;
  EXPECT_FALSE(make_vbi_data_placement(format, wst_service(),
                                       pal_output_frame(), std::nan(""),
                                       placement, error));
  EXPECT_NE(error.find("capture offset"), std::string::npos) << error;
}

}  // namespace
}  // namespace orc
