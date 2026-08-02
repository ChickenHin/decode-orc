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

// The assembler decodes frames in order and waits for any it has not been
// given, because a decoder cannot be fed a field it has already passed. The
// dialog's request loop delivers every frame of the window for that reason;
// tests that care about only a few of them fill in the rest here.
void fillGapsBefore(TeletextPageAssembler& assembler, uint64_t frame) {
  for (uint64_t earlier = assembler.windowStartFrame(); earlier < frame;
       ++earlier) {
    if (!assembler.hasFrame(earlier)) {
      assembler.markFrameUnavailable(earlier);
    }
  }
}

// Store one complete transmission of page 100 in |frame|, terminated by a
// time-filling header in the frame's second field.
void storePage100(TeletextPageAssembler& assembler, uint64_t frame,
                  const std::string& row_text) {
  fillGapsBefore(assembler, frame);
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

// Frames are named a batch at a time: a window is minutes of video, and one
// observation read per frame of it, queued at once, would sit in front of
// everything else the previewer wants to do.
TEST(TeletextPageAssemblerTest, FramesAreRequestedInPacedBatches) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(kWindow - 1);

  const auto first = assembler.framesNeedingData();
  ASSERT_EQ(first.size(), TeletextPageAssembler::kMaxFramesPerRequest);
  EXPECT_EQ(first.front(), 0u);
  // The whole outstanding count is still reported, so the progress readout
  // counts down rather than sitting at the batch size.
  EXPECT_EQ(assembler.framesNeedingDataCount(),
            static_cast<std::size_t>(kWindow));

  for (const uint64_t frame : first) {
    assembler.markFrameUnavailable(frame);
  }
  ASSERT_EQ(assembler.cataloguedPages().size(), 0u);  // forces a decode pass

  // The delivered batch has been consumed, so the next one carries on from
  // where it left off rather than naming the same frames again.
  const auto second = assembler.framesNeedingData();
  ASSERT_FALSE(second.empty());
  EXPECT_EQ(second.front(), first.back() + 1);
}

// A decoded frame's packets are released: the assembler holds frames waiting
// their turn, not a transcript of everything it has ever read, which is what
// keeps an unbounded forward run flat in memory.
TEST(TeletextPageAssemblerTest, DecodedFramesAreNotRequestedAgain) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(2);
  assembler.markFrameUnavailable(0);
  assembler.markFrameUnavailable(1);
  assembler.markFrameUnavailable(2);
  ASSERT_EQ(assembler.cataloguedPages().size(), 0u);  // forces a decode pass

  EXPECT_TRUE(assembler.hasFrame(0));
  EXPECT_TRUE(assembler.framesNeedingData().empty());
  EXPECT_EQ(assembler.framesNeedingDataCount(), 0u);
}

TEST(TeletextPageAssemblerTest, StaleDeliveriesOutsideWindowAreIgnored) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(kWindow * 2);

  assembler.storeFrame(0, makeEmptyFieldView(), makeEmptyFieldView());
  assembler.storeFrame(assembler.retainedFrameLimit() + 1, makeEmptyFieldView(),
                       makeEmptyFieldView());

  EXPECT_FALSE(assembler.hasFrame(0));
  EXPECT_FALSE(assembler.hasFrame(assembler.retainedFrameLimit() + 1));
}

// A read already in flight when the previewer steps back is not wasted: the
// frame is still one a continuous move could return to, and re-reading it
// would cost another observation read for data already in hand.
TEST(TeletextPageAssemblerTest, DeliveriesAheadOfThePreviewerAreKept) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(100);
  assembler.setCurrentFrame(90);

  assembler.storeFrame(95, makeEmptyFieldView(), makeEmptyFieldView());
  EXPECT_TRUE(assembler.hasFrame(95));

  // ...and stepping back again does not throw them away either.
  assembler.setCurrentFrame(80);
  EXPECT_TRUE(assembler.hasFrame(95));
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
  fillGapsBefore(assembler, 1);
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

  fillGapsBefore(assembler, 1);
  assembler.storeFrame(1,
                       makeFieldView({makeHeaderPacket(1, 0x00),
                                      makeRowPacket(1, 1, "FIRST PASS")}),
                       makeEmptyFieldView());
  fillGapsBefore(assembler, 3);
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

// A frame's packets are released the moment the decoder has consumed them, so
// a later retransmission carrying only part of the page has nothing left to
// re-read. Rows it did not carry must survive from the earlier transmission —
// the row copies the assembler accumulates outlive the frames that delivered
// them, which is the whole point of keeping them separately.
TEST(TeletextPageAssemblerTest, ClippedRetransmissionKeepsRowsItDidNotCarry) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(2);

  fillGapsBefore(assembler, 1);
  assembler.storeFrame(1,
                       makeFieldView({makeHeaderPacket(1, 0x00),
                                      makeRowPacket(1, 1, "FULL PAGE"),
                                      makeRowPacket(1, 2, "SECOND ROW")}),
                       makeFieldView({makeRowPacket(1, 3, "THIRD ROW"),
                                      makeTimeFillingHeader(1)}));
  ASSERT_EQ(rowText(assembler.findPage(1, 0x00)->page, 1), "FULL PAGE");

  // Play on well past that frame, then take a retransmission carrying only
  // the header and the first row.
  const uint64_t clipped_frame =
      TeletextPageAssembler::kTrailingWindowFrames + 5;
  stepTo(assembler, clipped_frame);
  fillGapsBefore(assembler, clipped_frame);
  assembler.storeFrame(
      clipped_frame,
      makeFieldView({makeHeaderPacket(1, 0x00), makeRowPacket(1, 1, "CLIP")}),
      makeFieldView({makeTimeFillingHeader(1)}));

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

  fillGapsBefore(assembler, 1);
  assembler.storeFrame(
      1,
      makeFieldView({makeHeaderPacket(1, 0x00), makeRowPacket(1, 1, "OLD PAGE"),
                     makeRowPacket(1, 2, "OLD SECOND ROW")}),
      makeFieldView({makeTimeFillingHeader(1)}));
  ASSERT_EQ(rowText(assembler.findPage(1, 0x00)->page, 2), "OLD SECOND ROW");

  const uint64_t erase_frame = TeletextPageAssembler::kTrailingWindowFrames + 5;
  stepTo(assembler, erase_frame);
  fillGapsBefore(assembler, erase_frame);
  assembler.storeFrame(erase_frame,
                       makeFieldView({makeHeaderPacket(1, 0x00, /*subcode=*/0,
                                                       /*erase_page=*/true),
                                      makeRowPacket(1, 1, "NEW PAGE")}),
                       makeFieldView({makeTimeFillingHeader(1)}));

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

  fillGapsBefore(assembler, 1);
  assembler.storeFrame(1,
                       makeFieldView({makeHeaderPacket(1, 0x00, /*subcode=*/1),
                                      makeRowPacket(1, 1, "SUBPAGE ONE"),
                                      makeRowPacket(1, 2, "MORE TEXT")}),
                       makeFieldView({makeTimeFillingHeader(1)}));
  fillGapsBefore(assembler, 3);
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

// The header arrives a frame or two ahead of the first row. Listing the page
// on the strength of the header alone puts an empty grid on screen under the
// page's own number, which reads as "this page is blank" rather than "its rows
// have not arrived" — and on a seek, where the whole catalogue is rebuilt,
// that is what the reader sees happen to the page they were looking at.
TEST(TeletextPageAssemblerTest, HeaderWithoutRowsIsNotCataloguedYet) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(2);

  assembler.storeFrame(0, makeFieldView({makeHeaderPacket(1, 0x00)}),
                       makeEmptyFieldView());
  EXPECT_EQ(assembler.findPage(1, 0x00), nullptr);
  EXPECT_TRUE(assembler.cataloguedPages().empty());

  assembler.storeFrame(1, makeFieldView({makeRowPacket(1, 1, "FIRST ROW")}),
                       makeEmptyFieldView());

  const auto* entry = assembler.findPage(1, 0x00);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(rowText(entry->page, 1), "FIRST ROW");
  EXPECT_FALSE(entry->page.transmission_complete);
}

TEST(TeletextPageAssemblerTest, UnseenPageReturnsNothing) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(1);
  fillGapsBefore(assembler, 1);
  assembler.storeFrame(
      1, makeFieldView({makeHeaderPacket(1, 0x00), makeTimeFillingHeader(1)}),
      makeEmptyFieldView());
  ASSERT_NE(assembler.findPage(1, 0x00), nullptr);

  EXPECT_EQ(assembler.findPage(1, 0x01), nullptr);
  EXPECT_EQ(assembler.findPage(2, 0x00), nullptr);
}

TEST(TeletextPageAssemblerTest, CataloguesEveryPageSeenInWindow) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(2);

  storePage100(assembler, 1, "HELLO TELETEXT");
  fillGapsBefore(assembler, 2);
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

// The service names its own subtitle page through C6 (EN 300 706 §9.3.1.3).
// 888 is only the broadcast convention — the LaserDisc samples this was built
// against use 190 — so the flag is the only reliable way to find it, and it
// has to survive the page coming round with C6 clear between captions.
TEST(TeletextPageAssemblerTest, SubtitlePageIsFlaggedFromC6AndStaysFlagged) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(3);

  fillGapsBefore(assembler, 1);
  assembler.storeFrame(
      1,
      makeFieldView({makeHeaderPacket(1, 0x90, /*subcode=*/0,
                                      /*erase_page=*/false, /*subtitle=*/true),
                     makeRowPacket(1, 20, "SUBTITLE TEXT")}),
      makeFieldView({makeTimeFillingHeader(1)}));
  storePage100(assembler, 2, "HELLO TELETEXT");
  // The caption ends: page 190 comes round again with C6 clear, which is the
  // decoder's clear event, not the page ceasing to be the subtitle page.
  fillGapsBefore(assembler, 3);
  assembler.storeFrame(3, makeFieldView({makeHeaderPacket(1, 0x90)}),
                       makeFieldView({makeTimeFillingHeader(1)}));

  const auto* subtitle_page = assembler.findPage(1, 0x90);
  ASSERT_NE(subtitle_page, nullptr);
  EXPECT_TRUE(subtitle_page->subtitle);
  const auto* plain_page = assembler.findPage(1, 0x00);
  ASSERT_NE(plain_page, nullptr);
  EXPECT_FALSE(plain_page->subtitle);

  const auto pages = assembler.cataloguedPages();
  ASSERT_EQ(pages.size(), 2u);
  EXPECT_FALSE(pages[0].subtitle) << "page 100 carries no subtitles";
  EXPECT_TRUE(pages[1].subtitle) << "page 190 does";
}

// A page can be listed from a transmission before the one that declares C6,
// so the flag arriving has to be a catalogue change in its own right —
// otherwise the marker only appears when something else about the page does.
TEST(TeletextPageAssemblerTest, SubtitleFlagArrivingBumpsTheCatalogueRevision) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(2);

  fillGapsBefore(assembler, 1);
  assembler.storeFrame(
      1,
      makeFieldView({makeHeaderPacket(1, 0x90), makeRowPacket(1, 20, "TEXT")}),
      makeFieldView({makeTimeFillingHeader(1)}));
  const uint64_t before = assembler.catalogueRevision();
  ASSERT_FALSE(assembler.findPage(1, 0x90)->subtitle);

  fillGapsBefore(assembler, 2);
  assembler.storeFrame(
      2,
      makeFieldView({makeHeaderPacket(1, 0x90, /*subcode=*/0,
                                      /*erase_page=*/false, /*subtitle=*/true),
                     makeRowPacket(1, 20, "TEXT")}),
      makeFieldView({makeTimeFillingHeader(1)}));

  EXPECT_TRUE(assembler.findPage(1, 0x90)->subtitle);
  EXPECT_GT(assembler.catalogueRevision(), before);
}

TEST(TeletextPageAssemblerTest, PageStaysCataloguedAfterItsFramesAreReleased) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(1);
  storePage100(assembler, 1, "HELLO TELETEXT");
  ASSERT_NE(assembler.findPage(1, 0x00), nullptr);

  // Sequential playback well past the window length. Frame 1's packets are
  // long gone, but the page they carried stays listed at the frame it was
  // seen: what the catalogue remembers is not bounded by the window.
  stepTo(assembler, kWindow + 20);

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

// Dragging the slider backwards leaves the span the run has decoded, so the
// run has to be laid out again from further back. The frames in between are
// the same recording, though, so nothing it learned is thrown away: the page
// the reader is looking at stays on screen and stays whole while the frames
// behind the new position are read.
TEST(TeletextPageAssemblerTest, RewindPastTheAnchorKeepsWhatWasDecoded) {
  TeletextPageAssembler assembler;
  const uint64_t start = kWindow * 3;
  assembler.setCurrentFrame(start);
  storePage100(assembler, start, "HELLO TELETEXT");
  ASSERT_NE(assembler.findPage(1, 0x00), nullptr);
  const uint64_t anchor = assembler.windowStartFrame();

  // Half a window back — past the anchor, but well within a window of it.
  assembler.setCurrentFrame(anchor - kWindow / 2);

  ASSERT_EQ(assembler.cataloguedPages().size(), 1u);
  const auto* entry = assembler.findPage(1, 0x00);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(rowText(entry->page, 1), "HELLO TELETEXT");
  // The run itself has moved back and will re-read from there.
  EXPECT_LT(assembler.windowStartFrame(), anchor);
  EXPECT_FALSE(assembler.framesNeedingData().empty());
}

// A seek is different: land more than a window before where the run began and
// nothing it decoded describes anywhere near here, so it all goes.
TEST(TeletextPageAssemblerTest, LongBackwardSeekClearsCatalogue) {
  TeletextPageAssembler assembler;
  const uint64_t start = kWindow * 4;
  assembler.setCurrentFrame(start);
  storePage100(assembler, start, "HELLO TELETEXT");
  ASSERT_NE(assembler.findPage(1, 0x00), nullptr);

  assembler.setCurrentFrame(assembler.windowStartFrame() - kWindow - 1);

  EXPECT_TRUE(assembler.cataloguedPages().empty());
  EXPECT_EQ(assembler.findPage(1, 0x00), nullptr);
}

TEST(TeletextPageAssemblerTest, RevisionOnlyChangesWhenTheCatalogueChanges) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(3);
  storePage100(assembler, 1, "HELLO TELETEXT");

  const uint64_t revision = assembler.catalogueRevision();
  ASSERT_EQ(assembler.cataloguedPages().size(), 1u);
  EXPECT_EQ(assembler.catalogueRevision(), revision);

  // A frame with no teletext content advances the decode without changing
  // what has been seen.
  assembler.storeFrame(2, makeEmptyFieldView(), makeEmptyFieldView());
  EXPECT_EQ(assembler.catalogueRevision(), revision);

  // A new page does change it.
  assembler.storeFrame(
      3,
      makeFieldView({makeHeaderPacket(8, 0x88), makeRowPacket(8, 1, "SUBS")}),
      makeFieldView({makeTimeFillingHeader(8)}));
  EXPECT_NE(assembler.catalogueRevision(), revision);
}

// On a source carrying only a couple of teletext lines per field, a page is
// spread over several frames, and stepping through them shows it filling in.
// Each of those renders is right for the packets received so far — but a row
// not yet sent looks exactly like a transmitted blank one, so the page has to
// say which it is until the transmission ends.
TEST(TeletextPageAssemblerTest, PageArrivingIsFlaggedUntilItsTransmissionEnds) {
  TeletextPageAssembler assembler;

  assembler.setCurrentFrame(0);
  assembler.storeFrame(0,
                       makeFieldView({makeHeaderPacket(1, 0x00),
                                      makeRowPacket(1, 1, "FIRST ROW")}),
                       makeEmptyFieldView());
  const auto* entry = assembler.findPage(1, 0x00);
  ASSERT_NE(entry, nullptr);
  EXPECT_FALSE(entry->page.transmission_complete);
  EXPECT_EQ(rowText(entry->page, 1), "FIRST ROW");
  EXPECT_EQ(rowText(entry->page, 2), "");

  // A later frame of the same transmission adds a row; still arriving.
  assembler.setCurrentFrame(1);
  assembler.storeFrame(1, makeFieldView({makeRowPacket(1, 2, "SECOND ROW")}),
                       makeEmptyFieldView());
  entry = assembler.findPage(1, 0x00);
  ASSERT_NE(entry, nullptr);
  EXPECT_FALSE(entry->page.transmission_complete);
  EXPECT_EQ(rowText(entry->page, 2), "SECOND ROW");

  // The next page's header ends the transmission.
  assembler.setCurrentFrame(2);
  assembler.storeFrame(2, makeFieldView({makeTimeFillingHeader(1)}),
                       makeEmptyFieldView());
  entry = assembler.findPage(1, 0x00);
  ASSERT_NE(entry, nullptr);
  EXPECT_TRUE(entry->page.transmission_complete);
  EXPECT_EQ(rowText(entry->page, 2), "SECOND ROW");

  ASSERT_EQ(assembler.cataloguedPages().size(), 1u);
  EXPECT_TRUE(assembler.cataloguedPages().front().transmission_complete);
}

// Services re-send a page's header while its rows are still going out, to keep
// the clock in the header live. Every one of those closes the assembly, but
// they are all one appearance of the page: counting them separately made the
// list claim a page had come round several times when the carousel had brought
// it round once.
TEST(TeletextPageAssemblerTest, RepeatedHeaderMidTransmissionIsOneAppearance) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(0);

  // Header, header again, then the rows — the pattern a real service sends.
  assembler.storeFrame(
      0,
      makeFieldView({makeHeaderPacket(1, 0x00), makeHeaderPacket(1, 0x00),
                     makeRowPacket(1, 1, "ROW ONE")}),
      makeFieldView({makeHeaderPacket(1, 0x00), makeRowPacket(1, 2, "ROW TWO"),
                     makeTimeFillingHeader(1)}));

  const auto* entry = assembler.findPage(1, 0x00);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->times_seen, 1u);
  EXPECT_TRUE(entry->page.transmission_complete);
  // Rows sent between the repeated headers all belong to the same page.
  EXPECT_EQ(rowText(entry->page, 1), "ROW ONE");
  EXPECT_EQ(rowText(entry->page, 2), "ROW TWO");

  // A genuine second appearance does count.
  assembler.setCurrentFrame(1);
  assembler.storeFrame(1,
                       makeFieldView({makeHeaderPacket(1, 0x00),
                                      makeRowPacket(1, 1, "ROW ONE")}),
                       makeFieldView({makeTimeFillingHeader(1)}));
  entry = assembler.findPage(1, 0x00);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->times_seen, 2u);
}

// A missing row proves nothing — services habitually omit the blank rows that
// space a page out. An empty VBI *slot* does prove something: a service
// part-way through sending a page fills every line it is using, in every
// field. The lines in use are read off the transmission itself, so a
// recording inserting on one line per field is not accused of losing half its
// packets.
TEST(TeletextPageAssemblerTest, OmittedRowsAreNotCountedAsLostPackets) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(1);

  // Two packets in every field throughout, but rows 2 and 4 are never sent.
  assembler.storeFrame(0,
                       makeFieldView({makeHeaderPacket(1, 0x00),
                                      makeRowPacket(1, 1, "ROW ONE")}),
                       makeFieldView({makeRowPacket(1, 3, "ROW THREE"),
                                      makeRowPacket(1, 5, "ROW FIVE")}));
  assembler.storeFrame(
      1,
      makeFieldView({makeRowPacket(1, 6, "ROW SIX"), makeTimeFillingHeader(1)}),
      makeEmptyFieldView());

  const auto* entry = assembler.findPage(1, 0x00);
  ASSERT_NE(entry, nullptr);
  ASSERT_TRUE(entry->page.transmission_complete);
  EXPECT_FALSE(entry->page.row_received[2]);
  EXPECT_FALSE(entry->page.row_received[4]);
  EXPECT_EQ(entry->page.recovery.lost_packets, 0)
      << "rows the service chose not to send are not lost packets";
}

TEST(TeletextPageAssemblerTest, EmptySlotMidTransmissionIsALostPacket) {
  TeletextPageAssembler assembler;
  assembler.setCurrentFrame(1);

  // Field 1 of frame 0 yields only one packet where every other field yields
  // two: a slot the service must have filled came back empty.
  assembler.storeFrame(0,
                       makeFieldView({makeHeaderPacket(1, 0x00),
                                      makeRowPacket(1, 1, "ROW ONE")}),
                       makeFieldView({makeRowPacket(1, 3, "ROW THREE")}));
  assembler.storeFrame(
      1,
      makeFieldView({makeRowPacket(1, 6, "ROW SIX"), makeTimeFillingHeader(1)}),
      makeEmptyFieldView());

  const auto* entry = assembler.findPage(1, 0x00);
  ASSERT_NE(entry, nullptr);
  ASSERT_TRUE(entry->page.transmission_complete);
  EXPECT_EQ(entry->page.recovery.lost_packets, 1);
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
