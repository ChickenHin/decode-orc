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

namespace {

// Store one complete transmission of page 100 in |frame|, terminated by a
// time-filling header in the frame's second field.
void storePage100(TeletextPageAssembler& assembler, uint64_t frame,
                  const std::string& row_text) {
  assembler.storeFrame(
      frame,
      makeFieldView({makeHeaderPacket(1, 0x00), makeRowPacket(1, 1, row_text)}),
      makeFieldView({makeTimeFillingHeader(1)}));
}

// Step the window forward one frame at a time (as the previewer does), so the
// move stays continuous and the catalogue is retained.
void stepTo(TeletextPageAssembler& assembler, uint64_t target_frame) {
  for (uint64_t frame = assembler.currentFrame() + 1; frame <= target_frame;
       ++frame) {
    assembler.setCurrentFrame(frame);
  }
}

}  // namespace

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

TEST(TeletextPageAssemblerTest, UnavailableFrameIsNotRequestedAgain) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(2);

  assembler.markFrameUnavailable(1);

  EXPECT_TRUE(assembler.hasFrame(1));
  const auto needed = assembler.framesNeedingData();
  ASSERT_EQ(needed.size(), 2u);
  EXPECT_EQ(needed.front(), 0u);
  EXPECT_EQ(needed.back(), 2u);
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

  const auto* entry = assembler.findPage(1, 0x00);

  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->magazine, 1);
  EXPECT_EQ(entry->page_number, 0x00);
  EXPECT_EQ(entry->seen_frame, 1u);
  EXPECT_EQ(rowText(entry->page, 1), "HELLO TELETEXT");
  EXPECT_EQ(rowText(entry->page, 2), "ROW TWO");
  // Header packet was fed with field index 2 (frame 1, field 1).
  EXPECT_EQ(entry->page.header_field_index, 2);
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

  const auto* entry = assembler.findPage(1, 0x00);

  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(rowText(entry->page, 1), "SECOND PASS");
  EXPECT_EQ(entry->page.header_field_index, 6);  // frame 3, field 1
  EXPECT_EQ(entry->seen_frame, 3u);
}

// The trailing window regularly clips a page transmission at its edge, and on
// a source carrying only a couple of teletext lines per field a whole page
// takes longer to transmit than the window is wide. Rows the clipped
// retransmission did not carry must survive from the earlier one — the row
// copies the assembler accumulates are not bounded by the window.
TEST(TeletextPageAssemblerTest, ClippedRetransmissionKeepsRowsItDidNotCarry) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(2);

  assembler.storeFrame(1,
                       makeFieldView({makeHeaderPacket(1, 0x00),
                                      makeRowPacket(1, 1, "FULL PAGE"),
                                      makeRowPacket(1, 2, "SECOND ROW")}),
                       makeFieldView({makeRowPacket(1, 3, "THIRD ROW"),
                                      makeTimeFillingHeader(1)}));
  ASSERT_EQ(rowText(assembler.findPage(1, 0x00)->page, 1), "FULL PAGE");

  // Step forward one frame at a time until the frame carrying the full
  // transmission has left the window (the catalogue survives stepping). A
  // later retransmission is then clipped by the window edge: only its header
  // and first row fall inside, so a fresh decode of the window sees a
  // fragment of the page.
  const uint64_t clipped_frame =
      TeletextPageAssembler::kTrailingWindowFrames + 5;
  for (uint64_t frame = 3; frame <= clipped_frame; ++frame) {
    assembler.setCurrentFrame(frame);
    if (frame == clipped_frame) {
      assembler.storeFrame(frame,
                           makeFieldView({makeHeaderPacket(1, 0x00),
                                          makeRowPacket(1, 1, "CLIP")}),
                           makeEmptyFieldView());
    }
  }
  ASSERT_FALSE(assembler.hasFrame(1));

  const auto* entry = assembler.findPage(1, 0x00);

  ASSERT_NE(entry, nullptr);
  // Row 1 was retransmitted, so the newer copy of it wins the two-way vote —
  // the same row a plain Level 1 decoder would be showing.
  EXPECT_EQ(rowText(entry->page, 1), "CLIP");
  // Rows the clipped retransmission never carried are still there. Before the
  // accumulation they were lost with the frame that had delivered them.
  EXPECT_EQ(rowText(entry->page, 2), "SECOND ROW");
  EXPECT_EQ(rowText(entry->page, 3), "THIRD ROW");
  EXPECT_TRUE(entry->page.row_received[2]);
  EXPECT_TRUE(entry->page.row_received[3]);
  // The page is still reported at the frame it was most recently seen.
  EXPECT_EQ(entry->seen_frame, clipped_frame);
}

// C4 (erase page, EN 300 706 §9.3.1.3 Table 2) replaces the page's content
// rather than updating it, so accumulated rows must not survive it.
TEST(TeletextPageAssemblerTest, ErasePageDropsAccumulatedRows) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(2);

  assembler.storeFrame(
      1,
      makeFieldView({makeHeaderPacket(1, 0x00), makeRowPacket(1, 1, "OLD PAGE"),
                     makeRowPacket(1, 2, "OLD SECOND ROW")}),
      makeFieldView({makeTimeFillingHeader(1)}));
  ASSERT_EQ(rowText(assembler.findPage(1, 0x00)->page, 2), "OLD SECOND ROW");

  const uint64_t erase_frame = TeletextPageAssembler::kTrailingWindowFrames + 5;
  for (uint64_t frame = 3; frame <= erase_frame; ++frame) {
    assembler.setCurrentFrame(frame);
    if (frame == erase_frame) {
      assembler.storeFrame(
          frame,
          makeFieldView({makeHeaderPacket(1, 0x00, /*subcode=*/0,
                                          /*erase_page=*/true),
                         makeRowPacket(1, 1, "NEW PAGE")}),
          makeFieldView({makeTimeFillingHeader(1)}));
    }
  }

  const auto* entry = assembler.findPage(1, 0x00);

  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(rowText(entry->page, 1), "NEW PAGE");
  EXPECT_EQ(rowText(entry->page, 2), "") << "erased row survived the erase";
  EXPECT_FALSE(entry->page.row_received[2]);
}

// A different sub-code is a different page (EN 300 706 §9.3.1.2) and replaces
// outright, however little of it arrived.
TEST(TeletextPageAssemblerTest, NewSubcodeReplacesEvenWhenLessComplete) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(4);

  assembler.storeFrame(1,
                       makeFieldView({makeHeaderPacket(1, 0x00, /*subcode=*/1),
                                      makeRowPacket(1, 1, "SUBPAGE ONE"),
                                      makeRowPacket(1, 2, "MORE TEXT")}),
                       makeFieldView({makeTimeFillingHeader(1)}));
  assembler.storeFrame(3,
                       makeFieldView({makeHeaderPacket(1, 0x00, /*subcode=*/2),
                                      makeRowPacket(1, 1, "SUBPAGE TWO")}),
                       makeFieldView({makeTimeFillingHeader(1)}));

  const auto* entry = assembler.findPage(1, 0x00);

  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->page.subcode, 2);
  EXPECT_EQ(rowText(entry->page, 1), "SUBPAGE TWO");
  EXPECT_EQ(rowText(entry->page, 2), "");
}

TEST(TeletextPageAssemblerTest, UnseenPageReturnsNothing) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(1);
  assembler.storeFrame(
      1, makeFieldView({makeHeaderPacket(1, 0x00), makeTimeFillingHeader(1)}),
      makeEmptyFieldView());

  EXPECT_EQ(assembler.findPage(1, 0x01), nullptr);
  EXPECT_EQ(assembler.findPage(2, 0x00), nullptr);
}

TEST(TeletextPageAssemblerTest, CataloguesEveryPageSeenInWindow) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(2);

  storePage100(assembler, 1, "HELLO TELETEXT");
  assembler.storeFrame(
      2,
      makeFieldView({makeHeaderPacket(8, 0x88), makeRowPacket(8, 1, "SUBS")}),
      makeFieldView({makeTimeFillingHeader(8)}));

  const auto pages = assembler.cataloguedPages();

  ASSERT_EQ(pages.size(), 2u);
  // Ascending page address: magazine 1 page 00 before magazine 8 page 88.
  EXPECT_EQ(pages[0].magazine, 1);
  EXPECT_EQ(pages[0].page_number, 0x00);
  EXPECT_EQ(pages[0].seen_frame, 1u);
  EXPECT_EQ(pages[1].magazine, 8);
  EXPECT_EQ(pages[1].page_number, 0x88);
  EXPECT_EQ(pages[1].seen_frame, 2u);
}

TEST(TeletextPageAssemblerTest, PageStaysCataloguedAfterItsFramesAreEvicted) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(1);
  storePage100(assembler, 1, "HELLO TELETEXT");
  ASSERT_NE(assembler.findPage(1, 0x00), nullptr);

  // Sequential playback well past the window length: frame 1 is evicted but
  // the page it carried stays listed at the frame it was seen.
  stepTo(assembler, kWindow + 20);

  EXPECT_FALSE(assembler.hasFrame(1));
  const auto* entry = assembler.findPage(1, 0x00);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->seen_frame, 1u);
  EXPECT_EQ(rowText(entry->page, 1), "HELLO TELETEXT");
}

TEST(TeletextPageAssemblerTest, DiscontinuousJumpClearsCatalogue) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(1);
  storePage100(assembler, 1, "HELLO TELETEXT");
  ASSERT_EQ(assembler.cataloguedPages().size(), 1u);

  // A jump with no overlap with the previous window restarts the catalogue.
  assembler.setCurrentFrame(1 + kWindow);

  EXPECT_TRUE(assembler.cataloguedPages().empty());
  EXPECT_EQ(assembler.findPage(1, 0x00), nullptr);
}

TEST(TeletextPageAssemblerTest, ShortBackwardStepKeepsCatalogue) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(10);
  storePage100(assembler, 10, "HELLO TELETEXT");

  assembler.setCurrentFrame(9);

  ASSERT_EQ(assembler.cataloguedPages().size(), 1u);
  EXPECT_EQ(assembler.cataloguedPages().front().seen_frame, 10u);
}

TEST(TeletextPageAssemblerTest, RevisionOnlyChangesWhenTheCatalogueChanges) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(2);
  storePage100(assembler, 1, "HELLO TELETEXT");

  const uint64_t revision = assembler.catalogueRevision();
  ASSERT_EQ(assembler.cataloguedPages().size(), 1u);
  EXPECT_EQ(assembler.catalogueRevision(), revision);

  // A frame with no teletext content re-decodes the window without changing
  // what has been seen.
  assembler.storeFrame(0, makeEmptyFieldView(), makeEmptyFieldView());
  EXPECT_EQ(assembler.catalogueRevision(), revision);

  // A new page does change it.
  assembler.storeFrame(
      2,
      makeFieldView({makeHeaderPacket(8, 0x88), makeRowPacket(8, 1, "SUBS")}),
      makeFieldView({makeTimeFillingHeader(8)}));
  EXPECT_NE(assembler.catalogueRevision(), revision);
}

TEST(TeletextPageAssemblerTest, ClearDropsAllCachedFramesAndPages) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(1);
  storePage100(assembler, 1, "HELLO TELETEXT");
  ASSERT_NE(assembler.findPage(1, 0x00), nullptr);

  assembler.clear();

  EXPECT_FALSE(assembler.hasFrame(1));
  EXPECT_EQ(assembler.findPage(1, 0x00), nullptr);
  EXPECT_TRUE(assembler.cataloguedPages().empty());
}

}  // namespace gui_unit_test
