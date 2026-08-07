/*
 * File:        teletext_page_decoder.cpp
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     WST (System B) teletext magazine/page decoder producing
 *              Level 1 page snapshots and subtitle cues
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/support/teletext_page_decoder.h>

#include <algorithm>
#include <utility>

namespace orc {

namespace {

// ETSI EN 300 706 §7.1.2 / ITU-R BT.653 Table 1b §3.3: every packet opens with
// the two Hamming 8/4 magazine-and-row address bytes, so the display bytes of
// a row start after them on either service.
constexpr int kMragBytes = 2;

// ETSI EN 300 706 §9.3.1: a page header (X/0) spends its first ten bytes on
// addressing and control — the MRAG, the two page-number bytes, the four
// sub-code bytes and the two control-bit bytes. The header text follows, and
// is displayed from column 8: columns 0-7 carry the receiver's own page number
// and clock.
constexpr size_t kHeaderControlBytes = 10;
constexpr int kHeaderTextColumn = 8;

// Magazine address bit that marks a 525-line row-extension packet (see the
// TeletextPageDecoder class comment): the service uses magazines 8, 1, 2 and 3
// for its pages and addresses each one's extension packets to that magazine
// with this bit set. Read only when the service's packets are short, so a
// 625-line stream's magazines 4-7 are untouched.
constexpr int kExtensionMagazineFlag = 0x4;

// Rows one extension packet serves: its display bytes are that many equal
// groups, one per row, in ascending row order.
constexpr int kExtensionPacketRows = 4;

// First display row an extension packet numbered |packet_number| serves. The
// number identifies a block of kExtensionPacketRows rows rather than naming a
// row, so it rounds down: the observed packets are numbered 1, 4, 8, 12, 16 and
// 20 and serve rows 0-3, 4-7, 8-11, 12-15, 16-19 and 20-23. The first block
// cannot be numbered 0 — that is the page header's packet number — which is why
// its packets carry 1.
int extension_first_row(int packet_number) {
  return (packet_number / kExtensionPacketRows) * kExtensionPacketRows;
}

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
    int64_t field_index, int64_t source,
    const TeletextPacketConfidence* confidence, size_t packet_bytes) {
  last_row_attribution_.reset();
  last_row_number_ = 0;

  // Display bytes one packet of this service carries: 42 bytes less the MRAG
  // gives the 40 of EN 300 706 §9.3.2, 34 gives the 32 of ITU-R BT.653
  // Table 1b §3.4. The grid stays 40 columns either way: what a short packet
  // does not fill comes from the row-extension packets, or shows as spaces.
  head_columns_ = std::clamp(static_cast<int>(packet_bytes) - kMragBytes, 0,
                             TeletextPageSnapshot::kColumns);

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

  if (head_columns_ < TeletextPageSnapshot::kColumns &&
      (magazine & kExtensionMagazineFlag) != 0) {
    // A row-extension packet, not a page of magazine 4-7: its row number is a
    // row range and it must not be read as a header or a display row. Checked
    // before either, because an extension packet whose range starts at row 0
    // would otherwise be taken for a page header and terminate the page it was
    // sent to complete.
    if (packet_number < TeletextPageSnapshot::kRows) {
      handle_extension_packet(magazine & ~kExtensionMagazineFlag, packet_number,
                              packet, field_index,
                              source == kAutoSource ? next_source_++ : source,
                              confidence);
    }
    return;
  }

  if (packet_number == 0) {
    handle_header_packet(magazine, packet, field_index);
  } else if (packet_number >= 1 &&
             packet_number < TeletextPageSnapshot::kRows) {
    // X/1 to X/24: directly displayable rows (EN 300 706 §9.3.2). X/25
    // (key-word search labels) and X/26-X/31 (enhancement / non-display
    // packets, §9.4-§9.8) are outside the Level 1 grid and are ignored.
    handle_display_packet(magazine, packet_number, packet, field_index,
                          source == kAutoSource ? next_source_++ : source,
                          confidence);
  }
}

int TeletextPageDecoder::erase_epoch(const PageIdentity& identity) const {
  const auto it = erase_epochs_.find(identity);
  return it == erase_epochs_.end() ? 0 : it->second;
}

TeletextPageKey TeletextPageDecoder::page_key(int transmission_magazine) const {
  const MagazineState& state =
      magazines_[static_cast<size_t>(transmission_magazine)];
  const int magazine = displayed_magazine(transmission_magazine);
  return TeletextPageKey{
      magazine, state.page_number, state.subcode,
      erase_epoch({magazine, state.page_number, state.subcode})};
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

  // A service may re-send a page's header while the page's rows are still
  // being transmitted (a rolling header keeps the on-screen clock live,
  // EN 300 706 §9.3.1.4). That header closes the assembly like any other,
  // but the transmission it closes has not finished — the same page simply
  // reopens and its remaining rows follow. Recognising this is what stops a
  // consumer counting one appearance of a page as several, and what lets it
  // tell a fragment from a finished page.
  const MagazineState& open_state =
      magazines_[static_cast<size_t>(transmission_magazine)];
  const bool same_page_continues =
      open_state.page_open && open_state.have_page && !erase_page &&
      open_state.page_number == page_number && open_state.subcode == subcode;

  // A page header terminates the page currently being transmitted: in serial
  // mode (C11 set) any magazine's page, in parallel mode only the page of
  // the same magazine (EN 300 706 §7.2.1).
  if (magazine_serial) {
    for (int m = 0; m < static_cast<int>(magazines_.size()); ++m) {
      terminate_page(m, m != transmission_magazine || !same_page_continues);
    }
  } else {
    terminate_page(transmission_magazine, !same_page_continues);
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

  // C4 replaces the page's content rather than updating it, so copies of its
  // rows recorded so far describe a page that no longer exists; combining them
  // with what follows would blend the old page into the new one. Advancing the
  // epoch moves the page to a fresh set of buckets, which separates the two
  // runs without discarding the first: a consumer replaying this stream feeds
  // the same packets in the same order, so it arrives at the same epoch at the
  // same point and can still ask about the rows that came before.
  if (erase_page) {
    ++erase_epochs_[{displayed_magazine(transmission_magazine), page_number,
                     subcode}];
  }
  state.newsflash = newsflash;
  state.subtitle = subtitle;
  state.suppress_header = (c7_c10 & 0x1) != 0;          // C7
  state.update_indicator = (c7_c10 & 0x2) != 0;         // C8
  state.interrupted_sequence = (c7_c10 & 0x4) != 0;     // C9
  state.inhibit_display = (c7_c10 & 0x8) != 0;          // C10
  state.magazine_serial = magazine_serial;              // C11
  state.national_option_subset = (c11_c14 >> 1) & 0x7;  // C12-C14
  // The header that *opened* this transmission stamps it, so a rolling header
  // re-sent while the rows are still going out does not make the same
  // appearance of the page look like a series of new ones.
  if (!same_page_continues) {
    state.header_field_index = field_index;
  }
  state.last_field_index = field_index;

  // Header display bytes: the packet bytes after the ten addressing and
  // control ones carry odd-parity characters shown in row 0 from column 8
  // (EN 300 706 §9.3.1.4) — 32 of them on 625 lines, 24 on 525, the addressing
  // ahead of them being identical (ITU-R BT.653 Table 1b §3.3). Columns 0-7
  // are decoder-generated (page number, clock) and left as spaces here.
  RowData& header_row = state.rows[0];
  const bool header_extension_present = header_row.extension_present;
  const auto header_extension_columns = header_row.characters;
  const auto header_extension_parity = header_row.parity_error;
  header_row.present = true;
  header_row.characters.fill(0x20);
  header_row.parity_error.fill(false);
  // The header row's own extension columns survive the rewrite: they were sent
  // in a different packet and this one says nothing about them.
  if (header_extension_present) {
    header_row.extension_present = true;
    for (int column = head_columns_; column < TeletextPageSnapshot::kColumns;
         ++column) {
      const auto i = static_cast<size_t>(column);
      header_row.characters[i] = header_extension_columns[i];
      header_row.parity_error[i] = header_extension_parity[i];
    }
  }
  for (int column = kHeaderTextColumn; column < head_columns_; ++column) {
    const auto i = static_cast<size_t>(column - kHeaderTextColumn);
    const uint8_t byte = packet[kHeaderControlBytes + i];
    if (teletext_odd_parity_valid(byte)) {
      header_row.characters[static_cast<size_t>(column)] = byte & 0x7F;
    } else {
      header_row.characters[static_cast<size_t>(column)] = 0x20;
      header_row.parity_error[static_cast<size_t>(column)] = true;
    }
  }
}

void TeletextPageDecoder::handle_display_packet(
    int transmission_magazine, int row,
    const std::array<uint8_t, kTeletextPacketBytes>& packet,
    int64_t field_index, int64_t source,
    const TeletextPacketConfidence* confidence) {
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
  for (size_t column = 0; column < static_cast<size_t>(head_columns_);
       ++column) {
    // The service's display bytes, 7 data bits + odd parity (EN 300 706
    // §9.3.2, §8.1).
    const uint8_t byte = packet[kMragBytes + column];
    if (teletext_odd_parity_valid(byte)) {
      row_data.characters[column] = byte & 0x7F;
      row_data.parity_error[column] = false;
    } else {
      row_data.characters[column] = 0x20;
      row_data.parity_error[column] = true;
    }
  }

  if (row_squasher_ != nullptr) {
    // The squasher's rows are the widest a service transmits; a service whose
    // packets carry fewer leaves the rest at zero, and the render below takes
    // those columns from the row-extension store instead, so the squasher needs
    // no notion of the width itself.
    TeletextRowBytes display{};
    std::copy(packet.begin() + kMragBytes,
              packet.begin() + kMragBytes + head_columns_, display.begin());
    // The row's own bytes are the packet bytes after the MRAG (§9.3.2), so its
    // confidences are the matching slice of the packet's.
    TeletextRowConfidence weights{};
    if (confidence != nullptr) {
      std::copy(confidence->begin() + kMragBytes,
                confidence->begin() + kMragBytes + head_columns_,
                weights.begin());
    }
    row_squasher_->add_row(key, row, display, source,
                           confidence != nullptr ? &weights : nullptr, 0,
                           static_cast<size_t>(head_columns_));
  }

  state.last_field_index = field_index;
}

void TeletextPageDecoder::handle_extension_packet(
    int transmission_magazine, int packet_number,
    const std::array<uint8_t, kTeletextPacketBytes>& packet,
    int64_t field_index, int64_t source,
    const TeletextPacketConfidence* confidence) {
  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  // Like a display row, an extension belongs to the page whose transmission is
  // in progress in the magazine it is addressed to (EN 300 706 §7.2.1).
  if (!state.page_open) {
    return;
  }

  const int extension_columns = TeletextPageSnapshot::kColumns - head_columns_;
  if (extension_columns <= 0) {
    return;
  }

  const TeletextPageKey key = page_key(transmission_magazine);
  const int first_row = extension_first_row(packet_number);

  for (int group = 0; group < kExtensionPacketRows; ++group) {
    const int row = first_row + group;
    if (row >= TeletextPageSnapshot::kRows) {
      break;
    }
    const size_t group_base = static_cast<size_t>(kMragBytes) +
                              static_cast<size_t>(group * extension_columns);

    RowData& row_data = state.rows[static_cast<size_t>(row)];
    for (int offset = 0; offset < extension_columns; ++offset) {
      const size_t column =
          static_cast<size_t>(head_columns_) + static_cast<size_t>(offset);
      const uint8_t byte = packet[group_base + static_cast<size_t>(offset)];
      // A byte that fails odd parity (EN 300 706 §8.1) is known to be corrupt,
      // so it never replaces a clean one already recovered for this column.
      // This is the decoder's own store, which is all a caller without a row
      // squasher has; with one attached the render takes these columns from its
      // vote across every copy instead.
      const bool clean = teletext_odd_parity_valid(byte);
      if (!clean && row_data.extension_present &&
          !row_data.parity_error[column]) {
        continue;
      }
      row_data.characters[column] = clean ? static_cast<uint8_t>(byte & 0x7F)
                                          : static_cast<uint8_t>(0x20);
      row_data.parity_error[column] = !clean;
    }
    row_data.extension_present = true;

    if (row_squasher_ == nullptr) {
      continue;
    }
    // The extension columns go into the squasher as a copy speaking for those
    // columns only, so repeated transmissions of them correct each other just
    // as the display packets' columns do. One packet contributes to four rows;
    // they are separate buckets, so the one source id serves all four.
    TeletextRowBytes display{};
    std::copy(packet.begin() + static_cast<std::ptrdiff_t>(group_base),
              packet.begin() + static_cast<std::ptrdiff_t>(group_base) +
                  extension_columns,
              display.begin() + head_columns_);
    TeletextRowConfidence weights{};
    if (confidence != nullptr) {
      std::copy(confidence->begin() + static_cast<std::ptrdiff_t>(group_base),
                confidence->begin() + static_cast<std::ptrdiff_t>(group_base) +
                    extension_columns,
                weights.begin() + head_columns_);
    }
    row_squasher_->add_row(key, row, display, source,
                           confidence != nullptr ? &weights : nullptr,
                           static_cast<size_t>(head_columns_),
                           static_cast<size_t>(extension_columns));
  }

  state.last_field_index = field_index;
}

void TeletextPageDecoder::terminate_page(int transmission_magazine,
                                         bool transmission_complete) {
  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  if (!state.page_open) {
    return;
  }
  state.page_open = false;

  // Rendering the snapshot is the expensive part of closing a page — with a
  // squasher attached it votes over every stored copy of all 24 rows — and a
  // header packet closes a page every few dozen packets for the whole of a
  // recording. Skip it when nothing will consume it: no page callback, and
  // the closed page is not the watched subtitle page (the only page
  // subtitle_page_completed() acts on).
  const bool watched_subtitle_page =
      subtitle_filter_.has_value() &&
      displayed_magazine(transmission_magazine) == subtitle_filter_->first &&
      state.page_number == subtitle_filter_->second;
  if (page_callback_ == nullptr && !watched_subtitle_page) {
    return;
  }

  TeletextPageSnapshot snapshot = render_snapshot(transmission_magazine, state);
  snapshot.transmission_complete = transmission_complete;
  if (page_callback_) {
    page_callback_(snapshot);
  }
  subtitle_page_completed(snapshot);
}

std::vector<TeletextPageSnapshot> TeletextPageDecoder::open_page_snapshots()
    const {
  std::vector<TeletextPageSnapshot> snapshots;
  for (int magazine = 0; magazine < static_cast<int>(magazines_.size());
       ++magazine) {
    const MagazineState& state = magazines_[static_cast<size_t>(magazine)];
    if (!state.page_open) {
      continue;
    }
    TeletextPageSnapshot snapshot = render_snapshot(magazine, state);
    snapshot.transmission_complete = false;
    snapshots.push_back(std::move(snapshot));
  }
  return snapshots;
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
  snapshot.columns = columns_;

  // With a squasher attached, display rows come from the combined copies
  // rather than from the last one received: repeated transmissions correct
  // each other, and a row recovered during an earlier transmission is still
  // available when the current one was clipped (teletext_row_squasher.h).
  const TeletextPageKey key{
      snapshot.magazine, state.page_number, state.subcode,
      erase_epoch({snapshot.magazine, state.page_number, state.subcode})};

  for (int row = 0; row < TeletextPageSnapshot::kRows; ++row) {
    const RowData& stored = state.rows[static_cast<size_t>(row)];
    RowData local_row_data;
    int row_copies = 0;
    const RowData* row_source = &stored;
    bool from_squasher = false;
    if (row_squasher_ != nullptr && row >= 1) {
      TeletextRowCoverage covered{};
      if (const auto squashed =
              row_squasher_->squashed_row(key, row, &covered)) {
        from_squasher = true;
        row_copies = static_cast<int>(row_squasher_->copy_count(key, row));
        // Every column the squasher has copies of, which for a service that
        // splits its rows means the display packet's columns and the extension
        // packets' are each voted on across their own repeats. A column no copy
        // spoke for shows as a space: it was not recovered, and the zero the
        // vote leaves there would read as a spacing attribute.
        for (int column = 0; column < columns_; ++column) {
          const auto i = static_cast<size_t>(column);
          if (!covered[i]) {
            local_row_data.characters[i] = 0x20;
            local_row_data.parity_error[i] = false;
            continue;
          }
          local_row_data.present = true;
          if (column >= head_columns_) {
            local_row_data.extension_present = true;
          }
          const uint8_t byte = (*squashed)[i];
          if (teletext_odd_parity_valid(byte)) {
            local_row_data.characters[i] = byte & 0x7F;
            local_row_data.parity_error[i] = false;
          } else {
            local_row_data.characters[i] = 0x20;
            local_row_data.parity_error[i] = true;
          }
        }
        row_source = &local_row_data;
      }
    }
    // Without a squasher the columns beyond what one packet carries come from
    // the decoder's own store, where handle_extension_packet() put them. A row
    // whose extension never arrived shows spaces there rather than whatever the
    // short packet left behind.
    if (!from_squasher && head_columns_ < columns_) {
      local_row_data = stored;
      row_source = &local_row_data;
      if (!local_row_data.present) {
        // Only the extension arrived. What the display packet would have
        // brought is unknown, and the stored bytes for those columns are the
        // zeros of a row never written — which would read as spacing attributes
        // rather than as the blank they stand for.
        std::fill(local_row_data.characters.begin(),
                  local_row_data.characters.begin() + head_columns_, 0x20);
        std::fill(local_row_data.parity_error.begin(),
                  local_row_data.parity_error.begin() + head_columns_, false);
      }
      for (int column = head_columns_; column < columns_; ++column) {
        const auto i = static_cast<size_t>(column);
        const bool have = stored.extension_present;
        local_row_data.characters[i] = have ? stored.characters[i] : 0x20;
        local_row_data.parity_error[i] = have && stored.parity_error[i];
      }
      local_row_data.present =
          local_row_data.present || stored.extension_present;
    }
    const RowData& row_data = *row_source;
    auto& cells = snapshot.cells[static_cast<size_t>(row)];
    snapshot.row_received[static_cast<size_t>(row)] = row_data.present;
    // Without a squasher — or for a row it has no copies of, which is this
    // magazine's own store answering — the row rests on the one copy received.
    if (row_copies == 0 && row_data.present && row >= 1) {
      row_copies = 1;
    }
    snapshot.row_copies[static_cast<size_t>(row)] = row_copies;

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

    for (int column = 0; column < columns_; ++column) {
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
    for (int column = 0; column < snapshot.columns; ++column) {
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
    for (int column = 0; column < snapshot.columns; ++column) {
      const TeletextPageCell& cell = cells[static_cast<size_t>(column)];
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
