/*
 * File:        audio_channel_pair_notice_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 (gui-logic) tests for the audio-stage "input carries no
 *              channel pairs" dialog notice helper
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "audio_channel_pair_notice.h"

#include <gtest/gtest.h>

namespace orc::gui {
namespace {

TEST(AudioChannelPairNotice, InputWithPairsNeedsNoNotice) {
  EXPECT_EQ(audioChannelPairNotice(1), "");
  EXPECT_EQ(audioChannelPairNotice(8), "");
}

TEST(AudioChannelPairNotice, InputWithoutPairsExplainsPassThrough) {
  const std::string note = audioChannelPairNotice(0);
  EXPECT_NE(note.find("no audio channel pairs"), std::string::npos);
  EXPECT_NE(note.find("pass its input through unchanged"), std::string::npos);
}

TEST(AudioChannelPairNotice, NoticeIsAppendedBelowTheStageDescription) {
  EXPECT_EQ(withAudioChannelPairNotice("Route audio channel pairs", 2),
            "Route audio channel pairs");

  const std::string combined =
      withAudioChannelPairNotice("Route audio channel pairs", 0);
  EXPECT_EQ(combined.rfind("Route audio channel pairs\n\n", 0), 0u);
  EXPECT_NE(combined.find(audioChannelPairNotice(0)), std::string::npos);
}

TEST(AudioChannelPairNotice, PreviewNoticeDescribesNothingToPlay) {
  // The preview is not passing anything through, so it must not borrow the
  // stage wording — it simply has no audio to offer.
  const std::string note = audioChannelPairPreviewNotice();
  EXPECT_NE(note.find("no audio channel pairs"), std::string::npos);
  EXPECT_NE(note.find("nothing to play"), std::string::npos);
  EXPECT_EQ(note.find("pass its input through unchanged"), std::string::npos);
}

TEST(AudioChannelPairNotice, EmptyDescriptionYieldsTheNoticeAlone) {
  EXPECT_EQ(withAudioChannelPairNotice("", 0), audioChannelPairNotice(0));
  EXPECT_EQ(withAudioChannelPairNotice("", 3), "");
}

}  // namespace
}  // namespace orc::gui
