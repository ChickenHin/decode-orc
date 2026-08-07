/*
 * File:        teletext_page_catalogue_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the teletext analysis sink's page catalogue
 *
 * Covers: snapshot merge and content replacement, appearance counting across
 * carousel repeats and rolling headers, subtitle-flag stickiness, page
 * ordering, and the page cap. Pure in-memory accumulation; no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_page_catalogue.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace orc_unit_test {

namespace {

// A snapshot of one appearance of a page, identified by the field its header
// arrived in — which is what tells one appearance from the next.
orc::TeletextPageSnapshot make_snapshot(int magazine, int page_number,
                                        int64_t header_field) {
  orc::TeletextPageSnapshot snapshot;
  snapshot.magazine = magazine;
  snapshot.page_number = page_number;
  snapshot.header_field_index = header_field;
  snapshot.last_field_index = header_field + 4;
  return snapshot;
}

// Write |text| into row 1 of a snapshot so a merge can be seen to replace the
// page's content.
void set_row_1(orc::TeletextPageSnapshot& snapshot, const std::string& text) {
  snapshot.row_received[1] = true;
  for (size_t column = 0; column < text.size(); ++column) {
    snapshot.cells[1][column].character = static_cast<uint8_t>(text[column]);
  }
}

std::string row_1_of(const orc::TeletextPageSnapshot& snapshot, size_t length) {
  std::string text;
  for (size_t column = 0; column < length; ++column) {
    text.push_back(static_cast<char>(snapshot.cells[1][column].character));
  }
  return text;
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////////////////

TEST(TeletextPageCatalogue, Merge_RecordsAPagesIdentityAndFirstFrame) {
  orc::TeletextPageCatalogue catalogue;
  catalogue.merge(make_snapshot(1, 0x23, 40), /*frame_id=*/20);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].magazine, 1);
  EXPECT_EQ(pages[0].page_number, 0x23);
  EXPECT_EQ(pages[0].first_seen_frame, 20u);
  EXPECT_EQ(pages[0].last_seen_frame, 20u);
  EXPECT_EQ(pages[0].times_seen, 1u);
  EXPECT_FALSE(pages[0].subtitle);
  EXPECT_FALSE(catalogue.truncated());
}

// The carousel brings a page round repeatedly; each distinct header is another
// appearance, and the newest assembly replaces the entry's content because a
// squasher-backed decoder renders it from every copy recovered so far.
TEST(TeletextPageCatalogue, Merge_CountsCarouselRepeatsAndTakesTheNewestPage) {
  orc::TeletextPageCatalogue catalogue;

  auto first = make_snapshot(1, 0x00, 10);
  set_row_1(first, "HELLO");
  catalogue.merge(first, /*frame_id=*/5);

  auto second = make_snapshot(1, 0x00, 600);
  set_row_1(second, "WORLD");
  catalogue.merge(second, /*frame_id=*/300);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].times_seen, 2u);
  EXPECT_EQ(pages[0].first_seen_frame, 5u);
  EXPECT_EQ(pages[0].last_seen_frame, 300u);
  EXPECT_EQ(row_1_of(pages[0].page, 5), "WORLD");
}

// A rolling header (ETSI EN 300 706 §9.3.1.4) re-emits the page mid
// transmission under its original header field index. That is the same
// appearance, not another one.
TEST(TeletextPageCatalogue, Merge_DoesNotCountARollingHeaderTwice) {
  orc::TeletextPageCatalogue catalogue;

  auto partial = make_snapshot(1, 0x00, 10);
  partial.transmission_complete = false;
  set_row_1(partial, "HALF ");
  catalogue.merge(partial, /*frame_id=*/5);

  auto complete = make_snapshot(1, 0x00, 10);
  set_row_1(complete, "WHOLE");
  catalogue.merge(complete, /*frame_id=*/6);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].times_seen, 1u);
  // The completed assembly still replaces the fragment.
  EXPECT_EQ(row_1_of(pages[0].page, 5), "WHOLE");
  EXPECT_TRUE(pages[0].page.transmission_complete);
}

// C6 is the service saying which page carries the subtitles. A service may
// drop it between captions, so the flag is sticky: a marker that blinked out
// whenever nothing was on screen would be worse than useless for finding it.
TEST(TeletextPageCatalogue, Merge_KeepsTheSubtitleFlagOnceSeen) {
  orc::TeletextPageCatalogue catalogue;

  auto with_c6 = make_snapshot(8, 0x88, 10);
  with_c6.subtitle = true;
  catalogue.merge(with_c6, /*frame_id=*/5);

  catalogue.merge(make_snapshot(8, 0x88, 600), /*frame_id=*/300);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_TRUE(pages[0].subtitle);
}

TEST(TeletextPageCatalogue, Pages_AreOrderedByAddress) {
  orc::TeletextPageCatalogue catalogue;
  catalogue.merge(make_snapshot(8, 0x88, 10), 1);
  catalogue.merge(make_snapshot(1, 0x00, 20), 2);
  catalogue.merge(make_snapshot(1, 0x0F, 30), 3);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 3u);
  EXPECT_EQ(pages[0].magazine, 1);
  EXPECT_EQ(pages[0].page_number, 0x00);
  EXPECT_EQ(pages[1].magazine, 1);
  EXPECT_EQ(pages[1].page_number, 0x0F);
  EXPECT_EQ(pages[2].magazine, 8);
  EXPECT_EQ(pages[2].page_number, 0x88);
}

// The cap is what bounds the run's memory by the size of the service rather
// than by the length of the recording; the least recently seen page goes.
TEST(TeletextPageCatalogue, Merge_DropsTheLeastRecentlySeenPageAtTheCap) {
  orc::TeletextPageCatalogue catalogue(/*max_pages=*/2);

  catalogue.merge(make_snapshot(1, 0x01, 10), 1);
  catalogue.merge(make_snapshot(1, 0x02, 20), 2);
  // Touch page 101 so page 102 becomes the oldest.
  catalogue.merge(make_snapshot(1, 0x01, 30), 3);
  catalogue.merge(make_snapshot(1, 0x03, 40), 4);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 2u);
  EXPECT_EQ(pages[0].page_number, 0x01);
  EXPECT_EQ(pages[1].page_number, 0x03);
  EXPECT_TRUE(catalogue.truncated());
}

TEST(TeletextPageCatalogue, Truncated_StaysFalseWhileUnderTheCap) {
  orc::TeletextPageCatalogue catalogue(/*max_pages=*/2);
  catalogue.merge(make_snapshot(1, 0x01, 10), 1);
  catalogue.merge(make_snapshot(1, 0x02, 20), 2);

  EXPECT_EQ(catalogue.size(), 2u);
  EXPECT_FALSE(catalogue.truncated());
}

}  // namespace orc_unit_test
