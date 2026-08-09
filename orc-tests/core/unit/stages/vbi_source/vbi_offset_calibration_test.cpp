/*
 * File:        vbi_offset_calibration_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for capture-offset fitting and its health reporting
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_offset_calibration.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "vbi_byte_source.h"
#include "vbi_line_placement.h"
#include "vbi_output_frame.h"
#include "vbi_source_format.h"
#include "vbi_synthetic_line.h"
#include "vbi_teletext_service.h"

namespace orc {
namespace {

using orc::testing::render_synthetic_vbi_line;
using orc::testing::SyntheticVBILine;

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

// Position the service's nominal timing puts the run-in at for a given
// capture offset.
double run_in_position_for_offset(const VBISourceFormat& format,
                                  const VBITeletextService& service,
                                  double capture_offset_samples) {
  return service.cri_start_samples(format.sample_rate_hz,
                                   capture_offset_samples);
}

std::vector<VBICRIObservation> observations_at(
    const std::vector<double>& positions, uint64_t first_sequence = 0,
    uint64_t stride = 1) {
  std::vector<VBICRIObservation> observations;
  observations.reserve(positions.size());
  uint64_t sequence = first_sequence;
  for (const double position : positions) {
    VBICRIObservation observation;
    observation.line_sequence = sequence;
    observation.anchor_position_samples = position;
    observation.peak_correlation = 0.95;
    observations.push_back(observation);
    sequence += stride;
  }
  return observations;
}

// Positions scattered about a truth by a deterministic saw, so a test can set
// the spread it wants without depending on a random generator.
std::vector<double> scattered_positions(double truth, double half_spread,
                                        size_t count) {
  std::vector<double> positions;
  positions.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    const double phase = (index % 4u) - 1.5;  // -1.5, -0.5, 0.5, 1.5
    positions.push_back(truth + half_spread * phase / 1.5);
  }
  return positions;
}

// ---------------------------------------------------------------------------
// Sampling the capture
// ---------------------------------------------------------------------------

TEST(VBIOffsetCalibration, SampledFramesAreSpreadAcrossTheCaptureNotItsHead) {
  const std::vector<uint64_t> frames =
      vbi_calibration_frame_indices(368007, 16);

  ASSERT_EQ(frames.size(), 16u);
  EXPECT_TRUE(std::is_sorted(frames.begin(), frames.end()));

  // Nothing from the opening segment, whose material is the most likely to be
  // atypical, and coverage all the way to the end.
  EXPECT_GT(frames.front(), 368007u / 64u);
  EXPECT_GT(frames.back(), 368007u - 368007u / 16u);
}

TEST(VBIOffsetCalibration, SamplingNeverAsksForMoreFramesThanExist) {
  const std::vector<uint64_t> frames = vbi_calibration_frame_indices(3, 16);
  ASSERT_FALSE(frames.empty());
  EXPECT_LE(frames.size(), 3u);
  EXPECT_LT(frames.back(), 3u);

  EXPECT_TRUE(vbi_calibration_frame_indices(0, 16).empty());
  EXPECT_TRUE(vbi_calibration_frame_indices(100, 0).empty());
}

TEST(VBIOffsetCalibration, LineSequenceCountsEveryStoredRecordOfTheCapture) {
  const VBISourceFormat format = bt8x8_pal_format();

  VBILineRecord record;
  record.frame_index = 3;
  record.field_index = 1;
  record.record_index = 4;

  // 3 frames of 32 records, then 16 records of the first field, then 4.
  EXPECT_EQ(vbi_line_sequence(format, record), 3u * 32u + 16u + 4u);
}

// ---------------------------------------------------------------------------
// The fit itself
// ---------------------------------------------------------------------------

TEST(VBIOffsetCalibration, FitsTheOffsetTheRunInPositionsImply) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const double truth_offset = 244.0;
  const double truth_position =
      run_in_position_for_offset(format, service, truth_offset);

  const VBIOffsetCalibration calibration = fit_vbi_capture_offset(
      format, service,
      observations_at(scattered_positions(truth_position, 0.2, 64)), 64);

  EXPECT_TRUE(calibration.converged) << calibration.summary;
  EXPECT_NEAR(calibration.capture_offset_samples, truth_offset, 0.05);
  EXPECT_NEAR(calibration.anchor_position_samples, truth_position, 0.05);
  EXPECT_EQ(calibration.records_accepted, 64u);
  EXPECT_EQ(calibration.records_examined, 64u);
  EXPECT_DOUBLE_EQ(calibration.acceptance_fraction, 1.0);
  EXPECT_EQ(calibration.spread_class, VBIOffsetSpreadClass::kTight);
  EXPECT_TRUE(calibration.diagnostics.empty());
  EXPECT_NE(calibration.summary.find("Capture offset"), std::string::npos);
}

// A mean would follow a partial match on a damaged line; a median does not
// (design §5.3.4 step 5).
TEST(VBIOffsetCalibration, OutliersDoNotMoveTheFittedOffset) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const double truth_position =
      run_in_position_for_offset(format, service, 244.0);

  std::vector<double> positions = scattered_positions(truth_position, 0.2, 60);
  for (int index = 0; index < 6; ++index) {
    positions.push_back(truth_position + 40.0);
  }

  const VBIOffsetCalibration calibration = fit_vbi_capture_offset(
      format, service, observations_at(positions), positions.size());

  EXPECT_NEAR(calibration.anchor_position_samples, truth_position, 0.2);
  EXPECT_NEAR(calibration.capture_offset_samples, 244.0, 0.2);
}

TEST(VBIOffsetCalibration, SpreadClassificationFollowsTheFormatsOwnThresholds) {
  VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const double truth_position =
      run_in_position_for_offset(format, service, 244.0);

  const VBIOffsetCalibration tight = fit_vbi_capture_offset(
      format, service,
      observations_at(scattered_positions(truth_position, 0.2, 64)), 64);
  EXPECT_EQ(tight.spread_class, VBIOffsetSpreadClass::kTight);
  EXPECT_TRUE(tight.converged);
  EXPECT_TRUE(tight.warnings.empty());

  const VBIOffsetCalibration mild = fit_vbi_capture_offset(
      format, service,
      observations_at(scattered_positions(truth_position, 3.0, 64)), 64);
  EXPECT_EQ(mild.spread_class, VBIOffsetSpreadClass::kMild);
  EXPECT_TRUE(mild.converged) << mild.summary;
  ASSERT_FALSE(mild.warnings.empty());
  EXPECT_NE(mild.warnings.front().find("jitter"), std::string::npos)
      << mild.warnings.front();

  const VBIOffsetCalibration unusable = fit_vbi_capture_offset(
      format, service,
      observations_at(scattered_positions(truth_position, 16.0, 64)), 64);
  EXPECT_EQ(unusable.spread_class, VBIOffsetSpreadClass::kUnusable);
  EXPECT_FALSE(unusable.converged);
  ASSERT_FALSE(unusable.diagnostics.empty());
  EXPECT_NE(unusable.diagnostics.front().find("not time-base corrected"),
            std::string::npos)
      << unusable.diagnostics.front();

  // The same scatter on a format that expects it is not a failure: the
  // thresholds belong to the format, not to the stage (design §5.3.6).
  format.calibration.tight_spread_samples = 4.0;
  format.calibration.maximum_spread_samples = 24.0;
  const VBIOffsetCalibration tolerated = fit_vbi_capture_offset(
      format, service,
      observations_at(scattered_positions(truth_position, 16.0, 64)), 64);
  EXPECT_EQ(tolerated.spread_class, VBIOffsetSpreadClass::kMild);
  EXPECT_TRUE(tolerated.converged) << tolerated.summary;
}

// A monotonic drift is diagnostic of a sampling-rate error specifically, and
// the slope gives the correction directly (design §5.3.4).
TEST(VBIOffsetCalibration, InjectedSampleRateErrorIsDetectedAndCorrected) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const double truth_position =
      run_in_position_for_offset(format, service, 244.0);

  // A configured rate 100 ppm below the truth walks the run-in later by
  // 100 ppm of a record's samples on every line.
  constexpr double kRateErrorFraction = 100e-6;
  const double slope =
      kRateErrorFraction * static_cast<double>(format.line_length);

  std::vector<VBICRIObservation> observations;
  for (uint64_t line = 0; line < 512; ++line) {
    VBICRIObservation observation;
    observation.line_sequence = line * 64u;
    observation.anchor_position_samples =
        truth_position + slope * static_cast<double>(line * 64u);
    observation.peak_correlation = 0.95;
    observations.push_back(observation);
  }

  const VBIOffsetCalibration calibration =
      fit_vbi_capture_offset(format, service, observations, 512);

  EXPECT_TRUE(calibration.drift_detected);
  EXPECT_FALSE(calibration.converged);
  ASSERT_FALSE(calibration.diagnostics.empty());
  EXPECT_NE(calibration.diagnostics.back().find("drifts monotonically"),
            std::string::npos)
      << calibration.diagnostics.back();

  const double true_rate_hz =
      format.sample_rate_hz * (1.0 + kRateErrorFraction);
  const double error_ppm =
      1e6 * std::abs(calibration.suggested_sample_rate_hz - true_rate_hz) /
      true_rate_hz;
  EXPECT_LT(error_ppm, 1.0)
      << "suggested " << calibration.suggested_sample_rate_hz << " Hz against "
      << true_rate_hz << " Hz";
}

// Scatter is not drift: the estimator's own noise must not be reported as a
// rate error, however long the span it was fitted over.
TEST(VBIOffsetCalibration, ScatterAloneIsNotReportedAsDrift) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const double truth_position =
      run_in_position_for_offset(format, service, 244.0);

  const VBIOffsetCalibration calibration = fit_vbi_capture_offset(
      format, service,
      observations_at(scattered_positions(truth_position, 0.4, 96), 0, 4096),
      96);

  EXPECT_FALSE(calibration.drift_detected);
  EXPECT_TRUE(calibration.converged) << calibration.summary;
  EXPECT_DOUBLE_EQ(calibration.suggested_sample_rate_hz, format.sample_rate_hz);
}

TEST(VBIOffsetCalibration, TooFewLocksIsReportedRatherThanFitted) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  const VBIOffsetCalibration calibration = fit_vbi_capture_offset(
      format, service, observations_at(scattered_positions(121.3, 0.1, 3)),
      512);

  EXPECT_FALSE(calibration.converged);
  EXPECT_EQ(calibration.records_accepted, 3u);
  ASSERT_FALSE(calibration.diagnostics.empty());
  EXPECT_NE(calibration.diagnostics.front().find("too few"), std::string::npos)
      << calibration.diagnostics.front();
}

TEST(VBIOffsetCalibration, LowAcceptanceIsReportedWithItsFraction) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const double truth_position =
      run_in_position_for_offset(format, service, 244.0);

  // Sixteen locks out of 512 records examined: a tenth of what this format
  // expects.
  const VBIOffsetCalibration calibration = fit_vbi_capture_offset(
      format, service,
      observations_at(scattered_positions(truth_position, 0.2, 16)), 512);

  EXPECT_FALSE(calibration.converged);
  EXPECT_NEAR(calibration.acceptance_fraction, 16.0 / 512.0, 1e-9);
  ASSERT_FALSE(calibration.diagnostics.empty());
  EXPECT_NE(calibration.diagnostics.front().find("3.1%"), std::string::npos)
      << calibration.diagnostics.front();
}

// The offset is applied globally and never per line: every record of every
// frame is placed by exactly the same map, and a record that carried no
// teletext becomes ordinary blanking (design §5.3.4).
TEST(VBIOffsetCalibration, EveryRecordIsPlacedByTheOneGlobalOffset) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const double truth_position =
      run_in_position_for_offset(format, service, 244.0);

  const VBIOffsetCalibration calibration = fit_vbi_capture_offset(
      format, service,
      observations_at(scattered_positions(truth_position, 0.2, 64)), 256);
  ASSERT_TRUE(calibration.converged) << calibration.summary;

  VBIOutputFrame output;
  std::string error;
  ASSERT_TRUE(
      make_vbi_output_frame(format.tv_system, format.tt_system, output, error))
      << error;

  VBIDataPlacement placement;
  ASSERT_TRUE(make_vbi_data_placement(format, service, output,
                                      calibration.capture_offset_samples,
                                      placement, error))
      << error;

  // The fitted offset is what sample zero of every record is placed at, so the
  // run-in lands at the service's own anchor on the output lattice.
  EXPECT_NEAR(placement.source_position_at_output_zero,
              -calibration.capture_offset_samples, 1e-9);
  const double run_in_output_index =
      (calibration.anchor_position_samples -
       placement.source_position_at_output_zero) /
      placement.source_samples_per_output_sample;
  EXPECT_NEAR(run_in_output_index, placement.data_start_samples, 0.5);
}

// ---------------------------------------------------------------------------
// The whole procedure against a capture
// ---------------------------------------------------------------------------

// In-memory capture. Unit tests never touch the filesystem, and the transport
// presents identical bytes whatever the container.
class FakeByteSource : public IVBIByteSource {
 public:
  explicit FakeByteSource(std::vector<uint8_t> bytes)
      : bytes_(std::move(bytes)) {}

  std::optional<uint64_t> size_bytes() const override {
    return static_cast<uint64_t>(bytes_.size());
  }

  size_t read_at(uint64_t byte_offset, size_t count, uint8_t* out_buffer,
                 std::string& error_message) override {
    (void)error_message;
    if (byte_offset >= bytes_.size()) {
      return 0;
    }
    const size_t available = bytes_.size() - static_cast<size_t>(byte_offset);
    const size_t produced = std::min(count, available);
    std::memcpy(out_buffer, bytes_.data() + byte_offset, produced);
    return produced;
  }

 private:
  std::vector<uint8_t> bytes_;
};

// A capture whose teletext lines all sit at one true capture offset, with
// every fourth record carrying no data service at all.
std::vector<uint8_t> make_synthetic_capture(const VBISourceFormat& format,
                                            const VBITeletextService& service,
                                            uint64_t frame_count,
                                            double capture_offset_samples) {
  std::vector<uint8_t> bytes(
      static_cast<size_t>(format.bytes_per_frame() * frame_count), 0);

  const double position =
      run_in_position_for_offset(format, service, capture_offset_samples);

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
        line.carries_data = (index % 4u) != 3u;

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
  }
  return bytes;
}

TEST(VBIOffsetCalibration, CalibratesACaptureFromItsOwnClockRunIn) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  constexpr double kTruthOffset = 231.5;

  FakeByteSource source(
      make_synthetic_capture(format, service, 6, kTruthOffset));
  VBILineReader reader(format, source);

  VBICalibrationConfig config;
  config.sample_frames = 4;

  VBIOffsetCalibration calibration;
  std::string error;
  ASSERT_TRUE(
      calibrate_vbi_capture_offset(reader, service, config, calibration, error))
      << error;

  EXPECT_TRUE(calibration.converged) << calibration.summary;
  EXPECT_NEAR(calibration.capture_offset_samples, kTruthOffset, 0.2);
  EXPECT_EQ(calibration.records_examined, 4u * 32u);

  // Three records in four carry data, and all of those should lock.
  EXPECT_EQ(calibration.records_accepted, 4u * 24u);
  EXPECT_NEAR(calibration.acceptance_fraction, 0.75, 1e-9);
  EXPECT_EQ(calibration.spread_class, VBIOffsetSpreadClass::kTight);
}

// A time-base corrected source has its origin at 0H by construction, so
// calibrating one would replace an exactly known value with a fitted one
// (design §5.3.3).
TEST(VBIOffsetCalibration, RefusesToCalibrateATimeBaseCorrectedSource) {
  VBISourceFormat format = bt8x8_pal_format();
  format.family = VBISourceFamily::kTBCDerived;

  FakeByteSource source(
      std::vector<uint8_t>(static_cast<size_t>(format.bytes_per_frame()), 0u));
  VBILineReader reader(format, source);

  VBIOffsetCalibration calibration;
  std::string error;
  EXPECT_FALSE(calibrate_vbi_capture_offset(
      reader, wst_service(), VBICalibrationConfig{}, calibration, error));
  EXPECT_NE(error.find("exactly zero"), std::string::npos) << error;
}

TEST(VBIOffsetCalibration, ReportsACaptureWithNoWholeFramesAsAnError) {
  const VBISourceFormat format = bt8x8_pal_format();

  FakeByteSource source(std::vector<uint8_t>(1024, 0u));
  VBILineReader reader(format, source);

  VBIOffsetCalibration calibration;
  std::string error;
  EXPECT_FALSE(calibrate_vbi_capture_offset(
      reader, wst_service(), VBICalibrationConfig{}, calibration, error));
  EXPECT_NE(error.find("no whole frames"), std::string::npos) << error;
}

}  // namespace
}  // namespace orc
