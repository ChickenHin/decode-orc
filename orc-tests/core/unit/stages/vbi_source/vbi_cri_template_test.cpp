/*
 * File:        vbi_cri_template_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for clock run-in and framing code templates
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_cri_template.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "vbi_source_format.h"
#include "vbi_teletext_service.h"

namespace orc {
namespace {

// The two sampling rates the same generator has to serve: the bt8x8 card's
// 8 x fsc, in which calibration happens, and the 4 x fsc output lattice.
constexpr double kCardSampleRateHz = 35468950.0;
constexpr double kOutputSampleRateHz = 17734475.0;

VBITeletextService wst_service() {
  VBITeletextService service;
  std::string error;
  EXPECT_TRUE(vbi_teletext_service(VBITVSystem::kPAL, VBITeletextSystem::kWST,
                                   service, error))
      << error;
  return service;
}

VBICRITemplate build(double sample_rate_hz,
                     VBICRITemplateConfig config = VBICRITemplateConfig{}) {
  VBICRITemplate result;
  std::string error;
  EXPECT_TRUE(make_vbi_cri_frc_template(wst_service(), sample_rate_hz, config,
                                        result, error))
      << error;
  return result;
}

TEST(VBICRITemplate, SpansTheRunInAndFramingCodeAtTheSourceRate) {
  const VBICRITemplate cri_template = build(kCardSampleRateHz);

  EXPECT_DOUBLE_EQ(cri_template.sample_rate_hz, kCardSampleRateHz);
  EXPECT_DOUBLE_EQ(cri_template.bit_rate_hz, 6937500.0);
  EXPECT_EQ(cri_template.bit_count, 24u);

  // 35 468 950 / 6 937 500.
  EXPECT_NEAR(cri_template.samples_per_bit, 5.1126, 1e-4);

  // One bit of lead-in by default, and the pattern's first bit is a one, so
  // the anchor sits exactly one bit period into the template.
  EXPECT_NEAR(cri_template.anchor_samples, cri_template.samples_per_bit, 1e-9);

  // Lead-in plus 24 bits, rounded up to whole samples.
  EXPECT_EQ(
      cri_template.size(),
      static_cast<size_t>(std::ceil(25.0 * cri_template.samples_per_bit)));
}

TEST(VBICRITemplate, IsHeldAtZeroMeanAndUnitNorm) {
  const VBICRITemplate cri_template = build(kCardSampleRateHz);

  double total = 0.0;
  double energy = 0.0;
  for (const double value : cri_template.samples) {
    total += value;
    energy += value * value;
  }

  EXPECT_NEAR(total, 0.0, 1e-9);
  EXPECT_NEAR(energy, 1.0, 1e-9);
  EXPECT_NEAR(vbi_template_autocorrelation(cri_template, 0.0), 1.0, 1e-9);
}

// The whole reason the framing code is in the template: an alternating run-in
// correlates just as well against itself shifted by two bit periods, which is
// five samples of positional uncertainty at 4 x fsc and quite enough to place
// the data in the wrong byte phase (design §5.3.1).
TEST(VBICRITemplate, FramingCodeBreaksTheTwoBitAmbiguityTheRunInAloneHas) {
  const VBITeletextService service = wst_service();
  const VBICRITemplateConfig config;

  VBICRITemplate run_in_only;
  std::string error;
  ASSERT_TRUE(make_vbi_pattern_template(0xAAAAu, service.cri_bits,
                                        service.bit_rate_hz, kCardSampleRateHz,
                                        config, run_in_only, error))
      << error;

  const VBICRITemplate combined = build(kCardSampleRateHz);

  const double two_bits = 2.0 * combined.samples_per_bit;
  const double run_in_sidelobe =
      vbi_template_autocorrelation(run_in_only, two_bits);
  const double combined_sidelobe =
      vbi_template_autocorrelation(combined, two_bits);

  // The run-in alone all but reproduces itself two bits away; what stops it
  // reaching unity is only the two bit periods of itself it has shifted off
  // the end.
  EXPECT_GT(run_in_sidelobe, 0.7);

  // With the framing code included the same shift is no longer a match, so the
  // main peak stands clear of it and the position is absolute.
  EXPECT_LT(combined_sidelobe, 0.6);
  EXPECT_GT(run_in_sidelobe - combined_sidelobe, 0.2);
}

TEST(VBICRITemplate, IsGeneratedCorrectlyAtBothTheCardAndOutputSamplingRates) {
  const VBICRITemplate card = build(kCardSampleRateHz);
  const VBICRITemplate output = build(kOutputSampleRateHz);

  EXPECT_NEAR(output.samples_per_bit, 2.5563, 1e-4);
  EXPECT_NEAR(card.samples_per_bit / output.samples_per_bit, 2.0, 1e-9);

  // The anchor is the same instant in both, so it scales with the rate.
  EXPECT_NEAR(card.anchor_samples / output.anchor_samples, 2.0, 1e-9);

  // The framing code disambiguates at the lower rate too, where there are
  // barely two and a half samples per bit to work with.
  EXPECT_LT(vbi_template_autocorrelation(output, 2.0 * output.samples_per_bit),
            0.6);
}

TEST(VBICRITemplate, PatternsStartingWithZeroAnchorAtTheirFirstOneBit) {
  // The closed-caption run-in used as an independent timing reference opens
  // with a zero bit; every service's 0H offset is measured to the first one.
  VBICRITemplate caption;
  std::string error;
  ASSERT_TRUE(make_vbi_pattern_template(0x5551u, 16u, 500000.0,
                                        kCardSampleRateHz,
                                        VBICRITemplateConfig{}, caption, error))
      << error;

  EXPECT_NEAR(caption.anchor_samples, 2.0 * caption.samples_per_bit, 1e-9);
}

TEST(VBICRITemplate, RejectsConfigurationsThatCannotProduceAPattern) {
  VBICRITemplate result;
  std::string error;

  EXPECT_FALSE(
      make_vbi_pattern_template(0xAAAAE4u, 0u, 6937500.0, kCardSampleRateHz,
                                VBICRITemplateConfig{}, result, error));
  EXPECT_NE(error.find("bits"), std::string::npos) << error;

  EXPECT_FALSE(make_vbi_pattern_template(0xAAAAE4u, 24u, 0.0, kCardSampleRateHz,
                                         VBICRITemplateConfig{}, result,
                                         error));
  EXPECT_NE(error.find("bit rate"), std::string::npos) << error;

  // Below two samples per bit the run-in's alternation is not represented at
  // all, so there is nothing to correlate against.
  EXPECT_FALSE(make_vbi_pattern_template(0xAAAAE4u, 24u, 6937500.0, 10000000.0,
                                         VBICRITemplateConfig{}, result,
                                         error));
  EXPECT_NE(error.find("samples per bit"), std::string::npos) << error;

  // A pattern with no transitions locates nothing.
  EXPECT_FALSE(make_vbi_pattern_template(0u, 24u, 6937500.0, kCardSampleRateHz,
                                         VBICRITemplateConfig{}, result,
                                         error));
  EXPECT_NE(error.find("transitions"), std::string::npos) << error;
}

}  // namespace
}  // namespace orc
