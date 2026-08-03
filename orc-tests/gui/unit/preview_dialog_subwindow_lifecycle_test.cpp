/*
 * File:        preview_dialog_subwindow_lifecycle_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 3 offscreen tests: closed sub-windows stay closed when a
 *              late asynchronous data response arrives
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>

#include "fieldpreviewwidget.h"
#include "framescopedialog.h"
#include "frametimingdialog.h"
#include "previewdialog.h"
#include "waveformmonitordialog.h"

namespace gui_unit_test {

namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-preview-subwindow-test";
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

constexpr const char* kLineScopeViewId = "preview.linescope";

// Mirrors what the line-scope response path hands PreviewDialog once the
// worker thread has produced samples for the requested frame line.
void deliverLineSamples(PreviewDialog& dialog) {
  const std::vector<int16_t> samples(16, 512);
  dialog.showLineScope("node-1", 1, /*field_index=*/4, /*line_number=*/95,
                       /*sample_x=*/100, samples, std::nullopt,
                       /*preview_image_width=*/1135, /*original_sample_x=*/100,
                       /*original_image_y=*/94,
                       orc::PreviewOutputType::Frame_Field1_First);
}

// Makes the line scope available and simulates the user click on the preview
// image that requests it, which is the only user-initiated way to open it.
void requestLineScope(PreviewDialog& dialog) {
  orc::PreviewViewDescriptor line_scope_view;
  line_scope_view.id = kLineScopeViewId;
  line_scope_view.display_name = "Line Scope";
  dialog.setAvailablePreviewViews({line_scope_view});

  QSignalSpy requested(&dialog, &PreviewDialog::lineScopeRequested);
  emit dialog.previewWidget()->lineClicked(100, 94);
  ASSERT_EQ(requested.count(), 1);
}

}  // namespace

// A line-sample response that was requested before the user clicked the click
// must not resurrect the scope. During playback a request is issued for every
// frame, so one is almost always in flight when the user closes the window.
TEST(PreviewDialogSubwindowLifecycle,
     LineSamplesAfterScopeClosed_DoesNotReopenScope) {
  ensureApplication();
  PreviewDialog dialog;
  ASSERT_NO_FATAL_FAILURE(requestLineScope(dialog));

  deliverLineSamples(dialog);
  ASSERT_TRUE(dialog.isLineScopeVisible());

  dialog.frameScopeDialog()->close();
  ASSERT_FALSE(dialog.isLineScopeVisible());

  // The in-flight response lands after the close.
  deliverLineSamples(dialog);

  EXPECT_FALSE(dialog.isLineScopeVisible());
  EXPECT_FALSE(dialog.previewWidget()->crosshairsEnabled());
}

// The scope must never open off the back of a data response alone — only a
// user request opens it.
TEST(PreviewDialogSubwindowLifecycle,
     LineSamplesWithoutUserRequest_DoesNotOpenScope) {
  ensureApplication();
  PreviewDialog dialog;

  deliverLineSamples(dialog);

  EXPECT_FALSE(dialog.isLineScopeVisible());
}

// Re-clicking the preview after a close is a fresh request and must reopen it.
TEST(PreviewDialogSubwindowLifecycle, ScopeReopensOnNewUserRequest) {
  ensureApplication();
  PreviewDialog dialog;
  ASSERT_NO_FATAL_FAILURE(requestLineScope(dialog));
  deliverLineSamples(dialog);
  dialog.frameScopeDialog()->close();
  ASSERT_FALSE(dialog.isLineScopeVisible());

  ASSERT_NO_FATAL_FAILURE(requestLineScope(dialog));
  deliverLineSamples(dialog);

  EXPECT_TRUE(dialog.isLineScopeVisible());
}

// The View-menu actions record the intent that the async data callbacks in
// MainWindow consult before showing their dialogs; closing the dialog clears
// it so a late response cannot re-open the window.
TEST(PreviewDialogSubwindowLifecycle, FrameTimingIntentClearedOnClose) {
  ensureApplication();
  PreviewDialog dialog;

  orc::PreviewViewDescriptor frame_timing_view;
  frame_timing_view.id = "preview.frame_timing";
  frame_timing_view.display_name = "Frame Timing";
  dialog.setAvailablePreviewViews({frame_timing_view});

  EXPECT_FALSE(dialog.isFrameTimingOpenRequested());

  dialog.frameTimingAction()->trigger();
  EXPECT_TRUE(dialog.isFrameTimingOpenRequested());

  // MainWindow shows the dialog once its data arrives; closing it must drop
  // the intent.
  dialog.frameTimingDialog()->show();
  dialog.frameTimingDialog()->close();

  EXPECT_FALSE(dialog.isFrameTimingOpenRequested());
}

TEST(PreviewDialogSubwindowLifecycle, WaveformMonitorIntentClearedOnClose) {
  ensureApplication();
  PreviewDialog dialog;

  orc::PreviewViewDescriptor waveform_view;
  waveform_view.id = "preview.frame_timing";
  waveform_view.display_name = "Waveform Monitor";
  dialog.setAvailablePreviewViews({waveform_view});

  EXPECT_FALSE(dialog.isWaveformMonitorOpenRequested());

  dialog.waveformMonitorAction()->trigger();
  EXPECT_TRUE(dialog.isWaveformMonitorOpenRequested());

  dialog.waveformMonitorDialog()->show();
  dialog.waveformMonitorDialog()->close();

  EXPECT_FALSE(dialog.isWaveformMonitorOpenRequested());
}

// Closing the preview window drops every intent, including for a dialog that
// had not opened yet because its data was still in flight.
TEST(PreviewDialogSubwindowLifecycle, ClosingPreviewClearsPendingIntents) {
  ensureApplication();
  PreviewDialog dialog;
  ASSERT_NO_FATAL_FAILURE(requestLineScope(dialog));

  dialog.closeChildDialogs();

  deliverLineSamples(dialog);
  EXPECT_FALSE(dialog.isLineScopeVisible());
  EXPECT_FALSE(dialog.isFrameTimingOpenRequested());
  EXPECT_FALSE(dialog.isWaveformMonitorOpenRequested());
}

}  // namespace gui_unit_test
