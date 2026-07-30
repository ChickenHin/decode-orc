/*
 * File:        audio_sync_head_offset_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the audio/video sync head alignment (issue
 *              #231): the writer-level pad/trim helper and the decoder
 *              head-loss counters it is computed from
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <vector>

#include "audio_head_offset.h"
#include "dec_f3frametof2section.h"
#include "dec_tvaluestochannel.h"
#include "efm_constants.h"
#include "frame.h"
#include "section.h"

namespace {

// ---------------------------------------------------------------------------
// AudioHeadOffset: the writer-level pad/trim primitive
// ---------------------------------------------------------------------------

// Interleaved stereo buffer of |pairs| pairs whose samples count up from
// |firstValue| so trims can be checked for exact position.
std::vector<int16_t> makeNumberedPairs(size_t pairs, int16_t firstValue) {
  std::vector<int16_t> data(pairs * 2);
  std::iota(data.begin(), data.end(), firstValue);
  return data;
}

TEST(AudioHeadOffset, ZeroOffsetLeavesBuffersUntouched) {
  AudioHeadOffset offset;
  offset.setOffsetPairs(0);

  std::vector<int16_t> section = makeNumberedPairs(588, 0);
  const std::vector<int16_t> expected = section;
  offset.apply(section);
  EXPECT_EQ(section, expected);
}

TEST(AudioHeadOffset, NegativeOffsetTrimsAcrossSectionBoundaries) {
  AudioHeadOffset offset;
  // More than one 588-pair section so the trim must span two buffers.
  offset.setOffsetPairs(-700);

  std::vector<int16_t> first = makeNumberedPairs(588, 0);
  offset.apply(first);
  EXPECT_TRUE(first.empty()) << "first section is consumed entirely";

  std::vector<int16_t> second = makeNumberedPairs(588, 1176);
  offset.apply(second);
  // 700 - 588 = 112 pairs trimmed from the second section.
  ASSERT_EQ(second.size(), (588u - 112u) * 2);
  EXPECT_EQ(second.front(), static_cast<int16_t>(1176 + 112 * 2));

  std::vector<int16_t> third = makeNumberedPairs(588, 2352);
  const std::vector<int16_t> expected = third;
  offset.apply(third);
  EXPECT_EQ(third, expected) << "trim is one-shot; later sections untouched";
}

TEST(AudioHeadOffset, PositiveOffsetPrependsSilenceOnce) {
  AudioHeadOffset offset;
  offset.setOffsetPairs(10);

  std::vector<int16_t> first = makeNumberedPairs(588, 100);
  offset.apply(first);
  ASSERT_EQ(first.size(), (588u + 10u) * 2);
  for (size_t i = 0; i < 20; ++i) {
    EXPECT_EQ(first[i], 0) << "sample " << i << " should be padding silence";
  }
  EXPECT_EQ(first[20], 100) << "decoded audio follows the padding";

  std::vector<int16_t> second = makeNumberedPairs(588, 200);
  const std::vector<int16_t> expected = second;
  offset.apply(second);
  EXPECT_EQ(second, expected) << "pad is one-shot; later sections untouched";
}

// ---------------------------------------------------------------------------
// TvaluesToChannel: channel bits discarded before the first channel frame
// ---------------------------------------------------------------------------

// One well-formed channel frame: a T11+T11 sync header followed by t-values
// summing to the remaining 566 bits (56 x T10 + T6), 588 bits in total.
void appendChannelFrame(std::vector<uint8_t>& stream) {
  stream.push_back(efm::kSyncSymbolT11);
  stream.push_back(efm::kSyncSymbolT11);
  for (int i = 0; i < 56; ++i) stream.push_back(10);
  stream.push_back(6);
}

TEST(TvaluesToChannelHeadLoss, CountsBitsDiscardedBeforeFirstFrameOnly) {
  TvaluesToChannel decoder;

  // 400 junk t-values of T5 contain no T11+T11 pair; once the buffer exceeds
  // the two-frame watermark the state machine drops all but the last one.
  decoder.pushFrame(std::vector<uint8_t>(400, 5));
  EXPECT_EQ(decoder.headDiscardedBits(), 399u * 5u);

  // Eight valid frames let the decoder lock and emit; the head statistic
  // must freeze at its pre-lock value.
  std::vector<uint8_t> frames;
  for (int i = 0; i < 8; ++i) appendChannelFrame(frames);
  decoder.pushFrame(frames);
  ASSERT_TRUE(decoder.isReady()) << "decoder should have emitted frames";
  EXPECT_EQ(decoder.headDiscardedBits(), 399u * 5u);

  // A mid-stream sync loss discards more input, but none of it is head loss.
  decoder.pushFrame(std::vector<uint8_t>(400, 5));
  std::vector<uint8_t> moreFrames;
  for (int i = 0; i < 8; ++i) appendChannelFrame(moreFrames);
  decoder.pushFrame(moreFrames);
  EXPECT_EQ(decoder.headDiscardedBits(), 399u * 5u);
}

// The cc-test1 capture shape (issue #231 verification): a short junk prefix
// followed immediately by valid frames. The initial-sync search finds the
// first sync while the junk is still in the buffer, so the first frame is
// recovered through the undershoot path — the prefix must be counted as head
// loss BEFORE that first frame closes the head-discard gate.
TEST(TvaluesToChannelHeadLoss, CountsJunkPrefixRecoveredViaUndershootPath) {
  TvaluesToChannel decoder;

  std::vector<uint8_t> stream(81, 5);  // 81 junk t-values = 405 channel bits
  for (int i = 0; i < 8; ++i) appendChannelFrame(stream);
  decoder.pushFrame(stream);

  ASSERT_TRUE(decoder.isReady()) << "decoder should have emitted frames";
  EXPECT_EQ(decoder.headDiscardedBits(), 81u * 5u);
}

// ---------------------------------------------------------------------------
// F3FrameToF2Section: F3 frames lost before the first F2 section
// ---------------------------------------------------------------------------

F3Frame makeSubcodeFrame() {
  F3Frame frame;
  frame.setData(std::vector<uint8_t>(32, 0x42));
  frame.setErrorData(std::vector<uint8_t>(32, 0));
  frame.setPaddedData(std::vector<uint8_t>(32, 0));
  frame.setFrameTypeAsSubcode(0);
  return frame;
}

F3Frame makeSync0Frame() {
  F3Frame frame = makeSubcodeFrame();
  frame.setFrameTypeAsSync0();
  return frame;
}

F3Frame makeSync1Frame() {
  F3Frame frame = makeSubcodeFrame();
  frame.setFrameTypeAsSync1();
  return frame;
}

void pushWholeSection(F3FrameToF2Section& decoder) {
  decoder.pushFrame(makeSync0Frame());
  decoder.pushFrame(makeSync1Frame());
  for (int i = 0; i < efm::kFramesPerSection - 2; ++i) {
    decoder.pushFrame(makeSubcodeFrame());
  }
}

TEST(F3FrameToF2SectionHeadLoss, CountsFramesDiscardedBeforeFirstSectionOnly) {
  F3FrameToF2Section decoder;

  // Seven junk frames precede the first section sync.
  for (int i = 0; i < 7; ++i) decoder.pushFrame(makeSubcodeFrame());

  // Two whole sections; the second section's sync0 carves the first.
  pushWholeSection(decoder);
  pushWholeSection(decoder);
  // A further boundary plus slack so the second section is carved too.
  decoder.pushFrame(makeSync0Frame());
  decoder.pushFrame(makeSync1Frame());
  for (int i = 0; i < 4; ++i) decoder.pushFrame(makeSubcodeFrame());

  ASSERT_TRUE(decoder.isReady()) << "decoder should have emitted a section";
  EXPECT_EQ(decoder.headLostF3Frames(), 7u);

  // Drain the output; the head statistic stays frozen.
  while (decoder.isReady()) decoder.popSection();
  EXPECT_EQ(decoder.headLostF3Frames(), 7u);
}

TEST(F3FrameToF2SectionHeadLoss, ZeroWhenStreamStartsOnSectionSync) {
  F3FrameToF2Section decoder;

  pushWholeSection(decoder);
  decoder.pushFrame(makeSync0Frame());
  decoder.pushFrame(makeSync1Frame());
  for (int i = 0; i < 4; ++i) decoder.pushFrame(makeSubcodeFrame());

  ASSERT_TRUE(decoder.isReady());
  EXPECT_EQ(decoder.headLostF3Frames(), 0u);
}

}  // namespace
