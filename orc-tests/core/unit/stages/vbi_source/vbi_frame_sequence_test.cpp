/*
 * File:        vbi_frame_sequence_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for field order, drop policy and signal state
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_frame_sequence.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace orc {
namespace {

VBISourceFormat bt8x8_pal_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(expand_vbi_source_preset("bt8x8-pal", format, error)) << error;
  return format;
}

// A source whose container carries no frame counter, as every format outside
// the bt8x8 family does.
VBISourceFormat format_without_counter() {
  VBISourceFormat format = bt8x8_pal_format();
  format.frame_trailer_is_counter = false;
  format.frame_trailer_bytes = 0;
  return format;
}

VBIFrameTimeline timeline(VBIDropPolicy drops, bool burst_synthesised = true,
                          VBISourceFormat format = bt8x8_pal_format()) {
  VBIFrameSequenceConfig config;
  config.drops = drops;
  config.burst_synthesised = burst_synthesised;
  return VBIFrameTimeline(format, config);
}

// Observe a run of stored frames whose counter advances by one each time.
void observe_contiguous(VBIFrameTimeline& sequence, uint64_t first_frame,
                        uint32_t first_counter, uint32_t count) {
  std::string error;
  for (uint32_t index = 0; index < count; ++index) {
    ASSERT_TRUE(sequence.observe_frame(first_frame + index,
                                       first_counter + index, error))
        << error;
  }
}

TEST(VBIFrameSequence, SignalStateSpellingsMatchTheMetadataVocabulary) {
  EXPECT_EQ(to_string(VBISignalState::kStandardTbcLocked),
            "STANDARD_TBC_LOCKED");
  EXPECT_EQ(to_string(VBISignalState::kStandardTbcUnlocked),
            "STANDARD_TBC_UNLOCKED");
}

// Design §2.4: a run with a coherent burst and an unbroken frame counter can
// claim both of the locked state's assertions.
TEST(VBIFrameSequence, AnUnbrokenRunWithBurstIsLocked) {
  VBIFrameTimeline sequence = timeline(VBIDropPolicy::kPreserve);
  observe_contiguous(sequence, 0, 100, 8);

  EXPECT_EQ(sequence.output_frame_count(), 8u);
  EXPECT_TRUE(sequence.discontinuities().empty());
  EXPECT_EQ(sequence.padded_frame_count(), 0u);
  EXPECT_EQ(sequence.signal_state(), VBISignalState::kStandardTbcLocked);

  VBIOutputFramePlan plan;
  ASSERT_TRUE(sequence.frame_plan(3, plan));
  EXPECT_EQ(plan.source_frame_index, 3u);
  EXPECT_EQ(plan.frame_counter, 103u);
  EXPECT_FALSE(plan.padding);
  EXPECT_FALSE(sequence.frame_plan(8, plan));
}

// Without a synthesised burst the subcarrier phase is neither stable nor
// known, so the state cannot be locked however clean the source is.
TEST(VBIFrameSequence, OmittingBurstForcesTheUnlockedState) {
  VBIFrameTimeline sequence =
      timeline(VBIDropPolicy::kPreserve, /*burst_synthesised=*/false);
  observe_contiguous(sequence, 0, 0, 4);

  EXPECT_TRUE(sequence.discontinuities().empty());
  EXPECT_EQ(sequence.signal_state(), VBISignalState::kStandardTbcUnlocked);
}

// Design §6.3: preserving drops means output frame numbering no longer matches
// the source's, so frame-boundary integrity is lost and every discontinuity
// has to be recorded for the mapping to stay recoverable.
TEST(VBIFrameSequence, PreservingACounterGapDowngradesToUnlocked) {
  VBIFrameTimeline sequence = timeline(VBIDropPolicy::kPreserve);
  std::string error;

  observe_contiguous(sequence, 0, 500, 3);
  // Three frames missing: the counter jumps from 502 to 506.
  ASSERT_TRUE(sequence.observe_frame(3, 506, error)) << error;
  ASSERT_TRUE(sequence.observe_frame(4, 507, error)) << error;

  EXPECT_EQ(sequence.output_frame_count(), 5u);
  EXPECT_EQ(sequence.padded_frame_count(), 0u);
  ASSERT_EQ(sequence.discontinuities().size(), 1u);

  const VBIFrameDiscontinuity& gap = sequence.discontinuities().front();
  EXPECT_EQ(gap.source_frame_index, 3u);
  EXPECT_EQ(gap.output_frame_index, 3u);
  EXPECT_EQ(gap.previous_counter, 502u);
  EXPECT_EQ(gap.counter, 506u);
  EXPECT_EQ(gap.missing_frames, 3u);
  EXPECT_FALSE(gap.padded);

  EXPECT_EQ(sequence.signal_state(), VBISignalState::kStandardTbcUnlocked);
}

TEST(VBIFrameSequence, EveryDiscontinuityIsRecordedNotJustTheFirst) {
  VBIFrameTimeline sequence = timeline(VBIDropPolicy::kPreserve);
  std::string error;

  ASSERT_TRUE(sequence.observe_frame(0, 10, error)) << error;
  ASSERT_TRUE(sequence.observe_frame(1, 12, error)) << error;
  ASSERT_TRUE(sequence.observe_frame(2, 13, error)) << error;
  ASSERT_TRUE(sequence.observe_frame(3, 20, error)) << error;

  ASSERT_EQ(sequence.discontinuities().size(), 2u);
  EXPECT_EQ(sequence.discontinuities()[0].missing_frames, 1u);
  EXPECT_EQ(sequence.discontinuities()[1].missing_frames, 6u);
}

// Design §6.3: padding keeps output frame n aligned with source frame n, so
// the timeline and the colour sequence survive the gap.
TEST(VBIFrameSequence, PaddingAGapKeepsTheTimelineAndTheLockedState) {
  VBIFrameTimeline sequence = timeline(VBIDropPolicy::kPad);
  std::string error;

  observe_contiguous(sequence, 0, 0, 3);
  // Frames 3 and 4 of the source's own timeline never arrived.
  ASSERT_TRUE(sequence.observe_frame(3, 5, error)) << error;

  EXPECT_EQ(sequence.output_frame_count(), 6u);
  EXPECT_EQ(sequence.padded_frame_count(), 2u);
  ASSERT_EQ(sequence.discontinuities().size(), 1u);
  EXPECT_TRUE(sequence.discontinuities().front().padded);
  EXPECT_EQ(sequence.discontinuities().front().missing_frames, 2u);
  EXPECT_EQ(sequence.signal_state(), VBISignalState::kStandardTbcLocked);

  // Output frame n carries the source frame whose counter is n, and the
  // invented frames are flagged as such.
  VBIOutputFramePlan plan;
  for (uint64_t index = 0; index < 3; ++index) {
    ASSERT_TRUE(sequence.frame_plan(index, plan));
    EXPECT_FALSE(plan.padding) << "output frame " << index;
    EXPECT_EQ(plan.frame_counter, static_cast<uint32_t>(index));
  }
  for (uint64_t index = 3; index < 5; ++index) {
    ASSERT_TRUE(sequence.frame_plan(index, plan));
    EXPECT_TRUE(plan.padding) << "output frame " << index;
    EXPECT_FALSE(plan.frame_counter.has_value());
  }
  ASSERT_TRUE(sequence.frame_plan(5, plan));
  EXPECT_FALSE(plan.padding);
  EXPECT_EQ(plan.frame_counter, 5u);
  EXPECT_EQ(plan.source_frame_index, 3u);

  // The output frames are a contiguous run, which is what keeps the burst
  // sequence coherent across the pad: burst phase is a function of the output
  // frame index.
  for (uint64_t index = 0; index < sequence.output_frame_count(); ++index) {
    ASSERT_TRUE(sequence.frame_plan(index, plan));
    EXPECT_EQ(plan.output_frame_index, index);
  }
}

// A counter that repeats or runs backwards is a break no padding can undo.
TEST(VBIFrameSequence, ACounterThatDoesNotAdvanceBreaksTheTimeline) {
  VBIFrameTimeline sequence = timeline(VBIDropPolicy::kPad);
  std::string error;

  ASSERT_TRUE(sequence.observe_frame(0, 40, error)) << error;
  ASSERT_TRUE(sequence.observe_frame(1, 40, error)) << error;
  ASSERT_TRUE(sequence.observe_frame(2, 39, error)) << error;

  EXPECT_EQ(sequence.output_frame_count(), 3u);
  EXPECT_EQ(sequence.padded_frame_count(), 0u);
  ASSERT_EQ(sequence.discontinuities().size(), 2u);
  EXPECT_EQ(sequence.discontinuities()[0].missing_frames, 0u);
  EXPECT_FALSE(sequence.discontinuities()[0].padded);
  EXPECT_EQ(sequence.signal_state(), VBISignalState::kStandardTbcUnlocked);
}

// The counter wraps at 32 bits, and a capture long enough to reach the wrap
// advances across it like anywhere else.
TEST(VBIFrameSequence, ACounterWrappingAtThirtyTwoBitsIsNotADiscontinuity) {
  VBIFrameTimeline sequence = timeline(VBIDropPolicy::kPad);
  std::string error;

  ASSERT_TRUE(sequence.observe_frame(0, 0xFFFFFFFEu, error)) << error;
  ASSERT_TRUE(sequence.observe_frame(1, 0xFFFFFFFFu, error)) << error;
  ASSERT_TRUE(sequence.observe_frame(2, 0u, error)) << error;
  ASSERT_TRUE(sequence.observe_frame(3, 1u, error)) << error;

  EXPECT_TRUE(sequence.discontinuities().empty());
  EXPECT_EQ(sequence.output_frame_count(), 4u);
  EXPECT_EQ(sequence.signal_state(), VBISignalState::kStandardTbcLocked);
}

// A counter that jumps by more than the policy will synthesise is a corrupt
// trailer, not a capture that many frames short.
TEST(VBIFrameSequence, AnAbsurdGapIsRefusedRatherThanPadded) {
  VBIFrameSequenceConfig config;
  config.drops = VBIDropPolicy::kPad;
  config.maximum_pad_frames = 8;
  VBIFrameTimeline sequence(bt8x8_pal_format(), config);

  std::string error;
  ASSERT_TRUE(sequence.observe_frame(0, 0, error)) << error;
  EXPECT_FALSE(sequence.observe_frame(1, 1000, error));
  EXPECT_FALSE(error.empty());
  EXPECT_NE(error.find("1000"), std::string::npos) << error;

  // Nothing was invented and the observed frame was not added.
  EXPECT_EQ(sequence.output_frame_count(), 1u);
  EXPECT_EQ(sequence.padded_frame_count(), 0u);
}

TEST(VBIFrameSequence, FramesMustBeObservedInStoredOrder) {
  VBIFrameTimeline sequence = timeline(VBIDropPolicy::kPreserve);
  std::string error;

  ASSERT_TRUE(sequence.observe_frame(5, 5, error)) << error;
  EXPECT_FALSE(sequence.observe_frame(4, 4, error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(sequence.observe_frame(5, 5, error));
}

// Design §6.1: which television field the first stored field carries is a
// driver convention, so it is configuration, and the assumption is recorded.
TEST(VBIFrameSequence, FieldOrderIsRecordedForTheCaptureNotes) {
  VBIFrameTimeline field1_first = timeline(VBIDropPolicy::kPreserve);
  EXPECT_EQ(field1_first.first_tv_field(), 1u);
  EXPECT_NE(field1_first.field_order_note().find("field 1"), std::string::npos);

  VBISourceFormat swapped = bt8x8_pal_format();
  swapped.first_field = 2;
  VBIFrameTimeline field2_first =
      timeline(VBIDropPolicy::kPreserve, true, swapped);
  EXPECT_EQ(field2_first.first_tv_field(), 2u);
  EXPECT_NE(field2_first.field_order_note().find("field 2"), std::string::npos);
  EXPECT_NE(field2_first.summary().find("field 2"), std::string::npos);
}

// Design §6.3: for a source with no counter, the absence of reported drops
// means nothing, and the summary has to say so rather than imply continuity.
TEST(VBIFrameSequence, ASourceWithoutACounterSaysSoRatherThanImplyingSafety) {
  VBIFrameTimeline sequence =
      timeline(VBIDropPolicy::kPreserve, true, format_without_counter());
  EXPECT_FALSE(sequence.counter_available());

  std::string error;
  for (uint64_t index = 0; index < 4; ++index) {
    ASSERT_TRUE(sequence.observe_frame(index, std::nullopt, error)) << error;
  }

  EXPECT_EQ(sequence.output_frame_count(), 4u);
  EXPECT_TRUE(sequence.discontinuities().empty());
  EXPECT_NE(sequence.summary().find("no frame counter"), std::string::npos)
      << sequence.summary();

  VBIFrameTimeline counted = timeline(VBIDropPolicy::kPreserve);
  EXPECT_TRUE(counted.counter_available());
}

TEST(VBIFrameSequence, TheSummaryReportsWhatTheRunDidAboutDrops) {
  VBIFrameTimeline sequence = timeline(VBIDropPolicy::kPad);
  std::string error;
  observe_contiguous(sequence, 0, 0, 2);
  ASSERT_TRUE(sequence.observe_frame(2, 4, error)) << error;

  const std::string summary = sequence.summary();
  EXPECT_NE(summary.find("STANDARD_TBC_LOCKED"), std::string::npos) << summary;
  EXPECT_NE(summary.find("padded"), std::string::npos) << summary;
  EXPECT_NE(summary.find("discontinuities: 1"), std::string::npos) << summary;
}

}  // namespace
}  // namespace orc
