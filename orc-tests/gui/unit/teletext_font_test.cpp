/*
 * File:        teletext_font_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 tests for the teletext character generator's face and
 *              the character rounding applied to it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_font.h"

#include <gtest/gtest.h>

#include <QRectF>
#include <string>

#include "teletext_glyph_painter.h"

namespace gui_unit_test {

namespace {

// Every character a Level 1 page can put in an alphanumeric cell, as the
// decoder's G0 tables spell them (ETSI EN 300 706 clause 15.6). Written out
// here rather than reached for through the plugin, both because the GUI does
// not include plugin headers and because a copy is what makes this a test:
// the face has to hold these whatever either side does later.

// Positions of the Latin primary set (Table 35) that no national option
// sub-set replaces, plus the filled rectangle at 7/F.
const char32_t kLatinPrimary[] =
    U" !\"#$%&'()*+,-./0123456789:;<=>?"
    U"@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"
    U"`abcdefghijklmnopqrstuvwxyz{|}~■";

// The thirteen replaceable positions across all seven sub-sets of Table 36.
const char32_t kNationalOptions[] =
    U"¡£¤§°¼½¾¿ÄÅÉÖÜßàáâäåçèéêëìíîïñòóôö÷ùúûüýčěřšťůž—‖←↑→";

// The three Cyrillic primary sets of Tables 38 to 40, less the positions they
// share with the Latin set.
const char32_t kCyrillic[] =
    U"ЂЃЄІЇЈЉЊЋЌЏАБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ"
    U"абвгдежзийклмнопрстуфхцчшщъыьэюяђѓєіїјљњћќ";

// A glyph with a single down-left diagonal: a pixel at the fifth column of
// the top row, and one below and to its left. Enough to show the rounding
// without a letterform's other features confusing what caused what.
TeletextGlyph diagonalGlyph() {
  TeletextGlyph glyph{};
  glyph.rows[0] = 0b00010;
  glyph.rows[1] = 0b00100;
  return glyph;
}

void expectHasGlyphs(const char32_t* characters, const char* what) {
  for (const char32_t* character = characters; *character != U'\0';
       ++character) {
    EXPECT_NE(teletext_font_glyph(*character), nullptr)
        << what << " is missing U+" << std::hex << std::uppercase
        << static_cast<uint32_t>(*character);
  }
}

}  // namespace

// A page drawn half in the character generator's face and half in the
// platform's is worse than one drawn wholly in either, so the face has to
// cover the whole repertoire the decoder can hand the viewer.
TEST(TeletextFontTest, HoldsEveryCharacterALevel1PageCanShow) {
  expectHasGlyphs(kLatinPrimary, "the Latin primary set");
  expectHasGlyphs(kNationalOptions, "the national option sub-sets");
  expectHasGlyphs(kCyrillic, "the Cyrillic primary sets");
}

// The face is a teletext character generator's, not a Unicode font: a
// character outside it has no glyph, which is what sends the viewer to the
// platform font rather than drawing a blank.
TEST(TeletextFontTest, HasNothingForACharacterOutsideTheFace) {
  EXPECT_EQ(teletext_font_glyph(U'一'), nullptr);
}

// SPACE is in the face and draws nothing, which is not the same as being
// absent: an absent character would be drawn by the platform font instead.
TEST(TeletextFontTest, SpaceIsPresentAndEmpty) {
  const TeletextGlyph* const space = teletext_font_glyph(U' ');
  ASSERT_NE(space, nullptr);
  for (const uint8_t row : space->rows) {
    EXPECT_EQ(row, 0);
  }
  EXPECT_TRUE(teletext_glyph_path(U' ').isEmpty());
}

// The 5 by 9 matrix sits against the bottom right of the 6 by 10 character
// rectangle: the blank column is down the left, between a character and the
// one beside it, and the blank row is along the top, between a row of text and
// the one above. Set it against the top instead and every character rides high
// in its own background, which a page of coloured rows makes obvious.
TEST(TeletextGlyphPainterTest, MatrixSitsAgainstTheBottomRightOfTheCell) {
  // 7/F of every G0 set: seven full rows of the matrix from its top, so its
  // left and top edges are the matrix's own.
  const TeletextGlyph* const block = teletext_font_glyph(U'■');
  ASSERT_NE(block, nullptr);
  const TeletextRoundedCell rounded = teletext_rounded_cell(*block);

  // The blank row along the top, and the blank column down the left.
  EXPECT_FALSE(teletext_rounded_at(rounded, 6, 0));
  EXPECT_FALSE(teletext_rounded_at(rounded, 6, 1));
  EXPECT_FALSE(teletext_rounded_at(rounded, 0, 2));
  EXPECT_FALSE(teletext_rounded_at(rounded, 1, 2));
  EXPECT_TRUE(teletext_rounded_at(rounded, 2, 2));
  EXPECT_TRUE(teletext_rounded_at(rounded, kTeletextRoundedColumns - 1, 2));

  // The same thing said as a shape: ten sub-pixels across from column 2, and
  // fourteen down from row 2 — the two rows under it being the descender rows
  // this character has nothing in.
  EXPECT_EQ(teletext_glyph_path(U'■').boundingRect(), QRectF(2, 2, 10, 14));
}

// A descender reaches the foot of the character rectangle, which is what the
// blank row being at the top leaves room for.
TEST(TeletextGlyphPainterTest, ADescenderReachesTheFootOfTheCell) {
  const QRectF bounds = teletext_glyph_path(U'g').boundingRect();
  EXPECT_EQ(bounds.bottom(), static_cast<qreal>(kTeletextRoundedRows));
}

// The rounding the SAA5050 applies on the way from its 5 by 9 matrix to the
// 10 by 18 it displays: an empty pixel gains the corner sub-pixel of each
// diagonal that meets in it.
TEST(TeletextGlyphPainterTest, DiagonalsGainTheirCornerSubPixels) {
  const TeletextRoundedCell rounded = teletext_rounded_cell(diagonalGlyph());

  // The two set pixels, each filling its own four sub-pixels. Matrix row 0 is
  // row 1 of the character rectangle, so rows 2 and 3 of the rounded one.
  EXPECT_TRUE(teletext_rounded_at(rounded, 8, 2));
  EXPECT_TRUE(teletext_rounded_at(rounded, 9, 3));
  EXPECT_TRUE(teletext_rounded_at(rounded, 6, 4));
  EXPECT_TRUE(teletext_rounded_at(rounded, 7, 5));

  // The corner the diagonal passes through, filled from both sides.
  EXPECT_TRUE(teletext_rounded_at(rounded, 7, 3));
  EXPECT_TRUE(teletext_rounded_at(rounded, 8, 4));

  // The other two corners of the same 2x2 clump, which no diagonal meets in.
  EXPECT_FALSE(teletext_rounded_at(rounded, 6, 3));
  EXPECT_FALSE(teletext_rounded_at(rounded, 9, 4));
}

// Rounding is a property of one character, not of the row it is in: the chip
// draws each character rectangle from its own matrix, so nothing may be added
// outside the rectangle's edges.
TEST(TeletextGlyphPainterTest, RoundingStopsAtTheCharacterRectangle) {
  const TeletextRoundedCell rounded = teletext_rounded_cell(diagonalGlyph());
  for (int row = 0; row < kTeletextRoundedRows; ++row) {
    EXPECT_FALSE(teletext_rounded_at(rounded, 0, row)) << "row " << row;
    EXPECT_FALSE(teletext_rounded_at(rounded, kTeletextRoundedColumns - 1, row))
        << "row " << row;
  }
}

// The path is in sub-pixels of the character rectangle, so the viewer scales
// it to whatever size it draws a cell at.
TEST(TeletextGlyphPainterTest, PathIsInTheUnitCharacterRectangle) {
  const QRectF bounds = teletext_glyph_path(U'A').boundingRect();
  EXPECT_GE(bounds.left(), 0.0);
  EXPECT_GE(bounds.top(), 0.0);
  EXPECT_LE(bounds.right(), static_cast<qreal>(kTeletextRoundedColumns));
  EXPECT_LE(bounds.bottom(), static_cast<qreal>(kTeletextRoundedRows));
}

TEST(TeletextGlyphPainterTest, PathIsEmptyForACharacterOutsideTheFace) {
  EXPECT_TRUE(teletext_glyph_path(U'一').isEmpty());
}

}  // namespace gui_unit_test
