/*
 * File:        teletext_squash_stats_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the teletext squash statistics
 *
 * Covers: the parity reading either side of the rewrite, the copies-per-row
 * distribution, the single-copy notice, and the empty case. Hand-built rows
 * only; deterministic, no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_squash_stats.h"

#include <gtest/gtest.h>

#include <string>

#include "vbi-services/teletext_page_decoder.h"

// using different namespace from module-under-test so that we can use the same
// class names in the tests as in the module-under-test
namespace orc_unit_test {

namespace {

// 40 odd-parity display bytes carrying |text|, space-padded.
orc::TeletextRowBytes make_row(const std::string& text) {
  orc::TeletextRowBytes row{};
  for (size_t i = 0; i < orc::kTeletextRowBytes; ++i) {
    const char c = i < text.size() ? text[i] : ' ';
    row[i] = orc::teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return row;
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////////////////

TEST(TeletextSquashStats, Summary_SaysNothingHappenedWithNoRows) {
  orc::TeletextSquashStats stats;
  EXPECT_EQ(stats.summary(), "Teletext squashing: no row packets to combine");
  EXPECT_TRUE(stats.character_loss_summary().empty());
}

// The headline: how much of what came out is known damaged, in characters a
// reader can weigh without knowing anything about parity or voting.
TEST(TeletextSquashStats, CharacterLoss_ReportsDamagedOfRecovered) {
  auto clean = make_row("HELLO");
  auto two_damaged = clean;
  two_damaged[0] ^= 0x01;
  two_damaged[5] ^= 0x01;

  orc::TeletextSquashStats stats;
  // One row the vote mended entirely, one it could not touch.
  stats.add_row(two_damaged, clean, /*copies=*/4);
  stats.add_row(two_damaged, two_damaged, /*copies=*/1);

  EXPECT_EQ(stats.bytes_total(), 2 * orc::kTeletextRowBytes);
  EXPECT_EQ(stats.parity_failures_before(), 4u);
  EXPECT_EQ(stats.parity_failures_after(), 2u);

  const std::string headline = stats.character_loss_summary();
  EXPECT_NE(headline.find("Data loss 2.50%"), std::string::npos) << headline;
  EXPECT_NE(headline.find("2 of 80 recovered characters are damaged"),
            std::string::npos)
      << headline;
  EXPECT_NE(headline.find("mended 2 of the 4 characters that arrived damaged"),
            std::string::npos)
      << headline;
  EXPECT_NE(headline.find("without it the loss would be 5.00%"),
            std::string::npos)
      << headline;
}

// With nothing mended there is no before-and-after to draw, so the headline
// stays one sentence rather than claiming a gain of zero.
TEST(TeletextSquashStats, CharacterLoss_OmitsTheMendedClauseWhenNothingMoved) {
  auto clean = make_row("HELLO");
  auto damaged = clean;
  damaged[0] ^= 0x01;

  orc::TeletextSquashStats stats;
  stats.add_row(damaged, damaged, /*copies=*/1);

  const std::string headline = stats.character_loss_summary();
  EXPECT_NE(headline.find("Data loss 2.50%"), std::string::npos) << headline;
  EXPECT_EQ(headline.find("mended"), std::string::npos) << headline;
}

TEST(TeletextSquashStats, CharacterLoss_GroupsLargeCountsInThrees) {
  const auto row = make_row("X");
  orc::TeletextSquashStats stats;
  for (int i = 0; i < 300; ++i) {
    stats.add_row(row, row, /*copies=*/2);
  }
  // 300 rows × 40 bytes = 12,000 characters.
  EXPECT_NE(stats.character_loss_summary().find("12,000"), std::string::npos)
      << stats.character_loss_summary();
}

// A row written without the rewrite pass still contributes its characters —
// the headline has to mean the same thing whether or not rows were combined —
// but there was no vote, so it says nothing about copies.
TEST(TeletextSquashStats, WrittenRowsCountCharactersButNotCopies) {
  auto clean = make_row("HELLO");
  auto damaged = clean;
  damaged[0] ^= 0x01;

  orc::TeletextSquashStats stats;
  stats.add_written_row(damaged);

  EXPECT_EQ(stats.rows(), 1u);
  EXPECT_EQ(stats.rows_attributed(), 0u);
  EXPECT_EQ(stats.rows_rewritten(), 0u);
  EXPECT_EQ(stats.bytes_total(), orc::kTeletextRowBytes);
  EXPECT_EQ(stats.parity_failures_before(), 1u);
  EXPECT_EQ(stats.parity_failures_after(), 1u);
  for (size_t bucket = 0; bucket < orc::TeletextSquashStats::kCopyBuckets;
       ++bucket) {
    EXPECT_EQ(stats.copies_in_bucket(bucket), 0u) << "bucket " << bucket;
  }
  EXPECT_NE(stats.character_loss_summary().find("Data loss 2.50%"),
            std::string::npos);
  // With no attributed row there is no distribution to print.
  EXPECT_EQ(stats.summary().find("Copies per row packet"), std::string::npos)
      << stats.summary();
}

// The parity count either side of the rewrite is the reading: a byte that
// fails odd parity is known to be damaged (ETSI EN 300 706 §8.1), so fewer
// failures after the vote is damage the vote removed.
TEST(TeletextSquashStats, ParityFailuresFallWhenTheVoteMendsAByte) {
  const auto clean = make_row("HELLO");
  auto damaged = clean;
  damaged[0] ^= 0x01;
  ASSERT_FALSE(orc::teletext_odd_parity_valid(damaged[0]));

  orc::TeletextSquashStats stats;
  stats.add_row(damaged, clean, /*copies=*/3);

  EXPECT_EQ(stats.rows(), 1u);
  EXPECT_EQ(stats.rows_rewritten(), 1u);
  EXPECT_EQ(stats.bytes_changed(), 1u);
  EXPECT_EQ(stats.bytes_total(), orc::kTeletextRowBytes);
  EXPECT_EQ(stats.parity_failures_before(), 1u);
  EXPECT_EQ(stats.parity_failures_after(), 0u);

  const std::string summary = stats.summary();
  EXPECT_NE(summary.find("1 rewritten (100.0%)"), std::string::npos) << summary;
  EXPECT_NE(summary.find("Odd-parity failures: 1 before"), std::string::npos)
      << summary;
  EXPECT_NE(summary.find("3-7 copies 1"), std::string::npos) << summary;
}

TEST(TeletextSquashStats, AnUnchangedRowIsNotCountedAsRewritten) {
  const auto row = make_row("HELLO");

  orc::TeletextSquashStats stats;
  stats.add_row(row, row, /*copies=*/4);

  EXPECT_EQ(stats.rows(), 1u);
  EXPECT_EQ(stats.rows_rewritten(), 0u);
  EXPECT_EQ(stats.bytes_changed(), 0u);
  EXPECT_EQ(stats.parity_failures_before(), stats.parity_failures_after());
}

// A row transmitted once cannot be corrected however good the vote is, so the
// distribution has to separate "nothing to combine" from "combined and
// unchanged" — they look identical in the rewritten count.
TEST(TeletextSquashStats, SingleCopyRowsAreCalledOutInTheSummary) {
  const auto row = make_row("ONLY ONCE");

  orc::TeletextSquashStats stats;
  stats.add_row(row, row, /*copies=*/1);
  stats.add_row(row, row, /*copies=*/1);
  stats.set_page_runs(2);

  const std::string summary = stats.summary();
  EXPECT_NE(summary.find("2 row packets over 2 page runs"), std::string::npos)
      << summary;
  EXPECT_NE(summary.find("1 copy 2 (100.0%)"), std::string::npos) << summary;
  EXPECT_NE(summary.find("No row was transmitted more than once"),
            std::string::npos)
      << summary;
}

TEST(TeletextSquashStats, CopyCountsLandInTheRightBuckets) {
  const auto row = make_row("X");

  orc::TeletextSquashStats stats;
  stats.add_row(row, row, 1);
  stats.add_row(row, row, 2);
  stats.add_row(row, row, 7);
  stats.add_row(row, row, 8);
  stats.add_row(row, row, 40);

  EXPECT_EQ(stats.copies_in_bucket(0), 1u);
  EXPECT_EQ(stats.copies_in_bucket(1), 1u);
  EXPECT_EQ(stats.copies_in_bucket(2), 1u);
  EXPECT_EQ(stats.copies_in_bucket(3), 2u);
  EXPECT_EQ(stats.copies_in_bucket(99), 0u) << "out of range reads as zero";
}

// The parity-first rule stops the vote preferring a damaged byte to a clean
// one, but it cannot help when every copy of a position is damaged. Saying so
// keeps a reader from taking a rise in failures for a bug.
TEST(TeletextSquashStats, ARiseInParityFailuresIsExplained) {
  const auto clean = make_row("HELLO");
  auto damaged = clean;
  damaged[0] ^= 0x01;

  orc::TeletextSquashStats stats;
  stats.add_row(clean, damaged, /*copies=*/2);

  EXPECT_EQ(stats.parity_failures_before(), 0u);
  EXPECT_EQ(stats.parity_failures_after(), 1u);
  EXPECT_NE(stats.summary().find("found no parity-clean copy"),
            std::string::npos)
      << stats.summary();
}

}  // namespace orc_unit_test
