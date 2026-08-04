/*
 * File:        preview_audio_chase_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 coverage of the preview audio clock arithmetic
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview_audio_chase.h"

#include <gtest/gtest.h>
#include <orc/stage/audio/audio_channel_pair.h>

namespace {

using namespace orc::gui::preview_audio;

constexpr uint32_t kRate = orc::kAudioSampleRateHz;

TEST(PreviewAudioChase, StereoPairsPlayed_IsZero_AtTheStartOfPlayback) {
  EXPECT_EQ(stereoPairsPlayed(0, kRate), 0u);
}

TEST(PreviewAudioChase, StereoPairsPlayed_MatchesSampleRate_AfterOneSecond) {
  EXPECT_EQ(stereoPairsPlayed(1000000, kRate), 48000u);
}

TEST(PreviewAudioChase, StereoPairsPlayed_MatchesPalFrame_AfterFortyMs) {
  // ITU-R BT.1700 Annex 1 Part B (625-line PAL): 1920 stereo pairs per frame.
  EXPECT_EQ(stereoPairsPlayed(40000, kRate), 1920u);
}

TEST(PreviewAudioChase, StereoPairsPlayed_TruncatesToTheLastCompletedPair) {
  // 20 us is 0.96 of a pair: nothing has finished playing yet.
  EXPECT_EQ(stereoPairsPlayed(20, kRate), 0u);
  EXPECT_EQ(stereoPairsPlayed(21, kRate), 1u);
}

TEST(PreviewAudioChase, StereoPairsPlayed_StaysExact_OverAFeatureLengthRun) {
  // Three hours of audio must not overflow or lose a pair.
  constexpr uint64_t kThreeHoursUs = 3ULL * 60 * 60 * 1000000ULL;
  EXPECT_EQ(stereoPairsPlayed(kThreeHoursUs, kRate), 3ULL * 60 * 60 * kRate);
}

TEST(PreviewAudioChase, MicrosecondsForStereoPairs_InvertsPlayedPairs) {
  EXPECT_EQ(microsecondsForStereoPairs(1920, kRate), 40000u);
  EXPECT_EQ(microsecondsForStereoPairs(kRate, kRate), 1000000u);
}

TEST(PreviewAudioChase, MicrosecondsForStereoPairs_IsZero_ForAZeroSampleRate) {
  EXPECT_EQ(microsecondsForStereoPairs(1920, 0), 0u);
}

TEST(PreviewAudioChase, StereoPairsForMilliseconds_MatchesTheFeedLead) {
  EXPECT_EQ(stereoPairsForMilliseconds(kFeedLeadMs, kRate), 24000u);
  EXPECT_EQ(stereoPairsForMilliseconds(0, kRate), 0u);
}

TEST(PreviewAudioChase,
     PreviewIndexForFrame_IsTheFrame_ForFrameIndexedOutputs) {
  EXPECT_EQ(previewIndexForFrame(0, 1), 0u);
  EXPECT_EQ(previewIndexForFrame(7, 1), 7u);
}

TEST(PreviewAudioChase,
     PreviewIndexForFrame_SelectsTheFirstField_ForFieldIndexedOutputs) {
  // Audio for a frame starts with its first field, so the chase must land
  // there rather than half a frame ahead on the second.
  EXPECT_EQ(previewIndexForFrame(0, 2), 0u);
  EXPECT_EQ(previewIndexForFrame(7, 2), 14u);
}

TEST(PreviewAudioChase, PreviewIndexForFrame_TreatsZeroItemsPerFrameAsOne) {
  EXPECT_EQ(previewIndexForFrame(7, 0), 7u);
}

TEST(PreviewAudioChase, FrameForPreviewIndex_MapsBothFieldsToTheSameFrame) {
  EXPECT_EQ(frameForPreviewIndex(14, 2), 7u);
  EXPECT_EQ(frameForPreviewIndex(15, 2), 7u);
  EXPECT_EQ(frameForPreviewIndex(16, 2), 8u);
}

TEST(PreviewAudioChase, FrameForPreviewIndex_InvertsPreviewIndexForFrame) {
  for (uint32_t items : {1u, 2u}) {
    for (uint64_t frame = 0; frame < 50; ++frame) {
      EXPECT_EQ(frameForPreviewIndex(previewIndexForFrame(frame, items), items),
                frame)
          << "items_per_frame=" << items << " frame=" << frame;
    }
  }
}

TEST(PreviewAudioChase, FrameForPreviewIndex_TreatsZeroItemsPerFrameAsOne) {
  EXPECT_EQ(frameForPreviewIndex(7, 0), 7u);
}

TEST(PreviewAudioChase, ItemsPerFrame_IsTwoForFieldIndexedOutputs) {
  // A field-indexed output steps a field per preview index, so one frame's
  // audio spans two of them.
  EXPECT_EQ(itemsPerFrameForOutputType(orc::PreviewOutputType::Frame_Field1),
            2u);
  EXPECT_EQ(itemsPerFrameForOutputType(orc::PreviewOutputType::Frame_Field2),
            2u);
  EXPECT_EQ(itemsPerFrameForOutputType(orc::PreviewOutputType::Luma), 2u);
}

TEST(PreviewAudioChase, ItemsPerFrame_IsOneForFrameIndexedOutputs) {
  EXPECT_EQ(
      itemsPerFrameForOutputType(orc::PreviewOutputType::Frame_Field1_First),
      1u);
  EXPECT_EQ(itemsPerFrameForOutputType(orc::PreviewOutputType::Frame_Reversed),
            1u);
  EXPECT_EQ(itemsPerFrameForOutputType(orc::PreviewOutputType::Split), 1u);
  EXPECT_EQ(itemsPerFrameForOutputType(orc::PreviewOutputType::Chroma), 1u);
  EXPECT_EQ(itemsPerFrameForOutputType(orc::PreviewOutputType::Composite), 1u);
}

TEST(PreviewAudioChase, FeedChunk_IsShorterThanTheFeedLead) {
  // A chunk read must be small next to the lead, otherwise a single read
  // decides whether the device starves.
  const uint64_t chunk_pairs_pal = kFeedChunkFrames * 1920;
  EXPECT_LT(chunk_pairs_pal, stereoPairsForMilliseconds(kFeedLeadMs, kRate));
  EXPECT_GT(microsecondsForStereoPairs(chunk_pairs_pal, kRate),
            kFeedIntervalMs * 1000ULL);
}

}  // namespace
