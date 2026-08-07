/*
 * File:        teletext_analysis_presenter_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 tests for TeletextAnalysisPresenter dataset and
 *              page-view conversion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_analysis_presenter.h"

#include <gtest/gtest.h>
#include <orc/stage/analysis_sink_results.h>
#include <orc/support/teletext_page_decoder.h>

namespace gui_unit_test {

using orc::presenters::TeletextAnalysisPresenter;

TEST(TeletextAnalysisPresenterTest, PageView_MapsIdentityAndAsciiCells) {
  orc::TeletextPageSnapshot snapshot;
  snapshot.magazine = 1;
  snapshot.page_number = 0x00;
  snapshot.subcode = 0x3F7F & 0x1FFF;
  snapshot.subtitle = true;
  snapshot.header_field_index = 84;
  snapshot.last_field_index = 90;
  snapshot.cells[1][0].character = 'H';
  snapshot.cells[1][1].character = 'i';
  snapshot.cells[1][1].foreground = orc::TeletextColour::Yellow;
  snapshot.cells[1][1].background = orc::TeletextColour::Blue;
  snapshot.cells[1][2].character = 0x23;  // English (the default subset): £
  snapshot.cells[2][0].character = 'X';
  snapshot.cells[2][0].parity_error = true;

  const auto view = TeletextAnalysisPresenter::makePageView(snapshot);

  EXPECT_EQ(view.magazine, 1);
  EXPECT_EQ(view.page_number, 0x00);
  EXPECT_TRUE(view.subtitle);
  EXPECT_EQ(view.header_field_index, 84);
  EXPECT_EQ(view.last_field_index, 90);
  EXPECT_EQ(view.cells[1][0].character, U'H');
  EXPECT_FALSE(view.cells[1][0].mosaic);
  EXPECT_EQ(view.cells[1][1].character, U'i');
  EXPECT_EQ(view.cells[1][1].foreground, 3);  // yellow
  EXPECT_EQ(view.cells[1][1].background, 4);  // blue
  EXPECT_EQ(view.cells[1][2].character, U'£');
  EXPECT_TRUE(view.cells[2][0].parity_error);
}

TEST(TeletextAnalysisPresenterTest, PageView_AppliesThePagesNationalOption) {
  // The same code renders differently depending on the sub-set the page
  // header selected (EN 300 706 §15.2, §15.6.2 Table 36) — 2/3 is "£" on an
  // English service and "#" on a German one, whose 7/E is "ß".
  orc::TeletextPageSnapshot snapshot;
  snapshot.national_option_subset =
      static_cast<int>(orc::TeletextNationalOption::German);
  snapshot.cells[1][0].character = 0x23;
  snapshot.cells[1][1].character = 0x7E;

  const auto view = TeletextAnalysisPresenter::makePageView(snapshot);

  EXPECT_EQ(view.cells[1][0].character, U'#');
  EXPECT_EQ(view.cells[1][1].character, U'ß');
}

TEST(TeletextAnalysisPresenterTest, PageView_MapsMosaicSixels) {
  orc::TeletextPageSnapshot snapshot;
  auto& all_set = snapshot.cells[3][0];
  all_set.character = 0x7F;  // all six sixels set
  all_set.mosaic = true;
  auto& mixed = snapshot.cells[3][1];
  // EN 300 706 §15.7.1 Table 47: bits 1-5 then bit 7 (0x40) select sixels.
  mixed.character = 0x20 | 0x01 | 0x40;  // top-left + bottom-right
  mixed.mosaic = true;
  mixed.separated_mosaic = true;
  auto& blast_through = snapshot.cells[3][2];
  blast_through.character = 'A';  // codes 4/0-5/F stay alphanumeric
  blast_through.mosaic = true;
  auto& held = snapshot.cells[3][3];
  held.character = 0x30;
  held.mosaic = false;
  held.held_mosaic = true;

  const auto view = TeletextAnalysisPresenter::makePageView(snapshot);

  EXPECT_TRUE(view.cells[3][0].mosaic);
  EXPECT_EQ(view.cells[3][0].mosaic_pattern, 0x3F);
  EXPECT_TRUE(view.cells[3][1].mosaic);
  EXPECT_EQ(view.cells[3][1].mosaic_pattern, 0x01 | 0x20);
  EXPECT_TRUE(view.cells[3][1].mosaic_separated);
  EXPECT_FALSE(view.cells[3][2].mosaic);
  EXPECT_EQ(view.cells[3][2].character, U'A');
  EXPECT_TRUE(view.cells[3][3].mosaic);
  EXPECT_EQ(view.cells[3][3].mosaic_pattern, 0x10);
}

TEST(TeletextAnalysisPresenterTest, PageView_DrawsBlastThroughCodesAsMosaics) {
  // A page whose service has no blast-through region reads every code from 2/0
  // up as a mosaic, so the codes that would otherwise put capitals through a
  // drawing become the block patterns the drawing is made of.
  orc::TeletextPageSnapshot snapshot;
  snapshot.mosaic_blast_through = false;
  auto& solid = snapshot.cells[3][0];
  solid.character = 0x5F;  // 'link' in the 625-line reading; solid block here
  solid.mosaic = true;
  auto& partial = snapshot.cells[3][1];
  partial.character = 0x57;  // 'W' in the 625-line reading
  partial.mosaic = true;
  auto& alpha = snapshot.cells[3][2];
  alpha.character = 'A';
  alpha.mosaic = false;  // not in mosaic mode: still a letter

  const auto view = TeletextAnalysisPresenter::makePageView(snapshot);

  EXPECT_TRUE(view.cells[3][0].mosaic);
  EXPECT_EQ(view.cells[3][0].mosaic_pattern, 0x3F);
  EXPECT_TRUE(view.cells[3][1].mosaic);
  EXPECT_EQ(view.cells[3][1].mosaic_pattern, 0x37);
  EXPECT_FALSE(view.cells[3][2].mosaic);
  EXPECT_EQ(view.cells[3][2].character, U'A');
}

TEST(TeletextAnalysisPresenterTest, PageView_SummarisesRecovery) {
  orc::TeletextPageSnapshot snapshot;
  snapshot.row_received[0] = true;  // header row: not a display row
  for (int row = 1; row <= 20; ++row) {
    snapshot.row_received[static_cast<size_t>(row)] = true;
  }
  // Rows 21-24 never arrived.
  snapshot.cells[5][0].parity_error = true;
  snapshot.cells[5][1].parity_error = true;
  snapshot.cells[6][0].parity_error = true;

  const auto view = TeletextAnalysisPresenter::makePageView(snapshot);

  EXPECT_EQ(view.recovery.rows_expected, 24);
  EXPECT_EQ(view.recovery.rows_received, 20);
  EXPECT_EQ(view.recovery.damaged_bytes, 3);
  EXPECT_FALSE(view.recovery.complete());
  EXPECT_TRUE(view.row_received[20]);
  EXPECT_FALSE(view.row_received[21]);
}

// A row that arrived once is only worth flagging where the page has rows the
// carousel has corrected against a repeat: that is what makes the odd one out
// odd. It is where a row carried onto the wrong address by a burst survives.
TEST(TeletextAnalysisPresenterTest, PageView_FlagsRowsSeenOnlyOnce) {
  orc::TeletextPageSnapshot snapshot;
  for (int row = 1; row <= 5; ++row) {
    snapshot.row_received[static_cast<size_t>(row)] = true;
    snapshot.row_copies[static_cast<size_t>(row)] = 3;
  }
  snapshot.row_copies[3] = 1;  // never confirmed by a repeat

  const auto view = TeletextAnalysisPresenter::makePageView(snapshot);

  EXPECT_EQ(view.recovery.unconfirmed_rows, 1);
  EXPECT_TRUE(view.row_unconfirmed[3]);
  EXPECT_FALSE(view.row_unconfirmed[2]);
  EXPECT_FALSE(view.row_unconfirmed[6]) << "a row never received is not this";
}

// On a page seen once nothing has been confirmed, so saying so of every row
// says nothing at all.
TEST(TeletextAnalysisPresenterTest, PageView_FirstSightingFlagsNothing) {
  orc::TeletextPageSnapshot snapshot;
  for (int row = 1; row <= 5; ++row) {
    snapshot.row_received[static_cast<size_t>(row)] = true;
    snapshot.row_copies[static_cast<size_t>(row)] = 1;
  }

  const auto view = TeletextAnalysisPresenter::makePageView(snapshot);

  EXPECT_EQ(view.recovery.unconfirmed_rows, 0);
}

// A row consumed by a double-height character above it carries no data by
// definition (EN 300 706 §12.2 code 0/D), so its absence is not a gap.
TEST(TeletextAnalysisPresenterTest,
     PageView_ExcludesDoubleHeightLowerRowsFromRecovery) {
  orc::TeletextPageSnapshot snapshot;
  for (int row = 1; row < orc::TeletextPageSnapshot::kRows; ++row) {
    snapshot.row_received[static_cast<size_t>(row)] = true;
  }
  snapshot.row_received[2] = false;  // lower row was never transmitted
  for (auto& cell : snapshot.cells[1]) {
    cell.double_height = true;
  }
  for (auto& cell : snapshot.cells[2]) {
    cell.double_height_lower = true;
  }

  const auto view = TeletextAnalysisPresenter::makePageView(snapshot);

  EXPECT_EQ(view.recovery.rows_expected, 23);
  EXPECT_EQ(view.recovery.rows_received, 23);
  EXPECT_TRUE(view.recovery.complete());
}

TEST(TeletextAnalysisPresenterTest, PageViewCarriesTheServiceWidth) {
  orc::TeletextPageSnapshot snapshot;
  snapshot.columns = 32;
  snapshot.cells[1][31].character = 'A';
  // Beyond the service width the snapshot holds nothing; the view must not
  // walk there either.
  snapshot.cells[1][39].parity_error = true;

  const auto view = TeletextAnalysisPresenter::makePageView(snapshot);

  EXPECT_EQ(view.columns, 32);
  EXPECT_EQ(view.cells[1][31].character, U'A');
  EXPECT_EQ(view.recovery.damaged_bytes, 0);
}

// ---------------------------------------------------------------------------
// Dataset conversion: what the stage cached from its last trigger run
// ---------------------------------------------------------------------------

TEST(TeletextAnalysisPresenterTest, Dataset_ConvertsCatalogueAndSummary) {
  orc::TeletextAnalysisDataset dataset;

  orc::TeletextCataloguedPage page100;
  page100.magazine = 1;
  page100.page_number = 0x00;
  page100.first_seen_frame = 12;
  page100.last_seen_frame = 4096;
  page100.times_seen = 37;
  orc::TeletextCataloguedSubPage page100_only;
  page100_only.page.magazine = 1;
  page100_only.page.cells[1][0].character = 'A';
  page100.subpages.push_back(page100_only);
  dataset.pages.push_back(page100);

  orc::TeletextCataloguedPage subtitles;
  subtitles.magazine = 1;
  subtitles.page_number = 0x90;
  subtitles.subtitle = true;
  subtitles.times_seen = 4;
  subtitles.subpages.emplace_back();
  dataset.pages.push_back(subtitles);

  dataset.summary.frames_analysed = 5000;
  dataset.summary.fields_with_data = 3200;
  dataset.summary.packets_recovered = 6400;
  dataset.summary.packets_corrected = 91;
  dataset.summary.bytes_repaired = 12;
  dataset.summary.characters_written = 250000;
  dataset.summary.characters_damaged = 44;
  dataset.summary.lost_packets_estimate = 7;
  dataset.summary.pages_truncated = true;

  const auto view = TeletextAnalysisPresenter::makeAnalysisView(dataset);

  ASSERT_EQ(view.pages.size(), 2u);
  EXPECT_EQ(view.pages[0].magazine, 1);
  EXPECT_EQ(view.pages[0].page_number, 0x00);
  EXPECT_EQ(view.pages[0].first_seen_frame, 12u);
  EXPECT_EQ(view.pages[0].last_seen_frame, 4096u);
  EXPECT_EQ(view.pages[0].times_seen, 37u);
  EXPECT_FALSE(view.pages[0].subtitle);
  ASSERT_EQ(view.pages[0].subpages.size(), 1u);
  EXPECT_EQ(view.pages[0].subpages[0].page.cells[1][0].character, U'A');
  EXPECT_TRUE(view.pages[1].subtitle);

  EXPECT_EQ(view.summary.frames_analysed, 5000u);
  EXPECT_EQ(view.summary.fields_with_data, 3200u);
  EXPECT_EQ(view.summary.packets_recovered, 6400u);
  EXPECT_EQ(view.summary.packets_corrected, 91u);
  EXPECT_EQ(view.summary.bytes_repaired, 12u);
  EXPECT_EQ(view.summary.characters_written, 250000u);
  EXPECT_EQ(view.summary.characters_damaged, 44u);
  EXPECT_EQ(view.summary.lost_packets_estimate, 7u);
  EXPECT_TRUE(view.summary.pages_truncated);
}

// A page number transmitted as a sequence of sub-pages (ETSI EN 300 706 Annex
// A.1) converts as the whole sequence, in the order the catalogue holds it,
// each sub-page carrying its own figures and its own rendered page.
TEST(TeletextAnalysisPresenterTest, Dataset_ConvertsEverySubpageOfAPage) {
  orc::TeletextAnalysisDataset dataset;
  orc::TeletextCataloguedPage page;
  page.magazine = 1;
  page.page_number = 0x50;
  page.times_seen = 9;

  orc::TeletextCataloguedSubPage one;
  one.subcode = 0x0001;
  one.times_seen = 5;
  one.first_seen_frame = 10;
  one.last_seen_frame = 900;
  one.page.cells[1][0].character = '1';
  page.subpages.push_back(one);

  orc::TeletextCataloguedSubPage two;
  two.subcode = 0x0002;
  two.times_seen = 4;
  two.page.cells[1][0].character = '2';
  page.subpages.push_back(two);

  dataset.pages.push_back(page);

  const auto view = TeletextAnalysisPresenter::makeAnalysisView(dataset);

  ASSERT_EQ(view.pages.size(), 1u);
  EXPECT_EQ(view.pages[0].times_seen, 9u);
  ASSERT_EQ(view.pages[0].subpages.size(), 2u);
  EXPECT_EQ(view.pages[0].subpages[0].subcode, 0x0001);
  EXPECT_EQ(view.pages[0].subpages[0].times_seen, 5u);
  EXPECT_EQ(view.pages[0].subpages[0].first_seen_frame, 10u);
  EXPECT_EQ(view.pages[0].subpages[0].last_seen_frame, 900u);
  EXPECT_EQ(view.pages[0].subpages[0].page.cells[1][0].character, U'1');
  EXPECT_EQ(view.pages[0].subpages[1].subcode, 0x0002);
  EXPECT_EQ(view.pages[0].subpages[1].times_seen, 4u);
  EXPECT_EQ(view.pages[0].subpages[1].page.cells[1][0].character, U'2');
}

// A range that carried no teletext converts to an empty catalogue rather than
// to anything the viewer has to special-case.
TEST(TeletextAnalysisPresenterTest, Dataset_EmptyConvertsToEmptyView) {
  const orc::TeletextAnalysisDataset dataset;

  const auto view = TeletextAnalysisPresenter::makeAnalysisView(dataset);

  EXPECT_TRUE(view.pages.empty());
  EXPECT_EQ(view.summary.frames_analysed, 0u);
  EXPECT_FALSE(view.summary.pages_truncated);
}

// ITU-R BT.653 Table 1b: a 525-line service sends 34-byte packets carrying 32
// display columns. The page is still drawn on the 40-column Level 1 grid, and
// the conversion must not walk past the columns the service transmitted.
TEST(TeletextAnalysisPresenterTest, Dataset_Carries525LinePageWidth) {
  orc::TeletextAnalysisDataset dataset;
  orc::TeletextCataloguedPage page;
  page.magazine = 1;
  page.page_number = 0x00;
  orc::TeletextCataloguedSubPage only;
  only.page.columns = 32;
  only.page.cells[1][31].character = 'Z';
  only.page.cells[1][39].parity_error = true;  // never transmitted
  only.page.row_received[1] = true;
  page.subpages.push_back(only);
  dataset.pages.push_back(page);

  const auto view = TeletextAnalysisPresenter::makeAnalysisView(dataset);

  ASSERT_EQ(view.pages.size(), 1u);
  ASSERT_EQ(view.pages[0].subpages.size(), 1u);
  EXPECT_EQ(view.pages[0].subpages[0].page.columns, 32);
  EXPECT_EQ(view.pages[0].subpages[0].page.cells[1][31].character, U'Z');
  EXPECT_EQ(view.pages[0].subpages[0].page.recovery.damaged_bytes, 0);
}

}  // namespace gui_unit_test
