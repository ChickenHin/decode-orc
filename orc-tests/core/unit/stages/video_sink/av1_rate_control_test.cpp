/*
 * File:        av1_rate_control_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for AV1 rate-control precedence
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/sinks/common/av1_rate_control.h"

#include <gtest/gtest.h>

namespace orc_unit_test {

TEST(Av1RateControlTest, Resolve_LosslessOverridesTargetBitrate) {
  EXPECT_EQ(orc::resolve_av1_rate_control(true, 8'000'000),
            orc::Av1RateControl::kLossless);
}

TEST(Av1RateControlTest, Resolve_ZeroBitrateUsesCrfQuality) {
  EXPECT_EQ(orc::resolve_av1_rate_control(false, 0),
            orc::Av1RateControl::kCrfQuality);
}

TEST(Av1RateControlTest, Resolve_NonZeroBitrateUsesTargetBitrate) {
  EXPECT_EQ(orc::resolve_av1_rate_control(false, 8'000'000),
            orc::Av1RateControl::kTargetBitrate);
}

}  // namespace orc_unit_test
