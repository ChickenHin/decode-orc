/*
 * File:        teletext_page_decoder_test.cpp
 * Module:      orc-tests/core/unit/support
 * Purpose:     Unit tests for the PAL WST teletext page decoder
 *
 * Covers: page assembly across header/body packets, serial vs parallel
 * magazine modes, sub-page replacement and row retention, Hamming 8/4
 * single-bit correction and double-bit rejection, odd-parity cell flagging,
 * Level 1 attribute rendering, and the subtitle cue lifecycle (C6 page
 * arrival / clear / erase). Hand-built packet sequences only; deterministic,
 * no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/support/teletext_page_decoder.h>
#include <orc/support/teletext_row_squasher.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "teletext_line_synthesizer.h"

namespace orc_unit_test {

namespace {

using orc::kTeletextPacketBytes;
using orc::TeletextColour;
using orc::TeletextPageDecoder;
using orc::TeletextPageSnapshot;
using orc::TeletextSubtitleCue;
using orc::tests::make_mrag;

struct HeaderFlags {
  bool erase_page = false;
  bool newsflash = false;
  bool subtitle = false;
  bool magazine_serial = false;
  int national_option_subset = 0;
};

// Build an X/0 page header packet (ETSI EN 300 706 §9.3.1): MRAG, Hamming
// 8/4 page number / sub-code / control nibbles, then 32 odd-parity header
// display characters.
std::array<uint8_t, kTeletextPacketBytes> make_header(
    int magazine, int page_number, int subcode, HeaderFlags flags = {},
    const std::string& header_text = "") {
  std::array<uint8_t, kTeletextPacketBytes> packet{};
  const auto mrag = make_mrag(magazine, 0);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  packet[2] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>(page_number & 0xF));  // page units
  packet[3] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>((page_number >> 4) & 0xF));  // page tens
  packet[4] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>(subcode & 0xF));  // S1
  packet[5] = orc::teletext_hamming84_encode(static_cast<uint8_t>(
      ((subcode >> 4) & 0x7) | (flags.erase_page ? 0x8 : 0x0)));  // S2 + C4
  packet[6] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>((subcode >> 7) & 0xF));  // S3
  packet[7] = orc::teletext_hamming84_encode(static_cast<uint8_t>(
      ((subcode >> 11) & 0x3) | (flags.newsflash ? 0x4 : 0x0) |
      (flags.subtitle ? 0x8 : 0x0)));               // S4 + C5 + C6
  packet[8] = orc::teletext_hamming84_encode(0x0);  // C7-C10
  packet[9] = orc::teletext_hamming84_encode(static_cast<uint8_t>(
      (flags.magazine_serial ? 0x1 : 0x0) |
      ((flags.national_option_subset & 0x7) << 1)));  // C11-C14
  for (size_t i = 0; i < 32; ++i) {
    const char c = i < header_text.size() ? header_text[i] : ' ';
    packet[10 + i] = orc::teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return packet;
}

// Build a directly displayable row packet X/1 to X/24 (EN 300 706 §9.3.2):
// MRAG then 40 odd-parity display bytes. Bytes of |text| are used verbatim
// (they may include spacing-attribute codes < 0x20), padded with spaces.
std::array<uint8_t, kTeletextPacketBytes> make_row(int magazine, int row,
                                                   const std::string& text) {
  std::array<uint8_t, kTeletextPacketBytes> packet{};
  const auto mrag = make_mrag(magazine, row);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  for (size_t i = 0; i < 40; ++i) {
    const char c = i < text.size() ? text[i] : ' ';
    packet[2 + i] = orc::teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return packet;
}

// A time-filling header (page number FF) that terminates transmissions
// without opening a page (EN 300 706 §7.3).
std::array<uint8_t, kTeletextPacketBytes> make_time_filling_header(
    int magazine, bool magazine_serial = false) {
  HeaderFlags flags;
  flags.magazine_serial = magazine_serial;
  return make_header(magazine, 0xFF, 0x3F7F & 0x1FFF, flags);
}

std::string row_text(const TeletextPageSnapshot& snapshot, int row) {
  std::string text;
  for (const auto& cell : snapshot.cells[static_cast<size_t>(row)]) {
    text.push_back(static_cast<char>(cell.character));
  }
  while (!text.empty() && text.back() == ' ') {
    text.pop_back();
  }
  return text;
}

class TeletextPageDecoderTest : public ::testing::Test {
 protected:
  TeletextPageDecoderTest() {
    decoder_.set_page_callback([this](const TeletextPageSnapshot& snapshot) {
      snapshots_.push_back(snapshot);
    });
  }

  TeletextPageDecoder decoder_;
  std::vector<TeletextPageSnapshot> snapshots_;
};

}  // namespace

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TeletextPageDecoderTest, ParsePageNumber_AcceptsConventionalForms) {
  const auto p100 = TeletextPageDecoder::parse_page_number("100");
  ASSERT_TRUE(p100.has_value());
  EXPECT_EQ(p100->first, 1);
  EXPECT_EQ(p100->second, 0x00);

  const auto p888 = TeletextPageDecoder::parse_page_number("888");
  ASSERT_TRUE(p888.has_value());
  EXPECT_EQ(p888->first, 8);
  EXPECT_EQ(p888->second, 0x88);

  const auto hex_page = TeletextPageDecoder::parse_page_number("1fA");
  ASSERT_TRUE(hex_page.has_value());
  EXPECT_EQ(hex_page->first, 1);
  EXPECT_EQ(hex_page->second, 0xFA);
}

TEST_F(TeletextPageDecoderTest, ParsePageNumber_RejectsMalformedStrings) {
  EXPECT_FALSE(TeletextPageDecoder::parse_page_number("").has_value());
  EXPECT_FALSE(TeletextPageDecoder::parse_page_number("88").has_value());
  EXPECT_FALSE(TeletextPageDecoder::parse_page_number("8888").has_value());
  EXPECT_FALSE(TeletextPageDecoder::parse_page_number("088").has_value());
  EXPECT_FALSE(TeletextPageDecoder::parse_page_number("988").has_value());
  EXPECT_FALSE(TeletextPageDecoder::parse_page_number("8G8").has_value());
}

TEST_F(TeletextPageDecoderTest, AssemblesPageAcrossHeaderAndBodyPackets) {
  decoder_.process_packet(make_header(1, 0x00, 0x0001, {}, "P100 HEADER"), 0);
  decoder_.process_packet(make_row(1, 1, "HELLO TELETEXT"), 1);
  decoder_.process_packet(make_row(1, 3, "ROW THREE"), 2);
  decoder_.process_packet(make_time_filling_header(1), 3);

  ASSERT_EQ(snapshots_.size(), 1u);
  const auto& page = snapshots_[0];
  EXPECT_EQ(page.magazine, 1);
  EXPECT_EQ(page.page_number, 0x00);
  EXPECT_EQ(page.subcode, 0x0001);
  EXPECT_EQ(page.header_field_index, 0);
  EXPECT_EQ(page.last_field_index, 2);
  // Header display text lands in row 0 columns 8-39 (EN 300 706 §9.3.1.4).
  EXPECT_EQ(row_text(page, 0), "        P100 HEADER");
  EXPECT_EQ(row_text(page, 1), "HELLO TELETEXT");
  EXPECT_EQ(row_text(page, 2), "");
  EXPECT_EQ(row_text(page, 3), "ROW THREE");
}

TEST_F(TeletextPageDecoderTest, FinalizeFlushesOpenPageAssemblies) {
  decoder_.process_packet(make_header(2, 0x34, 0, {}), 0);
  decoder_.process_packet(make_row(2, 1, "UNTERMINATED"), 1);
  EXPECT_TRUE(snapshots_.empty());

  decoder_.finalize(2);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(snapshots_[0].magazine, 2);
  EXPECT_EQ(snapshots_[0].page_number, 0x34);
  EXPECT_EQ(row_text(snapshots_[0], 1), "UNTERMINATED");
}

TEST_F(TeletextPageDecoderTest, ParallelMode_InterleavedMagazinesAssemble) {
  // Parallel mode (C11 clear): a header only terminates the page of its own
  // magazine (EN 300 706 §7.2.1), so rows of two magazines may interleave.
  decoder_.process_packet(make_header(1, 0x11, 0, {}), 0);
  decoder_.process_packet(make_header(2, 0x22, 0, {}), 1);
  decoder_.process_packet(make_row(1, 1, "MAGAZINE ONE"), 2);
  decoder_.process_packet(make_row(2, 1, "MAGAZINE TWO"), 3);
  decoder_.process_packet(make_row(1, 2, "MORE OF ONE"), 4);
  EXPECT_TRUE(snapshots_.empty());

  decoder_.process_packet(make_time_filling_header(1), 5);
  decoder_.process_packet(make_time_filling_header(2), 6);

  ASSERT_EQ(snapshots_.size(), 2u);
  EXPECT_EQ(snapshots_[0].page_number, 0x11);
  EXPECT_EQ(row_text(snapshots_[0], 1), "MAGAZINE ONE");
  EXPECT_EQ(row_text(snapshots_[0], 2), "MORE OF ONE");
  EXPECT_EQ(snapshots_[1].page_number, 0x22);
  EXPECT_EQ(row_text(snapshots_[1], 1), "MAGAZINE TWO");
}

TEST_F(TeletextPageDecoderTest, SerialMode_AnyHeaderTerminatesOpenPage) {
  // Serial mode (C11 set): any page header terminates the page in
  // transmission regardless of magazine (EN 300 706 §7.2.1).
  HeaderFlags serial;
  serial.magazine_serial = true;
  decoder_.process_packet(make_header(1, 0x11, 0, serial), 0);
  decoder_.process_packet(make_row(1, 1, "SERIAL PAGE"), 1);

  decoder_.process_packet(make_header(2, 0x22, 0, serial), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(snapshots_[0].magazine, 1);
  EXPECT_EQ(snapshots_[0].page_number, 0x11);
  EXPECT_TRUE(snapshots_[0].magazine_serial);
  EXPECT_EQ(row_text(snapshots_[0], 1), "SERIAL PAGE");

  // A row for magazine 1 after the terminating header is an orphan (no page
  // open) and must be dropped.
  decoder_.process_packet(make_row(1, 2, "ORPHAN"), 3);
  decoder_.finalize(4);
  ASSERT_EQ(snapshots_.size(), 2u);
  EXPECT_EQ(snapshots_[1].page_number, 0x22);
  EXPECT_EQ(row_text(snapshots_[1], 2), "");
}

TEST_F(TeletextPageDecoderTest, SubpageReplacement_ClearsStoredRows) {
  decoder_.process_packet(make_header(1, 0x50, 0x0001, {}), 0);
  decoder_.process_packet(make_row(1, 1, "SUBPAGE ONE"), 1);
  decoder_.process_packet(make_row(1, 2, "SHARED ROW"), 2);

  // Same page, new sub-code: sub-page replacement starts from a clean grid.
  decoder_.process_packet(make_header(1, 0x50, 0x0002, {}), 3);
  decoder_.process_packet(make_row(1, 1, "SUBPAGE TWO"), 4);
  decoder_.process_packet(make_time_filling_header(1), 5);

  ASSERT_EQ(snapshots_.size(), 2u);
  EXPECT_EQ(snapshots_[0].subcode, 0x0001);
  EXPECT_EQ(row_text(snapshots_[0], 1), "SUBPAGE ONE");
  EXPECT_EQ(snapshots_[1].subcode, 0x0002);
  EXPECT_EQ(row_text(snapshots_[1], 1), "SUBPAGE TWO");
  EXPECT_EQ(row_text(snapshots_[1], 2), "");
}

TEST_F(TeletextPageDecoderTest, RetransmissionWithoutErase_RetainsStoredRows) {
  decoder_.process_packet(make_header(1, 0x50, 0x0001, {}), 0);
  decoder_.process_packet(make_row(1, 1, "ROW ONE"), 1);
  decoder_.process_packet(make_row(1, 2, "ROW TWO"), 2);

  // Retransmission of the same page and sub-code without C4: stored rows
  // persist and newly received rows overwrite (EN 300 706 §9.3.1.3, C4).
  decoder_.process_packet(make_header(1, 0x50, 0x0001, {}), 3);
  decoder_.process_packet(make_row(1, 1, "ROW ONE UPDATED"), 4);
  decoder_.process_packet(make_time_filling_header(1), 5);

  ASSERT_EQ(snapshots_.size(), 2u);
  EXPECT_EQ(row_text(snapshots_[1], 1), "ROW ONE UPDATED");
  EXPECT_EQ(row_text(snapshots_[1], 2), "ROW TWO");
}

TEST_F(TeletextPageDecoderTest, EraseControlBit_ClearsStoredRows) {
  decoder_.process_packet(make_header(1, 0x50, 0x0001, {}), 0);
  decoder_.process_packet(make_row(1, 2, "STALE ROW"), 1);

  HeaderFlags erase;
  erase.erase_page = true;
  decoder_.process_packet(make_header(1, 0x50, 0x0001, erase), 2);
  decoder_.process_packet(make_row(1, 1, "FRESH ROW"), 3);
  decoder_.process_packet(make_time_filling_header(1), 4);

  ASSERT_EQ(snapshots_.size(), 2u);
  EXPECT_TRUE(snapshots_[1].erase_page);
  EXPECT_EQ(row_text(snapshots_[1], 1), "FRESH ROW");
  EXPECT_EQ(row_text(snapshots_[1], 2), "");
}

TEST_F(TeletextPageDecoderTest, Hamming_SingleBitErrorInMragIsCorrected) {
  auto row = make_row(1, 1, "CORRECTED");
  row[0] ^= 0x10;  // single-bit error in the first MRAG byte (§8.2)

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(row, 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(row_text(snapshots_[0], 1), "CORRECTED");
}

TEST_F(TeletextPageDecoderTest, Hamming_DoubleBitErrorDropsPacket) {
  auto row = make_row(1, 1, "REJECTED");
  row[0] ^= 0x12;  // double-bit error: uncorrectable (§8.2)

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(row, 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(row_text(snapshots_[0], 1), "");
}

TEST_F(TeletextPageDecoderTest,
       Hamming_UncorrectablePageNumberDropsHeaderOnly) {
  decoder_.process_packet(make_header(1, 0x11, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "FIRST PAGE"), 1);

  // A header whose page-units byte carries a double-bit error cannot be
  // attributed to a page; the packet is dropped and the open page keeps
  // assembling.
  auto bad_header = make_header(1, 0x22, 0, {});
  bad_header[2] ^= 0x12;
  decoder_.process_packet(bad_header, 2);
  decoder_.process_packet(make_row(1, 2, "STILL FIRST"), 3);
  decoder_.process_packet(make_time_filling_header(1), 4);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(snapshots_[0].page_number, 0x11);
  EXPECT_EQ(row_text(snapshots_[0], 1), "FIRST PAGE");
  EXPECT_EQ(row_text(snapshots_[0], 2), "STILL FIRST");
}

TEST_F(TeletextPageDecoderTest, ParityError_FlagsCellWithoutCorruptingPage) {
  auto row = make_row(1, 1, "AXC");
  row[2 + 1] ^= 0x01;  // break odd parity on the 'X' (EN 300 706 §8.1)

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(row, 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  const auto& cells = snapshots_[0].cells[1];
  EXPECT_EQ(cells[0].character, 'A');
  EXPECT_FALSE(cells[0].parity_error);
  EXPECT_EQ(cells[1].character, 0x20);
  EXPECT_TRUE(cells[1].parity_error);
  EXPECT_EQ(cells[2].character, 'C');
  EXPECT_FALSE(cells[2].parity_error);
}

// A row that never arrived renders identically to a transmitted blank row,
// so only row_received can tell a recovery gap from page content.
TEST_F(TeletextPageDecoderTest, RowReceivedMarksTheRowsThatArrived) {
  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "ROW ONE"), 1);
  // No packet for row 2 — lost in recovery.
  decoder_.process_packet(make_row(1, 3, "   "), 2);  // transmitted but blank
  decoder_.process_packet(make_time_filling_header(1), 3);

  ASSERT_EQ(snapshots_.size(), 1u);
  const auto& snapshot = snapshots_[0];
  EXPECT_TRUE(snapshot.row_received[0]);  // the X/0 header
  EXPECT_TRUE(snapshot.row_received[1]);
  EXPECT_FALSE(snapshot.row_received[2]);
  // A transmitted blank row looks like row 2 in the cells but is not a gap.
  EXPECT_TRUE(snapshot.row_received[3]);
  EXPECT_EQ(row_text(snapshot, 2), row_text(snapshot, 3));
  EXPECT_FALSE(snapshot.row_received[4]);
}

// With a squasher attached, rows recovered during an earlier transmission
// stay available when a later one is clipped, and repeated copies of a row
// correct each other (orc/support/teletext_row_squasher.h).
TEST_F(TeletextPageDecoderTest, SquasherKeepsRowsAcrossClippedTransmissions) {
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "ROW ONE"), 1);
  decoder_.process_packet(make_row(1, 2, "ROW TWO"), 2);
  // A second transmission carrying only row 1 — the rest fell outside.
  decoder_.process_packet(make_header(1, 0x00, 0, {}), 3);
  decoder_.process_packet(make_row(1, 1, "ROW ONE"), 4);
  decoder_.process_packet(make_time_filling_header(1), 5);

  ASSERT_EQ(snapshots_.size(), 2u);
  const auto& clipped = snapshots_.back();
  EXPECT_EQ(row_text(clipped, 1), "ROW ONE");
  EXPECT_EQ(row_text(clipped, 2), "ROW TWO")
      << "a row the clipped transmission did not carry was lost";
  EXPECT_TRUE(clipped.row_received[2]);
}

// How many copies a row rests on is the page's confidence in it. Hamming 8/4
// corrects one bit and detects two (EN 300 706 §8.2), but a longer burst can
// carry a row's address onto another valid one, and a row that arrived once
// has nothing to contradict it if that happens.
TEST_F(TeletextPageDecoderTest, RowCopiesCountWhatEachRowRestsOn) {
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "ROW ONE"), 1);
  decoder_.process_packet(make_row(1, 2, "ROW TWO"), 2);
  // A second pass of the carousel confirms row 1 but not row 2.
  decoder_.process_packet(make_header(1, 0x00, 0, {}), 3);
  decoder_.process_packet(make_row(1, 1, "ROW ONE"), 4);
  decoder_.process_packet(make_time_filling_header(1), 5);

  ASSERT_EQ(snapshots_.size(), 2u);
  const auto& snapshot = snapshots_.back();
  EXPECT_EQ(snapshot.row_copies[0], 0) << "header rows are never squashed";
  EXPECT_EQ(snapshot.row_copies[1], 2);
  EXPECT_EQ(snapshot.row_copies[2], 1);
  EXPECT_EQ(snapshot.row_copies[3], 0) << "no packet was received for row 3";
}

TEST_F(TeletextPageDecoderTest, RowCopiesIsOneWithoutASquasher) {
  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "ROW ONE"), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(snapshots_[0].row_copies[1], 1);
  EXPECT_EQ(snapshots_[0].row_copies[2], 0);
}

TEST_F(TeletextPageDecoderTest, SquasherRepairsAParityDamagedByte) {
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  auto damaged = make_row(1, 1, "HELLO");
  damaged[2] ^= 0x01;  // break odd parity on the leading display byte

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(damaged, 1);
  decoder_.process_packet(make_header(1, 0x00, 0, {}), 2);
  decoder_.process_packet(make_row(1, 1, "HELLO"), 3);
  decoder_.process_packet(make_time_filling_header(1), 4);

  ASSERT_GE(snapshots_.size(), 2u);
  const auto& combined = snapshots_.back();
  EXPECT_EQ(row_text(combined, 1), "HELLO");
  EXPECT_FALSE(combined.cells[1][0].parity_error)
      << "the damaged byte was not repaired from the clean copy";
}

// C4 replaces the page rather than updating it (EN 300 706 §9.3.1.3 Table 2),
// so accumulated copies must not bleed into the new content.
TEST_F(TeletextPageDecoderTest, SquasherDropsAccumulatedRowsOnErasePage) {
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "OLD ONE"), 1);
  decoder_.process_packet(make_row(1, 2, "OLD TWO"), 2);
  // Erase (C4) then a page that only uses row 1.
  HeaderFlags erase;
  erase.erase_page = true;
  decoder_.process_packet(make_header(1, 0x00, 0, erase), 3);
  decoder_.process_packet(make_row(1, 1, "NEW ONE"), 4);
  decoder_.process_packet(make_time_filling_header(1), 5);

  ASSERT_EQ(snapshots_.size(), 2u);
  const auto& fresh = snapshots_.back();
  EXPECT_EQ(row_text(fresh, 1), "NEW ONE");
  EXPECT_EQ(row_text(fresh, 2), "") << "erased row survived the erase";
  EXPECT_FALSE(fresh.row_received[2]);
}

// The copies from before an erase are separated from those after it by the
// key's erase_epoch, not deleted: a consumer replaying the same stream reaches
// the same epoch at the same packet and can still ask about the earlier run.
TEST_F(TeletextPageDecoderTest, ErasePageSeparatesRunsWithoutDiscardingThem) {
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "OLD ONE"), 1);
  HeaderFlags erase;
  erase.erase_page = true;
  decoder_.process_packet(make_header(1, 0x00, 0, erase), 2);
  decoder_.process_packet(make_row(1, 1, "NEW ONE"), 3);

  const orc::TeletextPageKey before{1, 0x00, 0, 0};
  const orc::TeletextPageKey after{1, 0x00, 0, 1};
  ASSERT_EQ(squasher.copy_count(before, 1), 1u);
  ASSERT_EQ(squasher.copy_count(after, 1), 1u);

  const auto old_row = squasher.squashed_row(before, 1);
  ASSERT_TRUE(old_row.has_value());
  EXPECT_EQ((*old_row)[0], orc::teletext_odd_parity_encode('O'));
  const auto new_row = squasher.squashed_row(after, 1);
  ASSERT_TRUE(new_row.has_value());
  EXPECT_EQ((*new_row)[0], orc::teletext_odd_parity_encode('N'));
}

// Erasing one page must not orphan the copies of another page carried in the
// same magazine: the epoch is counted per sub-page, not per magazine.
TEST_F(TeletextPageDecoderTest, ErasePageLeavesOtherPagesOfTheMagazineAlone) {
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  decoder_.process_packet(make_header(1, 0x10, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "PAGE TEN"), 1);
  HeaderFlags erase;
  erase.erase_page = true;
  decoder_.process_packet(make_header(1, 0x20, 0, erase), 2);
  decoder_.process_packet(make_row(1, 1, "PAGE TWENTY"), 3);
  // Page 0x10 comes round again and must land in the epoch it started in.
  decoder_.process_packet(make_header(1, 0x10, 0, {}), 4);
  decoder_.process_packet(make_row(1, 1, "PAGE TEN"), 5);

  EXPECT_EQ(squasher.copy_count(orc::TeletextPageKey{1, 0x10, 0, 0}, 1), 2u);
  EXPECT_EQ(squasher.copy_count(orc::TeletextPageKey{1, 0x20, 0, 1}, 1), 1u);
}

// A consumer rewriting a recovered stream feeds it once to build the squasher
// and again to apply it. The second pass must not destroy what the first
// built — which is what deleting the copies on C4 used to do, leaving the
// rewrite with nothing to correct against.
TEST_F(TeletextPageDecoderTest, ReplayingAStreamPreservesTheAccumulatedCopies) {
  orc::TeletextRowSquasher squasher;

  HeaderFlags erase;
  erase.erase_page = true;
  const std::vector<std::array<uint8_t, kTeletextPacketBytes>> stream{
      make_header(1, 0x00, 0, erase),
      make_row(1, 1, "HELLO"),
      make_header(1, 0x00, 0, erase),
      make_row(1, 1, "HELLO"),
  };

  TeletextPageDecoder first;
  first.set_row_squasher(&squasher);
  for (size_t i = 0; i < stream.size(); ++i) {
    first.process_packet(stream[i], static_cast<int64_t>(i),
                         static_cast<int64_t>(i));
  }
  const size_t runs_after_first_pass = squasher.page_count();

  // Second pass, fresh decoder, same squasher, same source ids.
  TeletextPageDecoder second;
  second.set_row_squasher(&squasher);
  for (size_t i = 0; i < stream.size(); ++i) {
    second.process_packet(stream[i], static_cast<int64_t>(i),
                          static_cast<int64_t>(i));
  }

  EXPECT_EQ(squasher.page_count(), runs_after_first_pass)
      << "the replay created runs the first pass did not";
  // Each copy was replaced under its own source id, not counted again.
  EXPECT_EQ(squasher.copy_count(orc::TeletextPageKey{1, 0x00, 0, 1}, 1), 1u);
  EXPECT_EQ(squasher.copy_count(orc::TeletextPageKey{1, 0x00, 0, 2}, 1), 1u);
}

TEST_F(TeletextPageDecoderTest, RendersLevel1ColourAndMosaicAttributes) {
  // 0/1 alpha red ("Set-After"), text, 1/2 mosaic green, mosaic glyphs.
  std::string row;
  row.push_back(0x01);  // Alpha Red
  row += "AB";
  row.push_back(0x02);  // Alpha Green
  row.push_back(0x1D);  // New Background (Set-At: background = red)
  row += "C";
  row.push_back(0x13);  // Mosaic Yellow
  row.push_back(0x35);  // G1 mosaic glyph
  row.push_back(0x45);  // G1 column 4: alphanumeric capital even in mosaics

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, row), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  const auto& cells = snapshots_[0].cells[1];

  // Column 0: the colour code itself renders as a white space (Set-After).
  EXPECT_EQ(cells[0].character, 0x20);
  EXPECT_EQ(cells[0].foreground, TeletextColour::White);
  // Columns 1-2: red alphanumerics.
  EXPECT_EQ(cells[1].character, 'A');
  EXPECT_EQ(cells[1].foreground, TeletextColour::Red);
  EXPECT_FALSE(cells[1].mosaic);
  // Column 3 is the Alpha Green code (still red foreground, Set-After);
  // column 4 is New Background, Set-At: background becomes green.
  EXPECT_EQ(cells[3].foreground, TeletextColour::Red);
  EXPECT_EQ(cells[4].background, TeletextColour::Green);
  // Column 5: green 'C' on green background.
  EXPECT_EQ(cells[5].character, 'C');
  EXPECT_EQ(cells[5].foreground, TeletextColour::Green);
  EXPECT_EQ(cells[5].background, TeletextColour::Green);
  // Column 7: yellow mosaic glyph.
  EXPECT_EQ(cells[7].character, 0x35);
  EXPECT_TRUE(cells[7].mosaic);
  EXPECT_EQ(cells[7].foreground, TeletextColour::Yellow);
  // Column 8: G1 column 4/5 codes stay alphanumeric in mosaics mode.
  EXPECT_EQ(cells[8].character, 0x45);
  EXPECT_FALSE(cells[8].mosaic);
}

TEST_F(TeletextPageDecoderTest, DoubleHeight_ConsumesTheRowBelow) {
  std::string row;
  row.push_back(0x0D);  // Double Height ("Set-After")
  row += "BIG";

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, row), 1);
  decoder_.process_packet(make_row(1, 2, "IGNORED DATA"), 2);
  decoder_.process_packet(make_time_filling_header(1), 3);

  ASSERT_EQ(snapshots_.size(), 1u);
  const auto& page = snapshots_[0];
  EXPECT_TRUE(page.cells[1][1].double_height);
  EXPECT_EQ(page.cells[1][1].character, 'B');
  // The transmitted row 2 is ignored; it renders as the lower half of the
  // double-height pair (EN 300 706 §12.2 0/D).
  EXPECT_TRUE(page.cells[2][1].double_height_lower);
  EXPECT_EQ(page.cells[2][1].character, 0x20);
  EXPECT_EQ(row_text(page, 2), "");
}

TEST_F(TeletextPageDecoderTest, HoldMosaics_SubstitutesHeldCharacter) {
  std::string row;
  row.push_back(0x11);  // Mosaic Red
  row.push_back(0x1E);  // Hold Mosaics ("Set-At")
  row.push_back(0x3F);  // mosaic glyph -> becomes the held character
  row.push_back(0x12);  // Mosaic Green: displayed as the held glyph
  row.push_back(0x3A);

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, row), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  const auto& cells = snapshots_[0].cells[1];
  EXPECT_EQ(cells[2].character, 0x3F);
  EXPECT_TRUE(cells[2].mosaic);
  // The colour-change cell shows the held mosaic instead of a space.
  EXPECT_EQ(cells[3].character, 0x3F);
  EXPECT_TRUE(cells[3].held_mosaic);
  EXPECT_EQ(cells[3].foreground, TeletextColour::Red);
  EXPECT_EQ(cells[4].character, 0x3A);
  EXPECT_EQ(cells[4].foreground, TeletextColour::Green);
}

////////////////////////////////////////////////////////////////////////////////////////////

namespace {

// Subtitle-style row: boxed text per the C5/C6 double Start Box convention
// (EN 300 706 §12.2 0/B).
std::string boxed(const std::string& text) {
  std::string row;
  row.push_back(0x0B);
  row.push_back(0x0B);
  row += text;
  row.push_back(0x0A);
  return row;
}

HeaderFlags subtitle_header_flags(bool erase) {
  HeaderFlags flags;
  flags.subtitle = true;
  flags.erase_page = erase;
  return flags;
}

}  // namespace

class TeletextSubtitleCueTest : public ::testing::Test {
 protected:
  TeletextSubtitleCueTest() { EXPECT_TRUE(decoder_.set_subtitle_page("888")); }

  // Transmission magazine 0 carries displayed magazine 8 (page 888).
  static constexpr int kMagazine = 0;
  static constexpr int kPage = 0x88;

  TeletextPageDecoder decoder_;
};

TEST_F(TeletextSubtitleCueTest, SetSubtitlePage_RejectsMalformedPage) {
  TeletextPageDecoder decoder;
  EXPECT_FALSE(decoder.set_subtitle_page("98"));
  EXPECT_FALSE(decoder.set_subtitle_page("hello"));
}

TEST_F(TeletextSubtitleCueTest, PageArrivalOpensCueAndFinalizeClosesIt) {
  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 10);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("HELLO SUBTITLE")), 12);
  decoder_.process_packet(make_time_filling_header(kMagazine), 14);
  EXPECT_TRUE(decoder_.subtitle_cues().empty());  // cue still on screen

  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "HELLO SUBTITLE");
  EXPECT_EQ(cues[0].start_field_index, 12);
  EXPECT_EQ(cues[0].end_field_index, 100);
}

TEST_F(TeletextSubtitleCueTest, EraseHeaderWithEmptyPageClearsTheCue) {
  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 0);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("FIRST CUE")), 2);
  decoder_.process_packet(make_time_filling_header(kMagazine), 4);

  // Erase transmission with no rows: clears the display at header arrival.
  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 50);
  decoder_.process_packet(make_time_filling_header(kMagazine), 52);
  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "FIRST CUE");
  EXPECT_EQ(cues[0].start_field_index, 2);
  EXPECT_EQ(cues[0].end_field_index, 50);
}

TEST_F(TeletextSubtitleCueTest, ChangedTextReplacesTheOpenCue) {
  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 0);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("FIRST CUE")), 2);

  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 40);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("SECOND CUE")), 42);
  decoder_.process_packet(make_time_filling_header(kMagazine), 44);
  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 2u);
  EXPECT_EQ(cues[0].text, "FIRST CUE");
  EXPECT_EQ(cues[0].start_field_index, 2);
  EXPECT_EQ(cues[0].end_field_index, 40);
  EXPECT_EQ(cues[1].text, "SECOND CUE");
  EXPECT_EQ(cues[1].start_field_index, 42);
  EXPECT_EQ(cues[1].end_field_index, 100);
}

TEST_F(TeletextSubtitleCueTest, UnchangedRetransmissionKeepsTheCueOnScreen) {
  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 0);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("STEADY CUE")), 2);

  // Carousel repeat of the identical page must not split the cue. The
  // repeat is a non-erase retransmission (rows retained).
  HeaderFlags repeat = subtitle_header_flags(false);
  decoder_.process_packet(make_header(kMagazine, kPage, 0, repeat), 40);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("STEADY CUE")), 42);
  decoder_.process_packet(make_time_filling_header(kMagazine), 44);
  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "STEADY CUE");
  EXPECT_EQ(cues[0].start_field_index, 2);
  EXPECT_EQ(cues[0].end_field_index, 100);
}

TEST_F(TeletextSubtitleCueTest, HeaderWithoutSubtitleFlagClearsTheCue) {
  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 0);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("VANISHING")), 2);
  decoder_.process_packet(make_time_filling_header(kMagazine), 4);

  // The page reappears without C6: no longer a subtitle page → clear.
  decoder_.process_packet(make_header(kMagazine, kPage, 0, {}), 60);
  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].end_field_index, 60);
}

TEST_F(TeletextSubtitleCueTest, OtherPagesDoNotEmitCues) {
  decoder_.process_packet(
      make_header(kMagazine, 0x77, 0, subtitle_header_flags(true)), 0);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("WRONG PAGE")), 2);
  decoder_.process_packet(make_time_filling_header(kMagazine), 4);
  decoder_.finalize(100);

  EXPECT_TRUE(decoder_.subtitle_cues().empty());
}

TEST_F(TeletextSubtitleCueTest, UnboxedTextIsExcludedFromSubtitlePageCues) {
  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 0);
  // "NOISE" is outside the boxed region and must not leak into the cue
  // (EN 300 706 §12.2 0/B: characters outside the box are not displayed).
  decoder_.process_packet(
      make_row(kMagazine, 20, boxed("BOXED TEXT") + " NOISE"), 2);
  decoder_.process_packet(make_time_filling_header(kMagazine), 4);
  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "BOXED TEXT");
}

TEST_F(TeletextSubtitleCueTest, MultiRowSubtitlesJoinWithNewlines) {
  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 0);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("LINE ONE")), 1);
  decoder_.process_packet(make_row(kMagazine, 22, boxed("LINE TWO")), 2);
  decoder_.process_packet(make_time_filling_header(kMagazine), 3);
  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "LINE ONE\nLINE TWO");
}

// ---------------------------------------------------------------------------
// 525-line WST (ITU-R BT.653 Table 1b): a 34-byte packet, so 32-column rows
// and 24 header-text characters. Everything the decoder reads by position —
// MRAG, page number, sub-code, control bits — is at the same offsets, so these
// build on the 625-line helpers and simply stop short.
// ---------------------------------------------------------------------------

using orc::kTeletext525PacketBytes;

// Display columns of a 525-line page: the packet less its two MRAG bytes.
constexpr int k525Columns = static_cast<int>(kTeletext525PacketBytes) - 2;

std::array<uint8_t, kTeletextPacketBytes> make_525_header(
    int magazine, int page_number, int subcode, HeaderFlags flags = {},
    const std::string& header_text = "") {
  auto packet = make_header(magazine, page_number, subcode, flags, header_text);
  // Bytes past the 34 the service transmits were never sent.
  for (size_t i = kTeletext525PacketBytes; i < kTeletextPacketBytes; ++i) {
    packet[i] = 0;
  }
  return packet;
}

std::array<uint8_t, kTeletextPacketBytes> make_525_row(
    int magazine, int row, const std::string& text) {
  auto packet = make_row(magazine, row, text);
  for (size_t i = kTeletext525PacketBytes; i < kTeletextPacketBytes; ++i) {
    packet[i] = 0;
  }
  return packet;
}

std::array<uint8_t, kTeletextPacketBytes> make_525_time_filling_header(
    int magazine) {
  return make_525_header(magazine, 0xFF, 0x3F7F & 0x1FFF);
}

class Teletext525PageDecoderTest : public TeletextPageDecoderTest {
 protected:
  // Every packet of a 525-line stream carries its own length; the decoder
  // takes the row width from it.
  void feed(const std::array<uint8_t, kTeletextPacketBytes>& packet,
            int64_t field_index) {
    decoder_.process_packet(packet, field_index,
                            TeletextPageDecoder::kAutoSource,
                            /*confidence=*/nullptr, kTeletext525PacketBytes);
  }
};

TEST_F(Teletext525PageDecoderTest, PageIsThirtyTwoColumnsWide) {
  feed(make_525_header(1, 0x00, 0, {}, "ELECTRA NEWS"), 0);
  feed(make_525_row(1, 1, "TOP STORY"), 1);
  feed(make_525_time_filling_header(1), 2);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& page = snapshots_.front();
  EXPECT_EQ(page.columns, k525Columns);
  EXPECT_EQ(page.magazine, 1);
  EXPECT_EQ(page.page_number, 0x00);
  EXPECT_EQ(row_text(page, 1), "TOP STORY");
}

TEST_F(Teletext525PageDecoderTest, HeaderTextStopsAtTheServiceWidth) {
  // 24 header-text characters from column 8, not 32 (Table 1b §3.4 leaves a
  // 32-byte data block, of which the header spends 8 on addressing).
  const std::string text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  feed(make_525_header(1, 0x00, 0, {}, text), 0);
  feed(make_525_time_filling_header(1), 1);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& page = snapshots_.front();
  EXPECT_EQ(row_text(page, 0), "        " + text.substr(0, k525Columns - 8));
}

TEST_F(Teletext525PageDecoderTest, ColumnsBeyondTheServiceWidthStayBlank) {
  // The bytes past the packet were never transmitted; they must not surface as
  // content, and — being zero — must not surface as parity damage either.
  feed(make_525_header(1, 0x00, 0), 0);
  feed(make_525_row(1, 5, std::string(40, 'X')), 1);
  feed(make_525_time_filling_header(1), 2);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& page = snapshots_.front();
  EXPECT_EQ(row_text(page, 5), std::string(k525Columns, 'X'));
  for (int column = k525Columns; column < TeletextPageSnapshot::kColumns;
       ++column) {
    const auto& cell = page.cells[5][static_cast<size_t>(column)];
    EXPECT_EQ(cell.character, 0x20) << "column " << column;
    EXPECT_FALSE(cell.parity_error) << "column " << column;
  }
}

TEST_F(Teletext525PageDecoderTest, SubtitleTextStopsAtTheServiceWidth) {
  HeaderFlags flags;
  flags.subtitle = true;
  ASSERT_TRUE(decoder_.set_subtitle_page("100"));
  feed(make_525_header(1, 0x00, 0, flags), 0);
  // Start Box / End Box around the text (EN 300 706 §12.2 0/A-0/B).
  std::string boxed_row;
  boxed_row.push_back(0x0B);
  boxed_row.push_back(0x0B);
  boxed_row += "ELECTRA";
  boxed_row.push_back(0x0A);
  feed(make_525_row(1, 20, boxed_row), 1);
  feed(make_525_time_filling_header(1), 2);
  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "ELECTRA");
}

TEST_F(TeletextPageDecoderTest, DefaultPacketLengthKeepsTheFortyColumnPage) {
  // The 625-line default is unchanged by the width having become a parameter.
  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(make_row(1, 1, std::string(40, 'Y')), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_EQ(snapshots_.front().columns, TeletextPageSnapshot::kColumns);
  EXPECT_EQ(row_text(snapshots_.front(), 1), std::string(40, 'Y'));
}

}  // namespace orc_unit_test
