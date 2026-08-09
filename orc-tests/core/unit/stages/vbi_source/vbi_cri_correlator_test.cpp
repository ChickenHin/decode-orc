/*
 * File:        vbi_cri_correlator_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for locating the clock run-in within a record
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_cri_correlator.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "vbi_cri_template.h"
#include "vbi_source_format.h"
#include "vbi_synthetic_line.h"
#include "vbi_teletext_service.h"

namespace orc {
namespace {

using orc::testing::render_synthetic_vbi_line;
using orc::testing::SyntheticVBILine;

constexpr double kCardSampleRateHz = 35468950.0;

VBITeletextService wst_service() {
  VBITeletextService service;
  std::string error;
  EXPECT_TRUE(vbi_teletext_service(VBITVSystem::kPAL, VBITeletextSystem::kWST,
                                   service, error))
      << error;
  return service;
}

VBISourceFormat bt8x8_pal_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", format, error))
      << error;
  return format;
}

VBICRITemplate wst_template(double sample_rate_hz = kCardSampleRateHz) {
  VBICRITemplate result;
  std::string error;
  EXPECT_TRUE(make_vbi_cri_frc_template(wst_service(), sample_rate_hz,
                                        VBICRITemplateConfig{}, result, error))
      << error;
  return result;
}

// A window wide enough to make the search do real work, centred where the
// bt8x8 folklore offset puts the run-in.
VBICRISearchWindow wide_window(double centre = 121.3, double radius = 48.0) {
  VBICRISearchWindow window;
  window.begin_samples = centre - radius;
  window.end_samples = centre + radius;
  return window;
}

// A clean broadcast or LaserDisc capture: per-line precision of about a tenth
// of a sample is achievable, and it is what makes the median over a few
// hundred lines meaningful (design §5.3.4 step 4).
TEST(VBICRICorrelator, RecoversACleanLinePositionToATenthOfASample) {
  const VBICRITemplate cri_template = wst_template();

  for (const double truth : {110.0, 121.3, 121.75, 130.4, 145.9}) {
    SyntheticVBILine line;
    line.anchor_position_samples = truth;
    const std::vector<double> record = render_synthetic_vbi_line(line);

    const VBICRIDetection detection =
        detect_vbi_cri_position(record, cri_template, wide_window(), 0.5);

    ASSERT_TRUE(detection.accepted) << "truth " << truth;
    EXPECT_TRUE(detection.refined) << "truth " << truth;
    EXPECT_NEAR(detection.anchor_position_samples, truth, 0.1)
        << "truth " << truth;
    EXPECT_GT(detection.peak_correlation, 0.9) << "truth " << truth;
  }
}

// The VHS case from design §5.3.6: blurred to sigma of 0.8 bit periods with
// the run-in surviving at 5.5 per cent of the data amplitude.  The lock is
// weaker but it still holds, because the energy has moved into the framing
// code and the template covers it.
TEST(VBICRICorrelator, RecoversABlurredLinePositionToWithinOneSample) {
  const VBICRITemplate cri_template = wst_template();

  for (const double truth : {115.2, 121.3, 128.6}) {
    SyntheticVBILine line;
    line.anchor_position_samples = truth;
    line.blur_bit_periods = 0.8;
    line.run_in_amplitude_fraction = 0.055;
    line.noise_amplitude = 1.0;
    const std::vector<double> record = render_synthetic_vbi_line(line);

    const VBICRIDetection detection =
        detect_vbi_cri_position(record, cri_template, wide_window(), 0.3);

    ASSERT_TRUE(detection.accepted) << "truth " << truth;
    EXPECT_NEAR(detection.anchor_position_samples, truth, 1.0)
        << "truth " << truth;
  }
}

// There are many lines with no teletext on them and they are not an error;
// what matters is that they are rejected rather than contributing a position
// (design §5.3.4 step 3).
TEST(VBICRICorrelator, RejectsLinesCarryingNoDataService) {
  const VBICRITemplate cri_template = wst_template();

  SyntheticVBILine blank;
  blank.carries_data = false;
  EXPECT_FALSE(detect_vbi_cri_position(render_synthetic_vbi_line(blank),
                                       cri_template, wide_window(), 0.5)
                   .accepted);

  SyntheticVBILine noise;
  noise.carries_data = false;
  noise.noise_amplitude = 12.0;
  noise.seed = 7;
  const VBICRIDetection noise_detection = detect_vbi_cri_position(
      render_synthetic_vbi_line(noise), cri_template, wide_window(), 0.5);
  EXPECT_FALSE(noise_detection.accepted);
  EXPECT_LT(noise_detection.peak_correlation, 0.5);
}

// A single threshold has to mean the same thing on every line of a capture
// whose gain is moving under automatic gain control, so the correlation is
// normalised against the record's own level and energy.
TEST(VBICRICorrelator, CorrelationIsUnaffectedByALineLevelOrGain) {
  const VBICRITemplate cri_template = wst_template();

  SyntheticVBILine line;
  line.anchor_position_samples = 121.3;
  const std::vector<double> record = render_synthetic_vbi_line(line);

  SyntheticVBILine dim = line;
  dim.logic0 = 90.0;
  dim.logic1 = 130.0;
  const std::vector<double> dim_record = render_synthetic_vbi_line(dim);

  const VBICRIDetection full =
      detect_vbi_cri_position(record, cri_template, wide_window(), 0.5);
  const VBICRIDetection faint =
      detect_vbi_cri_position(dim_record, cri_template, wide_window(), 0.5);

  EXPECT_NEAR(faint.peak_correlation, full.peak_correlation, 1e-6);
  EXPECT_NEAR(faint.anchor_position_samples, full.anchor_position_samples,
              1e-6);
}

TEST(VBICRICorrelator, ConstantWindowsCorrelateWithNothing) {
  const VBICRITemplate cri_template = wst_template();
  const std::vector<double> flat(2044, 64.0);

  EXPECT_DOUBLE_EQ(vbi_normalised_correlation(flat, cri_template, 100), 0.0);
}

// A peak against the edge of the search window has no neighbour to interpolate
// against, and is itself a sign that the window is in the wrong place.
TEST(VBICRICorrelator, PeaksAtTheWindowEdgeAreNotRefined) {
  const VBICRITemplate cri_template = wst_template();

  SyntheticVBILine line;
  line.anchor_position_samples = 121.3;
  const std::vector<double> record = render_synthetic_vbi_line(line);

  // A window collapsed onto a single position, so the peak has no neighbour on
  // one side of it.
  VBICRISearchWindow window;
  window.begin_samples = 121.3;
  window.end_samples = 121.3;

  const VBICRIDetection detection =
      detect_vbi_cri_position(record, cri_template, window, 0.5);
  EXPECT_TRUE(detection.accepted);
  EXPECT_FALSE(detection.refined);
  EXPECT_DOUBLE_EQ(detection.refinement_samples, 0.0);

  // The position is then whole-sample only, so it is a sample away from the
  // truth rather than a tenth of one.
  EXPECT_NEAR(detection.anchor_position_samples, 121.3, 1.0);
}

// The window is a property of the source format, not a global constant: a
// tape-sourced format searches wider than a broadcast one without either being
// wrong (design §5.3.6).
TEST(VBICRICorrelator, SearchWindowComesFromTheFormatsOwnTolerance) {
  VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  const double predicted = service.cri_start_samples(
      format.sample_rate_hz, format.capture_offset_samples);

  const VBICRISearchWindow window = vbi_cri_search_window(format, service);
  EXPECT_NEAR(window.begin_samples,
              predicted - format.calibration.search_tolerance_samples, 1e-9);
  EXPECT_NEAR(window.end_samples,
              predicted + format.calibration.search_tolerance_samples, 1e-9);

  // The folkloric 244-sample offset puts the run-in at about 121 samples into
  // the record, which is where vhs-teletext looks for it (design §5.3.3).
  EXPECT_NEAR(predicted, 121.3, 0.5);

  format.calibration.search_tolerance_samples = 200.0;
  const VBICRISearchWindow wide = vbi_cri_search_window(format, service);
  EXPECT_GT(wide.end_samples - wide.begin_samples,
            window.end_samples - window.begin_samples);

  // The window never runs off the front of the record.
  EXPECT_GE(wide.begin_samples, 0.0);
}

TEST(VBICRICorrelator, TemplatesLongerThanTheRecordFindNothing) {
  const VBICRITemplate cri_template = wst_template();
  const std::vector<double> stub(10, 0.0);

  const VBICRIDetection detection =
      detect_vbi_cri_position(stub, cri_template, wide_window(), 0.5);
  EXPECT_FALSE(detection.accepted);
  EXPECT_DOUBLE_EQ(detection.peak_correlation, 0.0);
}

}  // namespace
}  // namespace orc
