/*
 * File:        eia608_decoder_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the EIA-608 caption decoder's rendering and its
 *              text-service mode
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/support/eia608_decoder.h>

#include <string>
#include <utility>
#include <vector>

namespace orc_unit_test {
namespace {

constexpr uint8_t kMisc = 0x14;  // Channel 1 miscellaneous control code
constexpr uint8_t kRCL = 0x20;   // Resume Caption Loading (pop-on)
constexpr uint8_t kEDM = 0x2C;   // Erase Displayed Memory
constexpr uint8_t kCR = 0x2D;    // Carriage Return
constexpr uint8_t kEOC = 0x2F;   // End of Caption
constexpr uint8_t kTR = 0x2A;    // Text Restart

// Preamble address codes (CTA-608-E Table 53), channel 1. The first byte
// selects a pair of rows, bit 5 of the second picks one of the pair, and its
// indent field moves the cursor along in steps of four columns.
//
// 0x11 addresses rows 1 and 2, which are indices 0 and 1.
std::pair<uint8_t, uint8_t> pac(int row_index, int indent_steps) {
  const bool odd_of_pair = (row_index % 2) == 1;
  const uint8_t byte2 = static_cast<uint8_t>(
      0x50 | (odd_of_pair ? 0x20 : 0x00) | ((indent_steps & 0x07) << 1));
  return {0x11, byte2};
}

// Row 14 (index 13), column 0 — one above the bottom of the display.
std::pair<uint8_t, uint8_t> pac_row14() { return {0x14, 0x50}; }

void feed(orc::EIA608Decoder& decoder, double& time,
          std::initializer_list<std::pair<uint8_t, uint8_t>> pairs) {
  for (const auto& pair : pairs) {
    time += 1.0 / 29.97;
    decoder.process_bytes(time, pair.first, pair.second);
  }
}

}  // namespace

TEST(CaptionBufferTest, RowsAreSeparateLinesAndKeepTheirIndent) {
  orc::CaptionBuffer buffer;
  buffer.set_cursor(13, 4);
  buffer.write_char('H');
  buffer.write_char('I');
  buffer.set_cursor(14, 0);
  buffer.write_char('Y');
  buffer.write_char('O');

  // Rows joined by newlines, not spaces: run together, two rows of a
  // text-service page read as one sentence and the columns stop lining up.
  EXPECT_EQ(buffer.render(), "    HI\nYO");
  EXPECT_EQ(buffer.render_row(13), "    HI");
  EXPECT_EQ(buffer.render_row(14), "YO");
  EXPECT_EQ(buffer.render_row(0), "");
}

TEST(CaptionBufferTest, RenderRowTrimsTrailingSpacesOnly) {
  orc::CaptionBuffer buffer;
  buffer.set_cursor(0, 2);
  buffer.write_char('A');
  buffer.write_char(' ');
  buffer.write_char(' ');

  EXPECT_EQ(buffer.render_row(0), "  A");
}

TEST(EIA608DecoderTest, PopOnCaptionKeepsItsRowsApart) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time,
       {{kMisc, kRCL},
        pac_row14(),
        {'H', 'I'},
        {kMisc, kCR},  // pop-on: move down a row for the second line
        {'Y', 'O'},
        {kMisc, kEOC},
        {kMisc, kEDM}});

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "HI\nYO");
}

TEST(EIA608DecoderTest, TextRestartSelectsTheTextServiceMode) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time, {{kMisc, kTR}, {'A', 'B'}});

  EXPECT_EQ(decoder.mode(), orc::CaptionMode::TEXT);
}

TEST(EIA608DecoderTest, TextServicePageFillsDownwardsAndKeepsColumns) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  // The shape a text service such as a scores page uses: a label at column 0
  // and a figure indented across the row, one line per carriage return.
  feed(decoder, time,
       {{kMisc, kTR},  // start the text service at the top of the page
        {'C', 'H'},    // row 0, columns 0-1
        pac(0, 4),     // same row, column 16
        {'3', '6'},    //
        {kMisc, kCR},  // end of the line
        {'H', 'O'},    // row 1
        pac(1, 4),     // same row, column 16
        {'2', '1'},    //
        {kMisc, kCR}});

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 2u);
  // The indent survives: a page of listings or scores is only readable while
  // its columns line up.
  EXPECT_EQ(cues[0].text, "CH              36");
  EXPECT_EQ(cues[1].text, "HO              21");
}

TEST(EIA608DecoderTest, TextServiceScrollsOnceThePageIsFull) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time, {{kMisc, kTR}});
  // Write one character per row for the whole 15-row page, then two more.
  for (int line = 0; line < 17; ++line) {
    const char ch = static_cast<char>('A' + line);
    feed(decoder, time, {{static_cast<uint8_t>(ch), 0x00}, {kMisc, kCR}});
  }

  // Nothing is lost from the cue list — every completed line was emitted.
  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 17u);
  EXPECT_EQ(cues.front().text, "A");
  EXPECT_EQ(cues.back().text, "Q");

  // The page holds the last lines written: A, B and C scrolled off the top,
  // and the carriage return after Q left the bottom row waiting for the next.
  const auto& rows = decoder.displayed().rows();
  EXPECT_EQ(rows[0], "D");
  EXPECT_EQ(rows[orc::CaptionBuffer::MAX_ROWS - 2], "Q");
  EXPECT_EQ(rows[orc::CaptionBuffer::MAX_ROWS - 1], "");
}

TEST(EIA608DecoderTest, TwoEndOfCaptionCodesAreTwoCaptions) {
  // The decoder used to suppress a second End of Caption arriving within a
  // tenth of a second, which at 29.97 fps merged captions three frames apart.
  // Suppressing the encoder's duplicate copy is the demux's job now, and it
  // does it by adjacency rather than by elapsed time.
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time,
       {{kMisc, kRCL},
        {'A', 'A'},
        {kMisc, kEOC},
        {'B', 'B'},
        {kMisc, kEOC},
        {kMisc, kEDM}});

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 2u);
  EXPECT_EQ(cues[0].text, "AA");
  EXPECT_EQ(cues[1].text, "BB");
}

}  // namespace orc_unit_test
