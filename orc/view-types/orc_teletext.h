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
 * The 42 bytes are the MRAG plus 40 data bytes in transmission coding
 * (ETSI EN 300 706 §7.1) exactly as recovered by the teletext observer.
 */
struct TeletextPacketView {
  /// 0-based field line the packet was recovered from
  int field_line = 0;
  /// MRAG + 40 data bytes, transmission coding (matches the SDK
  /// kTeletextPacketBytes contract; static_assert'ed in the presenter)
  std::array<uint8_t, 42> bytes{};
};

/**
 * @brief Teletext observations for a single field
 */
struct TeletextFieldPacketsView {
  /// The "teletext" observation namespace exists for this field (false for
  /// non-PAL sources or fields that have not been observed)
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
 * A row that was never recovered renders exactly like a transmitted blank
 * row, and a parity-damaged byte renders exactly like a transmitted SPACE,
 * so a viewer cannot tell a recovery gap from page content by looking. These
 * counts are the only way to make that distinction visible.
 */
struct TeletextPageRecoveryView {
  /// Display rows 1-24 that could carry data (excludes the rows consumed by
  /// double-height characters above them, whose content is ignored anyway)
  int rows_expected = 0;
  /// Of those, the rows a packet was actually recovered for
  int rows_received = 0;
  /// Display bytes that failed odd parity (EN 300 706 §8.1) and were
  /// substituted with SPACE
  int damaged_bytes = 0;

  /// True when every expected row arrived with no damaged bytes
  bool complete() const {
    return rows_received == rows_expected && damaged_bytes == 0;
  }
};

/**
 * @brief A rendered 40×25 Level 1 teletext page snapshot
 */
struct TeletextPageView {
  static constexpr int kRows = 25;     ///< header row 0 + display rows 1-24
  static constexpr int kColumns = 40;  ///< EN 300 706 §9.3.2: 40 bytes/row

  int magazine = 8;        ///< Displayed magazine number 1-8
  int page_number = 0;     ///< Two-digit hexadecimal page number 0x00-0xFF
  int subcode = 0;         ///< 13-bit page sub-code S1-S4
  bool subtitle = false;   ///< C6 subtitle control bit
  bool newsflash = false;  ///< C5 newsflash control bit
  /// Field index of the page header packet (frame seen = index / 2)
  int64_t header_field_index = 0;
  /// Field index of the last packet that contributed to the page
  int64_t last_field_index = 0;

  /// Whether a packet was recovered for each row (index 0 = the header row)
  std::array<bool, kRows> row_received{};
  /// Recovery summary for the display rows
  TeletextPageRecoveryView recovery;

  std::array<std::array<TeletextPageCellView, kColumns>, kRows> cells{};
};

}  // namespace orc::presenters
