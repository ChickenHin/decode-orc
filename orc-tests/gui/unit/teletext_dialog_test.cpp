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
#include <QBrush>
#include <QCheckBox>
#include <QImage>
#include <QLabel>
#include <QPalette>
#include <QTableWidget>

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

// Deliver a three-frame window carrying page 100 (frame 1) and page 888
// (frame 2).
void deliverTwoPageWindow(TeletextDialog& dialog) {
  dialog.setCurrentFrame(2);
  for (const uint64_t frame : dialog.framesNeedingData()) {
    if (frame == 1) {
      dialog.deliverFrameData(
          true, frame * 2,
          makeFieldView({makeHeaderPacket(1, 0x00),
                         makeRowPacket(1, 1, "HELLO TELETEXT")}),
          makeFieldView({makeTimeFillingHeader(1)}));
    } else if (frame == 2) {
      dialog.deliverFrameData(
          true, frame * 2,
          makeFieldView({makeHeaderPacket(8, 0x88),
                         makeRowPacket(8, 1, "SUBTITLE TEXT")}),
          makeFieldView({makeTimeFillingHeader(8)}));
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
  EXPECT_EQ(seen->text(),
            QString("Page 100 last seen at frame 2 (1 transmission(s))"));

  auto* page_widget = dialog.findChild<TeletextPageWidget*>();
  ASSERT_NE(page_widget, nullptr);
  EXPECT_TRUE(page_widget->hasPage());
}

TEST(TeletextDialogTest, UnavailableFramesResolveThePendingState) {
  (void)ensureApplication();

  TeletextDialog dialog;
  dialog.setCurrentFrame(2);
  dialog.showPending();

  for (const uint64_t frame : dialog.framesNeedingData()) {
    dialog.deliverFrameData(false, frame * 2, makeEmptyFieldView(),
                            makeEmptyFieldView());
  }

  // Unobservable frames are not re-requested, so the window converges.
  EXPECT_TRUE(dialog.framesNeedingData().empty());
  auto* status = dialog.findChild<QLabel*>("observationStatusLabel");
  ASSERT_NE(status, nullptr);
  EXPECT_FALSE(status->isVisible());
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

TEST(TeletextDialogTest, SeenPagesAreTabulatedInPageOrder) {
  (void)ensureApplication();

  TeletextDialog dialog;
  deliverTwoPageWindow(dialog);

  const auto listed = dialog.listedPages();
  ASSERT_EQ(listed.size(), 2u);
  EXPECT_EQ(listed[0], QString("100"));
  EXPECT_EQ(listed[1], QString("888"));

  auto* table = dialog.findChild<QTableWidget*>("teletextPagesTable");
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->columnCount(), 3);
  EXPECT_EQ(table->item(0, 0)->text(), QString("100"));
  // One transmission each, and where each was last seen (1-based frames).
  EXPECT_EQ(table->item(0, 1)->text(), QString("1"));
  EXPECT_EQ(table->item(0, 2)->text(), QString("2"));
  EXPECT_EQ(table->item(1, 2)->text(), QString("3"));
  // The rendered page is selected in the table.
  EXPECT_EQ(table->currentRow(), 0);
}

TEST(TeletextDialogTest, SelectingATabulatedPageRendersIt) {
  (void)ensureApplication();

  TeletextDialog dialog;
  deliverTwoPageWindow(dialog);

  auto* table = dialog.findChild<QTableWidget*>("teletextPagesTable");
  ASSERT_NE(table, nullptr);
  table->setCurrentCell(1, 0);

  EXPECT_EQ(dialog.pageNumberText(), QString("888"));
  ASSERT_TRUE(dialog.currentPage().has_value());
  EXPECT_EQ(dialog.currentPage()->magazine, 8);
  EXPECT_EQ(dialog.currentPage()->page_number, 0x88);
  EXPECT_EQ(rowText(*dialog.currentPage(), 1), "SUBTITLE TEXT");
}

// A carousel repeats its pages, so how often one came round is what tells the
// user whether it can be recovered reliably here.
TEST(TeletextDialogTest, RepeatedTransmissionsAccumulateASeenCount) {
  (void)ensureApplication();

  TeletextDialog dialog;
  deliverPage100Window(dialog);
  ASSERT_EQ(dialog.listedSeenCount("100"), 1u);

  // Step forward, re-transmitting page 100 every third frame.
  for (uint64_t frame = 3; frame <= 11; ++frame) {
    dialog.setCurrentFrame(frame);
    if (frame % 3 == 0) {
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

  // Frames 3, 6 and 9 carried the page on top of the original transmission.
  // The window is re-decoded on every frame change, so this also asserts that
  // replayed transmissions are not counted again.
  EXPECT_EQ(dialog.listedSeenCount("100"), 4u);
  EXPECT_EQ(dialog.listedPages().size(), 1u);
}

// EN 300 706 §9.3.1.1: a page number containing A-F cannot be selected on a
// receiver. Such pages stay listed — they are real recovered data — but sort
// below the selectable ones and are greyed.
TEST(TeletextDialogTest, NonSelectablePagesSortLastAndAreGreyed) {
  (void)ensureApplication();

  TeletextDialog dialog;
  dialog.setCurrentFrame(2);
  for (const uint64_t frame : dialog.framesNeedingData()) {
    if (frame == 1) {
      dialog.deliverFrameData(true, frame * 2,
                              makeFieldView({makeHeaderPacket(1, 0xAF)}),
                              makeFieldView({makeTimeFillingHeader(1)}));
    } else if (frame == 2) {
      dialog.deliverFrameData(true, frame * 2,
                              makeFieldView({makeHeaderPacket(8, 0x88)}),
                              makeFieldView({makeTimeFillingHeader(8)}));
    } else {
      dialog.deliverFrameData(true, frame * 2, makeEmptyFieldView(),
                              makeEmptyFieldView());
    }
  }

  const auto listed = dialog.listedPages();
  ASSERT_EQ(listed.size(), 2u);
  EXPECT_EQ(listed[0], QString("888")) << "selectable pages come first";
  EXPECT_EQ(listed[1], QString("1AF"));

  auto* table = dialog.findChild<QTableWidget*>("teletextPagesTable");
  ASSERT_NE(table, nullptr);
  const QBrush muted =
      table->palette().brush(QPalette::Disabled, QPalette::Text);
  EXPECT_EQ(table->item(1, 0)->foreground(), muted);
  EXPECT_NE(table->item(0, 0)->foreground(), muted);
}

// Playback bumps the catalogue on almost every frame; rebuilding the table
// each time dropped the scroll position and the selection under the user.
TEST(TeletextDialogTest, TableRowsSurviveOngoingDeliveries) {
  (void)ensureApplication();

  TeletextDialog dialog;
  deliverTwoPageWindow(dialog);

  auto* table = dialog.findChild<QTableWidget*>("teletextPagesTable");
  ASSERT_NE(table, nullptr);
  table->setCurrentCell(1, 0);
  ASSERT_EQ(dialog.pageNumberText(), QString("888"));

  const QTableWidgetItem* row0 = table->item(0, 0);
  const QTableWidgetItem* row1 = table->item(1, 0);

  // Keep delivering frames that re-transmit page 888 only.
  for (uint64_t frame = 3; frame <= 12; ++frame) {
    dialog.setCurrentFrame(frame);
    dialog.deliverFrameData(true, frame * 2,
                            makeFieldView({makeHeaderPacket(8, 0x88)}),
                            makeFieldView({makeTimeFillingHeader(8)}));
  }

  // The rows were updated in place, not recreated, and the user's selection
  // is untouched.
  EXPECT_EQ(table->item(0, 0), row0);
  EXPECT_EQ(table->item(1, 0), row1);
  EXPECT_EQ(table->currentRow(), 1);
  EXPECT_EQ(dialog.pageNumberText(), QString("888"));
  EXPECT_GT(dialog.listedSeenCount("888"), 1u);
}

TEST(TeletextDialogTest, PageListSurvivesSequentialStepping) {
  (void)ensureApplication();

  TeletextDialog dialog;
  deliverTwoPageWindow(dialog);
  ASSERT_EQ(dialog.listedPages().size(), 2u);

  // Step past the trailing window one frame at a time: the frames carrying
  // the pages are evicted, but the pages stay listed.
  for (uint64_t frame = 3;
       frame <= TeletextPageAssembler::kTrailingWindowFrames + 10; ++frame) {
    dialog.setCurrentFrame(frame);
    dialog.deliverFrameData(true, frame * 2, makeEmptyFieldView(),
                            makeEmptyFieldView());
  }

  EXPECT_EQ(dialog.listedPages().size(), 2u);
  EXPECT_TRUE(dialog.currentPage().has_value());
}

TEST(TeletextDialogTest, SkippingRestartsThePageList) {
  (void)ensureApplication();

  TeletextDialog dialog;
  deliverTwoPageWindow(dialog);
  ASSERT_EQ(dialog.listedPages().size(), 2u);

  // A jump with no overlap with the previous window discards the list; it is
  // rebuilt from the frames preceding the position jumped to.
  dialog.setCurrentFrame(2 + TeletextPageAssembler::kTrailingWindowFrames);

  EXPECT_TRUE(dialog.listedPages().empty());
  EXPECT_FALSE(dialog.currentPage().has_value());
}

TEST(TeletextDialogTest, ClearContentResetsCacheAndDisplay) {
  (void)ensureApplication();

  TeletextDialog dialog;
  deliverPage100Window(dialog);
  ASSERT_TRUE(dialog.currentPage().has_value());

  dialog.clearContent();

  EXPECT_FALSE(dialog.currentPage().has_value());
  EXPECT_TRUE(dialog.listedPages().empty());
  EXPECT_EQ(dialog.framesNeedingData().size(), 3u);  // cache dropped
}

namespace {

// Paint a page into an image sized so each character rectangle is a whole
// number of pixels and the aspect lock leaves no letterbox.
QImage renderPage(const orc::presenters::TeletextPageView& page) {
  TeletextPageWidget widget;
  widget.setPage(page);
  const QSize size(orc::presenters::TeletextPageView::kColumns * 12,
                   orc::presenters::TeletextPageView::kRows * 20);
  widget.resize(size);
  QImage image(size, QImage::Format_RGB32);
  image.fill(Qt::black);
  widget.render(&image);
  return image;
}

// True when any pixel of the character rectangle at (row, column) is neither
// the background colour nor black.
bool cellHasForeground(const QImage& image, int row, int column,
                       QRgb background) {
  for (int y = row * 20; y < (row + 1) * 20; ++y) {
    for (int x = column * 12; x < (column + 1) * 12; ++x) {
      const QRgb pixel = image.pixel(x, y);
      if (pixel != background && pixel != qRgb(0, 0, 0)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

// EN 300 706 §12.2 code 0/D / BBC Broadcast Teletext 1976 §3.1.5: a double
// height character is stretched into the row below, whose own background is
// painted underneath it. A row-sequential paint erased that lower half.
TEST(TeletextPageWidgetTest, DoubleHeightFillsTheRowBelowItsBackground) {
  (void)ensureApplication();

  orc::presenters::TeletextPageView page;
  auto& origin = page.cells[1][0];
  origin.character = U'H';
  origin.double_height = true;
  origin.foreground = 7;  // white
  origin.background = 4;  // blue
  auto& lower = page.cells[2][0];
  lower.double_height_lower = true;
  lower.background = 4;  // local background copied from the origin row

  const QImage image = renderPage(page);
  const QRgb blue = qRgb(0, 0, 255);

  EXPECT_TRUE(cellHasForeground(image, 1, 0, blue));
  EXPECT_TRUE(cellHasForeground(image, 2, 0, blue))
      << "lower half of the double-height character was not drawn";
}

// The same applies to mosaics: "The characters and mosaics following a double
// height code are stretched into the following row" (§12.2 code 0/D).
TEST(TeletextPageWidgetTest, DoubleHeightMosaicFillsBothRows) {
  (void)ensureApplication();

  orc::presenters::TeletextPageView page;
  auto& origin = page.cells[1][0];
  origin.mosaic = true;
  origin.mosaic_pattern = 0x15;  // left column of sixels
  origin.double_height = true;
  origin.foreground = 6;  // cyan
  origin.background = 4;  // blue, as the Ceefax logo pages use
  auto& lower = page.cells[2][0];
  lower.double_height_lower = true;
  lower.background = 4;

  const QImage image = renderPage(page);
  const QRgb blue = qRgb(0, 0, 255);

  EXPECT_TRUE(cellHasForeground(image, 1, 0, blue));
  EXPECT_TRUE(cellHasForeground(image, 2, 0, blue))
      << "lower half of the double-height mosaic was not drawn";
}

// Double height stretches vertically only; the character keeps its own
// single-width rectangle and must not bleed into its neighbour.
TEST(TeletextPageWidgetTest, DoubleHeightDoesNotWiden) {
  (void)ensureApplication();

  orc::presenters::TeletextPageView page;
  auto& origin = page.cells[1][0];
  origin.character = U'W';
  origin.double_height = true;
  origin.foreground = 7;
  page.cells[2][0].double_height_lower = true;

  const QImage image = renderPage(page);
  const QRgb black = qRgb(0, 0, 0);

  ASSERT_TRUE(cellHasForeground(image, 1, 0, black));
  EXPECT_FALSE(cellHasForeground(image, 1, 1, black))
      << "double-height glyph overflowed into the next column";
}

// EN 300 706 §12.2 code 1/A: each block of a separated mosaic is "surrounded
// by a border of the background colour" — on all four sides, per Fig. 8 of
// the BBC 1976 specification.
TEST(TeletextPageWidgetTest, SeparatedMosaicLeavesABorderOnEverySide) {
  (void)ensureApplication();

  orc::presenters::TeletextPageView page;
  auto& cell = page.cells[1][0];
  cell.mosaic = true;
  cell.mosaic_separated = true;
  cell.mosaic_pattern = 0x3F;  // every sixel set
  cell.foreground = 7;         // white

  const QImage image = renderPage(page);
  const QRgb black = qRgb(0, 0, 0);

  // Top-left corner of the character rectangle must stay background.
  EXPECT_EQ(image.pixel(0, 20), black);
  // As must the bottom-right.
  EXPECT_EQ(image.pixel(11, 39), black);
  // But the middle of the top-left block is foreground.
  EXPECT_NE(image.pixel(3, 23), black);
}

// EN 300 706 §12.2 codes 0/A-0/B: on newsflash (C5) and subtitle (C6) pages
// only the boxed area is displayed.
TEST(TeletextPageWidgetTest, SubtitlePagesDisplayOnlyTheBoxedArea) {
  (void)ensureApplication();

  orc::presenters::TeletextPageView page;
  page.subtitle = true;
  auto& boxed = page.cells[1][0];
  boxed.character = U'A';
  boxed.foreground = 7;
  boxed.boxed = true;
  auto& unboxed = page.cells[1][1];
  unboxed.character = U'B';
  unboxed.foreground = 7;
  unboxed.background = 4;  // would otherwise paint a blue cell

  const QImage image = renderPage(page);
  const QRgb black = qRgb(0, 0, 0);

  EXPECT_TRUE(cellHasForeground(image, 1, 0, black));
  EXPECT_FALSE(cellHasForeground(image, 1, 1, black));
  EXPECT_EQ(image.pixel(18, 30), black) << "unboxed background was painted";
}

// EN 300 706 §8.1 / recovery reporting: a row that never arrived and a
// parity-damaged byte both render as blank, so the overlay is the only way to
// tell a recovery gap from page content.
TEST(TeletextPageWidgetTest, DataErrorOverlayMarksRowsThatNeverArrived) {
  (void)ensureApplication();

  orc::presenters::TeletextPageView page;
  page.row_received.fill(true);
  page.row_received[5] = false;

  TeletextPageWidget widget;
  widget.setPage(page);
  const QSize size(orc::presenters::TeletextPageView::kColumns * 12,
                   orc::presenters::TeletextPageView::kRows * 20);
  widget.resize(size);

  const auto render = [&] {
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::black);
    widget.render(&image);
    return image;
  };

  // Off by default: the page renders as transmitted.
  EXPECT_FALSE(widget.showDataErrors());
  EXPECT_FALSE(cellHasForeground(render(), 5, 0, qRgb(0, 0, 0)));

  widget.setShowDataErrors(true);
  const QImage marked = render();
  EXPECT_TRUE(cellHasForeground(marked, 5, 0, qRgb(0, 0, 0)))
      << "row that carried no packet was not marked";
  EXPECT_FALSE(cellHasForeground(marked, 4, 0, qRgb(0, 0, 0)))
      << "a row that did arrive must not be marked";
}

TEST(TeletextDialogTest, RecoveryReadoutReportsRowsAndDamagedBytes) {
  (void)ensureApplication();

  TeletextDialog dialog;
  auto* recovery = dialog.findChild<QLabel*>("teletextRecoveryLabel");
  ASSERT_NE(recovery, nullptr);

  dialog.show();
  QCoreApplication::processEvents();
  // Nothing selected yet: no readout to make sense of.
  EXPECT_TRUE(dialog.recoveryText().isEmpty());

  deliverPage100Window(dialog);
  ASSERT_TRUE(dialog.currentPage().has_value());

  // The fixture transmits the header plus row 1 only.
  EXPECT_EQ(dialog.currentPage()->recovery.rows_received, 1);
  EXPECT_EQ(dialog.currentPage()->recovery.rows_expected, 24);
  EXPECT_EQ(dialog.recoveryText(), QStringLiteral("rows 1/24"));

  // A page that was never seen has nothing to report.
  dialog.setPageNumberText("777");
  EXPECT_TRUE(dialog.recoveryText().isEmpty());
}

TEST(TeletextDialogTest, ShowDataErrorsCheckDrivesThePageWidget) {
  (void)ensureApplication();

  TeletextDialog dialog;
  auto* check = dialog.findChild<QCheckBox*>("teletextShowErrorsCheck");
  auto* page_widget = dialog.findChild<TeletextPageWidget*>();
  ASSERT_NE(check, nullptr);
  ASSERT_NE(page_widget, nullptr);

  EXPECT_FALSE(check->isChecked());
  EXPECT_FALSE(page_widget->showDataErrors());

  check->setChecked(true);
  EXPECT_TRUE(page_widget->showDataErrors());

  check->setChecked(false);
  EXPECT_FALSE(page_widget->showDataErrors());
}

}  // namespace gui_unit_test
