/*
 * File:        teletext_dialog_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 3 offscreen tests for the teletext page preview dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>

#include "support/teletext_packet_fixtures.h"
#include "teletextdialog.h"
#include "teletextpagewidget.h"

namespace gui_unit_test {

namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-teletext-dialog-test";
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

// Deliver a three-frame window where frame 1 carries page 100.
void deliverPage100Window(TeletextDialog& dialog) {
  dialog.setCurrentFrame(2);
  for (const uint64_t frame : dialog.framesNeedingData()) {
    if (frame == 1) {
      dialog.deliverFrameData(
          true, frame * 2,
          makeFieldView({makeHeaderPacket(1, 0x00),
                         makeRowPacket(1, 1, "HELLO TELETEXT")}),
          makeFieldView({makeTimeFillingHeader(1)}));
    } else {
      dialog.deliverFrameData(true, frame * 2, makeEmptyFieldView(),
                              makeEmptyFieldView());
    }
  }
}

}  // namespace

TEST(TeletextDialogTest, CanShowAndClose) {
  (void)ensureApplication();

  TeletextDialog dialog;

  dialog.show();
  QCoreApplication::processEvents();
  EXPECT_TRUE(dialog.isVisible());

  dialog.close();
  QCoreApplication::processEvents();
  EXPECT_FALSE(dialog.isVisible());
}

TEST(TeletextDialogTest, PendingThenPopulated_RendersRequestedPage) {
  (void)ensureApplication();

  TeletextDialog dialog;
  auto* status = dialog.findChild<QLabel*>("observationStatusLabel");
  ASSERT_NE(status, nullptr);
  EXPECT_FALSE(status->isVisible());

  dialog.show();
  QCoreApplication::processEvents();

  dialog.setCurrentFrame(2);
  EXPECT_EQ(dialog.framesNeedingData().size(), 3u);
  dialog.showPending();
  QCoreApplication::processEvents();
  EXPECT_TRUE(status->isVisible());

  deliverPage100Window(dialog);
  QCoreApplication::processEvents();

  EXPECT_FALSE(status->isVisible());
  EXPECT_TRUE(dialog.framesNeedingData().empty());

  // Cell accuracy is asserted on the page-view model, not on pixels.
  ASSERT_TRUE(dialog.currentPage().has_value());
  EXPECT_EQ(dialog.currentPage()->magazine, 1);
  EXPECT_EQ(dialog.currentPage()->page_number, 0x00);
  EXPECT_EQ(rowText(*dialog.currentPage(), 1), "HELLO TELETEXT");

  auto* seen = dialog.findChild<QLabel*>("teletextSeenLabel");
  ASSERT_NE(seen, nullptr);
  EXPECT_EQ(seen->text(), QString("Page last seen at frame 2"));

  auto* page_widget = dialog.findChild<TeletextPageWidget*>();
  ASSERT_NE(page_widget, nullptr);
  EXPECT_TRUE(page_widget->hasPage());
}

TEST(TeletextDialogTest, PageNumberEntry_SelectsAndClearsPage) {
  (void)ensureApplication();

  TeletextDialog dialog;
  deliverPage100Window(dialog);

  ASSERT_TRUE(dialog.currentPage().has_value());

  // A page not present in the window clears the display.
  dialog.setPageNumberText("888");
  EXPECT_FALSE(dialog.currentPage().has_value());

  // Returning to the transmitted page re-renders it from the cache.
  dialog.setPageNumberText("100");
  ASSERT_TRUE(dialog.currentPage().has_value());
  EXPECT_EQ(rowText(*dialog.currentPage(), 1), "HELLO TELETEXT");
}

TEST(TeletextDialogTest, InvalidPageNumber_ShowsNotice) {
  (void)ensureApplication();

  TeletextDialog dialog;
  deliverPage100Window(dialog);

  dialog.setPageNumberText("9x");

  EXPECT_FALSE(dialog.currentPage().has_value());
  auto* seen = dialog.findChild<QLabel*>("teletextSeenLabel");
  ASSERT_NE(seen, nullptr);
  EXPECT_TRUE(seen->text().contains("Invalid"));
}

TEST(TeletextDialogTest, ClearContentResetsCacheAndDisplay) {
  (void)ensureApplication();

  TeletextDialog dialog;
  deliverPage100Window(dialog);
  ASSERT_TRUE(dialog.currentPage().has_value());

  dialog.clearContent();

  EXPECT_FALSE(dialog.currentPage().has_value());
  EXPECT_EQ(dialog.framesNeedingData().size(), 3u);  // cache dropped
}

}  // namespace gui_unit_test
