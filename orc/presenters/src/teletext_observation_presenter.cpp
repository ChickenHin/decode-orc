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

// The view type mirrors the SDK packet size without including SDK headers in
// the view-types tier; keep the two in lock-step.
static_assert(std::tuple_size<decltype(TeletextPacketView::bytes)>::value ==
                  orc::kTeletextPacketBytes,
              "TeletextPacketView::bytes must match the SDK T42 packet size");

// Map a 7-bit G0 character code to Unicode: Latin G0 primary set
// (ETSI EN 300 706 §15.6.1 Table 35) with the English national option
// sub-set substitutions (§15.6.2 Table 36). Codes outside 0x20-0x7F render
// as SPACE.
char32_t latin_g0_english(uint8_t code) {
  switch (code) {
    // EN 300 706 §15.6.2 Table 36, English national option sub-set.
    case 0x23:
      return U'£';
    case 0x24:
      return U'$';
    case 0x40:
      return U'@';
    case 0x5B:
      return U'←';
    case 0x5C:
      return U'½';
    case 0x5D:
      return U'→';
    case 0x5E:
      return U'↑';
    case 0x5F:
      return U'#';
    case 0x60:
      return U'—';
    case 0x7B:
      return U'¼';
    case 0x7C:
      return U'‖';
    case 0x7D:
      return U'¾';
    case 0x7E:
      return U'÷';
    // EN 300 706 §15.6.1 Table 35 NOTE 4: 7/F is a filled rectangle.
    case 0x7F:
      return U'■';
    default:
      break;
  }
  if (code < 0x20 || code > 0x7E) {
    return U' ';
  }
  // Remaining Table 35 positions coincide with ASCII.
  return static_cast<char32_t>(code);
}

// True when a 7-bit code selects a G1 block-mosaic character. Codes 4/0-5/F
// remain alphanumeric capitals even in mosaic mode (EN 300 706 §15.7.1
// Table 47 NOTE 1, "blast-through").
bool is_mosaic_code(uint8_t code) {
  return (code >= 0x20 && code <= 0x3F) || (code >= 0x60 && code <= 0x7F);
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
    const auto bytes =
        orc::teletext_hex_to_packet(std::get<std::string>(*packet_obs));
    if (!bytes) {
      continue;
    }

    TeletextPacketView packet;
    packet.field_line = field_line;
    packet.bytes = *bytes;
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
  view.magazine = snapshot.magazine;
  view.page_number = snapshot.page_number;
  view.subcode = snapshot.subcode;
  view.subtitle = snapshot.subtitle;
  view.newsflash = snapshot.newsflash;
  view.header_field_index = snapshot.header_field_index;
  view.last_field_index = snapshot.last_field_index;
  view.transmission_complete = snapshot.transmission_complete;

  for (int row = 0; row < TeletextPageView::kRows; ++row) {
    for (int col = 0; col < TeletextPageView::kColumns; ++col) {
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
      if ((cell.mosaic || cell.held_mosaic) && is_mosaic_code(cell.character)) {
        out.mosaic = true;
        out.mosaic_pattern = mosaic_sixels(cell.character);
        out.mosaic_separated = cell.separated_mosaic;
        out.character = U' ';
      } else {
        out.character = latin_g0_english(cell.character);
      }
    }
  }

  view.row_received = snapshot.row_received;

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
    for (const auto& cell : cells) {
      if (cell.parity_error) {
        ++view.recovery.damaged_bytes;
      }
    }
  }

  return view;
}

}  // namespace orc::presenters
