/*
 * File:        nabts_catalogue_view_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NAPLPS page to display-list conversion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_catalogue_view.h"

#include <gtest/gtest.h>

#include <vector>

#include "nabts_record_catalogue.h"
#include "naplps_render_grid.h"

namespace orc {
namespace {

/// A snapshot holding one primitive, which is enough to check what the list
/// carries alongside its operations.
NabtsPageSnapshot pageWithOneLine(double pel) {
  NabtsPageSnapshot snapshot;
  nabts_default_colour_map(snapshot.colour_map);

  NabtsPrimitive primitive;
  primitive.kind = NabtsPrimitiveKind::kLine;
  primitive.points = {NabtsPoint{0.1, 0.1}, NabtsPoint{0.9, 0.6}};
  primitive.origin = primitive.points.front();
  primitive.logical_pel = NabtsSize{pel, pel};
  snapshot.primitives.push_back(primitive);
  return snapshot;
}

// The list states the receiver it was resolved against, because X3.110 sizes
// stroke width, texture dots and incremental rasters in that receiver's pixels
// (§5.3.2.2.6) and a renderer given no resolution can only guess at them.
TEST(NabtsCatalogueViewTest, TheDisplayListCarriesTheGridItWasResolvedAgainst) {
  struct Case {
    NaplpsRenderMode mode;
    int width;
    int height;
  };
  const std::vector<Case> cases = {{NaplpsRenderMode::kReference, 256, 200},
                                   {NaplpsRenderMode::kTwice, 512, 400},
                                   {NaplpsRenderMode::kThrice, 768, 600},
                                   {NaplpsRenderMode::kTwiceVector, 512, 400}};

  for (const Case& item : cases) {
    const CatalogueDisplayList list =
        nabts_page_display_list(pageWithOneLine(0.0), item.mode);
    EXPECT_EQ(list.nominal_width, item.width)
        << "mode " << naplps_render_mode_name(item.mode);
    EXPECT_EQ(list.nominal_height, item.height)
        << "mode " << naplps_render_mode_name(item.mode);
  }
}

// Table D1 item 10 makes unit y 0 to 0.78125 the visible extent, while §4.2.2
// puts that extent in a 4:3 display area. The two are different numbers and the
// list carries both, which is what lets a renderer draw a receiver's
// rectangular pixel as a rectangle instead of squaring the picture up.
TEST(NabtsCatalogueViewTest, TheVisibleExtentAndTheDisplayShapeAreBothStated) {
  const CatalogueDisplayList list =
      nabts_page_display_list(pageWithOneLine(0.0));
  EXPECT_NEAR(list.aspect_height, 0.78125, 1e-12);
  EXPECT_NEAR(list.display_aspect_height, 0.75, 1e-12);
  EXPECT_NE(list.aspect_height, list.display_aspect_height);
}

// The vector mode hands a renderer the geometry the record described, so the
// page's operations are the record's shapes and the pel stays in unit space:
// turning it into pixels there would bake one receiver into a list whose whole
// point is that it has not chosen one.
TEST(NabtsCatalogueViewTest, TheVectorModeEmitsTheRecordsOwnGeometry) {
  const NabtsPageSnapshot snapshot = pageWithOneLine(0.02);
  const CatalogueDisplayList list =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kTwiceVector);

  ASSERT_EQ(list.ops.size(), 1u);
  EXPECT_EQ(list.ops[0].kind, CatalogueDrawKind::kLine);
  ASSERT_EQ(list.ops[0].points.size(), 2u);
  EXPECT_NEAR(list.ops[0].points[0].x, 0.1, 1e-12);
  EXPECT_NEAR(list.ops[0].pen_size.dx, 0.02, 1e-12);
}

// A pixel mode deposits the page into the receiver's frame buffer and emits
// what is on it, so the operations are runs of pixels rather than the shapes
// that put them there. They are rectangles because every renderer of a display
// list already draws those — pixel output needs no new operation kind.
TEST(NabtsCatalogueViewTest, APixelModeEmitsRunsOfPixels) {
  const NabtsPageSnapshot snapshot = pageWithOneLine(0.02);
  const CatalogueDisplayList list =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kReference);

  ASSERT_FALSE(list.ops.empty());
  for (const CatalogueDrawOp& op : list.ops) {
    EXPECT_EQ(op.kind, CatalogueDrawKind::kRectangle);
    EXPECT_TRUE(op.filled);
    // A pixel run has no pen: its extent is its own, not a stroke's.
    EXPECT_EQ(op.pen_size.dx, 0.0);
    EXPECT_EQ(op.pen_size.dy, 0.0);
    // Every run sits inside the unit screen it was rastered from.
    EXPECT_GE(op.origin.x, 0.0);
    EXPECT_GE(op.origin.y, 0.0);
    EXPECT_LE(op.origin.x + op.size.dx, 1.0 + 1e-9);
    EXPECT_LE(op.origin.y + op.size.dy, kNabtsDisplayAreaHeight + 1e-9);
  }
}

// A finer receiver resolves the same page into more, smaller pixels. That is
// the whole observable difference between the pixel modes, and it is what
// X3.110 §5.3.2.2.6's "at least one and possibly many display pixels" comes
// down to once a receiver is named.
TEST(NabtsCatalogueViewTest, AFinerReceiverEmitsSmallerPixels) {
  const NabtsPageSnapshot snapshot = pageWithOneLine(0.02);
  const CatalogueDisplayList coarse =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kReference);
  const CatalogueDisplayList fine =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kTwice);

  ASSERT_FALSE(coarse.ops.empty());
  ASSERT_FALSE(fine.ops.empty());
  // One row of the finer grid is half the height of one of the coarser.
  EXPECT_LT(fine.ops[0].size.dy, coarse.ops[0].size.dy);
  EXPECT_NEAR(fine.ops[0].size.dy, coarse.ops[0].size.dy / 2.0, 1e-9);
}

// The default is the receiver Table D1 requires, so a caller that names no mode
// gets the resolution a decoder of the period displayed.
TEST(NabtsCatalogueViewTest, TheDefaultResolutionIsTheReferenceReceiver) {
  const CatalogueDisplayList list =
      nabts_page_display_list(pageWithOneLine(0.0));
  EXPECT_EQ(list.nominal_width, 256);
  EXPECT_EQ(list.nominal_height, 200);
}

// ---------------------------------------------------------------------------
// Interpretation at catalogue-build time
// ---------------------------------------------------------------------------

/// A dataset holding one presentation record whose data draws |text| through a
/// DRCS definition, so the record exercises the resolution-dependent path.
NabtsAnalysisDataset datasetWithDrcsRecord() {
  NabtsAnalysisDataset data;
  NabtsCataloguedRecord record;
  record.channel = 0x000;
  record.address_text = "000";
  record.record_type = 1;  // §5.2.2.3 non-cyclic presentation
  record.times_seen = 1;
  record.times_intact = 1;
  record.complete = true;
  // ESC 4/3 (DEF DRCS), the character to define, then SO to invoke the PDI set
  // and a filled rectangle over the buffer, then ESC 4/5 (END).
  record.data = {0x1B, 0x43, 0x20, 0x0E, 0x6B, 0x7F, 0x1B, 0x45};
  data.records.push_back(record);
  return data;
}

// The receiver is chosen when the page is browsed, not when the recording is
// read, so changing it takes effect without reading the recording again. X3.110
// §6.2.3 sizes a DRCS storage buffer from the receiver's resolution, which is
// the one thing that used to be fixed by the recovery pass.
TEST(NabtsCatalogueViewTest, TheResolutionAppliesWithoutReRunningRecovery) {
  const NabtsAnalysisDataset data = datasetWithDrcsRecord();

  std::vector<NabtsCataloguedRecord> coarse = data.records;
  nabts_interpret_records(coarse, kNaplpsGridReference);
  std::vector<NabtsCataloguedRecord> fine = data.records;
  nabts_interpret_records(fine, kNaplpsGridTwice);

  ASSERT_EQ(coarse.size(), 1u);
  ASSERT_EQ(fine.size(), 1u);
  ASSERT_FALSE(coarse[0].page.drcs.empty())
      << "the fixture defined no downloadable character";
  ASSERT_FALSE(fine[0].page.drcs.empty());

  // The same definition, stored at each receiver's own resolution.
  EXPECT_GT(fine[0].page.drcs[0].width, coarse[0].page.drcs[0].width);
  EXPECT_GT(fine[0].page.drcs[0].height, coarse[0].page.drcs[0].height);
}

// Recovery catalogues the record data and stops there: a page is what a
// receiver makes of that data, so building it belongs with the receiver.
TEST(NabtsCatalogueViewTest, ADatasetCarriesRecordDataRatherThanAPage) {
  const NabtsAnalysisDataset data = datasetWithDrcsRecord();
  EXPECT_FALSE(data.records[0].data.empty());
  EXPECT_TRUE(data.records[0].page.primitives.empty());

  // The catalogue is where it becomes something to look at.
  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  EXPECT_FALSE(catalogue.items.empty());
}

// What a page says is not what it looks like: the resolution decides how it is
// drawn, and the text a reader searches, the captions and the item table have
// to come out the same at every one of them.
TEST(NabtsCatalogueViewTest, TheReadingOfAPageIsTheSameAtEveryResolution) {
  const NabtsAnalysisDataset data = datasetWithDrcsRecord();

  const CatalogueDataset reference =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  for (const NaplpsRenderMode mode :
       {NaplpsRenderMode::kThrice, NaplpsRenderMode::kTwice,
        NaplpsRenderMode::kTwiceVector}) {
    const CatalogueDataset other = build_nabts_catalogue(data, mode);
    ASSERT_EQ(other.items.size(), reference.items.size())
        << "mode " << naplps_render_mode_name(mode);
    for (size_t i = 0; i < other.items.size(); ++i) {
      EXPECT_EQ(other.items[i].id, reference.items[i].id);
      EXPECT_EQ(other.items[i].values, reference.items[i].values);
      EXPECT_EQ(other.items[i].badges, reference.items[i].badges);
    }
    // The text a reader searches and the caption track are read off the page,
    // not off how it is drawn, so they cannot move with the resolution either.
    ASSERT_EQ(other.payloads.size(), reference.payloads.size());
    for (size_t i = 0; i < other.payloads.size(); ++i) {
      EXPECT_EQ(other.payloads[i].companion_text,
                reference.payloads[i].companion_text)
          << "the text a reader searches changed with the resolution";
      EXPECT_EQ(other.payloads[i].table.rows, reference.payloads[i].table.rows);
    }
  }
}

// The four receivers are offered to the reader through the schema, so the host
// can put them on a dropdown without knowing what a receiver is.
TEST(NabtsCatalogueViewTest, TheSchemaOffersTheReceiversAsViewOptions) {
  const CatalogueDataset catalogue =
      build_nabts_catalogue(datasetWithDrcsRecord(), NaplpsRenderMode::kThrice);

  EXPECT_FALSE(catalogue.schema.view_label.empty())
      << "an unlabelled set of options is not offered at all";
  ASSERT_EQ(catalogue.schema.view_options.size(), 4u);

  // Every option names a mode back, so what the host round-trips selects the
  // receiver the reader picked rather than falling back to another.
  for (const auto& option : catalogue.schema.view_options) {
    EXPECT_FALSE(option.label.empty());
    EXPECT_FALSE(option.tooltip.empty());
    EXPECT_EQ(naplps_render_mode_name(naplps_render_mode_from_name(
                  option.id, NaplpsRenderMode::kTwiceVector)),
              option.id)
        << "option '" << option.id << "' names no receiver";
  }

  // And the catalogue says which of them it was built under.
  EXPECT_EQ(catalogue.schema.view_option,
            naplps_render_mode_name(NaplpsRenderMode::kThrice));
}

}  // namespace
}  // namespace orc
