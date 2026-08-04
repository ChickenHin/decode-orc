/*
 * File:        vbi_output_levels_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the synthesised CVBS amplitude domain
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_output_levels.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace orc {
namespace {

VBIOutputLevels pal_levels() {
  VBIOutputLevels levels;
  std::string error;
  EXPECT_TRUE(vbi_output_levels(VBITVSystem::kPAL, levels, error)) << error;
  return levels;
}

TEST(VBIOutputLevels, PALLevelsMatchTheSampleEncodingPreset) {
  const VBIOutputLevels levels = pal_levels();

  EXPECT_EQ(levels.sync_tip, 4u);
  EXPECT_EQ(levels.blanking, 256u);
  EXPECT_EQ(levels.black, 256u);
  EXPECT_EQ(levels.white, 844u);
}

// WST logic 0 is at black, logic 1 at 66 % of the black-to-white excursion,
// giving 388 counts of data amplitude.
TEST(VBIOutputLevels, PALTeletextLogicLevelsAreTheDerivedWSTLevels) {
  const VBIOutputLevels levels = pal_levels();

  EXPECT_EQ(levels.logic0, 256u);
  EXPECT_EQ(levels.logic1, 644u);
  EXPECT_EQ(levels.data_amplitude(), 388);

  // PAL has no pedestal, so black and blanking coincide and logic 0 sits on
  // both.
  EXPECT_EQ(levels.logic0, levels.black);
  EXPECT_EQ(levels.logic0, levels.blanking);
}

TEST(VBIOutputLevels, EveryPALLevelIsInsideTheProtectedBounds) {
  const VBIOutputLevels levels = pal_levels();

  for (const uint16_t level : {levels.sync_tip, levels.blanking, levels.black,
                               levels.white, levels.logic0, levels.logic1}) {
    EXPECT_GE(level, kVBIOutputSampleMin);
    EXPECT_LE(level, kVBIOutputSampleMax);
  }
}

TEST(VBIOutputLevels, FiveHundredTwentyFiveLineLevelsAreNotYetAvailable) {
  VBIOutputLevels levels;
  std::string error;

  EXPECT_FALSE(vbi_output_levels(VBITVSystem::kNTSC, levels, error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(levels.white, 0u);

  error.clear();
  EXPECT_FALSE(vbi_output_levels(VBITVSystem::kPALM, levels, error));
  EXPECT_FALSE(error.empty());
}

// The protected ranges must never be written, whatever the sample path hands
// to the writer.
TEST(VBIOutputLevels, ProtectedValuesNeverLeaveTheClamp) {
  for (int step = -8000; step <= 12000; ++step) {
    const double value = static_cast<double>(step) * 0.25;
    const uint16_t clamped = clamp_vbi_output_sample(value);
    EXPECT_GE(clamped, kVBIOutputSampleMin) << "input " << value;
    EXPECT_LE(clamped, kVBIOutputSampleMax) << "input " << value;
  }

  EXPECT_EQ(clamp_vbi_output_sample(0.0), 4u);
  EXPECT_EQ(clamp_vbi_output_sample(3.0), 4u);
  EXPECT_EQ(clamp_vbi_output_sample(1020.0), 1019u);
  EXPECT_EQ(clamp_vbi_output_sample(1023.0), 1019u);
}

// Overshoot is held at the boundary, not wrapped round it and not rescaled:
// only the overshooting samples change.
TEST(VBIOutputLevels, AnOvershootingEdgeIsClampedRatherThanWrappedOrRescaled) {
  // A sharp clock run-in edge with filter overshoot at both ends.
  const std::vector<double> edge = {256.0, 250.0, -30.0,  120.0,
                                    644.0, 900.0, 1080.0, 700.0};
  const std::vector<uint16_t> expected = {256, 250, 4,    120,
                                          644, 900, 1019, 700};

  ASSERT_EQ(edge.size(), expected.size());
  for (size_t index = 0; index < edge.size(); ++index) {
    EXPECT_EQ(clamp_vbi_output_sample(edge[index]), expected[index])
        << "sample " << index;
  }
}

TEST(VBIOutputLevels, SamplesInsideTheBandAreRoundedToTheNearestWord) {
  EXPECT_EQ(clamp_vbi_output_sample(255.4), 255u);
  EXPECT_EQ(clamp_vbi_output_sample(255.5), 256u);
  EXPECT_EQ(clamp_vbi_output_sample(256.0), 256u);
  EXPECT_EQ(clamp_vbi_output_sample(643.6), 644u);
}

// A non-finite sample is not signal; it must not become an arbitrary word.
TEST(VBIOutputLevels, NonFiniteSamplesAreHeldAtTheProtectedBoundary) {
  EXPECT_EQ(clamp_vbi_output_sample(std::numeric_limits<double>::quiet_NaN()),
            kVBIOutputSampleMin);
  EXPECT_EQ(clamp_vbi_output_sample(-std::numeric_limits<double>::infinity()),
            kVBIOutputSampleMin);
  EXPECT_EQ(clamp_vbi_output_sample(std::numeric_limits<double>::infinity()),
            kVBIOutputSampleMax);
}

}  // namespace
}  // namespace orc
