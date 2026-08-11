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

// The channel-pair dropdown shows the pair's description next to its index so
// the user picks by name, while the stored value stays the bare index.
TEST(AudioChannelPairNotice, ComboEntryShowsIndexAndName) {
  EXPECT_EQ(audioChannelPairComboEntry(0, "Analogue", '\x1f'),
            std::string("0\x1f") + "0: Analogue");
  EXPECT_EQ(audioChannelPairComboEntry(1, "EFM digital audio", '\x1f'),
            std::string("1\x1f") + "1: EFM digital audio");
}

// An unnamed pair still has to be selectable; it shows its index alone.
TEST(AudioChannelPairNotice, ComboEntryFallsBackToTheBareIndex) {
  EXPECT_EQ(audioChannelPairComboEntry(2, "", '\x1f'),
            std::string("2\x1f") + "2");
}

// A sink that writes audio only as an optional sidecar still exports; only the
// channel-pair setting is inert, so it must not claim a pass-through.
TEST(AudioChannelPairNotice, SidecarNoticeIsEmptyWhenAudioIsPresent) {
  EXPECT_EQ(audioChannelPairSidecarNotice(1), "");
  EXPECT_EQ(audioChannelPairSidecarNotice(8), "");
}

TEST(AudioChannelPairNotice, SidecarNoticeExplainsTheMissingPcmOnly) {
  const std::string note = audioChannelPairSidecarNotice(0);
  EXPECT_NE(note.find(".pcm"), std::string::npos);
  EXPECT_NE(note.find("no effect"), std::string::npos);
  EXPECT_NE(note.find("exported as normal"), std::string::npos);
  // The pass-through wording belongs to the transform stages, not here.
  EXPECT_EQ(note.find("pass its input through unchanged"), std::string::npos);
}

TEST(AudioChannelPairNotice, SidecarNoticeAppendsToADescription) {
  const std::string combined =
      withAudioChannelPairSidecarNotice("Writes a TBC.", 0);
  EXPECT_NE(combined.find("Writes a TBC."), std::string::npos);
  EXPECT_NE(combined.find(audioChannelPairSidecarNotice(0)), std::string::npos);
  EXPECT_EQ(withAudioChannelPairSidecarNotice("Writes a TBC.", 2),
            "Writes a TBC.");
}

}  // namespace
}  // namespace orc::gui
