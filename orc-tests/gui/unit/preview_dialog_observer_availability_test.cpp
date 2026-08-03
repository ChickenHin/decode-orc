/*
 * File:        preview_dialog_observer_availability_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 3 offscreen tests for standard-specific observer gating
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QCoreApplication>

#include "presenters/include/project_presenter_types.h"
#include "previewdialog.h"

namespace gui_unit_test {

namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-preview-observer-test";
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

using orc::presenters::VideoFormat;

}  // namespace

TEST(PreviewDialogObserverAvailability, NtscFormat_EnablesClosedCaptionsOnly) {
  ensureApplication();
  PreviewDialog dialog;

  dialog.setObserverAvailabilityForFormat(VideoFormat::NTSC);

  EXPECT_TRUE(dialog.closedCaptionAction()->isEnabled());
  EXPECT_TRUE(dialog.ntscObserverAction()->isEnabled());
  EXPECT_FALSE(dialog.teletextAction()->isEnabled());
}

TEST(PreviewDialogObserverAvailability, PalFormat_EnablesTeletextOnly) {
  ensureApplication();
  PreviewDialog dialog;

  dialog.setObserverAvailabilityForFormat(VideoFormat::PAL);

  EXPECT_TRUE(dialog.teletextAction()->isEnabled());
  EXPECT_FALSE(dialog.closedCaptionAction()->isEnabled());
  EXPECT_FALSE(dialog.ntscObserverAction()->isEnabled());
}

// PAL-M is 525-line, so it carries neither 625-line System B teletext nor the
// NTSC-specific observers.
TEST(PreviewDialogObserverAvailability, PalMFormat_DisablesBothPreviews) {
  ensureApplication();
  PreviewDialog dialog;

  dialog.setObserverAvailabilityForFormat(VideoFormat::PAL_M);

  EXPECT_FALSE(dialog.teletextAction()->isEnabled());
  EXPECT_FALSE(dialog.closedCaptionAction()->isEnabled());
  EXPECT_FALSE(dialog.ntscObserverAction()->isEnabled());
}

TEST(PreviewDialogObserverAvailability, UnknownFormat_DisablesBothPreviews) {
  ensureApplication();
  PreviewDialog dialog;

  dialog.setObserverAvailabilityForFormat(VideoFormat::Unknown);

  EXPECT_FALSE(dialog.teletextAction()->isEnabled());
  EXPECT_FALSE(dialog.closedCaptionAction()->isEnabled());
  EXPECT_FALSE(dialog.ntscObserverAction()->isEnabled());
}

// Switching standards must re-enable what the new standard supports, not just
// disable what it does not.
TEST(PreviewDialogObserverAvailability, FormatChange_RestoresAvailability) {
  ensureApplication();
  PreviewDialog dialog;

  dialog.setObserverAvailabilityForFormat(VideoFormat::NTSC);
  dialog.setObserverAvailabilityForFormat(VideoFormat::PAL);
  EXPECT_TRUE(dialog.teletextAction()->isEnabled());
  EXPECT_FALSE(dialog.closedCaptionAction()->isEnabled());

  dialog.setObserverAvailabilityForFormat(VideoFormat::NTSC);
  EXPECT_FALSE(dialog.teletextAction()->isEnabled());
  EXPECT_TRUE(dialog.closedCaptionAction()->isEnabled());
}

}  // namespace gui_unit_test
