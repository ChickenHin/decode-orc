/*
 * File:        teletext_page_assembler_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 tests for the teletext trailing-window page assembler
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_page_assembler.h"

#include <gtest/gtest.h>

#include "support/teletext_packet_fixtures.h"

namespace gui_unit_test {

constexpr uint64_t kWindow = TeletextPageAssembler::kTrailingWindowFrames;

TEST(TeletextPageAssemblerTest, WindowStartsAtZeroForEarlyFrames) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(3);

  EXPECT_EQ(assembler.windowStartFrame(), 0u);
  const auto needed = assembler.framesNeedingData();
  ASSERT_EQ(needed.size(), 4u);
  EXPECT_EQ(needed.front(), 0u);
  EXPECT_EQ(needed.back(), 3u);
}

TEST(TeletextPageAssemblerTest, AdvancingWindowEvictsOldFrames) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(kWindow - 1);
  assembler.storeFrame(0, makeEmptyFieldView(), makeEmptyFieldView());
  assembler.storeFrame(kWindow - 1, makeEmptyFieldView(), makeEmptyFieldView());
  ASSERT_TRUE(assembler.hasFrame(0));

  assembler.setCurrentFrame(kWindow);  // window is now [1, kWindow]

  EXPECT_FALSE(assembler.hasFrame(0));
  EXPECT_TRUE(assembler.hasFrame(kWindow - 1));
  const auto needed = assembler.framesNeedingData();
  EXPECT_EQ(needed.size(), kWindow - 1);  // all but the one cached frame
}

TEST(TeletextPageAssemblerTest, StaleDeliveriesOutsideWindowAreIgnored) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(kWindow * 2);

  assembler.storeFrame(0, makeEmptyFieldView(), makeEmptyFieldView());
  assembler.storeFrame(kWindow * 2 + 1, makeEmptyFieldView(),
                       makeEmptyFieldView());

  EXPECT_FALSE(assembler.hasFrame(0));
  EXPECT_FALSE(assembler.hasFrame(kWindow * 2 + 1));
}

TEST(TeletextPageAssemblerTest, AssemblesPageFromCachedWindow) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(2);

  // Page 100 (magazine 1, page 0x00) transmitted across frame 1's fields.
  assembler.storeFrame(1,
                       makeFieldView({makeHeaderPacket(1, 0x00),
                                      makeRowPacket(1, 1, "HELLO TELETEXT")}),
                       makeFieldView({makeRowPacket(1, 2, "ROW TWO"),
                                      makeTimeFillingHeader(1)}));

  const auto page = assembler.assemblePage(1, 0x00);

  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(page->magazine, 1);
  EXPECT_EQ(page->page_number, 0x00);
  EXPECT_EQ(rowText(*page, 1), "HELLO TELETEXT");
  EXPECT_EQ(rowText(*page, 2), "ROW TWO");
  // Header packet was fed with field index 2 (frame 1, field 1).
  EXPECT_EQ(page->header_field_index, 2);
}

TEST(TeletextPageAssemblerTest, LatestTransmissionOfPageWins) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(4);

  assembler.storeFrame(1,
                       makeFieldView({makeHeaderPacket(1, 0x00),
                                      makeRowPacket(1, 1, "FIRST PASS")}),
                       makeEmptyFieldView());
  assembler.storeFrame(3,
                       makeFieldView({makeHeaderPacket(1, 0x00, /*subcode=*/0,
                                                       /*erase_page=*/true),
                                      makeRowPacket(1, 1, "SECOND PASS")}),
                       makeFieldView({makeTimeFillingHeader(1)}));

  const auto page = assembler.assemblePage(1, 0x00);

  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(rowText(*page, 1), "SECOND PASS");
  EXPECT_EQ(page->header_field_index, 6);  // frame 3, field 1
}

TEST(TeletextPageAssemblerTest, UnseenPageReturnsNothing) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(1);
  assembler.storeFrame(
      1, makeFieldView({makeHeaderPacket(1, 0x00), makeTimeFillingHeader(1)}),
      makeEmptyFieldView());

  EXPECT_FALSE(assembler.assemblePage(1, 0x01).has_value());
  EXPECT_FALSE(assembler.assemblePage(2, 0x00).has_value());
}

TEST(TeletextPageAssemblerTest, ClearDropsAllCachedFrames) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(1);
  assembler.storeFrame(
      1, makeFieldView({makeHeaderPacket(1, 0x00), makeTimeFillingHeader(1)}),
      makeEmptyFieldView());

  assembler.clear();

  EXPECT_FALSE(assembler.hasFrame(1));
  EXPECT_FALSE(assembler.assemblePage(1, 0x00).has_value());
}

}  // namespace gui_unit_test
