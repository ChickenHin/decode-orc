/*
 * File:        teletext_page_fixtures.h
 * Module:      orc-tests/gui/unit
 * Purpose:     Hand-built teletext page and catalogue view fixtures for the
 *              analysis viewer tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc_teletext.h>

#include <cstdint>
#include <string>
#include <vector>

namespace gui_unit_test {

// A rendered page carrying @p text on display row 1, as the presenter would
// hand it over: the header row and row 1 received, nothing damaged.
inline orc::presenters::TeletextPageView makePageView(int magazine,
                                                      int page_number,
                                                      const std::string& text) {
  orc::presenters::TeletextPageView page;
  page.magazine = magazine;
  page.page_number = page_number;
  page.row_received[0] = true;
  page.row_received[1] = true;
  for (size_t column = 0;
       column < text.size() &&
       column <
           static_cast<size_t>(orc::presenters::TeletextPageView::kColumns);
       ++column) {
    page.cells[1][column].character =
        static_cast<char32_t>(static_cast<unsigned char>(text[column]));
  }
  page.recovery.rows_expected = 24;
  page.recovery.rows_received = 1;
  return page;
}

// One sub-page of a catalogue entry, carrying @p text on display row 1.
inline orc::presenters::TeletextSubPageView makeSubPage(
    int magazine, int page_number, int subcode, const std::string& text,
    uint64_t first_seen_frame = 0, uint64_t last_seen_frame = 0,
    uint64_t times_seen = 1) {
  orc::presenters::TeletextSubPageView subpage;
  subpage.subcode = subcode;
  subpage.first_seen_frame = first_seen_frame;
  subpage.last_seen_frame = last_seen_frame;
  subpage.times_seen = times_seen;
  subpage.page = makePageView(magazine, page_number, text);
  return subpage;
}

// One catalogue entry that is not a multi-page set, as the viewer receives it
// for a triggered node.
inline orc::presenters::TeletextCataloguedPageView makeCataloguedPage(
    int magazine, int page_number, const std::string& text,
    uint64_t first_seen_frame = 0, uint64_t last_seen_frame = 0,
    uint64_t times_seen = 1, bool subtitle = false) {
  orc::presenters::TeletextCataloguedPageView entry;
  entry.magazine = magazine;
  entry.page_number = page_number;
  entry.first_seen_frame = first_seen_frame;
  entry.last_seen_frame = last_seen_frame;
  entry.times_seen = times_seen;
  entry.subtitle = subtitle;
  // Annex A.1: a page with no sub-pages associated is coded Mxx-0000.
  entry.subpages.push_back(makeSubPage(magazine, page_number, /*subcode=*/0,
                                       text, first_seen_frame, last_seen_frame,
                                       times_seen));
  return entry;
}

// A catalogue entry for a multi-page set: one sub-page per entry of @p texts,
// numbered from sub-code 0001 as Annex A.1 numbers them, each seen once.
inline orc::presenters::TeletextCataloguedPageView makeCarouselPage(
    int magazine, int page_number, const std::vector<std::string>& texts) {
  orc::presenters::TeletextCataloguedPageView entry;
  entry.magazine = magazine;
  entry.page_number = page_number;
  entry.times_seen = texts.size();
  entry.last_seen_frame = texts.empty() ? 0 : texts.size() - 1;
  for (size_t index = 0; index < texts.size(); ++index) {
    // Annex A.1 numbers display sub-pages with the decimal digits of S2 and
    // S1, so sub-page 10 is coded 0010 rather than 000A.
    const int number = static_cast<int>(index) + 1;
    const int subcode = ((number / 10) << 4) | (number % 10);
    entry.subpages.push_back(makeSubPage(magazine, page_number, subcode,
                                         texts[index],
                                         /*first_seen_frame=*/index,
                                         /*last_seen_frame=*/index));
  }
  return entry;
}

// Extract a display row of a page view as trimmed ASCII-ish text.
inline std::string rowText(const orc::presenters::TeletextPageView& page,
                           int row) {
  std::string text;
  for (const auto& cell : page.cells[static_cast<size_t>(row)]) {
    text.push_back(cell.character < 0x80 ? static_cast<char>(cell.character)
                                         : '?');
  }
  while (!text.empty() && text.back() == ' ') {
    text.pop_back();
  }
  return text;
}

}  // namespace gui_unit_test
