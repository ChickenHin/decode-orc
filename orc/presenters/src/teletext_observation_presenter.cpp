/*
 * File:        teletext_observation_presenter.cpp
 * Module:      orc-presenters
 * Purpose:     Teletext observation presenter implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../include/teletext_observation_presenter.h"

#include <orc/stage/observation/observation_context.h>
#include <orc/support/teletext_slicer.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <variant>

namespace orc::presenters {

namespace {

// The view type mirrors the SDK packet buffer size without including SDK
// headers in the view-types tier; keep the two in lock-step.
static_assert(std::tuple_size<decltype(TeletextPacketView::bytes)>::value ==
                  orc::kTeletextPacketBytes,
              "TeletextPacketView::bytes must match the SDK T42 packet size");
static_assert(
    std::tuple_size<decltype(TeletextPacketView::confidence)>::value ==
        orc::kTeletextPacketBytes,
    "TeletextPacketView::confidence must match the SDK T42 packet size");

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

TeletextFieldPacketsView TeletextObservationPresenter::extractFieldObservations(
    FieldID field_id, const void* obs_context_ptr) {
  const auto* obs_context =
      static_cast<const orc::ObservationContext*>(obs_context_ptr);
  TeletextFieldPacketsView result;

  auto present_obs = obs_context->get(field_id, "teletext", "present");
  if (!present_obs) {
    // Non-PAL sources (and unobserved fields) carry no "teletext" namespace.
    return result;
  }
  result.observed = true;

  if (std::holds_alternative<bool>(*present_obs)) {
    result.present = std::get<bool>(*present_obs);
  }

  auto count_obs = obs_context->get(field_id, "teletext", "line_count");
  if (count_obs && std::holds_alternative<int32_t>(*count_obs)) {
    result.line_count = std::get<int32_t>(*count_obs);
  }

  // The t42_<n> keys are sparse (absent for lines that carried no data);
  // enumerate whatever the observer recorded and order by field line.
  for (const auto& key : obs_context->get_keys(field_id, "teletext")) {
    constexpr std::string_view kPrefix = "t42_";
    if (key.rfind(kPrefix, 0) != 0) {
      continue;
    }
    int field_line = 0;
    try {
      field_line = std::stoi(key.substr(kPrefix.size()));
    } catch (const std::exception&) {
      continue;
    }

    auto packet_obs = obs_context->get(field_id, "teletext", key);
    if (!packet_obs || !std::holds_alternative<std::string>(*packet_obs)) {
      continue;
    }
    const auto observed = orc::teletext_hex_to_observed_packet(
        std::get<std::string>(*packet_obs));
    if (!observed) {
      continue;
    }

    TeletextPacketView packet;
    packet.field_line = field_line;
    packet.bytes = observed->bytes;
    // Carried rather than assumed: the page decoder needs to know how many of
    // a row's columns one packet brought (ITU-R BT.653 Table 1b gives 525-line
    // WST a 34-byte packet, so 32 of the 40, the rest arriving separately).
    packet.byte_count = static_cast<int>(observed->byte_count);
    packet.has_confidence = observed->has_confidence;
    packet.confidence = observed->confidence;
    result.packets.push_back(packet);
  }

  std::sort(result.packets.begin(), result.packets.end(),
            [](const TeletextPacketView& a, const TeletextPacketView& b) {
              return a.field_line < b.field_line;
            });

  return result;
}

TeletextPageView TeletextObservationPresenter::makePageView(
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
