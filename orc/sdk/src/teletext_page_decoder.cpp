/*
 * File:        teletext_page_decoder.cpp
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     PAL WST (System B) teletext magazine/page decoder producing
 *              Level 1 page snapshots and subtitle cues
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/support/teletext_page_decoder.h>

#include <utility>

namespace orc {

namespace {

// ETSI EN 300 706 §9.3.1.1: page number 0xFF marks time-filling / page
// terminating headers ("null page" convention, §7.2.2 with sub-code 3F7F);
// such headers terminate a transmission but never open a page.
constexpr int kTimeFillingPageNumber = 0xFF;

// Spacing-attribute codes of ETSI EN 300 706 §12.2 Table 26 (7-bit values).
constexpr uint8_t kAlphaColourBase = 0x00;   // 0/0-0/7 (0/0 no Level 1 action)
constexpr uint8_t kFlash = 0x08;             // 0/8
constexpr uint8_t kSteady = 0x09;            // 0/9
constexpr uint8_t kEndBox = 0x0A;            // 0/A
constexpr uint8_t kStartBox = 0x0B;          // 0/B
constexpr uint8_t kNormalSize = 0x0C;        // 0/C
constexpr uint8_t kDoubleHeight = 0x0D;      // 0/D
constexpr uint8_t kMosaicColourBase = 0x10;  // 1/0-1/7 (1/0 no Level 1 action)
constexpr uint8_t kConceal = 0x18;           // 1/8
constexpr uint8_t kContiguousMosaic = 0x19;  // 1/9
constexpr uint8_t kSeparatedMosaic = 0x1A;   // 1/A
constexpr uint8_t kBlackBackground = 0x1C;   // 1/C
constexpr uint8_t kNewBackground = 0x1D;     // 1/D
constexpr uint8_t kHoldMosaics = 0x1E;       // 1/E
constexpr uint8_t kReleaseMosaics = 0x1F;    // 1/F

int hex_digit_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Transmission magazine 0 is displayed as magazine 8 (ETSI EN 300 706 §3.1:
// page 800-8FF carries magazine address 0).
int displayed_magazine(int transmission_magazine) {
  return transmission_magazine == 0 ? 8 : transmission_magazine;
}

}  // namespace

bool teletext_odd_parity_valid(uint8_t byte) {
  // ETSI EN 300 706 §8.1: accept when D1..D7 ⊕ P = 1 (odd number of set
  // bits over the whole byte).
  int ones = 0;
  for (int bit = 0; bit < 8; ++bit) {
    ones += (byte >> bit) & 1;
  }
  return (ones % 2) == 1;
}

uint8_t teletext_odd_parity_encode(uint8_t value) {
  uint8_t byte = value & 0x7F;
  int ones = 0;
  for (int bit = 0; bit < 7; ++bit) {
    ones += (byte >> bit) & 1;
  }
  // ETSI EN 300 706 §8.1: P = 1 ⊕ D1 ⊕ ... ⊕ D7.
  if ((ones % 2) == 0) {
    byte |= 0x80;
  }
  return byte;
}

TeletextPageDecoder::TeletextPageDecoder() = default;

std::optional<std::pair<int, int>> TeletextPageDecoder::parse_page_number(
    std::string_view page) {
  if (page.size() != 3) {
    return std::nullopt;
  }
  const char magazine_char = page[0];
  if (magazine_char < '1' || magazine_char > '8') {
    return std::nullopt;
  }
  const int tens = hex_digit_value(page[1]);
  const int units = hex_digit_value(page[2]);
  if (tens < 0 || units < 0) {
    return std::nullopt;
  }
  return std::make_pair(magazine_char - '0', (tens << 4) | units);
}

void TeletextPageDecoder::set_page_callback(PageCallback callback) {
  page_callback_ = std::move(callback);
}

bool TeletextPageDecoder::set_subtitle_page(std::string_view page) {
  const auto parsed = parse_page_number(page);
  if (!parsed.has_value()) {
    return false;
  }
  subtitle_filter_ = parsed;
  return true;
}

void TeletextPageDecoder::process_packet(
    const std::array<uint8_t, kTeletextPacketBytes>& packet,
    int64_t field_index, int64_t source) {
  last_row_attribution_.reset();
  last_row_number_ = 0;

  // MRAG: two Hamming 8/4 bytes carrying the 3-bit magazine and 5-bit packet
  // number (ETSI EN 300 706 §7.1.2). An uncorrectable MRAG byte means the
  // packet cannot be attributed; drop it.
  const int mrag_low = teletext_hamming84_decode(packet[0]);
  const int mrag_high = teletext_hamming84_decode(packet[1]);
  if (mrag_low < 0 || mrag_high < 0) {
    return;
  }
  const int magazine = mrag_low & 0x7;
  const int packet_number = ((mrag_low >> 3) & 0x1) | (mrag_high << 1);

  if (packet_number == 0) {
    handle_header_packet(magazine, packet, field_index);
  } else if (packet_number >= 1 &&
             packet_number < TeletextPageSnapshot::kRows) {
    // X/1 to X/24: directly displayable rows (EN 300 706 §9.3.2). X/25
    // (key-word search labels) and X/26-X/31 (enhancement / non-display
    // packets, §9.4-§9.8) are outside the Level 1 grid and are ignored.
    handle_display_packet(magazine, packet_number, packet, field_index,
                          source == kAutoSource ? next_source_++ : source);
  }
}

TeletextPageKey TeletextPageDecoder::page_key(int transmission_magazine) const {
  const MagazineState& state =
      magazines_[static_cast<size_t>(transmission_magazine)];
  return TeletextPageKey{displayed_magazine(transmission_magazine),
                         state.page_number, state.subcode};
}

void TeletextPageDecoder::handle_header_packet(
    int transmission_magazine,
    const std::array<uint8_t, kTeletextPacketBytes>& packet,
    int64_t field_index) {
  // Page number: bytes 6-7 of the transmission packet = packet bytes 2-3
  // (EN 300 706 §9.3.1.1). Without a correctable page number the header
  // cannot be attributed to a page; drop it entirely.
  const int units = teletext_hamming84_decode(packet[2]);
  const int tens = teletext_hamming84_decode(packet[3]);
  if (units < 0 || tens < 0) {
    return;
  }
  const int page_number = (tens << 4) | units;

  // Sub-code and control bits (EN 300 706 §9.3.1.2, §9.3.1.3 Table 2).
  // Uncorrectable nibbles degrade to zero rather than dropping the header.
  const int s1_raw = teletext_hamming84_decode(packet[4]);
  const int s2_c4_raw = teletext_hamming84_decode(packet[5]);
  const int s3_raw = teletext_hamming84_decode(packet[6]);
  const int s4_c5_c6_raw = teletext_hamming84_decode(packet[7]);
  const int c7_c10_raw = teletext_hamming84_decode(packet[8]);
  const int c11_c14_raw = teletext_hamming84_decode(packet[9]);
  const int s1 = s1_raw < 0 ? 0 : s1_raw;
  const int s2_c4 = s2_c4_raw < 0 ? 0 : s2_c4_raw;
  const int s3 = s3_raw < 0 ? 0 : s3_raw;
  const int s4_c5_c6 = s4_c5_c6_raw < 0 ? 0 : s4_c5_c6_raw;
  const int c7_c10 = c7_c10_raw < 0 ? 0 : c7_c10_raw;
  const int c11_c14 = c11_c14_raw < 0 ? 0 : c11_c14_raw;

  const int subcode =
      s1 | ((s2_c4 & 0x7) << 4) | (s3 << 7) | ((s4_c5_c6 & 0x3) << 11);
  const bool erase_page = (s2_c4 & 0x8) != 0;         // C4
  const bool newsflash = (s4_c5_c6 & 0x4) != 0;       // C5
  const bool subtitle = (s4_c5_c6 & 0x8) != 0;        // C6
  const bool magazine_serial = (c11_c14 & 0x1) != 0;  // C11

  // A page header terminates the page currently being transmitted: in serial
  // mode (C11 set) any magazine's page, in parallel mode only the page of
  // the same magazine (EN 300 706 §7.2.1).
  if (magazine_serial) {
    for (int m = 0; m < static_cast<int>(magazines_.size()); ++m) {
      terminate_page(m);
    }
  } else {
    terminate_page(transmission_magazine);
  }

  // Subtitle clear events act on header arrival: a header for the watched
  // page with C4 (erase) set, or with C6 no longer set, removes the display
  // (design §6; EN 300 706 §9.3.1.3 Table 2).
  if (subtitle_filter_.has_value() &&
      displayed_magazine(transmission_magazine) == subtitle_filter_->first &&
      page_number == subtitle_filter_->second && (erase_page || !subtitle)) {
    subtitle_clear_event(field_index);
  }

  // Time-filling / terminating headers never open a page (EN 300 706 §7.3).
  if (page_number == kTimeFillingPageNumber) {
    return;
  }

  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];

  // Retain stored rows only for a retransmission of the same page and
  // sub-code without C4 (erase); a different page, a different sub-code
  // (sub-page replacement), or an erase starts from a clean grid
  // (EN 300 706 §9.3.1.3 Table 2, C4).
  const bool retain_rows = state.have_page && !erase_page &&
                           state.page_number == page_number &&
                           state.subcode == subcode;
  if (!retain_rows) {
    state.rows = {};
  }

  state.page_open = true;
  state.have_page = true;
  state.page_number = page_number;
  state.subcode = subcode;
  state.erase_page = erase_page;

  // C4 replaces the page's content rather than updating it, so accumulated
  // copies of its rows describe a page that no longer exists; combining them
  // with what follows would blend the old page into the new one.
  if (erase_page && row_squasher_ != nullptr) {
    row_squasher_->erase_page(page_key(transmission_magazine));
  }
  state.newsflash = newsflash;
  state.subtitle = subtitle;
  state.suppress_header = (c7_c10 & 0x1) != 0;          // C7
  state.update_indicator = (c7_c10 & 0x2) != 0;         // C8
  state.interrupted_sequence = (c7_c10 & 0x4) != 0;     // C9
  state.inhibit_display = (c7_c10 & 0x8) != 0;          // C10
  state.magazine_serial = magazine_serial;              // C11
  state.national_option_subset = (c11_c14 >> 1) & 0x7;  // C12-C14
  state.header_field_index = field_index;
  state.last_field_index = field_index;

  // Header display bytes: transmission bytes 14-45 = packet bytes 10-41
  // carry 32 odd-parity characters shown in row 0 columns 8-39
  // (EN 300 706 §9.3.1.4). Columns 0-7 are decoder-generated (page number,
  // clock) and left as spaces here.
  RowData& header_row = state.rows[0];
  header_row.present = true;
  header_row.characters.fill(0x20);
  header_row.parity_error.fill(false);
  for (size_t i = 0; i < 32; ++i) {
    const uint8_t byte = packet[10 + i];
    if (teletext_odd_parity_valid(byte)) {
      header_row.characters[8 + i] = byte & 0x7F;
    } else {
      header_row.characters[8 + i] = 0x20;
      header_row.parity_error[8 + i] = true;
    }
  }
}

void TeletextPageDecoder::handle_display_packet(
    int transmission_magazine, int row,
    const std::array<uint8_t, kTeletextPacketBytes>& packet,
    int64_t field_index, int64_t source) {
  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  // Rows belong to the page whose transmission is in progress in this
  // magazine (EN 300 706 §7.2.1); orphan rows with no open page are dropped.
  if (!state.page_open) {
    return;
  }

  const TeletextPageKey key = page_key(transmission_magazine);
  last_row_attribution_ = key;
  last_row_number_ = row;

  RowData& row_data = state.rows[static_cast<size_t>(row)];
  row_data.present = true;
  for (size_t column = 0; column < TeletextPageSnapshot::kColumns; ++column) {
    // 40 display bytes, 7 data bits + odd parity (EN 300 706 §9.3.2, §8.1).
    const uint8_t byte = packet[2 + column];
    if (teletext_odd_parity_valid(byte)) {
      row_data.characters[column] = byte & 0x7F;
      row_data.parity_error[column] = false;
    } else {
      row_data.characters[column] = 0x20;
      row_data.parity_error[column] = true;
    }
  }

  if (row_squasher_ != nullptr) {
    TeletextRowBytes display{};
    std::copy(packet.begin() + 2, packet.begin() + 2 + kTeletextRowBytes,
              display.begin());
    row_squasher_->add_row(key, row, display, source);
  }

  state.last_field_index = field_index;
}

void TeletextPageDecoder::terminate_page(int transmission_magazine) {
  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  if (!state.page_open) {
    return;
  }
  state.page_open = false;

  const TeletextPageSnapshot snapshot =
      render_snapshot(transmission_magazine, state);
  if (page_callback_) {
    page_callback_(snapshot);
  }
  subtitle_page_completed(snapshot);
}

TeletextPageSnapshot TeletextPageDecoder::render_snapshot(
    int transmission_magazine, const MagazineState& state) const {
  TeletextPageSnapshot snapshot;
  snapshot.magazine = displayed_magazine(transmission_magazine);
  snapshot.page_number = state.page_number;
  snapshot.subcode = state.subcode;
  snapshot.erase_page = state.erase_page;
  snapshot.newsflash = state.newsflash;
  snapshot.subtitle = state.subtitle;
  snapshot.suppress_header = state.suppress_header;
  snapshot.update_indicator = state.update_indicator;
  snapshot.interrupted_sequence = state.interrupted_sequence;
  snapshot.inhibit_display = state.inhibit_display;
  snapshot.magazine_serial = state.magazine_serial;
  snapshot.national_option_subset = state.national_option_subset;
  snapshot.header_field_index = state.header_field_index;
  snapshot.last_field_index = state.last_field_index;

  // With a squasher attached, display rows come from the combined copies
  // rather than from the last one received: repeated transmissions correct
  // each other, and a row recovered during an earlier transmission is still
  // available when the current one was clipped (teletext_row_squasher.h).
  const TeletextPageKey key{snapshot.magazine, state.page_number,
                            state.subcode};

  for (int row = 0; row < TeletextPageSnapshot::kRows; ++row) {
    RowData local_row_data;
    const RowData* row_source = &state.rows[static_cast<size_t>(row)];
    if (row_squasher_ != nullptr && row >= 1) {
      if (const auto squashed = row_squasher_->squashed_row(key, row)) {
        local_row_data.present = true;
        for (size_t column = 0; column < TeletextPageSnapshot::kColumns;
             ++column) {
          const uint8_t byte = (*squashed)[column];
          if (teletext_odd_parity_valid(byte)) {
            local_row_data.characters[column] = byte & 0x7F;
            local_row_data.parity_error[column] = false;
          } else {
            local_row_data.characters[column] = 0x20;
            local_row_data.parity_error[column] = true;
          }
        }
        row_source = &local_row_data;
      }
    }
    const RowData& row_data = *row_source;
    auto& cells = snapshot.cells[static_cast<size_t>(row)];
    snapshot.row_received[static_cast<size_t>(row)] = row_data.present;

    // Start-of-row default conditions (EN 300 706 §12.2 Table 26): white
    // alphanumeric foreground, black background, steady, unboxed, normal
    // size, contiguous mosaics, hold off.
    TeletextColour foreground = TeletextColour::White;
    TeletextColour background = TeletextColour::Black;
    bool mosaic = false;
    bool separated = false;
    bool hold = false;
    bool flash = false;
    bool conceal = false;
    bool boxed = false;
    bool double_height = false;
    uint8_t held_character = 0x20;
    bool held_separated = false;

    for (int column = 0; column < TeletextPageSnapshot::kColumns; ++column) {
      const uint8_t code =
          row_data.present ? row_data.characters[static_cast<size_t>(column)]
                           : static_cast<uint8_t>(0x20);
      const bool parity_error =
          row_data.present &&
          row_data.parity_error[static_cast<size_t>(column)];
      TeletextPageCell& cell = cells[static_cast<size_t>(column)];

      if (code < 0x20 && !parity_error) {
        // Spacing attribute: "Set-At" codes act on this cell, "Set-After"
        // codes from the next cell (EN 300 706 §12.2 Table 26).
        switch (code) {
          case kSteady:
            flash = false;
            break;
          case kNormalSize:
            if (double_height) {
              held_character = 0x20;  // size change resets the held mosaic
              held_separated = false;
            }
            double_height = false;
            break;
          case kConceal:
            conceal = true;
            break;
          case kContiguousMosaic:
            separated = false;
            break;
          case kSeparatedMosaic:
            separated = true;
            break;
          case kBlackBackground:
            background = TeletextColour::Black;
            break;
          case kNewBackground:
            background = foreground;
            break;
          case kHoldMosaics:
            hold = true;
            break;
          default:
            break;
        }

        // The attribute cell displays as SPACE, or as the held mosaic
        // character in mosaics + Hold Mosaics mode (§12.2 1/E); the held
        // character keeps its original contiguous/separated form.
        const bool substitute = hold && mosaic && held_character != 0x20;
        cell.character = substitute ? held_character : 0x20;
        cell.held_mosaic = substitute;
        cell.mosaic = substitute;
        cell.separated_mosaic = substitute ? held_separated : separated;
        cell.foreground = foreground;
        cell.background = background;
        cell.flash = flash;
        cell.conceal = conceal;
        cell.boxed = boxed;
        cell.double_height = double_height;
        cell.parity_error = false;

        // "Set-After" actions.
        if (code >= kAlphaColourBase + 1 && code <= kAlphaColourBase + 7) {
          // 0/1-0/7 alpha colours (0/0 has no Level 1 response). Colour
          // codes cancel conceal (§12.2 1/8) and select the G0 set.
          foreground = static_cast<TeletextColour>(code);
          conceal = false;
          if (mosaic) {
            mosaic = false;
            held_character = 0x20;  // mode change resets the held mosaic
            held_separated = false;
          }
        } else if (code >= kMosaicColourBase + 1 &&
                   code <= kMosaicColourBase + 7) {
          // 1/1-1/7 mosaic colours (1/0 has no Level 1 response).
          foreground = static_cast<TeletextColour>(code - kMosaicColourBase);
          conceal = false;
          if (!mosaic) {
            mosaic = true;
            held_character = 0x20;
            held_separated = false;
          }
        } else if (code == kFlash) {
          flash = true;
        } else if (code == kEndBox) {
          boxed = false;
        } else if (code == kStartBox) {
          boxed = true;
        } else if (code == kDoubleHeight) {
          if (!double_height) {
            double_height = true;
            held_character = 0x20;
            held_separated = false;
          }
        } else if (code == kReleaseMosaics) {
          hold = false;
        }
        // 0/E double width, 0/F double size, 1/B ESC: no Level 1 response.
      } else {
        // Displayable character (or a parity-damaged byte rendered as a
        // flagged SPACE).
        cell.character = parity_error ? 0x20 : code;
        cell.parity_error = parity_error;
        // G1 codes 0x40-0x5F are alphanumeric capitals even in mosaics mode
        // (§12.2: mosaic blocks live in G1 columns 2, 3, 6 and 7).
        const bool mosaic_glyph = mosaic && (code & 0x20) != 0;
        cell.mosaic = !parity_error && mosaic_glyph;
        cell.separated_mosaic = cell.mosaic && separated;
        cell.held_mosaic = false;
        cell.foreground = foreground;
        cell.background = background;
        cell.flash = flash;
        cell.conceal = conceal;
        cell.boxed = boxed;
        cell.double_height = double_height;
        if (cell.mosaic) {
          // §12.2 1/E: the held mosaic is the most recent G1 mosaic
          // character with bit 6 set on this row.
          held_character = code;
          held_separated = separated;
        }
      }
    }
  }

  // Double-height post-pass (EN 300 706 §12.2 0/D): a row containing double
  // height characters consumes the row below — its transmitted data is
  // ignored and it displays only the origin row's background.
  std::array<bool, TeletextPageSnapshot::kRows> is_lower_row{};
  for (int row = 0; row + 1 < TeletextPageSnapshot::kRows; ++row) {
    if (is_lower_row[static_cast<size_t>(row)]) {
      continue;
    }
    const auto& origin_cells = snapshot.cells[static_cast<size_t>(row)];
    bool row_has_double_height = false;
    for (const auto& cell : origin_cells) {
      if (cell.double_height) {
        row_has_double_height = true;
        break;
      }
    }
    if (!row_has_double_height) {
      continue;
    }
    const size_t lower_row = static_cast<size_t>(row) + 1;
    is_lower_row[lower_row] = true;
    auto& lower_cells = snapshot.cells[lower_row];
    for (int column = 0; column < TeletextPageSnapshot::kColumns; ++column) {
      TeletextPageCell lower;
      lower.background = origin_cells[static_cast<size_t>(column)].background;
      lower.double_height_lower = true;
      lower_cells[static_cast<size_t>(column)] = lower;
    }
  }

  return snapshot;
}

void TeletextPageDecoder::subtitle_page_completed(
    const TeletextPageSnapshot& snapshot) {
  if (!subtitle_filter_.has_value() ||
      snapshot.magazine != subtitle_filter_->first ||
      snapshot.page_number != subtitle_filter_->second) {
    return;
  }

  // Only C6-flagged transmissions display subtitle text (EN 300 706
  // §9.3.1.3 Table 2); a completion without C6 clears at most (the header
  // arrival already fired the clear event).
  if (!snapshot.subtitle) {
    subtitle_clear_event(snapshot.last_field_index);
    return;
  }

  const std::string text = extract_subtitle_text(snapshot);
  const int64_t field_index = snapshot.last_field_index;

  if (open_cue_.has_value()) {
    if (open_cue_->text == text) {
      return;  // unchanged retransmission: the cue stays on screen
    }
    subtitle_clear_event(field_index);
  }
  if (!text.empty()) {
    TeletextSubtitleCue cue;
    cue.start_field_index = field_index;
    cue.text = text;
    open_cue_ = std::move(cue);
  }
}

void TeletextPageDecoder::subtitle_clear_event(int64_t field_index) {
  if (!open_cue_.has_value()) {
    return;
  }
  open_cue_->end_field_index = field_index;
  // Degenerate zero-length cues (cleared in the same field they appeared)
  // are dropped.
  if (open_cue_->end_field_index > open_cue_->start_field_index) {
    subtitle_cues_.push_back(std::move(*open_cue_));
  }
  open_cue_.reset();
}

std::string TeletextPageDecoder::extract_subtitle_text(
    const TeletextPageSnapshot& snapshot) {
  // On C5/C6 pages only boxed regions are displayed (EN 300 706 §12.2
  // 0/A-0/B); respect that so stray unboxed bytes never leak into cues.
  const bool boxed_only = snapshot.newsflash || snapshot.subtitle;

  std::string text;
  for (int row = 1; row < TeletextPageSnapshot::kRows; ++row) {
    const auto& cells = snapshot.cells[static_cast<size_t>(row)];
    std::string row_text;
    bool last_was_space = true;  // collapses runs and trims the left edge
    for (const auto& cell : cells) {
      char c = ' ';
      if (!cell.mosaic && !cell.held_mosaic && !cell.conceal &&
          !cell.double_height_lower && !cell.parity_error &&
          (!boxed_only || cell.boxed) && cell.character > 0x20 &&
          cell.character < 0x7F) {
        c = static_cast<char>(cell.character);
      }
      if (c == ' ') {
        if (!last_was_space) {
          row_text.push_back(' ');
        }
        last_was_space = true;
      } else {
        row_text.push_back(c);
        last_was_space = false;
      }
    }
    while (!row_text.empty() && row_text.back() == ' ') {
      row_text.pop_back();
    }
    if (!row_text.empty()) {
      if (!text.empty()) {
        text.push_back('\n');
      }
      text += row_text;
    }
  }
  return text;
}

void TeletextPageDecoder::finalize(int64_t end_field_index) {
  for (int magazine = 0; magazine < static_cast<int>(magazines_.size());
       ++magazine) {
    terminate_page(magazine);
  }
  subtitle_clear_event(end_field_index);
}

}  // namespace orc
