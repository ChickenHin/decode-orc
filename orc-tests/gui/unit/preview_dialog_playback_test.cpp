/*
 * File:        preview_dialog_playback_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Playback start/stop behaviour tests for the preview dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QPushButton>
#include <QSlider>

#include "previewdialog.h"

namespace gui_unit_test {

namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-widget-test";
  static char platform_opt[] = "-platform";
  static char platform_val[] = "offscreen";
  static char* argv[] = {app_name, platform_opt, platform_val, nullptr};
  static QApplication* app = [] {
    auto* created_app = new QApplication(argc, argv);
    created_app->setQuitOnLastWindowClosed(false);
    return created_app;
  }();
  return *app;
}

constexpr int kFirstIndex = 0;
constexpr int kLastIndex = 9;

}  // namespace

TEST(PreviewDialogPlayback, PlayAtLastFrame_RestartsFromFirstFrame) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  dialog.navigateToIndex(kLastIndex);
  ASSERT_EQ(dialog.currentIndex(), kLastIndex);

  dialog.playPauseButton()->click();

  EXPECT_EQ(dialog.currentIndex(), kFirstIndex);
  // Playback must actually be running (button shows the pause symbol).
  EXPECT_EQ(dialog.playPauseButton()->text(), QString::fromUtf8("⏸"));

  dialog.stopPlayback();
  EXPECT_EQ(dialog.playPauseButton()->text(), QString::fromUtf8("▶"));
}

TEST(PreviewDialogPlayback, PlayMidRange_KeepsCurrentPosition) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  const int mid_index = (kFirstIndex + kLastIndex) / 2;
  dialog.navigateToIndex(mid_index);

  dialog.playPauseButton()->click();

  EXPECT_EQ(dialog.currentIndex(), mid_index);
  EXPECT_EQ(dialog.playPauseButton()->text(), QString::fromUtf8("⏸"));

  dialog.stopPlayback();
}

TEST(PreviewDialogPlayback, PlayThenPause_TogglesWithoutMoving) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  dialog.navigateToIndex(kLastIndex);

  dialog.playPauseButton()->click();  // play: rewinds to first frame
  dialog.playPauseButton()->click();  // pause immediately

  EXPECT_EQ(dialog.currentIndex(), kFirstIndex);
  EXPECT_EQ(dialog.playPauseButton()->text(), QString::fromUtf8("▶"));
}

// The owner reads isPlaying() to decide between the Sequential and Random
// navigation hints, so it has to track the transport exactly — including the
// stop that the end of the range triggers.
TEST(PreviewDialogPlayback, IsPlaying_TracksTheTransportState) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  dialog.navigateToIndex(kFirstIndex);

  EXPECT_FALSE(dialog.isPlaying());

  dialog.playPauseButton()->click();
  EXPECT_TRUE(dialog.isPlaying());

  dialog.playPauseButton()->click();
  EXPECT_FALSE(dialog.isPlaying());

  dialog.playPauseButton()->click();
  ASSERT_TRUE(dialog.isPlaying());
  dialog.stopPlayback();
  EXPECT_FALSE(dialog.isPlaying());
}

}  // namespace gui_unit_test
