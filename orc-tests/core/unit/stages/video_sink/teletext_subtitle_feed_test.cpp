/*
 * File:        teletext_subtitle_feed_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the video sink teletext subtitle collection
 *              pass
 *
 * Covers: cue extraction from stored "teletext" observations (the
 * deps/backend seam of the mov_text embedding path), timing conversion to
 * seconds, malformed page rejection, and empty-context behaviour. All
 * observations are served by a mocked context; no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/plugins/stages/sinks/common/teletext_subtitle_feed.h"

#include <gtest/gtest.h>
#include <orc/support/teletext_page_decoder.h>
#include <orc/support/teletext_slicer.h>

#include <array>
#include <optional>
#include <string>

#include "../../include/observation_context_interface_mock.h"
#include "../../support/teletext_line_synthesizer.h"

using testing::_;  // NOLINT(bugprone-reserved-identifier)
using testing::NiceMock;
using testing::Return;

namespace orc_unit_test {

namespace {

// X/0 page header for transmission magazine 0 (displayed magazine 8) with
// C6 (subtitle) and optionally C4 (erase) set; 32 space header characters.
std::array<uint8_t, orc::kTeletextPacketBytes> make_header_packet(
    int page_number, bool subtitle, bool erase) {
  std::array<uint8_t, orc::kTeletextPacketBytes> packet{};
  const auto mrag = orc::tests::make_mrag(0, 0);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  packet[2] =
      orc::teletext_hamming84_encode(static_cast<uint8_t>(page_number & 0xF));
  packet[3] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>((page_number >> 4) & 0xF));
  packet[4] = orc::teletext_hamming84_encode(0);
  packet[5] = orc::teletext_hamming84_encode(erase ? 0x8 : 0x0);  // C4
  packet[6] = orc::teletext_hamming84_encode(0);
  packet[7] = orc::teletext_hamming84_encode(subtitle ? 0x8 : 0x0);  // C6
  packet[8] = orc::teletext_hamming84_encode(0);
  packet[9] = orc::teletext_hamming84_encode(0);
  for (size_t i = 0; i < 32; ++i) {
    packet[10 + i] = orc::teletext_odd_parity_encode(' ');
  }
  return packet;
}

// Displayable row with boxed subtitle text (Start Box ×2 ... End Box).
std::array<uint8_t, orc::kTeletextPacketBytes> make_subtitle_row_packet(
    int row, const std::string& text) {
  std::array<uint8_t, orc::kTeletextPacketBytes> packet{};
  const auto mrag = orc::tests::make_mrag(0, row);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  std::string bytes;
  bytes.push_back(0x0B);
  bytes.push_back(0x0B);
  bytes += text;
  bytes.push_back(0x0A);
  for (size_t i = 0; i < 40; ++i) {
    const char c = i < bytes.size() ? bytes[i] : ' ';
    packet[2 + i] = orc::teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return packet;
}

std::optional<orc::ObservationValue> hex_observation(
    const std::array<uint8_t, orc::kTeletextPacketBytes>& packet) {
  return std::optional<orc::ObservationValue>(
      orc::teletext_packet_to_hex(packet));
}

}  // namespace

class TeletextSubtitleFeedTest : public ::testing::Test {
 protected:
  NiceMock<MockObservationContext> context_;
};

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TeletextSubtitleFeedTest, CollectsCuesFromObservations) {
  // Subtitle transmission on 0-based field line 7: header in field 0, boxed
  // row in field 1, terminating time-filling header in field 2.
  EXPECT_CALL(context_, get(_, _, _)).WillRepeatedly(Return(std::nullopt));
  EXPECT_CALL(context_, get(orc::FieldID(0), "teletext", "t42_7"))
      .WillRepeatedly(Return(hex_observation(
          make_header_packet(0x88, /*subtitle=*/true, /*erase=*/true))));
  EXPECT_CALL(context_, get(orc::FieldID(1), "teletext", "t42_7"))
      .WillRepeatedly(
          Return(hex_observation(make_subtitle_row_packet(20, "HELLO"))));
  EXPECT_CALL(context_, get(orc::FieldID(2), "teletext", "t42_7"))
      .WillRepeatedly(Return(hex_observation(
          make_header_packet(0xFF, /*subtitle=*/false, /*erase=*/false))));

  const auto cues = orc::collect_teletext_subtitle_cues(
      context_, /*field_start=*/0, /*field_count=*/10, "888");

  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "HELLO");
  // Cue displayed from field 1 (20 ms at 50 fields/s), closed by finalize
  // at the end of the 10-field range (200 ms).
  EXPECT_DOUBLE_EQ(cues[0].start_time, 0.02);
  EXPECT_DOUBLE_EQ(cues[0].end_time, 0.2);
}

TEST_F(TeletextSubtitleFeedTest, TimesAreRelativeToFieldStart) {
  // The same transmission stored at absolute fields 100-102 must produce
  // the same relative cue timing.
  EXPECT_CALL(context_, get(_, _, _)).WillRepeatedly(Return(std::nullopt));
  EXPECT_CALL(context_, get(orc::FieldID(100), "teletext", "t42_7"))
      .WillRepeatedly(Return(hex_observation(
          make_header_packet(0x88, /*subtitle=*/true, /*erase=*/true))));
  EXPECT_CALL(context_, get(orc::FieldID(101), "teletext", "t42_7"))
      .WillRepeatedly(
          Return(hex_observation(make_subtitle_row_packet(20, "SHIFTED"))));
  EXPECT_CALL(context_, get(orc::FieldID(102), "teletext", "t42_7"))
      .WillRepeatedly(Return(hex_observation(
          make_header_packet(0xFF, /*subtitle=*/false, /*erase=*/false))));

  const auto cues = orc::collect_teletext_subtitle_cues(
      context_, /*field_start=*/100, /*field_count=*/10, "888");

  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "SHIFTED");
  EXPECT_DOUBLE_EQ(cues[0].start_time, 0.02);
  EXPECT_DOUBLE_EQ(cues[0].end_time, 0.2);
}

TEST_F(TeletextSubtitleFeedTest, MalformedPageReturnsNoCues) {
  EXPECT_CALL(context_, get(_, _, _)).Times(0);

  const auto cues = orc::collect_teletext_subtitle_cues(context_, 0, 10, "98X");

  EXPECT_TRUE(cues.empty());
}

TEST_F(TeletextSubtitleFeedTest, EmptyContextReturnsNoCues) {
  EXPECT_CALL(context_, get(_, _, _)).WillRepeatedly(Return(std::nullopt));

  const auto cues = orc::collect_teletext_subtitle_cues(context_, 0, 10, "888");

  EXPECT_TRUE(cues.empty());
}

}  // namespace orc_unit_test
