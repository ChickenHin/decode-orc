/*
 * File:        orc_teletext.h
 * Module:      orc-view-types
 * Purpose:     Teletext observation and page view models for MVP architecture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace orc::presenters {

/**
 * @brief One recovered T42 teletext packet from a single VBI line
 *
 * The bytes are the MRAG plus the service's data bytes in transmission coding,
 * exactly as recovered by the teletext observer: 42 in all on 625-line systems
 * (ETSI EN 300 706 §7.1) and 34 on 525-line ones (ITU-R BT.653 Table 1b).
 */
struct TeletextPacketView {
  /// 0-based field line the packet was recovered from
  int field_line = 0;
  /// MRAG + data bytes, transmission coding. The buffer is the widest a
  /// service transmits (matches the SDK kTeletextPacketBytes contract;
  /// static_assert'ed in the presenter); only the leading @ref byte_count were
  /// sent, and the rest are zero.
  std::array<uint8_t, 42> bytes{};
  /// Bytes of @ref bytes the service transmitted: 42 on 625-line systems, 34
  /// on 525-line ones.
  int byte_count = 42;
  /// Whether the recovery chain could say how sure it was of each byte. False
  /// for packets recovered by threshold slicing and for observations stored
  /// before confidences existed.
  bool has_confidence = false;
  /// How sure it was, 0-1 per byte — 1 throughout when it could not say, so a
  /// consumer weighting a vote by this needs no special case. Combining
  /// repeated copies of a page row uses it to prefer the copy that was read
  /// cleanly (see the SDK's teletext_row_squasher.h).
  std::array<float, 42> confidence{};
};

/**
 * @brief Teletext observations for a single field
 */
struct TeletextFieldPacketsView {
  /// The "teletext" observation namespace exists for this field (false for
  /// systems with no WST service, or fields that have not been observed)
  bool observed = false;
  /// At least one valid packet was recovered in the field
  bool present = false;
  /// Number of candidate VBI lines that yielded packets
  int32_t line_count = 0;
  /// Recovered packets in ascending field-line order
  std::vector<TeletextPacketView> packets;
};

/**
 * @brief One character cell of a rendered Level 1 teletext page
 *
 * Colour indices follow the spacing-attribute code order of
 * ETSI EN 300 706 §12.2 Table 26: 0 black, 1 red, 2 green, 3 yellow,
 * 4 blue, 5 magenta, 6 cyan, 7 white.
 */
struct TeletextPageCellView {
  /// Render as a 2×3 block-mosaic cell instead of a character glyph
  bool mosaic = false;
  /// Unicode code point for alphanumeric cells (Latin G0 with the English
  /// national option sub-set applied by the presenter)
  char32_t character = U' ';
  /// Six sixel bits for mosaic cells (EN 300 706 §15.7.1 Table 47):
  /// bit 0 top-left, 1 top-right, 2 middle-left, 3 middle-right,
  /// 4 bottom-left, 5 bottom-right
  uint8_t mosaic_pattern = 0;
  /// Separated (bordered) rather than contiguous mosaic blocks
  bool mosaic_separated = false;
  uint8_t foreground = 7;  ///< Foreground colour index 0-7 (white)
  uint8_t background = 0;  ///< Background colour index 0-7 (black)
  /// Origin (upper) cell of a double-height pair
  bool double_height = false;
  /// Lower cell of a double-height pair (background only)
  bool double_height_lower = false;
  bool flash = false;      ///< Flash attribute (rendered static)
  bool concealed = false;  ///< Render as SPACE until revealed
  bool boxed = false;      ///< Inside a Start Box/End Box region
  /// Transmitted byte failed odd parity; character replaced with SPACE
  bool parity_error = false;
};

/**
 * @brief How much of a page actually came back from the recovery chain
 *
 * A parity-damaged byte renders exactly like a transmitted SPACE, so a viewer
 * cannot tell it from page content by looking; @ref damaged_bytes is the only
 * way to make that visible.
 *
 * A *missing row* is a different matter, and not by itself a fault. Services
 * routinely omit the blank rows that space a page out rather than transmitting
 * 40 spaces: on the recording this was measured against, 134 of 140 page
 * transmissions left out at least one row inside their own extent while not a
 * single packet was lost. So @ref rows_received falling short of the grid is
 * normal, and only @ref lost_packets says data actually went astray.
 */
struct TeletextPageRecoveryView {
  /// Display rows 1-24 that could carry data (excludes the rows consumed by
  /// double-height characters above them, whose content is ignored anyway)
  int rows_expected = 0;
  /// Of those, the rows a packet was actually recovered for. Rows the service
  /// never sent are absent from this count and are not a defect.
  int rows_received = 0;
  /// Display bytes that failed odd parity (EN 300 706 §8.1) and were
  /// substituted with SPACE
  int damaged_bytes = 0;
  /// Rows this page rests on a single unchecked copy of, while other rows of
  /// the same page have been confirmed by a repeat. Teletext is a carousel, so
  /// a page seen more than once has had its rows corrected against each other;
  /// a row that stands alone in such a page was recovered once and never
  /// checked, and is where a misplaced or damaged row survives to the screen.
  /// Zero on a page seen only once — there, nothing has been confirmed yet and
  /// saying so of every row would say nothing.
  int unconfirmed_rows = 0;
  /// VBI packet slots inside this transmission that yielded nothing. The
  /// slots are the field lines the transmission itself was using, so this
  /// self-calibrates to the recording: a service inserting teletext on two
  /// lines per field should fill both in every field of a page it is in the
  /// middle of sending, and a slot that came back empty is a lost packet.
  /// Which row it would have carried is not knowable.
  int lost_packets = 0;

  /// True when the page arrived with nothing damaged and nothing lost
  bool complete() const { return damaged_bytes == 0 && lost_packets == 0; }
};

/**
 * @brief A rendered Level 1 teletext page snapshot
 */
struct TeletextPageView {
  static constexpr int kRows = 25;     ///< header row 0 + display rows 1-24
  static constexpr int kColumns = 40;  ///< EN 300 706 §9.3.2: 40 bytes/row

  /// Display columns to draw. kColumns on both services: the Level 1 display
  /// is a 40-column grid whatever the packet length, and a row a service left
  /// short simply shows spaces to the right of what it sent.
  int columns = kColumns;

  int magazine = 8;        ///< Displayed magazine number 1-8
  int page_number = 0;     ///< Two-digit hexadecimal page number 0x00-0xFF
  int subcode = 0;         ///< 13-bit page sub-code S1-S4
  bool subtitle = false;   ///< C6 subtitle control bit
  bool newsflash = false;  ///< C5 newsflash control bit
  /// Field index of the header packet that opened this transmission (frame
  /// seen = index / 2). A header re-sent part-way through the page does not
  /// restamp it, so it identifies the appearance rather than the packet.
  int64_t header_field_index = 0;
  /// Field index of the last packet that contributed to the page
  int64_t last_field_index = 0;

  /// Whether a packet was recovered for each row (index 0 = the header row)
  std::array<bool, kRows> row_received{};
  /// Rows resting on a single unchecked copy while the page has confirmed
  /// rows elsewhere (see TeletextPageRecoveryView::unconfirmed_rows, which
  /// counts these). The judgement is the presenter's, so a renderer and the
  /// recovery readout cannot disagree about which rows they mean.
  std::array<bool, kRows> row_unconfirmed{};
  /// Recovery summary for the display rows
  TeletextPageRecoveryView recovery;
  /// False when this is a page still arriving rather than a finished
  /// transmission: rows below those received so far are yet to be sent, and
  /// look exactly like transmitted blank ones. Distinct from
  /// TeletextPageRecoveryView::complete(), which asks whether the rows that
  /// *were* sent all came back intact.
  bool transmission_complete = true;

  std::array<std::array<TeletextPageCellView, kColumns>, kRows> cells{};
};

}  // namespace orc::presenters
