/*
 * File:        teletext_dialog_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 3 offscreen tests for the teletext analysis page viewer
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

#include "support/teletext_page_fixtures.h"
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

// One trigger run's worth of results: page 100 seen once early in the range.
orc::presenters::TeletextAnalysisView makePage100Catalogue() {
  orc::presenters::TeletextAnalysisView view;
  view.pages.push_back(makeCataloguedPage(1, 0x00, "HELLO TELETEXT",
                                          /*first_seen_frame=*/1,
                                          /*last_seen_frame=*/1));
  view.summary.frames_analysed = 12;
  view.summary.fields_with_data = 2;
  view.summary.packets_recovered = 4;
  return view;
}

// Page 100 and the subtitle page the service declared with C6.
orc::presenters::TeletextAnalysisView makeTwoPageCatalogue() {
  orc::presenters::TeletextAnalysisView view = makePage100Catalogue();
  view.pages.push_back(makeCataloguedPage(8, 0x88, "SUBTITLE TEXT",
                                          /*first_seen_frame=*/2,
                                          /*last_seen_frame=*/9,
                                          /*times_seen=*/3));
  return view;
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

  // The stage decode runs on the coordinator's worker thread; until it
  // delivers, the viewer says so rather than showing an empty page list as if
  // it were the answer.
  dialog.showPending();
  QCoreApplication::processEvents();
  EXPECT_TRUE(status->isVisible());

  dialog.setAnalysisData(makePage100Catalogue());
  QCoreApplication::processEvents();

  EXPECT_FALSE(status->isVisible());

  // Cell accuracy is asserted on the page-view model, not on pixels.
  ASSERT_TRUE(dialog.currentPage().has_value());
  EXPECT_EQ(dialog.currentPage()->magazine, 1);
  EXPECT_EQ(dialog.currentPage()->page_number, 0x00);
  EXPECT_EQ(rowText(*dialog.currentPage(), 1), "HELLO TELETEXT");

  auto* seen = dialog.findChild<QLabel*>("teletextSeenLabel");
  ASSERT_NE(seen, nullptr);
  // 1-based frame numbering in the UI.
  EXPECT_EQ(seen->text(), QString("Page 100 seen 1 time(s), frames 2-2"));

  auto* page_widget = dialog.findChild<TeletextPageWidget*>();
  ASSERT_NE(page_widget, nullptr);
  EXPECT_TRUE(page_widget->hasPage());
}

TEST(TeletextDialogTest, PageNumberEntry_SelectsAndClearsPage) {
  (void)ensureApplication();

  TeletextDialog dialog;
  dialog.setAnalysisData(makePage100Catalogue());

  ASSERT_TRUE(dialog.currentPage().has_value());

  // A page the range did not carry clears the display.
  dialog.setPageNumberText("888");
  EXPECT_FALSE(dialog.currentPage().has_value());

  // Returning to a catalogued page re-renders it.
  dialog.setPageNumberText("100");
  ASSERT_TRUE(dialog.currentPage().has_value());
  EXPECT_EQ(rowText(*dialog.currentPage(), 1), "HELLO TELETEXT");
}

TEST(TeletextDialogTest, InvalidPageNumber_ShowsNotice) {
  (void)ensureApplication();

  TeletextDialog dialog;
  dialog.setAnalysisData(makePage100Catalogue());

  dialog.setPageNumberText("9x");

  EXPECT_FALSE(dialog.currentPage().has_value());
  auto* seen = dialog.findChild<QLabel*>("teletextSeenLabel");
  ASSERT_NE(seen, nullptr);
  EXPECT_TRUE(seen->text().contains("Invalid"));
}

TEST(TeletextDialogTest, CataloguedPagesAreTabulatedInPageOrder) {
  (void)ensureApplication();

  TeletextDialog dialog;
  dialog.setAnalysisData(makeTwoPageCatalogue());

  const auto listed = dialog.listedPages();
  ASSERT_EQ(listed.size(), 2u);
  EXPECT_EQ(listed[0], QString("100"));
  EXPECT_EQ(listed[1], QString("888"));

  auto* table = dialog.findChild<QTableWidget*>("teletextPagesTable");
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->columnCount(), 3);
  EXPECT_EQ(table->item(0, 0)->text(), QString("100"));
  // Transmissions counted, and the frames the page was first and last seen at
  // (1-based). A page seen at one frame only shows that frame.
  EXPECT_EQ(table->item(0, 1)->text(), QString("1"));
  EXPECT_EQ(table->item(0, 2)->text(), QString("2"));
  EXPECT_EQ(table->item(1, 1)->text(), QString("3"));
  EXPECT_EQ(table->item(1, 2)->text(), QString("3-10"));
  // The rendered page is selected in the table.
  EXPECT_EQ(table->currentRow(), 0);
}

TEST(TeletextDialogTest, SelectingATabulatedPageRendersIt) {
  (void)ensureApplication();

  TeletextDialog dialog;
  dialog.setAnalysisData(makeTwoPageCatalogue());

  auto* table = dialog.findChild<QTableWidget*>("teletextPagesTable");
  ASSERT_NE(table, nullptr);
  table->setCurrentCell(1, 0);

  EXPECT_EQ(dialog.pageNumberText(), QString("888"));
  ASSERT_TRUE(dialog.currentPage().has_value());
  EXPECT_EQ(dialog.currentPage()->magazine, 8);
  EXPECT_EQ(dialog.currentPage()->page_number, 0x88);
  EXPECT_EQ(rowText(*dialog.currentPage(), 1), "SUBTITLE TEXT");
}

// A carousel repeats its pages, so how often one came round over the analysed
// range is what tells the user whether it can be recovered reliably here.
TEST(TeletextDialogTest, TimesSeenIsTabulated) {
  (void)ensureApplication();

  TeletextDialog dialog;
  dialog.setAnalysisData(makeTwoPageCatalogue());

  EXPECT_EQ(dialog.listedSeenCount("100"), 1u);
  EXPECT_EQ(dialog.listedSeenCount("888"), 3u);
}

// Which page carries the subtitles is a property of the recording: 888 is the
// broadcast convention, but the LaserDisc samples this was developed against
// use 190. The service declares it with C6, so the viewer says so outright
// rather than leaving the reader to guess a page number.
TEST(TeletextDialogTest, SubtitlePageIsAnnouncedAndMarkedInTheList) {
  (void)ensureApplication();

  TeletextDialog dialog;
  // Shown, because the notice reports itself through its visibility and a
  // child of a hidden window is never visible.
  dialog.show();
  EXPECT_TRUE(dialog.subtitleHintText().isEmpty());

  orc::presenters::TeletextAnalysisView view = makePage100Catalogue();
  view.pages.push_back(makeCataloguedPage(1, 0x90, "SUBTITLE TEXT",
                                          /*first_seen_frame=*/2,
                                          /*last_seen_frame=*/40,
                                          /*times_seen=*/6,
                                          /*subtitle=*/true));
  dialog.setAnalysisData(view);

  EXPECT_TRUE(dialog.subtitleHintText().contains("190"));
  EXPECT_FALSE(dialog.subtitleHintText().contains("100"));

  // The row keeps its plain page label for selection, and says what it is.
  const auto listed = dialog.listedPages();
  ASSERT_EQ(listed.size(), 2u);
  EXPECT_EQ(listed[1], QString("190"));
  auto* table = dialog.findChild<QTableWidget*>("teletextPagesTable");
  ASSERT_NE(table, nullptr);
  EXPECT_TRUE(table->item(1, 0)->text().contains("subs"));
  EXPECT_FALSE(table->item(0, 0)->text().contains("subs"));

  // Clearing the viewer takes the notice away with the catalogue.
  dialog.clearContent();
  EXPECT_TRUE(dialog.subtitleHintText().isEmpty());
}

// EN 300 706 §9.3.1.1: a page number containing A-F cannot be selected on a
// receiver. Such pages stay listed — they are real recovered data — but sort
// below the selectable ones and are greyed.
TEST(TeletextDialogTest, NonSelectablePagesSortLastAndAreGreyed) {
  (void)ensureApplication();

  TeletextDialog dialog;
  orc::presenters::TeletextAnalysisView view;
  view.pages.push_back(makeCataloguedPage(1, 0xAF, ""));
  view.pages.push_back(makeCataloguedPage(8, 0x88, ""));
  dialog.setAnalysisData(view);

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

// Re-triggering the node hands the viewer a fresh catalogue; nothing of the
// previous run may survive into it.
TEST(TeletextDialogTest, NewDataReplacesThePreviousCatalogue) {
  (void)ensureApplication();

  TeletextDialog dialog;
  dialog.setAnalysisData(makeTwoPageCatalogue());
  ASSERT_EQ(dialog.listedPages().size(), 2u);

  dialog.setAnalysisData(makePage100Catalogue());

  const auto listed = dialog.listedPages();
  ASSERT_EQ(listed.size(), 1u);
  EXPECT_EQ(listed[0], QString("100"));
}

TEST(TeletextDialogTest, ClearContentResetsTheDisplay) {
  (void)ensureApplication();

  TeletextDialog dialog;
  dialog.setAnalysisData(makePage100Catalogue());
  ASSERT_TRUE(dialog.currentPage().has_value());

  dialog.clearContent();

  EXPECT_FALSE(dialog.currentPage().has_value());
  EXPECT_TRUE(dialog.listedPages().empty());
  EXPECT_TRUE(dialog.summaryText().isEmpty());
}

// A range that yielded no page at all is a different answer from one whose
// pages simply do not include the one being asked for, and the summary is
// what tells them apart.
TEST(TeletextDialogTest, EmptyCatalogueReportsTheRunRatherThanNothing) {
  (void)ensureApplication();

  TeletextDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();

  orc::presenters::TeletextAnalysisView view;
  view.summary.frames_analysed = 500;
  dialog.setAnalysisData(view);

  EXPECT_TRUE(dialog.listedPages().empty());
  EXPECT_FALSE(dialog.currentPage().has_value());
  auto* seen = dialog.findChild<QLabel*>("teletextSeenLabel");
  ASSERT_NE(seen, nullptr);
  EXPECT_TRUE(seen->text().contains("No teletext pages"));
  EXPECT_TRUE(dialog.summaryText().contains("500 frames"));
}

TEST(TeletextDialogTest, SummaryReportsTheRunsRecoveryFigures) {
  (void)ensureApplication();

  TeletextDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();

  orc::presenters::TeletextAnalysisView view = makePage100Catalogue();
  view.summary.packets_recovered = 1200;
  view.summary.fields_with_data = 600;
  view.summary.frames_analysed = 400;
  view.summary.characters_written = 48000;
  view.summary.characters_damaged = 12;
  view.summary.lost_packets_estimate = 3;
  view.summary.pages_truncated = true;
  dialog.setAnalysisData(view);

  const QString summary = dialog.summaryText();
  EXPECT_TRUE(summary.contains("1200 packets"));
  EXPECT_TRUE(summary.contains("600 fields"));
  EXPECT_TRUE(summary.contains("12 of 48000 characters"));
  EXPECT_TRUE(summary.contains("about 3 packets lost"));
  EXPECT_TRUE(summary.contains("truncated"));
}

TEST(TeletextDialogTest, RecoveryReadoutReportsWhatArrived) {
  (void)ensureApplication();

  TeletextDialog dialog;
  auto* recovery = dialog.findChild<QLabel*>("teletextRecoveryLabel");
  ASSERT_NE(recovery, nullptr);

  dialog.show();
  QCoreApplication::processEvents();
  // Nothing selected yet: no readout to make sense of.
  EXPECT_TRUE(dialog.recoveryText().isEmpty());

  dialog.setAnalysisData(makePage100Catalogue());
  ASSERT_TRUE(dialog.currentPage().has_value());

  // The fixture page carries the header plus row 1 only. The 23 rows the
  // service chose not to send are not a shortfall, so the readout counts what
  // arrived rather than presenting a fraction of the grid.
  EXPECT_EQ(dialog.currentPage()->recovery.rows_received, 1);
  EXPECT_EQ(dialog.currentPage()->recovery.lost_packets, 0);
  EXPECT_EQ(dialog.recoveryText(), QStringLiteral("Complete (1 row(s))"));

  // A page the range never carried has nothing to report.
  dialog.setPageNumberText("777");
  EXPECT_TRUE(dialog.recoveryText().isEmpty());
}

// A page whose last transmission was still arriving when the range ran out
// looks exactly like a finished one with rows missing, so the readout has to
// say which it is.
TEST(TeletextDialogTest, RecoveryReadoutDistinguishesArrivingFromComplete) {
  (void)ensureApplication();

  TeletextDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();

  orc::presenters::TeletextAnalysisView view;
  auto entry = makeCataloguedPage(1, 0x00, "ROW ONE");
  entry.page.transmission_complete = false;
  view.pages.push_back(entry);
  dialog.setAnalysisData(view);

  EXPECT_EQ(dialog.recoveryText(),
            QStringLiteral("Partial - still arriving (1 row(s) so far)"));

  view.pages[0].page.transmission_complete = true;
  view.pages[0].page.recovery.damaged_bytes = 2;
  dialog.setAnalysisData(view);

  EXPECT_EQ(dialog.recoveryText(),
            QStringLiteral("1 row(s), 2 damaged byte(s)"));
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
// A page is free to leave rows out, and nearly every real one does — the
// blank lines that space it out are simply not transmitted. Banding those as
// errors put several marks on a page that had arrived perfectly.
TEST(TeletextPageWidgetTest, DataErrorOverlayIgnoresRowsThatWereNeverSent) {
  (void)ensureApplication();

  orc::presenters::TeletextPageView page;
  page.row_received.fill(true);
  page.row_received[5] = false;
  page.recovery.lost_packets = 0;  // every VBI slot gave up its packet

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

  widget.setShowDataErrors(true);
  EXPECT_FALSE(cellHasForeground(render(), 5, 0, qRgb(0, 0, 0)))
      << "a row the service never sent must not be marked as an error";
}

// When packets really were lost the page cannot say which row each would have
// carried, so every row still missing becomes a candidate and is banded.
TEST(TeletextPageWidgetTest, DataErrorOverlayMarksMissingRowsWhenPacketsLost) {
  (void)ensureApplication();

  orc::presenters::TeletextPageView page;
  page.row_received.fill(true);
  page.row_received[5] = false;
  page.recovery.lost_packets = 1;

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

// A row the carousel never got to check is not damage, so it is marked apart
// from the lost-packet banding and only when the reader has asked to see how
// the page was recovered.
TEST(TeletextPageWidgetTest, DataErrorOverlayMarksRowsSeenOnlyOnce) {
  (void)ensureApplication();

  orc::presenters::TeletextPageView page;
  page.row_received.fill(true);
  page.row_unconfirmed[7] = true;
  page.recovery.unconfirmed_rows = 1;

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

  EXPECT_FALSE(cellHasForeground(render(), 7, 0, qRgb(0, 0, 0)));

  widget.setShowDataErrors(true);
  const QImage marked = render();
  EXPECT_TRUE(cellHasForeground(marked, 7, 0, qRgb(0, 0, 0)))
      << "row resting on a single copy was not marked";
  EXPECT_FALSE(cellHasForeground(marked, 6, 0, qRgb(0, 0, 0)))
      << "a row confirmed by a repeat must not be marked";
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
