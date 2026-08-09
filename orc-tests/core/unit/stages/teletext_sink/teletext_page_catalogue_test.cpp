/*
 * File:        teletext_page_catalogue_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the teletext sink's page catalogue
 *
 * Covers: snapshot merge and content replacement, appearance counting across
 * carousel repeats and rolling headers, subtitle-flag stickiness, page and
 * sub-page ordering, multi-page sets, and the sub-page caps. Pure in-memory
 * accumulation; no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_page_catalogue.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace orc_unit_test {

namespace {

// A snapshot of one appearance of a page, identified by the field its header
// arrived in — which is what tells one appearance from the next.
orc::TeletextPageSnapshot make_snapshot(int magazine, int page_number,
                                        int64_t header_field, int subcode = 0) {
  orc::TeletextPageSnapshot snapshot;
  snapshot.magazine = magazine;
  snapshot.page_number = page_number;
  snapshot.subcode = subcode;
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

// The page of a catalogue entry that is not a multi-page set.
const orc::TeletextPageSnapshot& only_subpage(
    const orc::TeletextCataloguedPage& page) {
  EXPECT_EQ(page.subpages.size(), 1u);
  return page.subpages.at(0).page;
}

// A run's per-field packet record: |counts[i]| packets recovered in field i.
std::vector<uint8_t> field_counts(std::initializer_list<int> counts) {
  std::vector<uint8_t> out;
  out.reserve(counts.size());
  for (const int count : counts) {
    out.push_back(static_cast<uint8_t>(count));
  }
  return out;
}

// The one sub-page of a page that is not a multi-page set.
const orc::TeletextCataloguedSubPage& only_sub_entry(
    const orc::TeletextCataloguedPage& page) {
  EXPECT_EQ(page.subpages.size(), 1u);
  return page.subpages.at(0);
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
  EXPECT_EQ(row_1_of(only_subpage(pages[0]), 5), "WORLD");
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
  EXPECT_EQ(row_1_of(only_subpage(pages[0]), 5), "WHOLE");
  EXPECT_TRUE(only_subpage(pages[0]).transmission_complete);
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
  orc::TeletextPageCatalogue catalogue(/*max_subpages=*/2);

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
  orc::TeletextPageCatalogue catalogue(/*max_subpages=*/2);
  catalogue.merge(make_snapshot(1, 0x01, 10), 1);
  catalogue.merge(make_snapshot(1, 0x02, 20), 2);

  EXPECT_EQ(catalogue.size(), 2u);
  EXPECT_FALSE(catalogue.truncated());
}

// ---------------------------------------------------------------------------
// Multi-page sets (ETSI EN 300 706 Annex A.1)
// ---------------------------------------------------------------------------

// A page number can carry a sequence of sub-pages the service cycles through,
// told apart by the sub-code. Keyed on the page number alone the catalogue
// would hold whichever one was transmitted last and discard the rest of the
// sequence as if they had been retransmissions of the same page.
TEST(TeletextPageCatalogue, Merge_CataloguesEachSubpageOfAMultiPageSet) {
  orc::TeletextPageCatalogue catalogue;

  auto one = make_snapshot(1, 0x50, 10, /*subcode=*/0x0001);
  set_row_1(one, "ONE  ");
  catalogue.merge(one, /*frame_id=*/5);

  auto two = make_snapshot(1, 0x50, 20, /*subcode=*/0x0002);
  set_row_1(two, "TWO  ");
  catalogue.merge(two, /*frame_id=*/6);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u) << "sub-pages share one page number";
  ASSERT_EQ(pages[0].subpages.size(), 2u);
  EXPECT_EQ(pages[0].subpages[0].subcode, 0x0001);
  EXPECT_EQ(row_1_of(pages[0].subpages[0].page, 5), "ONE  ");
  EXPECT_EQ(pages[0].subpages[1].subcode, 0x0002);
  EXPECT_EQ(row_1_of(pages[0].subpages[1].page, 5), "TWO  ");
  EXPECT_EQ(catalogue.subpage_count(), 2u);
}

// Annex A.1 numbers the sub-pages of a display page sequentially from Mxx-0001,
// so ascending sub-code is the sequence the service cycles through — whatever
// order the recording happened to catch them in.
TEST(TeletextPageCatalogue, Subpages_AreOrderedBySubcode) {
  orc::TeletextPageCatalogue catalogue;
  catalogue.merge(make_snapshot(1, 0x50, 10, 0x0003), 1);
  catalogue.merge(make_snapshot(1, 0x50, 20, 0x0001), 2);
  catalogue.merge(make_snapshot(1, 0x50, 30, 0x0002), 3);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  ASSERT_EQ(pages[0].subpages.size(), 3u);
  EXPECT_EQ(pages[0].subpages[0].subcode, 0x0001);
  EXPECT_EQ(pages[0].subpages[1].subcode, 0x0002);
  EXPECT_EQ(pages[0].subpages[2].subcode, 0x0003);
}

// Each sub-page is a page in its own right, so it counts its own appearances
// and the frames it — not its siblings — was seen at. The page-level figures
// cover the whole set, so a table of page numbers reads the same whether or
// not a page turns out to be a multi-page set.
TEST(TeletextPageCatalogue, Merge_CountsAppearancesPerSubpageAndPerPage) {
  orc::TeletextPageCatalogue catalogue;

  catalogue.merge(make_snapshot(1, 0x50, 10, 0x0001), /*frame_id=*/5);
  catalogue.merge(make_snapshot(1, 0x50, 20, 0x0002), /*frame_id=*/6);
  // A second cycle of the carousel: sub-page 1 comes round again.
  catalogue.merge(make_snapshot(1, 0x50, 200, 0x0001), /*frame_id=*/100);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  ASSERT_EQ(pages[0].subpages.size(), 2u);
  EXPECT_EQ(pages[0].subpages[0].times_seen, 2u);
  EXPECT_EQ(pages[0].subpages[0].first_seen_frame, 5u);
  EXPECT_EQ(pages[0].subpages[0].last_seen_frame, 100u);
  EXPECT_EQ(pages[0].subpages[1].times_seen, 1u);
  EXPECT_EQ(pages[0].subpages[1].first_seen_frame, 6u);
  EXPECT_EQ(pages[0].subpages[1].last_seen_frame, 6u);

  EXPECT_EQ(pages[0].times_seen, 3u);
  EXPECT_EQ(pages[0].first_seen_frame, 5u);
  EXPECT_EQ(pages[0].last_seen_frame, 100u);
}

// C6 says which page carries the subtitles, and the service is free to set it
// on whichever sub-page happens to be carrying a caption.
TEST(TeletextPageCatalogue, Merge_SubtitleFlagBelongsToThePageNotTheSubpage) {
  orc::TeletextPageCatalogue catalogue;

  catalogue.merge(make_snapshot(8, 0x88, 10, 0x0001), 1);
  auto with_c6 = make_snapshot(8, 0x88, 20, 0x0002);
  with_c6.subtitle = true;
  catalogue.merge(with_c6, 2);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_TRUE(pages[0].subtitle);
}

// The cap bounds sub-pages, because that is what holds a page grid each. A
// page number stays in the catalogue only for the sub-pages it still has.
TEST(TeletextPageCatalogue, Merge_DropsTheLeastRecentlySeenSubpageAtTheCap) {
  orc::TeletextPageCatalogue catalogue(/*max_subpages=*/2);

  catalogue.merge(make_snapshot(1, 0x01, 10, 0x0001), 1);
  catalogue.merge(make_snapshot(1, 0x02, 20, 0x0001), 2);
  // Touch 101 so 102's only sub-page becomes the oldest, then add a third.
  catalogue.merge(make_snapshot(1, 0x01, 30, 0x0001), 3);
  catalogue.merge(make_snapshot(1, 0x01, 40, 0x0002), 4);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 1u) << "page 102 lost its last sub-page";
  EXPECT_EQ(pages[0].page_number, 0x01);
  EXPECT_EQ(pages[0].subpages.size(), 2u);
  EXPECT_EQ(catalogue.subpage_count(), 2u);
  EXPECT_TRUE(catalogue.truncated());
}

// A page whose sub-codes churn — a misdecoded header degrades its sub-code
// nibbles to zero — must lose its own oldest sub-pages rather than crowd the
// rest of the service out of the catalogue.
TEST(TeletextPageCatalogue, Merge_CapsTheSubpagesOfOnePage) {
  orc::TeletextPageCatalogue catalogue(/*max_subpages=*/16,
                                       /*max_subpages_per_page=*/2);

  catalogue.merge(make_snapshot(1, 0x01, 10, 0x0001), 1);
  catalogue.merge(make_snapshot(1, 0x01, 20, 0x0002), 2);
  catalogue.merge(make_snapshot(1, 0x01, 30, 0x0003), 3);
  catalogue.merge(make_snapshot(1, 0x02, 40, 0x0000), 4);

  const auto pages = catalogue.pages();
  ASSERT_EQ(pages.size(), 2u) << "the other page kept its place";
  ASSERT_EQ(pages[0].subpages.size(), 2u);
  EXPECT_EQ(pages[0].subpages[0].subcode, 0x0002);
  EXPECT_EQ(pages[0].subpages[1].subcode, 0x0003);
  EXPECT_TRUE(catalogue.truncated());
}

////////////////////////////////////////////////////////////////////////////////////////////
// Per-page lost-packet count
//
// A row the carousel brought round N times should have arrived N times; the
// copies actually combined say how many did. This is the only sound per-page
// count available: a service inserting on several VBI lines interleaves its
// magazines, so a field that came back short is short for whichever page that
// packet belonged to, and which one is not knowable.

namespace {

// A catalogued sub-page seen |times_seen| times, whose display rows arrived
// |copies| times each (index 0 of |copies| is display row 1).
orc::TeletextCataloguedSubPage seen_page(uint64_t times_seen,
                                         std::initializer_list<int> copies) {
  orc::TeletextCataloguedSubPage subpage;
  subpage.times_seen = times_seen;
  size_t row = 1;
  for (const int count : copies) {
    subpage.page.row_copies[row] = count;
    subpage.page.row_received[row] = count > 0;
    ++row;
  }
  return subpage;
}

}  // namespace

// Every row arrived on every appearance: nothing was lost.
TEST(TeletextSubPageLostPackets, AreZeroWhenEveryRowArrivedEveryTime) {
  const auto subpage = seen_page(/*times_seen=*/8, {8, 8, 8});
  EXPECT_EQ(orc::teletext_subpage_lost_packets(subpage), 0u);
}

// A row that arrived on six of eight appearances was lost twice, and the
// shortfalls sum over the page's rows.
TEST(TeletextSubPageLostPackets, CountTheShortfallAgainstTheAppearances) {
  const auto subpage = seen_page(/*times_seen=*/8, {8, 6, 5});
  EXPECT_EQ(orc::teletext_subpage_lost_packets(subpage), 5u);
}

// A row that never arrived at all is either one the service never sent — which
// most pages do, to space themselves out — or one lost every single time, and
// nothing here can tell those apart. Counting it would accuse every page of
// losing the blank rows it never carried.
TEST(TeletextSubPageLostPackets, IgnoreRowsThatNeverArrived) {
  const auto subpage = seen_page(/*times_seen=*/8, {8, 0, 0, 8});
  EXPECT_EQ(orc::teletext_subpage_lost_packets(subpage), 0u);
}

// A page seen once has nothing to compare against: every row it carried
// arrived once, and the rows it did not are unknowable. Silence is the only
// honest answer, and it is exactly where a gap is most likely to be a loss —
// so the figure is a floor, not a measurement.
TEST(TeletextSubPageLostPackets, AreZeroForAPageSeenOnce) {
  const auto subpage = seen_page(/*times_seen=*/1, {1, 0, 1});
  EXPECT_EQ(orc::teletext_subpage_lost_packets(subpage), 0u);
}

// More copies than appearances is not negative loss: a row re-sent inside one
// transmission is squashed as another copy.
TEST(TeletextSubPageLostPackets, ClampAtZeroOnMoreCopiesThanAppearances) {
  const auto subpage = seen_page(/*times_seen=*/4, {9, 4});
  EXPECT_EQ(orc::teletext_subpage_lost_packets(subpage), 0u);
}

// The header row carries a live clock and is never squashed, so its copy count
// is always zero and says nothing about loss.
TEST(TeletextSubPageLostPackets, IgnoreTheHeaderRow) {
  auto subpage = seen_page(/*times_seen=*/8, {8, 8});
  subpage.page.row_copies[0] = 0;
  EXPECT_EQ(orc::teletext_subpage_lost_packets(subpage), 0u);
}

}  // namespace orc_unit_test
