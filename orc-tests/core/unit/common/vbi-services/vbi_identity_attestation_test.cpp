/*
 * File:        vbi_identity_attestation_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the identity-attestation neighbour test and its
 *              reported summary
 *
 * Covers: what counts as a single-digit misreading, when several explanations
 * make one unusable, the width guard, and whether the summary says anything.
 * Pure value logic; no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi-services/vbi_identity_attestation.h"

#include <gtest/gtest.h>

#include <vector>

namespace orc_unit_test {
namespace {

using orc::VbiIdentityDigits;
using orc::VbiIdentityReconciliation;

TEST(VbiSingleDigitNeighbour, FindTheOneIdentityADigitAway) {
  const std::vector<VbiIdentityDigits> attested = {
      {0, 0, 0, 0, 0, 9},
      {0, 0, 0, 0, 2, 1},
  };
  // 0x7 in place of 0x0, which is the mis-correction Hamming 8/4 makes of an
  // alternating error run over codeword 0.
  const auto found =
      orc::vbi_single_digit_neighbour({0, 0, 0, 7, 0, 9}, attested);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, 0u);
}

TEST(VbiSingleDigitNeighbour, RefuseWhenTwoIdentitiesFitEqually) {
  // A service carrying both 003 and 007 explains a misread 005 twice over, and
  // neither explanation is evidence of anything.
  const std::vector<VbiIdentityDigits> attested = {
      {0, 0, 3},
      {0, 0, 7},
  };
  EXPECT_FALSE(
      orc::vbi_single_digit_neighbour({0, 0, 5}, attested).has_value());
}

TEST(VbiSingleDigitNeighbour, RefuseWhenNothingIsClose) {
  const std::vector<VbiIdentityDigits> attested = {{0, 0, 9}};
  // Two digits apart is two independent bursts inside one header, which is
  // possible but no longer points anywhere in particular.
  EXPECT_FALSE(
      orc::vbi_single_digit_neighbour({7, 0, 7}, attested).has_value());
}

TEST(VbiSingleDigitNeighbour, RefuseAnIdenticalIdentity) {
  const std::vector<VbiIdentityDigits> attested = {{0, 0, 9}};
  EXPECT_FALSE(
      orc::vbi_single_digit_neighbour({0, 0, 9}, attested).has_value());
}

TEST(VbiSingleDigitNeighbour, IgnoreIdentitiesOfAnotherWidth) {
  // A short and a long NABTS address are different shapes, not near misses.
  const std::vector<VbiIdentityDigits> attested = {{0, 0, 9}, {0, 0, 9, 0}};
  const auto found = orc::vbi_single_digit_neighbour({0, 7, 9}, attested);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, 0u);
}

TEST(VbiSingleDigitNeighbour, AcceptAnEmptyAttestedSet) {
  EXPECT_FALSE(orc::vbi_single_digit_neighbour({0, 0, 9}, {}).has_value());
}

TEST(VbiIdentityReconciliationApplies, NeedsOneAttestedIdentity) {
  EXPECT_FALSE(orc::vbi_identity_reconciliation_applies(0));
  EXPECT_TRUE(orc::vbi_identity_reconciliation_applies(1));
}

TEST(VbiIdentityReconciliationSummary, SaysNothingWhenNothingHappened) {
  VbiIdentityReconciliation reconciliation;
  reconciliation.identities_seen = 12;
  EXPECT_FALSE(reconciliation.acted());
  EXPECT_TRUE(reconciliation.summary("page").empty());
}

TEST(VbiIdentityReconciliationSummary, SaysSoWhenTheRuleStoodAside) {
  VbiIdentityReconciliation reconciliation;
  reconciliation.identities_seen = 4;
  reconciliation.identities_unattested = 4;
  reconciliation.withheld = true;
  // Not acted() — nothing was removed — but a reader still has to be told the
  // catalogue was left as recovered rather than judged and passed.
  EXPECT_FALSE(reconciliation.acted());
  EXPECT_NE(reconciliation.summary("record").find("left exactly as recovered"),
            std::string::npos);
}

TEST(VbiIdentityReconciliationSummary, ReportsBothOutcomes) {
  VbiIdentityReconciliation reconciliation;
  reconciliation.identities_seen = 600;
  reconciliation.identities_unattested = 480;
  reconciliation.identities_folded = 200;
  reconciliation.identities_dropped = 280;
  reconciliation.appearances_folded = 900;
  reconciliation.appearances_dropped = 310;

  const std::string summary = reconciliation.summary("record");
  EXPECT_NE(summary.find("480 of 600 record identities"), std::string::npos);
  EXPECT_NE(summary.find("200"), std::string::npos);
  EXPECT_NE(summary.find("280"), std::string::npos);
}

}  // namespace
}  // namespace orc_unit_test
