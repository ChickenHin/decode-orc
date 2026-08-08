/*
 * File:        nabts_analysis_presenter_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     NabtsAnalysisPresenter: display list, colour and text resolution
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_analysis_presenter.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace gui_unit_test {
namespace {

using orc::NabtsAnalysisDataset;
using orc::NabtsCataloguedRecord;
using orc::NabtsColour;
using orc::NabtsPageSnapshot;
using orc::NabtsPoint;
using orc::NabtsPrimitive;
using orc::NabtsPrimitiveKind;
using orc::NabtsSize;
using orc::presenters::NabtsAnalysisPresenter;
using orc::presenters::NabtsPrimitiveKindView;

using Repertoire = NabtsPrimitive::Repertoire;

// One 1/40 by 1/20 character field, which is the nominal text size of T.101
// Table II-3 to within the height typo §5.3.2.3.9 settles.
constexpr double kFieldW = 1.0 / 40.0;
constexpr double kFieldH = 1.0 / 20.0;

NabtsPrimitive character(uint8_t code, double x, double y,
                         Repertoire repertoire = Repertoire::kPrimary) {
  NabtsPrimitive primitive;
  primitive.kind = NabtsPrimitiveKind::kCharacter;
  primitive.character = code;
  primitive.repertoire = repertoire;
  primitive.origin = NabtsPoint{x, y};
  primitive.points.push_back(primitive.origin);
  primitive.size = NabtsSize{kFieldW, kFieldH};
  return primitive;
}

// A run of |text| starting at |x|,|y| on the default rightward character path.
void append_run(NabtsPageSnapshot& page, const std::string& text, double x,
                double y) {
  for (size_t i = 0; i < text.size(); ++i) {
    page.primitives.push_back(character(static_cast<uint8_t>(text[i]),
                                        x + static_cast<double>(i) * kFieldW,
                                        y));
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Text runs
// ---------------------------------------------------------------------------

// NAPLPS emits one primitive per character; a renderer would rather have the
// word, and the run is what a single drawText call can put on screen.
TEST(NabtsAnalysisPresenterTest, CoalescesAdjacentCharactersIntoOneTextRun) {
  NabtsPageSnapshot page;
  append_run(page, "HELLO", 0.1, 0.5);

  const auto view = NabtsAnalysisPresenter::makePageView(page);
  ASSERT_EQ(view.primitives.size(), 1u);
  EXPECT_EQ(view.primitives[0].kind, NabtsPrimitiveKindView::kText);
  EXPECT_EQ(view.primitives[0].text, "HELLO");
  EXPECT_EQ(view.primitives[0].character_count, 5);
  EXPECT_NEAR(view.primitives[0].origin.x, 0.1, 1e-9);
  EXPECT_NEAR(view.primitives[0].advance.dx, kFieldW, 1e-9);
  EXPECT_NEAR(view.primitives[0].advance.dy, 0.0, 1e-9);
}

// A character that is not where the cursor would have left it starts a new run,
// whatever its attributes say — otherwise two lines would be drawn as one.
TEST(NabtsAnalysisPresenterTest, ANewLineStartsANewRun) {
  NabtsPageSnapshot page;
  append_run(page, "TOP", 0.0, 0.6);
  append_run(page, "BOTTOM", 0.0, 0.5);

  const auto view = NabtsAnalysisPresenter::makePageView(page);
  ASSERT_EQ(view.primitives.size(), 2u);
  EXPECT_EQ(view.primitives[0].text, "TOP");
  EXPECT_EQ(view.primitives[1].text, "BOTTOM");
}

// A colour change mid-word is a new run: the attributes are what the run is
// for.
TEST(NabtsAnalysisPresenterTest, AColourChangeStartsANewRun) {
  NabtsPageSnapshot page;
  NabtsPrimitive first = character('A', 0.0, 0.5);
  first.colour = NabtsColour{7, 0, 0, false};
  NabtsPrimitive second = character('B', kFieldW, 0.5);
  second.colour = NabtsColour{0, 7, 0, false};
  page.primitives = {first, second};

  const auto view = NabtsAnalysisPresenter::makePageView(page);
  ASSERT_EQ(view.primitives.size(), 2u);
  EXPECT_EQ(view.primitives[0].text, "A");
  EXPECT_EQ(view.primitives[1].text, "B");
  // Three bits per gun resolved to a full 8-bit channel.
  EXPECT_EQ(view.primitives[0].colour.green, 255);
  EXPECT_EQ(view.primitives[0].colour.red, 0);
  EXPECT_EQ(view.primitives[1].colour.red, 255);
}

// §5.3.2.3.4 Table 8 allows spacings of 1, 1.25 and 1.5 character fields, so a
// run at 1.25 pitch is still one run — and its advance is the measured step.
TEST(NabtsAnalysisPresenterTest, WiderInterCharacterSpacingStaysOneRun) {
  NabtsPageSnapshot page;
  const double pitch = kFieldW * 1.25;
  page.primitives.push_back(character('A', 0.0, 0.5));
  page.primitives.push_back(character('B', pitch, 0.5));
  page.primitives.push_back(character('C', 2 * pitch, 0.5));

  const auto view = NabtsAnalysisPresenter::makePageView(page);
  ASSERT_EQ(view.primitives.size(), 1u);
  EXPECT_EQ(view.primitives[0].text, "ABC");
  EXPECT_NEAR(view.primitives[0].advance.dx, pitch, 1e-9);
}

// §7.2: the mark precedes the letter in transmission and shares its field, so
// the pair is one character of the run, in Unicode's order.
TEST(NabtsAnalysisPresenterTest, ComposesNonSpacingMarksOntoTheirLetters) {
  NabtsPageSnapshot page;
  page.primitives.push_back(character('c', 0.0, 0.5));
  // S 4/11 cedilla, then the letter it applies to, both at the same origin.
  page.primitives.push_back(
      character(0x4B, kFieldW, 0.5, Repertoire::kSupplementary));
  page.primitives.push_back(character('a', kFieldW, 0.5));

  const auto view = NabtsAnalysisPresenter::makePageView(page);
  ASSERT_EQ(view.primitives.size(), 1u);
  // "c", then "a" with U+0327 COMBINING CEDILLA behind it — the mark arrived
  // first and has been moved after the letter it applies to.
  EXPECT_EQ(view.primitives[0].text, std::string("ca̧"));
  // Two character fields, not three: the mark occupies none of its own.
  EXPECT_EQ(view.primitives[0].character_count, 2);
}

// ---------------------------------------------------------------------------
// The character kinds a run cannot hold
// ---------------------------------------------------------------------------

// A mosaic is a shape, so it becomes its own primitive with the six elements
// Figure 62 assigns — and §6.2.7.15's underline mode is what separates them.
TEST(NabtsAnalysisPresenterTest, AMosaicBecomesItsOwnPrimitiveWithSixels) {
  NabtsPageSnapshot page;
  NabtsPrimitive mosaic = character(0x7F, 0.0, 0.5, Repertoire::kMosaic);
  mosaic.underlined = true;
  page.primitives.push_back(mosaic);

  const auto view = NabtsAnalysisPresenter::makePageView(page);
  ASSERT_EQ(view.primitives.size(), 1u);
  EXPECT_EQ(view.primitives[0].kind, NabtsPrimitiveKindView::kMosaic);
  EXPECT_EQ(view.primitives[0].mosaic_pattern, 0x3F);
  EXPECT_TRUE(view.primitives[0].mosaic_separated);
  // The underline attribute has been consumed into the separation: §5.4 says
  // "Mosaic characters are not underlined".
  EXPECT_FALSE(view.primitives[0].underlined);
}

// §5.6: a DRCS character the record never defined "shall be displayed as if it
// were SPACE", so the view says there is no glyph rather than pointing at one.
TEST(NabtsAnalysisPresenterTest, AnUndefinedDrcsCharacterHasNoGlyph) {
  NabtsPageSnapshot page;
  page.primitives.push_back(character(0x21, 0.0, 0.5, Repertoire::kDrcs));

  const auto view = NabtsAnalysisPresenter::makePageView(page);
  ASSERT_EQ(view.primitives.size(), 1u);
  EXPECT_EQ(view.primitives[0].kind, NabtsPrimitiveKindView::kDrcs);
  EXPECT_EQ(view.primitives[0].drcs_index, -1);
  EXPECT_TRUE(view.drcs.empty());
}

TEST(NabtsAnalysisPresenterTest, ADefinedDrcsCharacterIndexesItsGlyph) {
  NabtsPageSnapshot page;
  orc::NabtsDrcsCharacter glyph;
  glyph.code = 0x21;
  glyph.width = 2;
  glyph.height = 2;
  glyph.elements = {true, false, false, true};
  page.drcs.push_back(glyph);
  page.primitives.push_back(character(0x21, 0.0, 0.5, Repertoire::kDrcs));

  const auto view = NabtsAnalysisPresenter::makePageView(page);
  ASSERT_EQ(view.primitives.size(), 1u);
  ASSERT_EQ(view.drcs.size(), 1u);
  EXPECT_EQ(view.primitives[0].drcs_index, 0);
  EXPECT_TRUE(view.drcs[0].defined());
  EXPECT_EQ(view.drcs[0].code, 0x21);
}

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------

// An unwritten colour map is the default one of X3.110 §5.3.2.5.2 / T.101
// Table II-3: a grey ramp in the low half, hues in the high half.
TEST(NabtsAnalysisPresenterTest, AnUnwrittenColourMapResolvesToTheDefault) {
  NabtsPageSnapshot page;
  orc::nabts_default_colour_map(page.colour_map);

  const auto view = NabtsAnalysisPresenter::makePageView(page);
  ASSERT_EQ(view.colour_map.size(), orc::kNabtsColourMapEntries);
  EXPECT_EQ(view.colour_map[0].red, 0);
  EXPECT_EQ(view.colour_map[0].green, 0);
  EXPECT_EQ(view.colour_map[0].blue, 0);
  // Entry 7 is the top of the grey ramp: white.
  EXPECT_EQ(view.colour_map[7].red, 255);
  EXPECT_EQ(view.colour_map[7].green, 255);
  EXPECT_EQ(view.colour_map[7].blue, 255);
  // Entry 8 is 0 degrees on the hue circle, which §5.3.2.5.2 puts at blue.
  EXPECT_EQ(view.colour_map[8].blue, 255);
  EXPECT_EQ(view.colour_map[8].red, 0);
  EXPECT_EQ(view.colour_map[8].green, 0);
}

// A map the record wrote overrides the default, entry by entry.
TEST(NabtsAnalysisPresenterTest, AWrittenColourMapOverridesTheDefault) {
  NabtsPageSnapshot page;
  orc::nabts_default_colour_map(page.colour_map);
  page.colour_map[3] = NabtsColour{7, 0, 7, false};  // green + blue

  const auto view = NabtsAnalysisPresenter::makePageView(page);
  EXPECT_EQ(view.colour_map[3].green, 255);
  EXPECT_EQ(view.colour_map[3].blue, 255);
  EXPECT_EQ(view.colour_map[3].red, 0);
}

// §5.3.3.6.3: an incremental run carries colour *values* in mode 0 and colour
// map *addresses* in modes 1 and 2, so the mode decides how they resolve.
TEST(NabtsAnalysisPresenterTest, IncrementalColoursResolveThroughTheMap) {
  NabtsPageSnapshot page;
  orc::nabts_default_colour_map(page.colour_map);

  NabtsPrimitive run;
  run.kind = NabtsPrimitiveKind::kIncrementalPoints;
  run.origin = NabtsPoint{0.0, 0.0};
  run.points.push_back(run.origin);
  run.size = NabtsSize{0.5, 0.5};
  run.colour_mode = orc::NabtsColourMode::kMapped;
  run.incremental_colours = {0, 7, 8};
  page.primitives.push_back(run);

  const auto view = NabtsAnalysisPresenter::makePageView(page);
  ASSERT_EQ(view.primitives.size(), 1u);
  ASSERT_EQ(view.primitives[0].incremental_colours.size(), 3u);
  EXPECT_EQ(view.primitives[0].incremental_colours[0].red, 0);
  EXPECT_EQ(view.primitives[0].incremental_colours[1].red, 255);
  EXPECT_EQ(view.primitives[0].incremental_colours[2].blue, 255);
}

// In colour mode 0 the same bytes are values: Figure 12's two G R B tuples per
// byte, so six payload bits give two bits per gun.
TEST(NabtsAnalysisPresenterTest, IncrementalColoursInModeZeroAreValues) {
  NabtsPageSnapshot page;
  NabtsPrimitive run;
  run.kind = NabtsPrimitiveKind::kIncrementalPoints;
  run.colour_mode = orc::NabtsColourMode::kDirect;
  // Both tuples green: bits 5 and 2 set, which is 0b100100.
  run.incremental_colours = {0x24};
  page.primitives.push_back(run);

  const auto view = NabtsAnalysisPresenter::makePageView(page);
  ASSERT_EQ(view.primitives[0].incremental_colours.size(), 1u);
  const auto& colour = view.primitives[0].incremental_colours[0];
  EXPECT_GT(colour.green, 200);
  EXPECT_EQ(colour.red, 0);
  EXPECT_EQ(colour.blue, 0);
}

// §5.3.2.6: only colour mode 2 has a background colour, and a renderer has to
// be able to tell "background black" from "no background at all".
TEST(NabtsAnalysisPresenterTest, OnlyColourModeTwoCarriesABackground) {
  NabtsPageSnapshot page;
  NabtsPrimitive plain = character('A', 0.0, 0.5);
  plain.colour_mode = orc::NabtsColourMode::kMapped;
  NabtsPrimitive boxed = character('B', 0.0, 0.4);
  boxed.colour_mode = orc::NabtsColourMode::kMappedWithBackground;
  boxed.background = NabtsColour{0, 0, 7, false};
  page.primitives = {plain, boxed};

  const auto view = NabtsAnalysisPresenter::makePageView(page);
  ASSERT_EQ(view.primitives.size(), 2u);
  EXPECT_FALSE(view.primitives[0].has_background);
  EXPECT_TRUE(view.primitives[1].has_background);
  EXPECT_EQ(view.primitives[1].background.blue, 255);
}

// ---------------------------------------------------------------------------
// The catalogue
// ---------------------------------------------------------------------------

TEST(NabtsAnalysisPresenterTest, CarriesTheCatalogueThroughUnchanged) {
  NabtsAnalysisDataset dataset;
  NabtsCataloguedRecord record;
  record.channel = 0x000;
  record.address = 0x1A400;
  record.address_text = "1A4";
  record.channel_text = "000/1A4";
  record.record_type = 0;
  record.version = 2;
  record.first_seen_frame = 10;
  record.last_seen_frame = 400;
  record.times_seen = 12;
  record.times_intact = 11;
  record.records_in_message = 3;
  record.complete = true;
  record.data = {0x41, 0x42};
  append_run(record.page, "PAGE", 0.0, 0.5);
  dataset.records.push_back(record);
  dataset.summary.frames_analysed = 500;
  dataset.summary.messages_complete = 12;

  const auto view = NabtsAnalysisPresenter::makeAnalysisView(dataset);
  ASSERT_EQ(view.records.size(), 1u);
  const auto& out = view.records[0];
  EXPECT_EQ(out.channel_text, "000/1A4");
  EXPECT_EQ(out.record_type_name, "Cyclic presentation");
  EXPECT_TRUE(out.presentation);
  EXPECT_EQ(out.times_seen, 12u);
  EXPECT_EQ(out.times_intact, 11u);
  EXPECT_EQ(out.records_in_message, 3u);
  EXPECT_EQ(out.data_bytes, 2u);
  EXPECT_EQ(out.page.text, "PAGE");
  EXPECT_EQ(view.summary.frames_analysed, 500u);
  EXPECT_EQ(view.summary.messages_complete, 12u);
}

// §5.2.2.4: an application record's data is function descriptors, not NAPLPS,
// so nothing is drawn for it and the descriptors are what a reader gets.
TEST(NabtsAnalysisPresenterTest, AnApplicationRecordCarriesFunctionsNotAPage) {
  NabtsAnalysisDataset dataset;
  NabtsCataloguedRecord record;
  record.record_type = 2;
  orc::NabtsRecordFunction function;
  function.code = "2/1";
  function.control = true;
  function.arguments = "ABC";
  record.functions.push_back(function);
  dataset.records.push_back(record);

  const auto view = NabtsAnalysisPresenter::makeAnalysisView(dataset);
  ASSERT_EQ(view.records.size(), 1u);
  EXPECT_FALSE(view.records[0].presentation);
  EXPECT_EQ(view.records[0].record_type_name, "Application");
  ASSERT_EQ(view.records[0].functions.size(), 1u);
  EXPECT_EQ(view.records[0].functions[0].code, "2/1");
  EXPECT_TRUE(view.records[0].functions[0].control);
  EXPECT_TRUE(view.records[0].page.empty());
}

// §5.2.2 reserves types 4 to 15; reporting the number is more use than
// inventing a name, and nothing is assumed about their data.
TEST(NabtsAnalysisPresenterTest, AReservedRecordTypeIsNamedByItsNumber) {
  NabtsAnalysisDataset dataset;
  NabtsCataloguedRecord record;
  record.record_type = 9;
  dataset.records.push_back(record);

  const auto view = NabtsAnalysisPresenter::makeAnalysisView(dataset);
  EXPECT_EQ(view.records[0].record_type_name, "Reserved (9)");
  EXPECT_FALSE(view.records[0].presentation);
}

// The caption track is built from the catalogue, so opening the viewer on a
// captioned recording shows the service without any further work.
TEST(NabtsAnalysisPresenterTest, BuildsTheCaptionTrackFromTheCatalogue) {
  NabtsAnalysisDataset dataset;
  for (int i = 0; i < 2; ++i) {
    NabtsCataloguedRecord record;
    record.channel = 0xA00;
    record.address_text = "000";
    record.record_type = 1;
    record.caption = true;
    record.version = static_cast<uint8_t>(i + 1);
    record.first_seen_frame = 100 + static_cast<uint64_t>(i) * 100;
    record.last_seen_frame = record.first_seen_frame + 5;
    append_run(record.page, i == 0 ? "FIRST" : "SECOND", 0.0, 0.2);
    dataset.records.push_back(record);
  }

  const auto view = NabtsAnalysisPresenter::makeAnalysisView(dataset);
  ASSERT_EQ(view.captions.size(), 2u);
  EXPECT_EQ(view.captions[0].text, "FIRST");
  EXPECT_EQ(view.captions[0].start_frame, 100u);
  EXPECT_EQ(view.captions[0].end_frame, 200u);
  EXPECT_EQ(view.captions[1].text, "SECOND");
  EXPECT_EQ(view.captions[1].channel, 0xA00);
}

// A recording with no captioning has no track, which is what lets the viewer
// leave the control off rather than showing an empty one.
TEST(NabtsAnalysisPresenterTest, HasNoCaptionTrackWithoutCaptionRecords) {
  NabtsAnalysisDataset dataset;
  NabtsCataloguedRecord record;
  record.record_type = 0;
  append_run(record.page, "PAGE", 0.0, 0.5);
  dataset.records.push_back(record);

  const auto view = NabtsAnalysisPresenter::makeAnalysisView(dataset);
  EXPECT_TRUE(view.captions.empty());
}

// The interpreter's diagnostics are what tell a short record from a damaged
// one, so they cross into the view rather than being summarised away.
TEST(NabtsAnalysisPresenterTest, CarriesTheDecodeDiagnostics) {
  NabtsPageSnapshot page;
  page.diagnostics.bytes_read = 200;
  page.diagnostics.truncated_pdis = 2;
  page.diagnostics.unresolved_macros = 1;
  page.diagnostics.storage_used = 512;

  const auto view = NabtsAnalysisPresenter::makePageView(page);
  EXPECT_EQ(view.recovery.bytes_read, 200u);
  EXPECT_EQ(view.recovery.truncated_pdis, 2u);
  EXPECT_EQ(view.recovery.unresolved_macros, 1u);
  EXPECT_EQ(view.recovery.storage_used, 512u);
  EXPECT_FALSE(view.recovery.clean());
}

TEST(NabtsAnalysisPresenterTest, ACleanRecordSaysSo) {
  NabtsPageSnapshot page;
  page.diagnostics.bytes_read = 40;
  append_run(page, "OK", 0.0, 0.5);

  const auto view = NabtsAnalysisPresenter::makePageView(page);
  EXPECT_TRUE(view.recovery.clean());
  EXPECT_DOUBLE_EQ(view.display_area_height, 0.78125);
}

}  // namespace gui_unit_test
