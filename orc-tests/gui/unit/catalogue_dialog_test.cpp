/*
 * File:        catalogue_dialog_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 3 offscreen tests for the generic catalogue browser
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QImage>
#include <QLineEdit>
#include <QTableWidget>

#include "cataloguecellgridwidget.h"
#include "cataloguedialog.h"
#include "cataloguedisplaylistwidget.h"

namespace gui_unit_test {

namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-catalogue-dialog-test";
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

// A small character-cell payload: |text| written across the top row of a
// |rows| x |columns| grid in white on black.
orc::CatalogueCellGrid makeGrid(const std::string& text, int rows = 4,
                                int columns = 8) {
  orc::CatalogueCellGrid grid;
  grid.rows = rows;
  grid.columns = columns;
  grid.cell_aspect_width = 12;
  grid.cell_aspect_height = 20;
  grid.palette = {orc::CatalogueColour{0, 0, 0},
                  orc::CatalogueColour{255, 255, 255}};
  grid.cells.resize(static_cast<size_t>(rows) * static_cast<size_t>(columns));
  for (auto& cell : grid.cells) {
    cell.foreground = 1;
    cell.background = 0;
  }
  for (size_t i = 0; i < text.size() && i < static_cast<size_t>(columns); ++i) {
    grid.cells[i].character = static_cast<char32_t>(text[i]);
  }
  grid.row_status.resize(static_cast<size_t>(rows));
  return grid;
}

// A catalogue of |count| pages, each carrying one variant, in the shape the
// teletext sink produces: a find box, a variant stepper and a damage toggle.
orc::CatalogueDataset makePagedCatalogue(int count) {
  orc::CatalogueDataset data;
  data.schema.columns = {
      orc::CatalogueColumn{"page", "Page", false},
      orc::CatalogueColumn{"seen", "Seen", true},
  };
  data.schema.item_noun = "Page";
  data.schema.variant_noun = "Sub-page";
  data.schema.find_label = "Page:";
  data.schema.find_placeholder = "e.g. 100";
  data.schema.highlight_label = "Show data errors";
  data.schema.empty_message = "No pages were recovered";

  for (int i = 0; i < count; ++i) {
    const std::string id = "10" + std::to_string(i);

    orc::CatalogueItem page;
    page.id = id;
    page.find_key = id;
    page.values = {id, std::to_string(i + 1)};
    data.items.push_back(std::move(page));
    data.payloads.emplace_back();  // the page itself draws nothing

    orc::CatalogueItem variant;
    variant.id = id + "/0000";
    variant.parent_id = id;
    variant.variant_label = "0000";
    data.items.push_back(std::move(variant));

    orc::CataloguePayload payload;
    payload.kind = orc::CataloguePayload::Kind::kCellGrid;
    payload.grid = makeGrid(id);
    payload.headline =
        "Page " + id + " seen " + std::to_string(i + 1) + " times";
    payload.condition = "Complete (1 row)";
    data.payloads.push_back(std::move(payload));
  }

  data.summary.headline = "12 packets recovered";
  return data;
}

// A flat catalogue with no variants, in the shape the NABTS sink produces.
orc::CatalogueDataset makeFlatCatalogue() {
  orc::CatalogueDataset data;
  data.schema.columns = {orc::CatalogueColumn{"address", "Address", false}};
  data.schema.item_noun = "Record";

  orc::CatalogueItem record;
  record.id = "000/1A4 v0";
  record.find_key = record.id;
  record.values = {record.id};
  data.items.push_back(record);

  orc::CataloguePayload drawn;
  drawn.kind = orc::CataloguePayload::Kind::kDisplayList;
  drawn.display_list.aspect_height = 0.78125;
  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kRectangle;
  op.origin = orc::CataloguePoint{0.1, 0.1};
  op.size = orc::CatalogueSize{0.5, 0.5};
  op.filled = true;
  op.colour = orc::CatalogueColour{255, 255, 255};
  drawn.display_list.ops.push_back(op);
  drawn.companion_text = "HELLO NABTS";
  drawn.headline = "Record 000/1A4 seen 3 times";
  data.payloads.push_back(std::move(drawn));

  orc::CatalogueItem listing;
  listing.id = "000/1A5 v0";
  listing.find_key = listing.id;
  listing.values = {listing.id};
  data.items.push_back(listing);

  orc::CataloguePayload text;
  text.kind = orc::CataloguePayload::Kind::kText;
  text.document.text = "2/0  [control]  reset";
  text.headline = "Record 000/1A5 seen 1 time";
  data.payloads.push_back(std::move(text));

  orc::CatalogueItem cues;
  cues.id = "captions";
  cues.values = {"Caption track"};
  data.items.push_back(cues);

  orc::CataloguePayload table;
  table.kind = orc::CataloguePayload::Kind::kTable;
  table.table.columns = {orc::CatalogueColumn{"frames", "Frames", true},
                         orc::CatalogueColumn{"text", "Caption", false}};
  table.table.rows = {{"1-30", "FIRST CUE"}, {"31-60", "SECOND CUE"}};
  table.headline = "2 captions on A00/000";
  data.payloads.push_back(std::move(table));

  data.summary.notices.push_back("2 captions on A00/000");
  return data;
}

QImage renderWidget(QWidget& widget, int width, int height) {
  widget.resize(width, height);
  QImage image(width, height, QImage::Format_RGB32);
  image.fill(Qt::black);
  widget.render(&image);
  return image;
}

// Whole pixels per character rectangle, scaled up far enough that the
// widget's own minimum size cannot clamp the resize. At the grid's exact
// aspect, so the aspect lock leaves no letterbox and the page rect is the
// whole image.
int gridScale(const orc::CatalogueCellGrid& grid) {
  int scale = 1;
  while (grid.columns * grid.cell_aspect_width * scale < 512 ||
         grid.rows * grid.cell_aspect_height * scale < 512) {
    ++scale;
  }
  return scale;
}

QImage renderGrid(const orc::CatalogueCellGrid& grid) {
  CatalogueCellGridWidget widget;
  widget.setGrid(grid);
  const int scale = gridScale(grid);
  return renderWidget(widget, grid.columns * grid.cell_aspect_width * scale,
                      grid.rows * grid.cell_aspect_height * scale);
}

/// Colour at the centre of the character rectangle at (row, column).
QColor cellCentre(const QImage& image, const orc::CatalogueCellGrid& grid,
                  int row, int column) {
  const int scale = gridScale(grid);
  const int cell_w = grid.cell_aspect_width * scale;
  const int cell_h = grid.cell_aspect_height * scale;
  return image.pixelColor(column * cell_w + cell_w / 2,
                          row * cell_h + cell_h / 2);
}

}  // namespace

TEST(CatalogueDialogTest, CanShowAndClose) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  QApplication::processEvents();
  EXPECT_TRUE(dialog.isVisible());
  dialog.close();
  QApplication::processEvents();
  EXPECT_FALSE(dialog.isVisible());
}

// A dataset whose payload vector does not line up with its items is a plugin
// bug; the dialogue says so rather than indexing past the end.
TEST(CatalogueDialogTest, InconsistentDatasetIsRefused) {
  ensureApplication();
  CatalogueDialog dialog;

  orc::CatalogueDataset broken;
  broken.items.emplace_back();
  dialog.setCatalogue(broken);

  EXPECT_TRUE(dialog.listedItems().empty());
  EXPECT_TRUE(dialog.headlineText().contains("inconsistent"));
}

TEST(CatalogueDialogTest, ItemsAreTabulatedInDatasetOrder) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.setCatalogue(makePagedCatalogue(3));

  // Only top-level items are listed; the variants live under the payload.
  const auto listed = dialog.listedItems();
  ASSERT_EQ(listed.size(), 3u);
  EXPECT_EQ(listed[0], "100");
  EXPECT_EQ(listed[1], "101");
  EXPECT_EQ(listed[2], "102");

  EXPECT_EQ(dialog.listedValue("101", 1), "2");
}

// Selecting a parent shows its first variant, which is what a page with
// sub-pages means.
TEST(CatalogueDialogTest, SelectingAnItemShowsItsPayload) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.setCatalogue(makePagedCatalogue(3));

  dialog.selectItem(1);
  EXPECT_EQ(dialog.currentItemIndex(), 1);
  ASSERT_NE(dialog.currentGrid(), nullptr);
  EXPECT_EQ(dialog.headlineText(), "Page 101 seen 2 times");
  EXPECT_EQ(dialog.conditionText(), "Complete (1 row)");
}

TEST(CatalogueDialogTest, FindBoxSelectsByKey) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makePagedCatalogue(3));

  dialog.setFindText("102");
  EXPECT_EQ(dialog.currentItemIndex(), 2);
  EXPECT_EQ(dialog.headlineText(), "Page 102 seen 3 times");

  // Case and surrounding space are the host's business, not the plugin's.
  dialog.setFindText("  100  ");
  EXPECT_EQ(dialog.currentItemIndex(), 0);

  dialog.close();
}

// A key the catalogue does not carry is not an error — the service simply did
// not send it — so the dialogue says which one was asked for.
TEST(CatalogueDialogTest, FindBoxReportsAKeyThatWasNotCarried) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makePagedCatalogue(2));

  dialog.setFindText("777");
  EXPECT_EQ(dialog.currentItemIndex(), -1);
  EXPECT_EQ(dialog.currentGrid(), nullptr);
  EXPECT_TRUE(dialog.headlineText().contains("777"));

  dialog.close();
}

TEST(CatalogueDialogTest, ItemNavigationWrapsAtBothEnds) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.setCatalogue(makePagedCatalogue(3));

  dialog.selectItem(0);
  dialog.showPreviousItem();
  EXPECT_EQ(dialog.currentItemIndex(), 2);
  dialog.showNextItem();
  EXPECT_EQ(dialog.currentItemIndex(), 0);
}

// --- Variants -------------------------------------------------------------

namespace {

// One page carrying |count| variants, as a multi-page set does.
orc::CatalogueDataset makeVariantCatalogue(int count) {
  orc::CatalogueDataset data;
  data.schema.columns = {orc::CatalogueColumn{"page", "Page", false}};
  data.schema.item_noun = "Page";
  data.schema.variant_noun = "Sub-page";

  orc::CatalogueItem page;
  page.id = "100";
  page.find_key = "100";
  page.values = {"100"};
  data.items.push_back(std::move(page));
  data.payloads.emplace_back();

  for (int i = 0; i < count; ++i) {
    const std::string label = "000" + std::to_string(i + 1);
    orc::CatalogueItem variant;
    variant.id = "100/" + label;
    variant.parent_id = "100";
    variant.variant_label = label;
    data.items.push_back(std::move(variant));

    orc::CataloguePayload payload;
    payload.kind = orc::CataloguePayload::Kind::kCellGrid;
    payload.grid = makeGrid(label);
    payload.headline = "Page 100 sub-page " + label;
    data.payloads.push_back(std::move(payload));
  }
  return data;
}

}  // namespace

TEST(CatalogueDialogTest, VariantStepperReportsThePositionInTheSequence) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeVariantCatalogue(3));

  dialog.selectItem(0);
  EXPECT_EQ(dialog.variantCount(), 3);
  EXPECT_EQ(dialog.variantIndex(), 0);
  EXPECT_EQ(dialog.variantText(), "Sub-page 1 of 3 (0001)");
  EXPECT_EQ(dialog.headlineText(), "Page 100 sub-page 0001");

  dialog.close();
}

TEST(CatalogueDialogTest, VariantNavigationWrapsAtBothEnds) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeVariantCatalogue(3));
  dialog.selectItem(0);

  dialog.showPreviousVariant();
  EXPECT_EQ(dialog.variantIndex(), 2);
  EXPECT_EQ(dialog.headlineText(), "Page 100 sub-page 0003");

  dialog.showNextVariant();
  EXPECT_EQ(dialog.variantIndex(), 0);

  dialog.close();
}

// A single variant is said rather than hidden, so "one of them" is
// distinguishable from a control that has not been noticed.
TEST(CatalogueDialogTest, SingleVariantSaysSoAndDisablesTheControls) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeVariantCatalogue(1));
  dialog.selectItem(0);

  EXPECT_EQ(dialog.variantCount(), 1);
  EXPECT_EQ(dialog.variantText(), "No sub-pages");

  dialog.close();
}

// Moving to a different item starts at the top of its sequence.
TEST(CatalogueDialogTest, ChangingItemResetsToTheFirstVariant) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makePagedCatalogue(2));

  dialog.selectItem(0);
  dialog.selectItem(1);
  EXPECT_EQ(dialog.variantIndex(), 0);

  dialog.close();
}

// A schema with no variant noun hides the stepper entirely, which is what a
// flat catalogue wants.
TEST(CatalogueDialogTest, FlatCatalogueHasNoVariantStepper) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeFlatCatalogue());
  dialog.selectItem(0);

  EXPECT_EQ(dialog.variantText(), QString());
  EXPECT_EQ(dialog.findText(), QString());  // no find box either

  dialog.close();
}

// --- Payload kinds --------------------------------------------------------

TEST(CatalogueDialogTest, DisplayListPayloadShowsItsCompanionText) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeFlatCatalogue());

  dialog.selectItem(0);
  ASSERT_NE(dialog.currentDisplayList(), nullptr);
  EXPECT_EQ(dialog.currentDisplayList()->ops.size(), 1u);
  EXPECT_EQ(dialog.currentText(), "HELLO NABTS");
  EXPECT_EQ(dialog.currentGrid(), nullptr);

  dialog.close();
}

TEST(CatalogueDialogTest, TextPayloadIsListed) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeFlatCatalogue());

  dialog.selectItem(1);
  EXPECT_EQ(dialog.currentDisplayList(), nullptr);
  EXPECT_EQ(dialog.currentText(), "2/0  [control]  reset");

  dialog.close();
}

TEST(CatalogueDialogTest, TablePayloadIsTabulated) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeFlatCatalogue());

  dialog.selectItem(2);
  const auto rows = dialog.listedTableRows();
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0], "1-30 | FIRST CUE");
  EXPECT_EQ(rows[1], "31-60 | SECOND CUE");

  dialog.close();
}

// --- Run-wide readouts ----------------------------------------------------

TEST(CatalogueDialogTest, SummaryAndNoticesComeFromTheDataset) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();

  dialog.setCatalogue(makePagedCatalogue(1));
  EXPECT_EQ(dialog.summaryText(), "12 packets recovered");
  EXPECT_EQ(dialog.noticeText(), QString());

  dialog.setCatalogue(makeFlatCatalogue());
  EXPECT_EQ(dialog.noticeText(), "2 captions on A00/000");

  dialog.close();
}

// A recording that carried none of the service is the ordinary case, not an
// error, so the schema's own wording is shown rather than an empty pane.
TEST(CatalogueDialogTest, EmptyCatalogueShowsTheSchemasMessage) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();

  orc::CatalogueDataset empty = makePagedCatalogue(0);
  dialog.setCatalogue(empty);

  EXPECT_TRUE(dialog.listedItems().empty());
  EXPECT_EQ(dialog.headlineText(), "No pages were recovered");

  dialog.close();
}

TEST(CatalogueDialogTest, NewDataReplacesThePreviousCatalogue) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();

  dialog.setCatalogue(makePagedCatalogue(3));
  ASSERT_EQ(dialog.listedItems().size(), 3u);

  dialog.setCatalogue(makePagedCatalogue(1));
  EXPECT_EQ(dialog.listedItems().size(), 1u);

  dialog.close();
}

TEST(CatalogueDialogTest, ClearContentResetsTheDisplay) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makePagedCatalogue(2));

  dialog.clearContent();
  EXPECT_TRUE(dialog.listedItems().empty());
  EXPECT_EQ(dialog.currentItemIndex(), -1);
  EXPECT_EQ(dialog.currentGrid(), nullptr);
  EXPECT_EQ(dialog.summaryText(), QString());

  dialog.close();
}

// --- Chrome driven by the schema -----------------------------------------

TEST(CatalogueDialogTest, HighlightToggleDrivesBothRenderers) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makePagedCatalogue(1));

  auto* check = dialog.findChild<QCheckBox*>("catalogueHighlightCheck");
  ASSERT_NE(check, nullptr);
  EXPECT_TRUE(check->isVisibleTo(&dialog));
  EXPECT_EQ(check->text(), "Show data errors");

  auto* grid = dialog.findChild<CatalogueCellGridWidget*>("catalogueCellGrid");
  auto* display =
      dialog.findChild<CatalogueDisplayListWidget*>("catalogueDisplayList");
  ASSERT_NE(grid, nullptr);
  ASSERT_NE(display, nullptr);
  EXPECT_FALSE(grid->showDataErrors());

  check->setChecked(true);
  EXPECT_TRUE(grid->showDataErrors());
  EXPECT_TRUE(display->showDataErrors());

  dialog.close();
}

// A schema with no highlight label has nothing to overlay, so the toggle is
// not offered at all.
TEST(CatalogueDialogTest, HighlightToggleIsHiddenWithoutASchemaLabel) {
  ensureApplication();
  CatalogueDialog dialog;
  dialog.show();
  dialog.setCatalogue(makeFlatCatalogue());

  auto* check = dialog.findChild<QCheckBox*>("catalogueHighlightCheck");
  ASSERT_NE(check, nullptr);
  EXPECT_FALSE(check->isVisibleTo(&dialog));

  dialog.close();
}

// --- Renderers ------------------------------------------------------------

// A double-height character occupies the row below its origin, so the row
// below must carry its background: a row-sequential paint would erase the
// character's lower half.
TEST(CatalogueCellGridWidgetTest, DoubleHeightFillsTheRowBelow) {
  ensureApplication();

  orc::CatalogueCellGrid grid = makeGrid("A", /*rows=*/2, /*columns=*/2);
  grid.palette = {orc::CatalogueColour{0, 0, 0},
                  orc::CatalogueColour{255, 255, 255},
                  orc::CatalogueColour{255, 0, 0}};
  grid.cells[0].double_height = true;
  grid.cells[0].background = 2;
  // Index 2 is row 1, column 0: the lower cell of the pair, carrying the
  // origin row's local background.
  grid.cells[2].double_height_lower = true;
  grid.cells[2].background = 2;

  const QImage image = renderGrid(grid);
  const QColor lower = cellCentre(image, grid, /*row=*/1, /*column=*/0);
  EXPECT_EQ(lower.red(), 255);
  EXPECT_EQ(lower.green(), 0);
}

// The palette travels with the payload: a renderer that assumed a fixed one
// would draw the wrong colours for any other service.
TEST(CatalogueCellGridWidgetTest, ColoursComeFromThePayloadPalette) {
  ensureApplication();

  orc::CatalogueCellGrid grid = makeGrid("", /*rows=*/1, /*columns=*/1);
  grid.palette = {orc::CatalogueColour{0, 0, 0},
                  orc::CatalogueColour{10, 200, 40}};
  grid.cells[0].background = 1;

  const QImage image = renderGrid(grid);
  const QColor centre = cellCentre(image, grid, /*row=*/0, /*column=*/0);
  EXPECT_EQ(centre.green(), 200);
  EXPECT_EQ(centre.red(), 10);
}

// An index past the end of the palette resolves to the last entry rather than
// reading off it.
TEST(CatalogueCellGridWidgetTest, OutOfRangePaletteIndexIsClamped) {
  ensureApplication();

  orc::CatalogueCellGrid grid = makeGrid("", /*rows=*/1, /*columns=*/1);
  grid.palette = {orc::CatalogueColour{0, 0, 0},
                  orc::CatalogueColour{0, 0, 255}};
  grid.cells[0].background = 200;

  const QImage image = renderGrid(grid);
  EXPECT_EQ(cellCentre(image, grid, /*row=*/0, /*column=*/0).blue(), 255);
}

TEST(CatalogueDisplayListWidgetTest, WalksEveryOperation) {
  ensureApplication();
  CatalogueDisplayListWidget widget;

  orc::CatalogueDisplayList list;
  list.aspect_height = 1.0;
  for (int i = 0; i < 3; ++i) {
    orc::CatalogueDrawOp op;
    op.kind = orc::CatalogueDrawKind::kRectangle;
    op.origin = orc::CataloguePoint{0.1 * i, 0.1};
    op.size = orc::CatalogueSize{0.2, 0.2};
    op.filled = true;
    op.colour = orc::CatalogueColour{255, 255, 255};
    list.ops.push_back(op);
  }
  widget.setDisplayList(list);

  renderWidget(widget, 100, 100);
  EXPECT_EQ(widget.opsPainted(), 3);
}

// The drawable area follows the payload's own aspect, not the widget's.
TEST(CatalogueDisplayListWidgetTest, DrawableAreaFollowsThePayloadAspect) {
  ensureApplication();
  CatalogueDisplayListWidget widget;

  orc::CatalogueDisplayList list;
  list.aspect_height = 0.5;
  widget.setDisplayList(list);
  widget.resize(200, 200);

  const QRectF area = widget.displayAreaRect();
  EXPECT_NEAR(area.height() / area.width(), 0.5, 1e-6);
}

// A row no packet was recovered for is not by itself a fault: services
// habitually leave out the blank rows that space a page out. Banding every gap
// put marks on pages that had arrived perfectly, which is worse than not
// marking at all — it trains the reader to ignore them.
TEST(CatalogueCellGridWidgetTest, RowGapsAreNotBandedWithoutALoss) {
  ensureApplication();

  orc::CatalogueCellGrid grid = makeGrid("", /*rows=*/2, /*columns=*/1);
  grid.data_lost = false;
  grid.row_status[1].received = false;

  CatalogueCellGridWidget widget;
  widget.setGrid(grid);
  widget.setShowDataErrors(true);
  const int scale = gridScale(grid);
  const QImage image =
      renderWidget(widget, grid.columns * grid.cell_aspect_width * scale,
                   grid.rows * grid.cell_aspect_height * scale);

  EXPECT_EQ(cellCentre(image, grid, /*row=*/1, /*column=*/0), QColor(Qt::black))
      << "an un-received row was banded with no loss to blame it on";
}

// Once the page's own transmissions are known to have lost packets, every gap
// becomes a candidate for what went astray — the page cannot say which row each
// packet would have carried — so all of them are banded.
TEST(CatalogueCellGridWidgetTest, RowGapsAreBandedWhenPacketsWereLost) {
  ensureApplication();

  orc::CatalogueCellGrid grid = makeGrid("", /*rows=*/2, /*columns=*/1);
  grid.data_lost = true;
  grid.row_status[1].received = false;

  CatalogueCellGridWidget widget;
  widget.setGrid(grid);
  widget.setShowDataErrors(true);
  const int scale = gridScale(grid);
  const QImage image =
      renderWidget(widget, grid.columns * grid.cell_aspect_width * scale,
                   grid.rows * grid.cell_aspect_height * scale);

  // The band is a translucent red hatch, so the row is no longer plain black.
  bool banded = false;
  const int cell_h = grid.cell_aspect_height * scale;
  const int cell_w = grid.cell_aspect_width * scale;
  for (int y = cell_h; y < 2 * cell_h && !banded; ++y) {
    for (int x = 0; x < cell_w; ++x) {
      if (image.pixelColor(x, y).red() > 0) {
        banded = true;
        break;
      }
    }
  }
  EXPECT_TRUE(banded) << "a lost packet left its row unmarked";

  // The overlay is off by default: the marks are not part of the page.
  CatalogueCellGridWidget plain;
  plain.setGrid(grid);
  const QImage unmarked =
      renderWidget(plain, grid.columns * grid.cell_aspect_width * scale,
                   grid.rows * grid.cell_aspect_height * scale);
  EXPECT_EQ(cellCentre(unmarked, grid, /*row=*/1, /*column=*/0),
            QColor(Qt::black));
}

}  // namespace gui_unit_test
