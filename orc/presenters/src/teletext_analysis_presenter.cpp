/*
 * File:        teletext_analysis_presenter.cpp
 * Module:      orc-presenters
 * Purpose:     Teletext analysis presenter implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../include/teletext_analysis_presenter.h"

#include <algorithm>
#include <utility>

namespace orc::presenters {

namespace {

// True when a 7-bit code selects a G1 block-mosaic character.
//
// With |blast_through|, codes 4/0-5/F remain alphanumeric capitals even in
// mosaic mode (EN 300 706 §15.7.1 Table 47 NOTE 1). Without it every code from
// 2/0 up is a mosaic — which is what the 525-line service's own graphics need
// (see TeletextPageSnapshot::mosaic_blast_through). Codes below 2/0 are
// spacing attributes and never reach here as characters.
bool is_mosaic_code(uint8_t code, bool blast_through) {
  if (code < 0x20) {
    return false;
  }
  if (!blast_through) {
    return true;
  }
  return code <= 0x3F || code >= 0x60;
}

// Extract the six sixel bits of a G1 mosaic code (EN 300 706 §15.7.1
// Table 47, bit allocation as Table 35): character bits 1-5 select the
// top-left through bottom-left cells and bit 7 the bottom-right cell.
uint8_t mosaic_sixels(uint8_t code) {
  return static_cast<uint8_t>((code & 0x1F) | ((code >> 1) & 0x20));
}

}  // namespace

TeletextAnalysisView TeletextAnalysisPresenter::makeAnalysisView(
    const TeletextAnalysisDataset& dataset) {
  TeletextAnalysisView view;

  view.pages.reserve(dataset.pages.size());
  for (const auto& catalogued : dataset.pages) {
    TeletextCataloguedPageView entry;
    entry.magazine = catalogued.magazine;
    entry.page_number = catalogued.page_number;
    entry.first_seen_frame = catalogued.first_seen_frame;
    entry.last_seen_frame = catalogued.last_seen_frame;
    entry.times_seen = catalogued.times_seen;
    entry.subtitle = catalogued.subtitle;
    entry.subpages.reserve(catalogued.subpages.size());
    for (const auto& subpage : catalogued.subpages) {
      TeletextSubPageView sub_view;
      sub_view.subcode = subpage.subcode;
      sub_view.first_seen_frame = subpage.first_seen_frame;
      sub_view.last_seen_frame = subpage.last_seen_frame;
      sub_view.times_seen = subpage.times_seen;
      sub_view.page = makePageView(subpage.page);
      // Only the catalogue knows this: it is the shortfall over the field
      // slots this sub-page's own transmissions occupied, which the page
      // snapshot carries no trace of.
      sub_view.page.recovery.lost_packets =
          static_cast<int>(subpage.lost_packets);
      entry.subpages.push_back(std::move(sub_view));
    }
    view.pages.push_back(std::move(entry));
  }

  const auto& summary = dataset.summary;
  view.summary.frames_analysed = summary.frames_analysed;
  view.summary.fields_with_data = summary.fields_with_data;
  view.summary.packets_recovered = summary.packets_recovered;
  view.summary.packets_corrected = summary.packets_corrected;
  view.summary.bytes_repaired = summary.bytes_repaired;
  view.summary.characters_written = summary.characters_written;
  view.summary.characters_damaged = summary.characters_damaged;
  view.summary.lost_packets_estimate = summary.lost_packets_estimate;
  view.summary.pages_truncated = summary.pages_truncated;

  return view;
}

TeletextPageView TeletextAnalysisPresenter::makePageView(
    const TeletextPageSnapshot& snapshot) {
  static_assert(TeletextPageView::kRows == TeletextPageSnapshot::kRows);
  static_assert(TeletextPageView::kColumns == TeletextPageSnapshot::kColumns);

  TeletextPageView view;
  view.columns = snapshot.columns;
  view.magazine = snapshot.magazine;
  view.page_number = snapshot.page_number;
  view.subcode = snapshot.subcode;
  view.subtitle = snapshot.subtitle;
  view.newsflash = snapshot.newsflash;
  view.header_field_index = snapshot.header_field_index;
  view.last_field_index = snapshot.last_field_index;
  view.transmission_complete = snapshot.transmission_complete;

  for (int row = 0; row < TeletextPageView::kRows; ++row) {
    for (int col = 0; col < view.columns; ++col) {
      const auto& cell =
          snapshot.cells[static_cast<size_t>(row)][static_cast<size_t>(col)];
      auto& out =
          view.cells[static_cast<size_t>(row)][static_cast<size_t>(col)];

      out.foreground = static_cast<uint8_t>(cell.foreground);
      out.background = static_cast<uint8_t>(cell.background);
      out.double_height = cell.double_height;
      out.double_height_lower = cell.double_height_lower;
      out.flash = cell.flash;
      out.concealed = cell.conceal;
      out.boxed = cell.boxed;
      out.parity_error = cell.parity_error;

      // Held-mosaic cells carry the held character; separated_mosaic then
      // reflects the held character's original mode.
      if ((cell.mosaic || cell.held_mosaic) &&
          is_mosaic_code(cell.character, snapshot.mosaic_blast_through)) {
        out.mosaic = true;
        out.mosaic_pattern = mosaic_sixels(cell.character);
        out.mosaic_separated = cell.separated_mosaic;
        out.character = U' ';
      } else {
        // The page's own G0 set: the national option sub-set its header
        // selected, not a fixed English one (EN 300 706 §15.2, §15.6.2).
        out.character = orc::teletext_latin_g0_to_unicode(
            cell.character, snapshot.national_option_subset);
      }
    }
  }

  view.row_received = snapshot.row_received;

  // A row is only worth calling unconfirmed where the page has something to
  // compare it against: on a page whose rows have all been seen once, every
  // row rests on one copy and the label would be noise. Where other rows *have*
  // been corrected by a repeat, a row that stands alone is the one the reader
  // should distrust.
  const int most_copies =
      *std::max_element(snapshot.row_copies.begin(), snapshot.row_copies.end());

  // Recovery summary over the display rows. Rows consumed by a double-height
  // character above are excluded: their transmitted content is ignored by
  // definition (EN 300 706 §12.2 code 0/D), so their absence is not a gap.
  for (int row = 1; row < TeletextPageView::kRows; ++row) {
    const auto& cells = view.cells[static_cast<size_t>(row)];
    if (cells[0].double_height_lower) {
      continue;
    }
    ++view.recovery.rows_expected;
    if (view.row_received[static_cast<size_t>(row)]) {
      ++view.recovery.rows_received;
    }
    if (most_copies > 1 && snapshot.row_copies[static_cast<size_t>(row)] == 1) {
      view.row_unconfirmed[static_cast<size_t>(row)] = true;
      ++view.recovery.unconfirmed_rows;
    }
    for (int col = 0; col < view.columns; ++col) {
      if (cells[static_cast<size_t>(col)].parity_error) {
        ++view.recovery.damaged_bytes;
      }
    }
  }

  return view;
}

}  // namespace orc::presenters
