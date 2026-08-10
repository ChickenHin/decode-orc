/*
 * File:        teletext_catalogue_view_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the teletext catalogue view the host draws
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_catalogue_view.h"

#include <gtest/gtest.h>

#include <map>
#include <string>

#include "vbi-services/teletext_page_decoder.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {
namespace {

// A dataset holding one page whose row 1 carries |codes| in |g0_set|. This is
// what the stage hands the host: the catalogue view is the last thing between
// a decoded page and the character-cell grid drawn on screen, so a set that
// stops here never reaches the reader however right the decoder was.
TeletextAnalysisDataset make_dataset(const std::string& codes,
                                     TeletextG0Set g0_set,
                                     int national_option_subset = 0) {
  TeletextPageSnapshot snapshot;
  snapshot.magazine = 1;
  snapshot.page_number = 0x00;
  snapshot.g0_set = g0_set;
  snapshot.national_option_subset = national_option_subset;
  snapshot.row_received[1] = true;
  for (size_t column = 0; column < codes.size(); ++column) {
    snapshot.cells[1][column].character = static_cast<uint8_t>(codes[column]);
  }

  TeletextCataloguedSubPage subpage;
  subpage.page = snapshot;

  TeletextCataloguedPage page;
  page.magazine = snapshot.magazine;
  page.page_number = snapshot.page_number;
  page.times_seen = 1;
  page.subpages.push_back(std::move(subpage));

  TeletextAnalysisDataset dataset;
  dataset.pages.push_back(std::move(page));
  return dataset;
}

// UTF-8 of the drawn row, which is what a reader sees. The grid is on whichever
// item carries the page: a catalogued page is a parent whose sub-pages are its
// children, and the payload runs parallel to the item list.
std::string drawn_row(const CatalogueDataset& catalogue, int row,
                      size_t count) {
  const CatalogueCellGrid* grid = nullptr;
  for (const CataloguePayload& payload : catalogue.payloads) {
    if (payload.kind == CataloguePayload::Kind::kCellGrid) {
      grid = &payload.grid;
      break;
    }
  }
  EXPECT_NE(grid, nullptr);
  if (grid == nullptr) {
    return {};
  }
  std::string text;
  for (size_t column = 0; column < count; ++column) {
    const char32_t cp = grid->at(row, static_cast<int>(column)).character;
    // Only the ranges these tests reach; enough for Latin and Cyrillic.
    if (cp < 0x80) {
      text.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      text.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return text;
}

// A dataset holding one page whose rows rest on |copies| copies each, indexed
// by row. Rows given a non-zero count are marked received.
TeletextAnalysisDataset make_dataset_with_copies(
    const std::map<int, int>& copies) {
  TeletextPageSnapshot snapshot;
  snapshot.magazine = 1;
  snapshot.page_number = 0x64;
  for (const auto& [row, count] : copies) {
    snapshot.row_copies[static_cast<size_t>(row)] = count;
    snapshot.row_received[static_cast<size_t>(row)] = count > 0;
  }

  TeletextCataloguedSubPage subpage;
  subpage.page = snapshot;

  TeletextCataloguedPage page;
  page.magazine = snapshot.magazine;
  page.page_number = snapshot.page_number;
  page.times_seen = 1;
  page.subpages.push_back(std::move(subpage));

  TeletextAnalysisDataset dataset;
  dataset.pages.push_back(std::move(page));
  return dataset;
}

bool row_is_unconfirmed(const CatalogueDataset& catalogue, int row) {
  for (const CataloguePayload& payload : catalogue.payloads) {
    if (payload.kind == CataloguePayload::Kind::kCellGrid) {
      EXPECT_LT(static_cast<size_t>(row), payload.grid.row_status.size());
      return payload.grid.row_status[static_cast<size_t>(row)].unconfirmed;
    }
  }
  ADD_FAILURE() << "no cell grid in the catalogue";
  return false;
}

// A row carried onto this page by a burst can arrive two or three times over a
// long recording while the page's own rows have hundreds. Marking only the rows
// that rest on a single copy left exactly that row drawn as ordinary content,
// which is what a reader has no other way to notice.
TEST(TeletextCatalogueView, ARowFarBelowThePagesOwnEvidenceIsUnconfirmed) {
  const CatalogueDataset catalogue = build_teletext_catalogue(
      make_dataset_with_copies({{1, 500}, {2, 480}, {3, 500}, {24, 3}}));
  EXPECT_FALSE(row_is_unconfirmed(catalogue, 1));
  EXPECT_FALSE(row_is_unconfirmed(catalogue, 2));
  EXPECT_TRUE(row_is_unconfirmed(catalogue, 24));
}

// The unevenness of a real recovery is not suspicion: a row the dropouts took
// more often than its neighbours still belongs to the page.
TEST(TeletextCatalogueView, ARowMerelyLessLuckyThanItsNeighboursIsNot) {
  const CatalogueDataset catalogue = build_teletext_catalogue(
      make_dataset_with_copies({{1, 500}, {2, 480}, {24, 120}}));
  EXPECT_FALSE(row_is_unconfirmed(catalogue, 24));
}

// The label needs something to compare against: where every row rests on one
// copy nothing distinguishes them, and marking the whole page says nothing.
TEST(TeletextCatalogueView, APageSeenOnceHasNoUnconfirmedRows) {
  const CatalogueDataset catalogue = build_teletext_catalogue(
      make_dataset_with_copies({{1, 1}, {2, 1}, {24, 1}}));
  EXPECT_FALSE(row_is_unconfirmed(catalogue, 1));
  EXPECT_FALSE(row_is_unconfirmed(catalogue, 24));
}

// The reader is told which alphabet the pages were read in, because it is the
// one thing about a rendered page that cannot be checked by looking at it.
TEST(TeletextCatalogueView, TheSummarySaysWhichCharacterSetWasUsed) {
  TeletextAnalysisDataset dataset = make_dataset("A", TeletextG0Set::Cyrillic2);
  dataset.summary.frames_analysed = 10;
  dataset.summary.packets_recovered = 20;
  dataset.summary.character_set = TeletextG0Set::Cyrillic2;

  const CatalogueDataset catalogue = build_teletext_catalogue(dataset);
  EXPECT_NE(
      catalogue.summary.headline.find("read as Cyrillic (Russian/Bulgarian)"),
      std::string::npos)
      << catalogue.summary.headline;

  dataset.summary.character_set = TeletextG0Set::Latin;
  const CatalogueDataset latin = build_teletext_catalogue(dataset);
  EXPECT_NE(latin.summary.headline.find("read as Latin"), std::string::npos)
      << latin.summary.headline;
}

TEST(TeletextCatalogueView, DrawsThePageInItsOwnG0Set) {
  // The same seven transmitted codes, drawn twice. This is the whole point of
  // the character set reaching the grid: nothing about the page changes, only
  // the alphabet its codes are read in.
  const std::string codes = "Wtornik";

  const CatalogueDataset latin =
      build_teletext_catalogue(make_dataset(codes, TeletextG0Set::Latin));
  ASSERT_FALSE(latin.items.empty());
  EXPECT_EQ(drawn_row(latin, 1, codes.size()), "Wtornik");

  const CatalogueDataset cyrillic =
      build_teletext_catalogue(make_dataset(codes, TeletextG0Set::Cyrillic2));
  ASSERT_FALSE(cyrillic.items.empty());
  EXPECT_EQ(drawn_row(cyrillic, 1, codes.size()), "Вторник");
}

TEST(TeletextCatalogueView, DrawsTheLatinNationalOptionSubset) {
  // The sub-set still applies within Latin, and must not have been lost when
  // the G0 set joined it: 2/3 is "£" for a UK page and "#" for a German one.
  const std::string codes = "#";

  const CatalogueDataset uk = build_teletext_catalogue(
      make_dataset(codes, TeletextG0Set::Latin,
                   static_cast<int>(TeletextNationalOption::English)));
  EXPECT_EQ(drawn_row(uk, 1, 1), "£");

  const CatalogueDataset german = build_teletext_catalogue(
      make_dataset(codes, TeletextG0Set::Latin,
                   static_cast<int>(TeletextNationalOption::German)));
  EXPECT_EQ(drawn_row(german, 1, 1), "#");
}

TEST(TeletextCatalogueView, TheCyrillicSetsIgnoreTheHeadersSubsetBits) {
  // A Cyrillic page whose header happens to carry a national option — which is
  // exactly what the reference Russian broadcast sends, C12-C14 = 100 — must
  // still draw as Cyrillic. 2/6 is the position the sub-set logic would most
  // easily corrupt: "&" in Latin, a letter here.
  const CatalogueDataset cyrillic = build_teletext_catalogue(
      make_dataset("&", TeletextG0Set::Cyrillic2,
                   static_cast<int>(TeletextNationalOption::French)));
  EXPECT_EQ(drawn_row(cyrillic, 1, 1), "ы");
}

}  // namespace
}  // namespace orc
