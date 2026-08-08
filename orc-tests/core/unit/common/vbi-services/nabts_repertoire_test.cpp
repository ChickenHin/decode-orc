/*
 * File:        nabts_repertoire_test.cpp
 * Module:      orc-core-tests
 * Purpose:     The NAPLPS character repertoires, page text reading and caption
 *              cue building (ANSI X3.110-1983 §5, §7; CEA-516 §7.3.10)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "vbi-services/nabts_page.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {
namespace {

using Repertoire = NabtsPrimitive::Repertoire;

// A character primitive at |x|,|y| in a field of |field| by |field| * 2.
NabtsPrimitive character(uint8_t code, Repertoire repertoire, double x,
                         double y, double field = 1.0 / 40.0) {
  NabtsPrimitive primitive;
  primitive.kind = NabtsPrimitiveKind::kCharacter;
  primitive.character = code;
  primitive.repertoire = repertoire;
  primitive.origin = NabtsPoint{x, y};
  primitive.size = NabtsSize{field, field * 2.0};
  return primitive;
}

// ---------------------------------------------------------------------------
// The primary set (X3.110 §5.1, §7)
// ---------------------------------------------------------------------------

// §7.2 bases the repertoire on ANSI X3.4-1977 and Table 25 puts the number
// sign at 2/3, the dollar at 2/4, the grave at 6/0, the circumflex at 5/14 and
// the tilde at 7/14 — each where ASCII has it.
TEST(NabtsPrimarySet, IsAsciiAcrossTheGraphicRange) {
  for (int code = 0x20; code < 0x7F; ++code) {
    EXPECT_EQ(nabts_primary_to_unicode(static_cast<uint8_t>(code)),
              static_cast<char32_t>(code))
        << "code " << code;
  }
  EXPECT_EQ(nabts_primary_to_unicode(0x23), U'#');
  EXPECT_EQ(nabts_primary_to_unicode(0x24), U'$');
  EXPECT_EQ(nabts_primary_to_unicode(0x60), U'`');
  EXPECT_EQ(nabts_primary_to_unicode(0x5E), U'^');
  EXPECT_EQ(nabts_primary_to_unicode(0x7E), U'~');
}

// 7/15 is DELETE rather than a graphic, and a control below 2/0 never reaches a
// character primitive at all.
TEST(NabtsPrimarySet, NonGraphicPositionsReadAsSpace) {
  EXPECT_EQ(nabts_primary_to_unicode(0x7F), U' ');
  EXPECT_EQ(nabts_primary_to_unicode(0x00), U' ');
  EXPECT_EQ(nabts_primary_to_unicode(0x1F), U' ');
}

// ---------------------------------------------------------------------------
// The supplementary set (X3.110 §5.2, Tables 18 to 27)
// ---------------------------------------------------------------------------

// Every one of these code positions is named by the Coded Representation column
// of Tables 18 to 27, so this is a transcription check rather than a guess.
TEST(NabtsSupplementarySet, MatchesTheCodedRepresentationsOfTables18To27) {
  struct Entry {
    uint8_t code;
    char32_t expected;
  };
  constexpr Entry kEntries[] = {
      {0x21, U'¡'},  // S 2/1 inverted exclamation point
      {0x22, U'¢'},  // S 2/2 cent sign
      {0x23, U'£'},  // S 2/3 pound sign
      {0x25, U'¥'},  // S 2/5 yen sign
      {0x27, U'§'},  // S 2/7 section sign
      {0x28, U'¤'},  // S 2/8 general currency sign
      {0x2B, U'«'},  // S 2/11 angle quotation marks left
      {0x2C, U'←'},  // S 2/12 leftward arrow
      {0x2E, U'→'},  // S 2/14 rightward arrow
      {0x30, U'°'},  // S 3/0 degree sign
      {0x34, U'×'},  // S 3/4 multiply sign
      {0x35, U'µ'},  // S 3/5 micro sign
      {0x38, U'÷'},  // S 3/8 divide sign
      {0x3B, U'»'},  // S 3/11 angle quotation marks right
      {0x3D, U'½'},  // S 3/13 fraction one-half
      {0x3F, U'¿'},  // S 3/15 inverted question mark
      {0x51, U'¹'},  // S 5/1 superscript 1
      {0x52, U'®'},  // S 5/2 registered sign
      {0x53, U'©'},  // S 5/3 copyright sign
      {0x55, U'♪'},  // S 5/5 musical note
      {0x5C, U'⅛'},  // S 5/12 fraction one-eighth
      // S 6/0 is Table 25's "ohm sign", which is U+2126 rather than the
      // Greek capital omega it looks like.
      {0x60, U'\u2126'},
      {0x61, U'Æ'},  // S 6/1 upper case AE diphthong
      {0x62, U'Đ'},  // S 6/2 upper case D with stroke
      {0x63, U'ª'},  // S 6/3 feminine ordinal indicator
      {0x64, U'Ħ'},  // S 6/4 upper case H with stroke
      {0x68, U'Ł'},  // S 6/8 upper case L with stroke
      {0x69, U'Ø'},  // S 6/9 upper case O with slash
      {0x6B, U'º'},  // S 6/11 masculine ordinal indicator
      {0x70, U'ĸ'},  // S 7/0 lower case k, Greenlandic
      {0x71, U'æ'},  // S 7/1 lower case ae diphthong
      {0x73, U'ð'},  // S 7/3 lower case eth, Icelandic
      {0x75, U'ı'},  // S 7/5 lower case i without dot
      {0x7B, U'ß'},  // S 7/11 lower case sharp s, German
      {0x7E, U'ŋ'},  // S 7/14 lower case eng, Lapp
  };
  for (const Entry& entry : kEntries) {
    EXPECT_EQ(nabts_supplementary_to_unicode(entry.code), entry.expected)
        << "code position " << std::hex << static_cast<int>(entry.code);
  }
}

// X3.110's own additions where ISO 6937-1982 leaves the positions vacant or
// uses them differently: Table 25 notes 1 to 7.
TEST(NabtsSupplementarySet, CarriesTheX3110LineAndDiagonalGraphics) {
  EXPECT_EQ(nabts_supplementary_to_unicode(0x56), U'─');  // full horizontal
  EXPECT_EQ(nabts_supplementary_to_unicode(0x57), U'│');  // full vertical
  EXPECT_EQ(nabts_supplementary_to_unicode(0x58), U'╱');  // diagonal
  EXPECT_EQ(nabts_supplementary_to_unicode(0x59), U'╲');  // reverse diagonal
  EXPECT_EQ(nabts_supplementary_to_unicode(0x5A), U'◢');  // filled diagonal
  EXPECT_EQ(nabts_supplementary_to_unicode(0x5B), U'◣');  // filled reverse
  EXPECT_EQ(nabts_supplementary_to_unicode(0x65), U'┼');  // cross
}

// Tables 26 and 27 fill column 4 and nothing else with non-spacing marks.
TEST(NabtsSupplementarySet, Column4IsTheNonSpacingMarksAndNothingElseIs) {
  for (int code = 0x20; code <= 0x7F; ++code) {
    const bool in_column_4 = code >= 0x40 && code <= 0x4F;
    EXPECT_EQ(nabts_supplementary_is_nonspacing(static_cast<uint8_t>(code)),
              in_column_4)
        << "code position " << std::hex << code;
  }
  // Table 26's coded representations: S 4/2 acute, S 4/1 grave, S 4/8
  // diaeresis, S 4/15 caron, S 4/11 cedilla.
  EXPECT_EQ(nabts_supplementary_to_unicode(0x42), U'́');
  EXPECT_EQ(nabts_supplementary_to_unicode(0x41), U'̀');
  EXPECT_EQ(nabts_supplementary_to_unicode(0x48), U'̈');
  EXPECT_EQ(nabts_supplementary_to_unicode(0x4F), U'̌');
  EXPECT_EQ(nabts_supplementary_to_unicode(0x4B), U'̧');
}

// ---------------------------------------------------------------------------
// The mosaic set (X3.110 §5.4, Figures 62 and 63)
// ---------------------------------------------------------------------------

// Figure 62 marks a mosaic position with b6 = 1 — columns 2, 3, 6 and 7 — and
// §5.4 adds the second copy of the solid mosaic at 5/15, giving 65 in all.
TEST(NabtsMosaicSet, HasSixtyFivePositions) {
  int mosaics = 0;
  for (int code = 0x20; code <= 0x7F; ++code) {
    if (nabts_is_mosaic_code(static_cast<uint8_t>(code))) {
      ++mosaics;
    }
  }
  EXPECT_EQ(mosaics, 65);
  EXPECT_TRUE(nabts_is_mosaic_code(0x5F));   // §5.4's second solid mosaic
  EXPECT_FALSE(nabts_is_mosaic_code(0x40));  // column 4 is not a mosaic
  EXPECT_FALSE(nabts_is_mosaic_code(0x5E));
}

// Figure 62's sub-element allocation: b1 top-left, b2 top-right, b3
// middle-left, b4 middle-right, b5 bottom-left, b7 bottom-right.
TEST(NabtsMosaicSet, PacksTheSubElementsInFigure62Order) {
  EXPECT_EQ(nabts_mosaic_sixels(0x20), 0x00);  // 2/0, nothing lit
  EXPECT_EQ(nabts_mosaic_sixels(0x21), 0x01);  // b1 alone: top-left
  EXPECT_EQ(nabts_mosaic_sixels(0x22), 0x02);  // b2 alone: top-right
  EXPECT_EQ(nabts_mosaic_sixels(0x24), 0x04);  // b3: middle-left
  EXPECT_EQ(nabts_mosaic_sixels(0x28), 0x08);  // b4: middle-right
  EXPECT_EQ(nabts_mosaic_sixels(0x30), 0x10);  // b5: bottom-left
  EXPECT_EQ(nabts_mosaic_sixels(0x60), 0x20);  // b7: bottom-right
  // 7/15 has b1 to b5 and b7 all set: the solid mosaic.
  EXPECT_EQ(nabts_mosaic_sixels(0x7F), 0x3F);
  // §5.4: "The code combination 5/15 (1011111) also implies that all
  // sub-elements are on."
  EXPECT_EQ(nabts_mosaic_sixels(0x5F), 0x3F);
}

// §5.4 and §5.6 make a mosaic and a DRCS character shapes rather than text.
TEST(NabtsCharacterText, ShapesHaveNoTextForm) {
  EXPECT_EQ(nabts_character_to_utf8(0x41, Repertoire::kPrimary), "A");
  EXPECT_EQ(nabts_character_to_utf8(0x61, Repertoire::kSupplementary), "Æ");
  EXPECT_EQ(nabts_character_to_utf8(0x7F, Repertoire::kMosaic), " ");
  EXPECT_EQ(nabts_character_to_utf8(0x41, Repertoire::kDrcs), " ");
}

// ---------------------------------------------------------------------------
// Reading a record back as text
// ---------------------------------------------------------------------------

TEST(NabtsPageText, ReadsACharacterRunLeftToRight) {
  NabtsPageSnapshot page;
  const double step = 1.0 / 40.0;
  page.primitives.push_back(character('H', Repertoire::kPrimary, 0.0, 0.5));
  page.primitives.push_back(character('I', Repertoire::kPrimary, step, 0.5));
  EXPECT_EQ(nabts_page_text(page), "HI");
}

// NAPLPS has no rows, so a "line" is a shared baseline. Two runs at different
// heights read as two lines, top one first — unit space has y upwards.
TEST(NabtsPageText, SplitsLinesByBaselineTopDown) {
  NabtsPageSnapshot page;
  const double step = 1.0 / 40.0;
  // Transmitted bottom line first, to prove the order is the geometry's rather
  // than the display list's.
  page.primitives.push_back(character('B', Repertoire::kPrimary, 0.0, 0.20));
  page.primitives.push_back(character('T', Repertoire::kPrimary, 0.0, 0.50));
  page.primitives.push_back(character('P', Repertoire::kPrimary, step, 0.50));
  EXPECT_EQ(nabts_page_text(page), "TP\nB");
}

// §7.2: the mark precedes the letter in transmission; Unicode puts it after.
TEST(NabtsPageText, ComposesANonSpacingMarkOntoTheLetterThatFollowsIt) {
  NabtsPageSnapshot page;
  page.primitives.push_back(
      character(0x48, Repertoire::kSupplementary, 0.0, 0.5));  // diaeresis
  page.primitives.push_back(character('e', Repertoire::kPrimary, 0.0, 0.5));
  EXPECT_EQ(nabts_page_text(page), "ë");
}

// Reading a mosaic as a space would put gaps in the text the record never had.
TEST(NabtsPageText, LeavesBlockGraphicsOut) {
  NabtsPageSnapshot page;
  const double step = 1.0 / 40.0;
  page.primitives.push_back(character('A', Repertoire::kPrimary, 0.0, 0.5));
  page.primitives.push_back(character(0x7F, Repertoire::kMosaic, step, 0.5));
  page.primitives.push_back(
      character('B', Repertoire::kPrimary, 2 * step, 0.5));
  EXPECT_EQ(nabts_page_text(page), "AB");
}

// A record that only drew geometry is legitimately textless.
TEST(NabtsPageText, IsEmptyForARecordThatDrewNoCharacters) {
  NabtsPageSnapshot page;
  NabtsPrimitive line;
  line.kind = NabtsPrimitiveKind::kLine;
  line.points = {NabtsPoint{0.0, 0.0}, NabtsPoint{0.5, 0.5}};
  page.primitives.push_back(line);
  EXPECT_TRUE(nabts_page_text(page).empty());
}

// ---------------------------------------------------------------------------
// The caption service (CEA-516 §7.3.10)
// ---------------------------------------------------------------------------

namespace {

NabtsCataloguedRecord caption_record(uint8_t version, uint64_t first,
                                     uint64_t last, const std::string& text) {
  NabtsCataloguedRecord record;
  record.channel = 0xA00;
  record.address_text = "000";
  record.record_type = 1;  // §5.2.2.3 non-cyclic presentation
  record.caption = true;
  record.version = version;
  record.first_seen_frame = first;
  record.last_seen_frame = last;
  double x = 0.0;
  for (const char letter : text) {
    record.page.primitives.push_back(
        character(static_cast<uint8_t>(letter), Repertoire::kPrimary, x, 0.2));
    x += 1.0 / 40.0;
  }
  return record;
}

}  // namespace

// §7.3.10.1 has a receiver replace the caption on screen rather than being told
// when to take it down, so a cue runs to the next caption.
TEST(NabtsCaptionCues, EachCueRunsToTheNextOne) {
  std::vector<NabtsCataloguedRecord> records = {
      caption_record(1, 100, 104, "ONE"),
      caption_record(2, 200, 206, "TWO"),
  };
  const auto cues = nabts_caption_cues(records);
  ASSERT_EQ(cues.size(), 2u);
  EXPECT_EQ(cues[0].text, "ONE");
  EXPECT_EQ(cues[0].start_frame, 100u);
  EXPECT_EQ(cues[0].end_frame, 200u);
  // The last cue has nothing after it, so it runs to the last frame its own
  // record was seen at — all the recording says about it.
  EXPECT_EQ(cues[1].start_frame, 200u);
  EXPECT_EQ(cues[1].end_frame, 206u);
}

// Cues come out in transmission order whatever order the catalogue is in — the
// catalogue is keyed on {channel, address, version}, and a version wraps.
TEST(NabtsCaptionCues, AreOrderedByTheFrameTheyWereFirstSeenAt) {
  std::vector<NabtsCataloguedRecord> records = {
      caption_record(9, 500, 505, "LATER"),
      caption_record(1, 100, 105, "EARLIER"),
  };
  const auto cues = nabts_caption_cues(records);
  ASSERT_EQ(cues.size(), 2u);
  EXPECT_EQ(cues[0].text, "EARLIER");
  EXPECT_EQ(cues[1].text, "LATER");
}

// §7.3.10.1: "Captions may be erased by the use of PLPS code that erases either
// the entire display" — such a record draws nothing, so it ends the caption
// before it rather than becoming one.
TEST(NabtsCaptionCues, ARecordThatDrewNothingEndsThePreviousCue) {
  std::vector<NabtsCataloguedRecord> records = {
      caption_record(1, 100, 104, "SHOWN"),
      caption_record(2, 150, 152, ""),
  };
  const auto cues = nabts_caption_cues(records);
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "SHOWN");
  EXPECT_EQ(cues[0].end_frame, 150u);
}

// The Caption Flag of §5.2.7.3 is what makes a record part of the service, not
// the channel it arrived on.
TEST(NabtsCaptionCues, OnlyRecordsCarryingTheCaptionFlagBecomeCues) {
  std::vector<NabtsCataloguedRecord> records = {
      caption_record(1, 100, 104, "CAPTION"),
      caption_record(2, 200, 204, "PAGE"),
  };
  records[1].caption = false;
  const auto cues = nabts_caption_cues(records);
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "CAPTION");
}

// An application record's data is function descriptors (§7.2.2), not NAPLPS, so
// there is nothing to read a caption out of even if the flag were set.
TEST(NabtsCaptionCues, IgnoresApplicationRecords) {
  std::vector<NabtsCataloguedRecord> records = {
      caption_record(1, 100, 104, "TEXT"),
  };
  records[0].record_type = 2;
  EXPECT_TRUE(nabts_caption_cues(records).empty());
}

// Two captions first seen in the same frame must still produce monotonic,
// non-empty extents, or an export would emit a zero-length cue.
TEST(NabtsCaptionCues, KeepExtentsMonotonicWhenCaptionsShareAFrame) {
  std::vector<NabtsCataloguedRecord> records = {
      caption_record(1, 300, 300, "FIRST"),
      caption_record(2, 300, 302, "SECOND"),
  };
  const auto cues = nabts_caption_cues(records);
  ASSERT_EQ(cues.size(), 2u);
  EXPECT_EQ(cues[0].text, "FIRST");
  EXPECT_GT(cues[0].end_frame, cues[0].start_frame);
  EXPECT_GT(cues[1].end_frame, cues[1].start_frame);
}

}  // namespace
}  // namespace orc
