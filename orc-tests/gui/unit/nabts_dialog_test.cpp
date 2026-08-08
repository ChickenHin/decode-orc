/*
 * File:        nabts_dialog_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Offscreen smoke tests for the NABTS record viewer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QPixmap>
#include <string>
#include <vector>

#include "nabtscanvaswidget.h"
#include "nabtsdialog.h"

namespace gui_unit_test {
namespace {

using orc::presenters::NabtsAnalysisView;
using orc::presenters::NabtsCaptionCueView;
using orc::presenters::NabtsCatalogueRecordView;
using orc::presenters::NabtsColourView;
using orc::presenters::NabtsPointView;
using orc::presenters::NabtsPrimitiveKindView;
using orc::presenters::NabtsPrimitiveView;
using orc::presenters::NabtsRecordFunctionView;
using orc::presenters::NabtsSizeView;

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

NabtsPrimitiveView text_run(const std::string& text, double x, double y) {
  NabtsPrimitiveView primitive;
  primitive.kind = NabtsPrimitiveKindView::kText;
  primitive.text = text;
  primitive.character_count = static_cast<int>(text.size());
  primitive.origin = NabtsPointView{x, y};
  primitive.size = NabtsSizeView{1.0 / 40.0, 1.0 / 20.0};
  primitive.advance = NabtsSizeView{1.0 / 40.0, 0.0};
  primitive.colour = NabtsColourView{255, 255, 255, false};
  primitive.points.push_back(primitive.origin);
  return primitive;
}

// One presentation record, drawn as a single line of text.
NabtsCatalogueRecordView presentation_record(const std::string& address,
                                             const std::string& text,
                                             uint8_t version = 0) {
  NabtsCatalogueRecordView record;
  record.channel = 0x000;
  record.address_text = address;
  record.channel_text = "000/" + address;
  record.record_type = 0;
  record.record_type_name = "Cyclic presentation";
  record.presentation = true;
  record.version = version;
  record.first_seen_frame = 10;
  record.last_seen_frame = 400;
  record.times_seen = 6;
  record.times_intact = 6;
  record.records_in_message = 1;
  record.complete = true;
  record.data_bytes = text.size();
  record.page.primitives.push_back(text_run(text, 0.05, 0.5));
  record.page.text = text;
  return record;
}

NabtsAnalysisView makeCatalogue() {
  NabtsAnalysisView view;
  view.records.push_back(presentation_record("100", "FIRST"));
  view.records.push_back(presentation_record("101", "SECOND"));
  view.records.push_back(presentation_record("102", "THIRD"));
  view.summary.frames_analysed = 500;
  view.summary.fields_with_data = 900;
  view.summary.packets_recovered = 1800;
  view.summary.groups_completed = 40;
  view.summary.messages_complete = 3;
  return view;
}

// Force a paint pass without a window server, which is what proves the canvas
// walks the display list rather than only holding it.
void renderOffscreen(QWidget* widget, int width = 400, int height = 320) {
  widget->resize(width, height);
  QPixmap target(width, height);
  widget->render(&target);
}

}  // namespace

TEST(NabtsDialogTest, OpensEmptyWithNoRecordData) {
  ensureApplication();
  NabtsDialog dialog;

  EXPECT_TRUE(dialog.listedRecords().empty());
  EXPECT_EQ(dialog.currentRecordIndex(), -1);
  EXPECT_EQ(dialog.currentRecord(), nullptr);
  EXPECT_TRUE(dialog.summaryText().isEmpty());
  EXPECT_TRUE(dialog.captionHintText().isEmpty());
}

TEST(NabtsDialogTest, ListsTheCatalogueAndShowsTheFirstRecord) {
  ensureApplication();
  NabtsDialog dialog;
  dialog.setAnalysisData(makeCatalogue());

  const auto listed = dialog.listedRecords();
  ASSERT_EQ(listed.size(), 3u);
  // §5.2.1's identity: channel, record address and version together.
  EXPECT_EQ(listed[0], QStringLiteral("000/100 V0"));
  EXPECT_EQ(listed[2], QStringLiteral("000/102 V0"));

  // A catalogue with records in it opens on one rather than on an empty pane.
  EXPECT_EQ(dialog.currentRecordIndex(), 0);
  ASSERT_NE(dialog.currentRecord(), nullptr);
  EXPECT_EQ(dialog.currentRecord()->address_text, "100");
  EXPECT_FALSE(dialog.summaryText().isEmpty());
  EXPECT_FALSE(dialog.detailText().isEmpty());
}

TEST(NabtsDialogTest, SelectingARecordShowsIt) {
  ensureApplication();
  NabtsDialog dialog;
  dialog.setAnalysisData(makeCatalogue());

  dialog.selectRecord(2);
  EXPECT_EQ(dialog.currentRecordIndex(), 2);
  ASSERT_NE(dialog.currentRecord(), nullptr);
  EXPECT_EQ(dialog.currentRecord()->page.text, "THIRD");
}

// §7.3 steps a service by ascending record address, which is the catalogue's
// own order, and the carousel wraps.
TEST(NabtsDialogTest, StepsThroughTheSeriesAndWrapsAtEitherEnd) {
  ensureApplication();
  NabtsDialog dialog;
  dialog.setAnalysisData(makeCatalogue());

  dialog.showNextRecord();
  EXPECT_EQ(dialog.currentRecordIndex(), 1);
  dialog.showNextRecord();
  EXPECT_EQ(dialog.currentRecordIndex(), 2);
  dialog.showNextRecord();
  EXPECT_EQ(dialog.currentRecordIndex(), 0) << "stepping past the end wraps";

  dialog.showPreviousRecord();
  EXPECT_EQ(dialog.currentRecordIndex(), 2) << "and so does stepping back";
}

// §5.2.2.4: an application record is a list of function descriptors, not
// something drawable, so the viewer lists them instead of drawing nothing.
TEST(NabtsDialogTest, ShowsAnApplicationRecordAsItsFunctionDescriptors) {
  ensureApplication();
  NabtsAnalysisView view;
  NabtsCatalogueRecordView record;
  record.channel = 0x000;
  record.address_text = "FFE";
  record.channel_text = "000/FFE";
  record.record_type = 2;
  record.record_type_name = "Application";
  record.presentation = false;
  record.complete = true;
  record.times_seen = 2;
  NabtsRecordFunctionView function;
  function.code = "2/1";
  function.control = true;
  function.arguments = "ABC";
  record.functions.push_back(function);
  view.records.push_back(record);

  NabtsDialog dialog;
  dialog.setAnalysisData(view);
  ASSERT_NE(dialog.currentRecord(), nullptr);
  EXPECT_FALSE(dialog.currentRecord()->presentation);
  EXPECT_EQ(dialog.currentRecord()->functions.size(), 1u);
}

TEST(NabtsDialogTest, ClearContentEmptiesEverything) {
  ensureApplication();
  NabtsDialog dialog;
  dialog.setAnalysisData(makeCatalogue());
  ASSERT_FALSE(dialog.listedRecords().empty());

  dialog.clearContent();
  EXPECT_TRUE(dialog.listedRecords().empty());
  EXPECT_EQ(dialog.currentRecordIndex(), -1);
  EXPECT_TRUE(dialog.summaryText().isEmpty());
  EXPECT_TRUE(dialog.listedCaptions().empty());
}

TEST(NabtsDialogTest, ShowPendingThenDataClearsThePendingNotice) {
  ensureApplication();
  NabtsDialog dialog;
  dialog.showPending();
  dialog.setAnalysisData(makeCatalogue());
  // The pending notice shares the status bar with the summary; the summary
  // arriving is what says the run finished.
  EXPECT_FALSE(dialog.summaryText().isEmpty());
}

// ---------------------------------------------------------------------------
// The caption track (CEA-516 §7.3.10)
// ---------------------------------------------------------------------------

TEST(NabtsDialogTest, ShowsNoCaptionTrackOnARecordingWithoutCaptioning) {
  ensureApplication();
  NabtsDialog dialog;
  dialog.setAnalysisData(makeCatalogue());

  EXPECT_TRUE(dialog.captionHintText().isEmpty());
  EXPECT_TRUE(dialog.listedCaptions().empty());
}

TEST(NabtsDialogTest, ListsTheCaptionCuesWhenTheRecordingCarriedThem) {
  ensureApplication();
  NabtsAnalysisView view = makeCatalogue();
  for (int i = 0; i < 2; ++i) {
    NabtsCaptionCueView cue;
    cue.channel = 0xA00;
    cue.address_text = "000";
    cue.version = static_cast<uint8_t>(i + 1);
    cue.start_frame = 100 + static_cast<uint64_t>(i) * 100;
    cue.end_frame = cue.start_frame + 100;
    cue.text = i == 0 ? "FIRST CAPTION" : "SECOND CAPTION";
    view.captions.push_back(cue);
  }

  NabtsDialog dialog;
  dialog.setAnalysisData(view);

  const auto cues = dialog.listedCaptions();
  ASSERT_EQ(cues.size(), 2u);
  EXPECT_EQ(cues[0], QStringLiteral("FIRST CAPTION"));
  EXPECT_EQ(cues[1], QStringLiteral("SECOND CAPTION"));
  // The hint names the channel the captions arrived on, which is a property of
  // the recording rather than a constant.
  EXPECT_TRUE(dialog.captionHintText().contains(QStringLiteral("A00/000")));
}

// ---------------------------------------------------------------------------
// The canvas
// ---------------------------------------------------------------------------

TEST(NabtsCanvasWidgetTest, PaintsNothingUntilItHasARecord) {
  ensureApplication();
  NabtsCanvasWidget canvas;
  EXPECT_FALSE(canvas.hasPage());
  renderOffscreen(&canvas);
  EXPECT_EQ(canvas.primitivesPainted(), 0);
}

TEST(NabtsCanvasWidgetTest, PaintsEveryPrimitiveOfTheDisplayList) {
  ensureApplication();
  orc::presenters::NabtsPageView page;
  page.primitives.push_back(text_run("HELLO", 0.05, 0.5));

  NabtsPrimitiveView line;
  line.kind = NabtsPrimitiveKindView::kLine;
  line.points = {NabtsPointView{0.0, 0.0}, NabtsPointView{0.9, 0.7}};
  line.colour = NabtsColourView{255, 0, 0, false};
  page.primitives.push_back(line);

  NabtsPrimitiveView box;
  box.kind = NabtsPrimitiveKindView::kRectangle;
  box.origin = NabtsPointView{0.1, 0.1};
  box.size = NabtsSizeView{0.3, 0.2};
  box.filled = true;
  box.colour = NabtsColourView{0, 0, 255, false};
  page.primitives.push_back(box);

  NabtsPrimitiveView mosaic;
  mosaic.kind = NabtsPrimitiveKindView::kMosaic;
  mosaic.origin = NabtsPointView{0.5, 0.3};
  mosaic.size = NabtsSizeView{1.0 / 40.0, 1.0 / 20.0};
  mosaic.mosaic_pattern = 0x2A;
  mosaic.colour = NabtsColourView{0, 255, 0, false};
  page.primitives.push_back(mosaic);

  NabtsCanvasWidget canvas;
  canvas.setPage(page);
  EXPECT_TRUE(canvas.hasPage());
  renderOffscreen(&canvas);
  EXPECT_EQ(canvas.primitivesPainted(), 4);
}

// A record drawn into a corner is otherwise hard to tell from one that was
// mis-scaled, so the display-area outline is available on request.
TEST(NabtsCanvasWidgetTest, DrawsTheDisplayAreaOutlineOnRequest) {
  ensureApplication();
  orc::presenters::NabtsPageView page;
  page.primitives.push_back(text_run("X", 0.0, 0.0));

  NabtsCanvasWidget canvas;
  canvas.setPage(page);
  EXPECT_FALSE(canvas.showDataErrors());
  canvas.setShowDataErrors(true);
  EXPECT_TRUE(canvas.showDataErrors());
  renderOffscreen(&canvas);
  EXPECT_EQ(canvas.primitivesPainted(), 1);
}

// A DRCS character the record never defined indexes nothing, and painting it
// must not reach past the glyph list (§5.6 shows it as SPACE).
TEST(NabtsCanvasWidgetTest, PaintsAnUndefinedDrcsCharacterAsNothing) {
  ensureApplication();
  orc::presenters::NabtsPageView page;
  NabtsPrimitiveView drcs;
  drcs.kind = NabtsPrimitiveKindView::kDrcs;
  drcs.origin = NabtsPointView{0.2, 0.2};
  drcs.size = NabtsSizeView{1.0 / 40.0, 1.0 / 20.0};
  drcs.drcs_index = -1;
  drcs.colour = NabtsColourView{255, 255, 255, false};
  page.primitives.push_back(drcs);

  NabtsCanvasWidget canvas;
  canvas.setPage(page);
  renderOffscreen(&canvas);
  EXPECT_EQ(canvas.primitivesPainted(), 1);
}

TEST(NabtsCanvasWidgetTest, ClearPageForgetsTheRecord) {
  ensureApplication();
  orc::presenters::NabtsPageView page;
  page.primitives.push_back(text_run("HELLO", 0.05, 0.5));

  NabtsCanvasWidget canvas;
  canvas.setPage(page);
  renderOffscreen(&canvas);
  ASSERT_EQ(canvas.primitivesPainted(), 1);

  canvas.clearPage();
  EXPECT_FALSE(canvas.hasPage());
  EXPECT_EQ(canvas.page(), nullptr);
  renderOffscreen(&canvas);
  EXPECT_EQ(canvas.primitivesPainted(), 0);
}

}  // namespace gui_unit_test
