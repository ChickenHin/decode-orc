/*
 * File:        teletext_packet_fixtures.h
 * Module:      orc-tests/gui/unit
 * Purpose:     Hand-built T42 packet fixtures for teletext preview tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/support/teletext_page_decoder.h>
#include <orc/support/teletext_slicer.h>
#include <orc_teletext.h>

#include <array>
#include <string>

namespace gui_unit_test {

// Build a valid MRAG (packet bytes 0-1). EN 300 706 §7.1.2: byte 0 carries
// magazine bits plus packet-number bit 0, byte 1 packet-number bits 1-4;
// both Hamming 8/4 coded (§8.2).
inline std::array<uint8_t, 2> makeMrag(int magazine, int packet_number) {
  const auto nibble1 =
      static_cast<uint8_t>((magazine & 0x7) | ((packet_number & 0x1) << 3));
  const auto nibble2 = static_cast<uint8_t>((packet_number >> 1) & 0xF);
  return {orc::teletext_hamming84_encode(nibble1),
          orc::teletext_hamming84_encode(nibble2)};
}

// Build an X/0 page header packet (EN 300 706 §9.3.1): MRAG, Hamming 8/4
// page number / sub-code / control nibbles, then 32 odd-parity header
// display characters. Control bits beyond C4/C6 are left clear.
inline std::array<uint8_t, orc::kTeletextPacketBytes> makeHeaderPacket(
    int magazine, int page_number, int subcode = 0, bool erase_page = false,
    bool subtitle = false, const std::string& header_text = "") {
  std::array<uint8_t, orc::kTeletextPacketBytes> packet{};
  const auto mrag = makeMrag(magazine, 0);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  packet[2] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>(page_number & 0xF));  // page units
  packet[3] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>((page_number >> 4) & 0xF));  // page tens
  packet[4] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>(subcode & 0xF));  // S1
  packet[5] = orc::teletext_hamming84_encode(static_cast<uint8_t>(
      ((subcode >> 4) & 0x7) | (erase_page ? 0x8 : 0x0)));  // S2 + C4
  packet[6] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>((subcode >> 7) & 0xF));  // S3
  packet[7] = orc::teletext_hamming84_encode(static_cast<uint8_t>(
      ((subcode >> 11) & 0x3) | (subtitle ? 0x8 : 0x0)));  // S4 + C5 + C6
  packet[8] = orc::teletext_hamming84_encode(0x0);         // C7-C10
  packet[9] = orc::teletext_hamming84_encode(0x0);         // C11-C14
  for (size_t i = 0; i < 32; ++i) {
    const char c = i < header_text.size() ? header_text[i] : ' ';
    packet[10 + i] = orc::teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return packet;
}

// Build a directly displayable row packet X/1 to X/24 (EN 300 706 §9.3.2):
// MRAG then 40 odd-parity display bytes, padded with spaces.
inline std::array<uint8_t, orc::kTeletextPacketBytes> makeRowPacket(
    int magazine, int row, const std::string& text) {
  std::array<uint8_t, orc::kTeletextPacketBytes> packet{};
  const auto mrag = makeMrag(magazine, row);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  for (size_t i = 0; i < 40; ++i) {
    const char c = i < text.size() ? text[i] : ' ';
    packet[2 + i] = orc::teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return packet;
}

// A time-filling header (page number FF) that terminates the open page of a
// magazine without starting a new one (EN 300 706 §7.3).
inline std::array<uint8_t, orc::kTeletextPacketBytes> makeTimeFillingHeader(
    int magazine) {
  return makeHeaderPacket(magazine, 0xFF, 0x3F7F & 0x1FFF);
}

// Wrap packets as the view model delivered for one field.
inline orc::presenters::TeletextFieldPacketsView makeFieldView(
    std::initializer_list<std::array<uint8_t, orc::kTeletextPacketBytes>>
        packets,
    int first_line = 7) {
  orc::presenters::TeletextFieldPacketsView view;
  view.observed = true;
  view.present = packets.size() > 0;
  view.line_count = static_cast<int32_t>(packets.size());
  int line = first_line;
  for (const auto& bytes : packets) {
    orc::presenters::TeletextPacketView packet;
    packet.field_line = line++;
    packet.bytes = bytes;
    view.packets.push_back(packet);
  }
  return view;
}

// As makeFieldView(), with every byte of every packet recovered at the given
// confidence — how sure the recovery chain was of it (see the SDK's
// teletext_slicer.h).
inline orc::presenters::TeletextFieldPacketsView makeFieldViewWithConfidence(
    std::initializer_list<std::array<uint8_t, orc::kTeletextPacketBytes>>
        packets,
    float confidence, int first_line = 7) {
  auto view = makeFieldView(packets, first_line);
  for (auto& packet : view.packets) {
    packet.has_confidence = true;
    packet.confidence.fill(confidence);
  }
  return view;
}

// An observed field that carried no teletext data.
inline orc::presenters::TeletextFieldPacketsView makeEmptyFieldView() {
  orc::presenters::TeletextFieldPacketsView view;
  view.observed = true;
  return view;
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
