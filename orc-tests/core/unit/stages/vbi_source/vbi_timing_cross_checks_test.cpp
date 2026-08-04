/*
 * File:        vbi_timing_cross_checks_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the independent corroborations of the timing fit
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_timing_cross_checks.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "vbi_source_format.h"
#include "vbi_synthetic_line.h"
#include "vbi_teletext_service.h"

namespace orc {
namespace {

using orc::testing::render_synthetic_vbi_line;
using orc::testing::SyntheticVBILine;

// The offset every synthetic record below is rendered at.
constexpr double kTruthOffsetSamples = 244.0;

// Records of the bt8x8 PAL field-1 line list, whose first record is broadcast
// line 7.
constexpr uint32_t kVPSRecordIndex = 9;       // broadcast line 16
constexpr uint32_t kCaptionRecordIndex = 15;  // broadcast line 22

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

double anchor_for(const VBISourceFormat& format, double t_offset_ns,
                  double capture_offset_samples) {
  return t_offset_ns * 1e-9 * format.sample_rate_hz - capture_offset_samples;
}

const VBITimingCrossCheck& find_check(
    const std::vector<VBITimingCrossCheck>& checks, const std::string& prefix) {
  for (const VBITimingCrossCheck& check : checks) {
    if (check.name.rfind(prefix, 0) == 0) {
      return check;
    }
  }
  ADD_FAILURE() << "no cross-check named " << prefix;
  static const VBITimingCrossCheck kMissing;
  return kMissing;
}

// A teletext record, optionally carrying the burst tail that a bt8x8 PAL
// capture window opens inside.
VBILineRecord teletext_record(const VBISourceFormat& format,
                              const VBITeletextService& service, uint64_t frame,
                              uint32_t field, uint32_t index,
                              double capture_offset_samples, bool with_burst,
                              uint32_t payload_bits = 336) {
  SyntheticVBILine line;
  line.sample_rate_hz = format.sample_rate_hz;
  line.valid_samples = format.valid_samples;
  line.anchor_position_samples =
      anchor_for(format, service.t_offset_ns, capture_offset_samples);
  line.payload_bits = payload_bits;
  line.noise_amplitude = 0.5;
  line.seed = static_cast<uint32_t>(frame * 64u + field * 32u + index + 1u);
  line.include_burst = with_burst;
  line.burst_end_samples =
      7850.0e-9 * format.sample_rate_hz - capture_offset_samples;

  VBILineRecord record;
  record.frame_index = frame;
  record.field_index = field;
  record.record_index = index;
  record.samples = render_synthetic_vbi_line(line);
  return record;
}

// A record carrying one of the independent reference services.
VBILineRecord reference_record(const VBISourceFormat& format,
                               const VBIReferenceService& service,
                               uint64_t frame, uint32_t field, uint32_t index,
                               double capture_offset_samples) {
  SyntheticVBILine line;
  line.sample_rate_hz = format.sample_rate_hz;
  line.valid_samples = format.valid_samples;
  line.bit_rate_hz = service.bit_rate_hz;
  line.pattern = service.pattern;
  line.pattern_bits = service.pattern_bits;
  line.run_in_bits = 16;
  line.payload_bits = 32;
  line.anchor_position_samples =
      anchor_for(format, service.t_offset_ns, capture_offset_samples);
  line.noise_amplitude = 0.5;
  line.seed = static_cast<uint32_t>(frame * 64u + index + 3u);

  VBILineRecord record;
  record.frame_index = frame;
  record.field_index = field;
  record.record_index = index;
  record.samples = render_synthetic_vbi_line(line);
  return record;
}

VBIReferenceService reference_service_on_line(VBITVSystem tv_system,
                                              uint32_t broadcast_line) {
  for (const VBIReferenceService& service : vbi_reference_services(tv_system)) {
    if (service.broadcast_line == broadcast_line) {
      return service;
    }
  }
  ADD_FAILURE() << "no reference service on broadcast line " << broadcast_line;
  return VBIReferenceService{};
}

// A frame's worth of teletext records, all rendered at the same offset.
std::vector<VBILineRecord> teletext_records(const VBISourceFormat& format,
                                            const VBITeletextService& service,
                                            double capture_offset_samples,
                                            bool with_burst,
                                            uint32_t payload_bits = 336) {
  std::vector<VBILineRecord> records;
  for (uint64_t frame = 0; frame < 2u; ++frame) {
    for (uint32_t index = 0; index < 8u; ++index) {
      records.push_back(teletext_record(format, service, frame, 0, index,
                                        capture_offset_samples, with_burst,
                                        payload_bits));
    }
  }
  return records;
}

// ---------------------------------------------------------------------------
// Colour burst remnant
// ---------------------------------------------------------------------------

TEST(VBITimingCrossChecks, BurstRemnantAgreesWithAFittedOffsetThatIsRight) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  const std::vector<VBITimingCrossCheck> checks = run_vbi_timing_cross_checks(
      format, service, kTruthOffsetSamples,
      teletext_records(format, service, kTruthOffsetSamples, true),
      VBICrossCheckConfig{});

  const VBITimingCrossCheck& burst = find_check(checks, "Colour burst");
  EXPECT_EQ(burst.outcome, VBICrossCheckOutcome::kAgreed) << burst.message;
  EXPECT_GE(burst.records_used, 4u);
  EXPECT_NEAR(burst.measured_samples, burst.expected_samples,
              burst.tolerance_samples);
}

TEST(VBITimingCrossChecks, BurstRemnantWarnsWhenTheFittedOffsetIsWrong) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  // The records were captured at the true offset; the fit came out 24 samples
  // away from it.
  const double wrong_offset = kTruthOffsetSamples - 24.0;
  const std::vector<VBITimingCrossCheck> checks = run_vbi_timing_cross_checks(
      format, service, wrong_offset,
      teletext_records(format, service, kTruthOffsetSamples, true),
      VBICrossCheckConfig{});

  const VBITimingCrossCheck& burst = find_check(checks, "Colour burst");
  EXPECT_EQ(burst.outcome, VBICrossCheckOutcome::kDisagreed) << burst.message;

  // Both estimates have to appear in the message, so the user can see which
  // two things disagree rather than only that something did.
  EXPECT_NE(burst.message.find("burst remnant ends at sample"),
            std::string::npos)
      << burst.message;
  EXPECT_NE(burst.message.find("fitted capture offset"), std::string::npos)
      << burst.message;
  // The fit is 24 samples early, so it predicts the burst ending 24 samples
  // later in the record than it really does.
  EXPECT_NEAR(burst.disagreement_samples(), -24.0, 4.0);
}

TEST(VBITimingCrossChecks,
     BurstRemnantIsNotApplicableWithoutABurstInTheWindow) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  const std::vector<VBITimingCrossCheck> checks = run_vbi_timing_cross_checks(
      format, service, kTruthOffsetSamples,
      teletext_records(format, service, kTruthOffsetSamples, false),
      VBICrossCheckConfig{});

  const VBITimingCrossCheck& burst = find_check(checks, "Colour burst");
  EXPECT_EQ(burst.outcome, VBICrossCheckOutcome::kNotApplicable)
      << burst.message;
  EXPECT_NE(burst.message.find("too few"), std::string::npos) << burst.message;
}

// A capture whose window opens after the burst has nothing to measure, and
// says so rather than reporting agreement it did not establish.
TEST(VBITimingCrossChecks, BurstRemnantIsNotApplicableWhenTheWindowOpensLate) {
  VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  const double late_offset = 400.0;  // past the 7.85 us end of the burst
  const std::vector<VBITimingCrossCheck> checks = run_vbi_timing_cross_checks(
      format, service, late_offset,
      teletext_records(format, service, late_offset, false),
      VBICrossCheckConfig{});

  const VBITimingCrossCheck& burst = find_check(checks, "Colour burst");
  EXPECT_EQ(burst.outcome, VBICrossCheckOutcome::kNotApplicable);
  EXPECT_NE(burst.message.find("does not open inside the colour burst"),
            std::string::npos)
      << burst.message;
}

// ---------------------------------------------------------------------------
// Other services in the captured range
// ---------------------------------------------------------------------------

TEST(VBITimingCrossChecks, VideoProgrammeSystemCorroboratesTheFittedOffset) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIReferenceService vps =
      reference_service_on_line(format.tv_system, 16);

  std::vector<VBILineRecord> records =
      teletext_records(format, service, kTruthOffsetSamples, false);
  for (uint64_t frame = 0; frame < 6u; ++frame) {
    records.push_back(reference_record(format, vps, frame, 0, kVPSRecordIndex,
                                       kTruthOffsetSamples));
  }

  const std::vector<VBITimingCrossCheck> checks = run_vbi_timing_cross_checks(
      format, service, kTruthOffsetSamples, records, VBICrossCheckConfig{});

  const VBITimingCrossCheck& check = find_check(checks, "VPS");
  EXPECT_EQ(check.outcome, VBICrossCheckOutcome::kAgreed) << check.message;
  EXPECT_GE(check.records_used, 4u);
  EXPECT_NEAR(check.measured_samples, kTruthOffsetSamples, 1.0);
}

TEST(VBITimingCrossChecks, ClosedCaptionsCorroborateTheFittedOffset) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIReferenceService caption =
      reference_service_on_line(format.tv_system, 22);

  std::vector<VBILineRecord> records =
      teletext_records(format, service, kTruthOffsetSamples, false);
  for (uint64_t frame = 0; frame < 6u; ++frame) {
    records.push_back(reference_record(
        format, caption, frame, 0, kCaptionRecordIndex, kTruthOffsetSamples));
  }

  const std::vector<VBITimingCrossCheck> checks = run_vbi_timing_cross_checks(
      format, service, kTruthOffsetSamples, records, VBICrossCheckConfig{});

  const VBITimingCrossCheck& check =
      find_check(checks, "Closed Caption 625 (broadcast line 22)");
  EXPECT_EQ(check.outcome, VBICrossCheckOutcome::kAgreed) << check.message;
  EXPECT_NEAR(check.measured_samples, kTruthOffsetSamples, 2.0);
}

TEST(VBITimingCrossChecks, ReferenceServiceWarnsWhenItDisagreesWithTheFit) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBIReferenceService vps =
      reference_service_on_line(format.tv_system, 16);

  // The VPS lines sit where the true offset puts them; the teletext fit landed
  // six samples away.
  const double fitted_offset = kTruthOffsetSamples + 6.0;
  std::vector<VBILineRecord> records =
      teletext_records(format, service, kTruthOffsetSamples, false);
  for (uint64_t frame = 0; frame < 6u; ++frame) {
    records.push_back(reference_record(format, vps, frame, 0, kVPSRecordIndex,
                                       kTruthOffsetSamples));
  }

  const std::vector<VBITimingCrossCheck> checks = run_vbi_timing_cross_checks(
      format, service, fitted_offset, records, VBICrossCheckConfig{});

  const VBITimingCrossCheck& check = find_check(checks, "VPS");
  EXPECT_EQ(check.outcome, VBICrossCheckOutcome::kDisagreed) << check.message;
  EXPECT_NE(check.message.find("puts the capture offset at"), std::string::npos)
      << check.message;
  EXPECT_NE(check.message.find("clock run-in fitted"), std::string::npos)
      << check.message;
  EXPECT_NEAR(check.measured_samples, kTruthOffsetSamples, 1.0);
  EXPECT_DOUBLE_EQ(check.expected_samples, fitted_offset);
}

TEST(VBITimingCrossChecks, ReferenceServiceIsNotApplicableWhenItIsAbsent) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  // Teletext lines only: line 16 is covered by the capture but carries no VPS.
  std::vector<VBILineRecord> records =
      teletext_records(format, service, kTruthOffsetSamples, false);
  for (uint64_t frame = 0; frame < 6u; ++frame) {
    records.push_back(teletext_record(format, service, frame, 0,
                                      kVPSRecordIndex, kTruthOffsetSamples,
                                      false));
  }

  const std::vector<VBITimingCrossCheck> checks = run_vbi_timing_cross_checks(
      format, service, kTruthOffsetSamples, records, VBICrossCheckConfig{});

  const VBITimingCrossCheck& check = find_check(checks, "VPS");
  EXPECT_EQ(check.outcome, VBICrossCheckOutcome::kNotApplicable)
      << check.message;
  EXPECT_NE(check.message.find("No VPS signal was found"), std::string::npos)
      << check.message;
}

// A source that does not cover a service's line cannot corroborate anything
// with it, and the check says so rather than staying silent.
TEST(VBITimingCrossChecks, ReferenceServiceIsNotApplicableOutsideTheLineRange) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  const std::vector<VBITimingCrossCheck> checks = run_vbi_timing_cross_checks(
      format, service, kTruthOffsetSamples,
      teletext_records(format, service, kTruthOffsetSamples, false),
      VBICrossCheckConfig{});

  const VBITimingCrossCheck& check =
      find_check(checks, "Closed Caption 625 (broadcast line 335)");
  EXPECT_EQ(check.outcome, VBICrossCheckOutcome::kNotApplicable);
}

// ---------------------------------------------------------------------------
// End of modulation
// ---------------------------------------------------------------------------

TEST(VBITimingCrossChecks, EndOfModulationValidatesTheConfiguredBitRate) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  const std::vector<VBITimingCrossCheck> checks = run_vbi_timing_cross_checks(
      format, service, kTruthOffsetSamples,
      teletext_records(format, service, kTruthOffsetSamples, false),
      VBICrossCheckConfig{});

  const VBITimingCrossCheck& check = find_check(checks, "End of teletext");
  EXPECT_EQ(check.outcome, VBICrossCheckOutcome::kAgreed) << check.message;
  EXPECT_GE(check.records_used, 4u);

  // 360 bits at 6 937 500 bit/s from the run-in, in 8 x fsc samples.
  EXPECT_NEAR(check.expected_samples, 121.3 + 360.0 * 5.1126, 1.0);
}

TEST(VBITimingCrossChecks,
     EndOfModulationWarnsWhenThePacketIsNotTheLengthConfigured) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  // Lines whose modulation stops 40 bits early: the configured bit rate or
  // payload length does not describe this source.
  const std::vector<VBITimingCrossCheck> checks = run_vbi_timing_cross_checks(
      format, service, kTruthOffsetSamples,
      teletext_records(format, service, kTruthOffsetSamples, false, 296),
      VBICrossCheckConfig{});

  const VBITimingCrossCheck& check = find_check(checks, "End of teletext");
  EXPECT_EQ(check.outcome, VBICrossCheckOutcome::kDisagreed) << check.message;
  EXPECT_NE(check.message.find("modulation stops at sample"), std::string::npos)
      << check.message;
  EXPECT_NE(check.message.find("bit/s put the end at sample"),
            std::string::npos)
      << check.message;
  EXPECT_LT(check.measured_samples, check.expected_samples);
}

TEST(VBITimingCrossChecks, EveryCheckRunsAndNoneOfThemIsAnError) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();

  const std::vector<VBITimingCrossCheck> checks = run_vbi_timing_cross_checks(
      format, service, kTruthOffsetSamples,
      teletext_records(format, service, kTruthOffsetSamples, true),
      VBICrossCheckConfig{});

  // Burst, three reference services, end of modulation.
  EXPECT_EQ(checks.size(), 5u);
  for (const VBITimingCrossCheck& check : checks) {
    EXPECT_FALSE(check.name.empty());
    EXPECT_FALSE(check.message.empty()) << check.name;
  }
}

TEST(VBITimingCrossChecks, ColourBurstWindowFollowsTheTelevisionSystem) {
  double begin_ns = 0.0;
  double end_ns = 0.0;

  ASSERT_TRUE(vbi_colour_burst_window_ns(VBITVSystem::kPAL, begin_ns, end_ns));
  EXPECT_DOUBLE_EQ(begin_ns, 5600.0);
  EXPECT_DOUBLE_EQ(end_ns, 7850.0);

  ASSERT_TRUE(vbi_colour_burst_window_ns(VBITVSystem::kNTSC, begin_ns, end_ns));
  EXPECT_DOUBLE_EQ(begin_ns, 5300.0);
  EXPECT_DOUBLE_EQ(end_ns, 7800.0);
}

}  // namespace
}  // namespace orc
