/*
 * File:        teletext_row_squasher_test.cpp
 * Module:      orc-tests/core/unit
 * Purpose:     Tests for the teletext row squasher (multi-copy combination)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/support/teletext_page_decoder.h>
#include <orc/support/teletext_row_squasher.h>

#include <string>

namespace orc {
namespace tests {
namespace {

constexpr TeletextPageKey kPage{1, 0x00, 0};

// Build a row of odd-parity display bytes from ASCII text, space-padded.
TeletextRowBytes row_of(const std::string& text) {
  TeletextRowBytes bytes{};
  for (size_t i = 0; i < kTeletextRowBytes; ++i) {
    const char c = i < text.size() ? text[i] : ' ';
    bytes[i] = teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return bytes;
}

// Read a squashed row back as text, stripping parity.
std::string text_of(const TeletextRowBytes& bytes) {
  std::string text;
  for (const uint8_t byte : bytes) {
    text.push_back(static_cast<char>(byte & 0x7F));
  }
  while (!text.empty() && text.back() == ' ') {
    text.pop_back();
  }
  return text;
}

}  // namespace

TEST(TeletextRowSquasher, SingleCopyIsReturnedUnchanged) {
  TeletextRowSquasher squasher;
  const auto row = row_of("HELLO");
  squasher.add_row(kPage, 1, row, /*source=*/0);

  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ(*squashed, row);
  EXPECT_EQ(squasher.copy_count(kPage, 1), 1u);
}

TEST(TeletextRowSquasher, UnseenRowHasNoResult) {
  TeletextRowSquasher squasher;
  EXPECT_FALSE(squasher.squashed_row(kPage, 1).has_value());
  EXPECT_EQ(squasher.copy_count(kPage, 1), 0u);
}

// The worked example from vhs-teletext's HOW_IT_WORKS: three copies of a row
// damaged in different places combine to the original.
TEST(TeletextRowSquasher, MostFrequentByteWinsAcrossCopies) {
  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 1, row_of("HELLO"), 0);
  squasher.add_row(kPage, 1, row_of("HELLP"), 1);
  squasher.add_row(kPage, 1, row_of("MELLO"), 2);

  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ(text_of(*squashed), "HELLO");
}

// Odd parity (EN 300 706 §8.1) detects every single-bit error, so a byte that
// fails it is known to be corrupt and must not win however often it appears.
TEST(TeletextRowSquasher, ParityCleanByteBeatsAMoreFrequentDamagedOne) {
  TeletextRowSquasher squasher;
  auto damaged = row_of("XELLO");
  damaged[0] ^= 0x01;  // break odd parity on the leading byte
  ASSERT_FALSE(teletext_odd_parity_valid(damaged[0]));

  squasher.add_row(kPage, 1, damaged, 0);
  squasher.add_row(kPage, 1, damaged, 1);
  squasher.add_row(kPage, 1, row_of("HELLO"), 2);

  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ(text_of(*squashed), "HELLO")
      << "the twice-seen but parity-damaged byte outvoted the clean one";
}

TEST(TeletextRowSquasher, FallsBackToPlainModeWhenEveryCopyIsDamaged) {
  TeletextRowSquasher squasher;
  auto damaged_a = row_of("A");
  damaged_a[0] ^= 0x01;
  auto damaged_b = row_of("B");
  damaged_b[0] ^= 0x01;
  ASSERT_FALSE(teletext_odd_parity_valid(damaged_a[0]));
  ASSERT_FALSE(teletext_odd_parity_valid(damaged_b[0]));

  squasher.add_row(kPage, 1, damaged_a, 0);
  squasher.add_row(kPage, 1, damaged_a, 1);
  squasher.add_row(kPage, 1, damaged_b, 2);

  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ((*squashed)[0], damaged_a[0]);  // the more frequent of the two
}

// A previewer rebuilding a sliding window re-reads the same recovered line
// many times; those repeats must not stuff the ballot.
TEST(TeletextRowSquasher, RepeatedSourceReplacesRatherThanRecounts) {
  TeletextRowSquasher squasher;
  for (int i = 0; i < 10; ++i) {
    squasher.add_row(kPage, 1, row_of("WRONG"), /*source=*/7);
  }
  squasher.add_row(kPage, 1, row_of("RIGHT"), /*source=*/8);
  squasher.add_row(kPage, 1, row_of("RIGHT"), /*source=*/9);

  EXPECT_EQ(squasher.copy_count(kPage, 1), 3u);
  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ(text_of(*squashed), "RIGHT");
}

// A different sub-code is a different page (EN 300 706 §9.3.1.2): merging
// rotating sub-pages would blend unrelated content.
TEST(TeletextRowSquasher, SubPagesAreKeptApart) {
  TeletextRowSquasher squasher;
  const TeletextPageKey sub1{1, 0x00, 1};
  const TeletextPageKey sub2{1, 0x00, 2};
  squasher.add_row(sub1, 1, row_of("SUBPAGE ONE"), 0);
  squasher.add_row(sub2, 1, row_of("SUBPAGE TWO"), 1);

  EXPECT_EQ(text_of(*squasher.squashed_row(sub1, 1)), "SUBPAGE ONE");
  EXPECT_EQ(text_of(*squasher.squashed_row(sub2, 1)), "SUBPAGE TWO");
  EXPECT_EQ(squasher.page_count(), 2u);
}

TEST(TeletextRowSquasher, OldestCopyIsDroppedBeyondTheBound) {
  TeletextRowSquasher::Options options;
  options.max_copies_per_row = 3;
  TeletextRowSquasher squasher(options);

  squasher.add_row(kPage, 1, row_of("OLD"), 0);
  squasher.add_row(kPage, 1, row_of("NEW"), 1);
  squasher.add_row(kPage, 1, row_of("NEW"), 2);
  squasher.add_row(kPage, 1, row_of("NEW"), 3);

  EXPECT_EQ(squasher.copy_count(kPage, 1), 3u);
  EXPECT_EQ(text_of(*squasher.squashed_row(kPage, 1)), "NEW");
}

TEST(TeletextRowSquasher, LeastRecentlyUpdatedPageIsEvicted) {
  TeletextRowSquasher::Options options;
  options.max_pages = 2;
  TeletextRowSquasher squasher(options);

  squasher.add_row({1, 0x00, 0}, 1, row_of("FIRST"), 0);
  squasher.add_row({1, 0x01, 0}, 1, row_of("SECOND"), 1);
  squasher.add_row({1, 0x02, 0}, 1, row_of("THIRD"), 2);

  EXPECT_EQ(squasher.page_count(), 2u);
  EXPECT_FALSE(squasher.squashed_row({1, 0x00, 0}, 1).has_value());
  EXPECT_TRUE(squasher.squashed_row({1, 0x02, 0}, 1).has_value());
}

// Row 0 is the page header, whose display bytes carry a real-time clock
// (§9.3.1.4) that legitimately differs between transmissions.
TEST(TeletextRowSquasher, HeaderAndOutOfRangeRowsAreNotStored) {
  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 0, row_of("HEADER"), 0);
  squasher.add_row(kPage, 25, row_of("ENHANCEMENT"), 1);

  EXPECT_EQ(squasher.copy_count(kPage, 0), 0u);
  EXPECT_EQ(squasher.copy_count(kPage, 25), 0u);
  EXPECT_EQ(squasher.page_count(), 0u);
}

TEST(TeletextRowSquasher, ClearDropsEverything) {
  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 1, row_of("TEXT"), 0);
  ASSERT_EQ(squasher.page_count(), 1u);

  squasher.clear();

  EXPECT_EQ(squasher.page_count(), 0u);
  EXPECT_FALSE(squasher.squashed_row(kPage, 1).has_value());
}

}  // namespace tests
}  // namespace orc
