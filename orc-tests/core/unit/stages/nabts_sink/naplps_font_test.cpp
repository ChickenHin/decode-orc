/*
 * File:        naplps_font_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NAPLPS character faces and the choice
 *              between them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_font.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace orc {
namespace {

/// The face a receiver at |grid| draws a |columns| by |rows| field from, named.
std::string face_for(const NaplpsRenderGrid& grid, int columns, int rows) {
  return naplps_font_face_for_field(grid, columns, rows).name;
}

// ---------------------------------------------------------------------------
// The faces themselves
// ---------------------------------------------------------------------------

// A page is drawn once and read at whatever resolution the reader picked, so a
// character legible on one receiver must not vanish on another: every face has
// to carry what the first one does.
TEST(NaplpsFontTest, EveryFaceCarriesTheSameRepertoireAsTheFirst) {
  ASSERT_GT(naplps_font_face_count(), 1u);

  const NaplpsFontFace& reference = naplps_font_face(0);
  ASSERT_GT(reference.count, 0u);

  for (size_t index = 1; index < naplps_font_face_count(); ++index) {
    const NaplpsFontFace& face = naplps_font_face(index);
    EXPECT_EQ(face.count, reference.count) << face.name;
    for (size_t entry = 0; entry < reference.count; ++entry) {
      const char32_t code = reference.codes[entry];
      EXPECT_NE(naplps_face_pattern(face, code), nullptr)
          << face.name << " has no pattern for code point " << code;
    }
  }
}

// The repertoire is what X3.110 §7.1's primary set and the accented letters
// §7.2's supplementary set composes come to: ISO 8859-1's printable positions.
TEST(NaplpsFontTest, TheRepertoireCoversThePrintableLatinPositions) {
  for (size_t index = 0; index < naplps_font_face_count(); ++index) {
    const NaplpsFontFace& face = naplps_font_face(index);
    for (char32_t code = 0x20; code <= 0x7E; ++code) {
      EXPECT_NE(naplps_face_pattern(face, code), nullptr)
          << face.name << " at " << code;
    }
    for (char32_t code = 0xA0; code <= 0xFF; ++code) {
      EXPECT_NE(naplps_face_pattern(face, code), nullptr)
          << face.name << " at " << code;
    }
    // A code point outside it draws nothing rather than drawing rubbish.
    EXPECT_EQ(naplps_face_pattern(face, 0x0080), nullptr) << face.name;
    EXPECT_EQ(naplps_face_pattern(face, 0x20AC), nullptr) << face.name;
  }
}

// A pattern row is read by testing bits below the face's width, so a bit set
// above it would be ink the deposit could never place — and a generator that
// mis-shifted a row would leave exactly that.
TEST(NaplpsFontTest, NoPatternHasInkOutsideItsOwnCell) {
  for (size_t index = 0; index < naplps_font_face_count(); ++index) {
    const NaplpsFontFace& face = naplps_font_face(index);
    const uint16_t beyond =
        static_cast<uint16_t>(~((1u << face.width) - 1u) & 0xFFFFu);
    for (size_t entry = 0; entry < face.count; ++entry) {
      const uint16_t* pattern = naplps_face_pattern(face, face.codes[entry]);
      ASSERT_NE(pattern, nullptr) << face.name;
      for (int row = 0; row < face.height; ++row) {
        EXPECT_EQ(pattern[row] & beyond, 0u)
            << face.name << " row " << row << " of code point "
            << face.codes[entry];
      }
    }
  }
}

/// The pattern for |code| in face |index|, drawn as one string per row.
std::vector<std::string> pattern_rows(size_t index, char32_t code) {
  const NaplpsFontFace& face = naplps_font_face(index);
  const uint16_t* pattern = naplps_face_pattern(face, code);
  std::vector<std::string> out;
  for (int row = 0; row < face.height; ++row) {
    std::string line;
    for (int column = 0; column < face.width; ++column) {
      line.push_back(
          (pattern[row] & (1u << (face.width - 1 - column))) != 0u ? '#' : '.');
    }
    out.push_back(line);
  }
  return out;
}

// The 6 by 10 face upstream draws a full stop as a five-pixel diamond
// straddling the baseline, and its colon and semicolon from the same mark,
// which is a terminal font's device for a cell only ten rows tall. On a page —
// where a line of leader dots is a common device — it reads as a row of
// crosses, so the generator substitutes the square block the finer faces use.
// A regeneration that dropped the substitution would put the crosses back.
TEST(NaplpsFontTest, TheSmallestFaceStopsWithADotRatherThanACross) {
  EXPECT_EQ(pattern_rows(0, U'.'),
            (std::vector<std::string>{"......", "......", "......", "......",
                                      "......", "......", "..##..", "..##..",
                                      "......", "......"}));
  EXPECT_EQ(pattern_rows(0, U':'),
            (std::vector<std::string>{"......", "......", "......", "..##..",
                                      "..##..", "......", "..##..", "..##..",
                                      "......", "......"}));
  // The semicolon keeps the tail that tells it apart from a colon.
  EXPECT_EQ(pattern_rows(0, U';'),
            (std::vector<std::string>{"......", "......", "......", "..##..",
                                      "..##..", "......", "..##..", "..#...",
                                      ".#....", "......"}));
}

// ---------------------------------------------------------------------------
// Choosing a face for a character field
// ---------------------------------------------------------------------------

// §5.1 leaves the patterns to the receiver "for a given display resolution",
// and the reference model's is the 6 by 10 and nothing else: its default field
// is exactly that cell, and the larger fields it offers are the ones a set-top
// decoder drew by doubling the cell it had.
TEST(NaplpsFontTest, TheReferenceReceiverDrawsEveryFieldFromItsOneGenerator) {
  // The five character field sizes §6.2.7.6-10 select, in the cells the
  // reference grid gives them.
  EXPECT_EQ(face_for(kNaplpsGridReference, 6, 10), "misc-fixed 6x10");
  EXPECT_EQ(face_for(kNaplpsGridReference, 3, 10), "misc-fixed 6x10");
  EXPECT_EQ(face_for(kNaplpsGridReference, 8, 12), "misc-fixed 6x10");
  EXPECT_EQ(face_for(kNaplpsGridReference, 6, 20), "misc-fixed 6x10");
  EXPECT_EQ(face_for(kNaplpsGridReference, 13, 20), "misc-fixed 6x10");
  // Even a field with room for every face the plugin carries.
  EXPECT_EQ(face_for(kNaplpsGridReference, 40, 60), "misc-fixed 6x10");
}

// A receiver above the reference grid held a character generator with finer
// patterns, so the default field — 12.8 by 20 cells at twice the reference
// grid, 19.2 by 30 at three times — comes from a finer face rather than from a
// magnified 6 by 10.
TEST(NaplpsFontTest, AFinerReceiverDrawsTheDefaultFieldFromAFinerFace) {
  EXPECT_EQ(face_for(kNaplpsGridTwice, 13, 20), "misc-fixed 10x20");
  EXPECT_EQ(face_for(kNaplpsGridThrice, 19, 30), "misc-fixed 9x15");
}

// The face has to divide the field a whole number of times: the pattern is
// deposited by sampling the field back into it, so a field that is not a whole
// multiple duplicates some of the face's rows and not others, which is what
// breaks a letterform. At 19 by 30 the 10 by 20 fits and has the most elements
// of any face that does, and is still not the one chosen — 30 is not a
// multiple of 20.
TEST(NaplpsFontTest, AFaceMustDivideTheFieldRatherThanMerelyFitIt) {
  EXPECT_EQ(face_for(kNaplpsGridThrice, 19, 30), "misc-fixed 9x15");
  EXPECT_EQ(face_for(kNaplpsGridThrice, 19, 60), "misc-fixed 10x20");
  EXPECT_EQ(face_for(kNaplpsGridThrice, 19, 45), "misc-fixed 9x15");
}

// Where no face divides the field, the finest one that fits is drawn instead:
// some resampling is unavoidable, and a coarser face would not avoid it.
TEST(NaplpsFontTest, AFieldNoFaceDividesTakesTheFinestThatFits) {
  EXPECT_EQ(face_for(kNaplpsGridTwice, 16, 24), "misc-fixed 10x20");
  EXPECT_EQ(face_for(kNaplpsGridTwice, 9, 17), "misc-fixed 9x15");
}

// A field below the smallest cell the plugin carries still draws: the 6 by 10
// is squeezed into it, which is what a receiver with one generator and a field
// under it does. Table D1 item 5(g) puts the standard's own floor at
// dx = 6/256 by dy = 8/256, which is under the face's ten rows.
TEST(NaplpsFontTest, AFieldBelowTheSmallestCellFallsBackToTheSixByTen) {
  EXPECT_EQ(face_for(kNaplpsGridTwice, 6, 8), "misc-fixed 6x10");
  EXPECT_EQ(face_for(kNaplpsGridTwice, 4, 6), "misc-fixed 6x10");
  EXPECT_EQ(face_for(kNaplpsGridThrice, 1, 1), "misc-fixed 6x10");
}

// A field narrow for its height takes a face narrow enough to sit in it, so a
// condensed field does not pick a face it would have to lose columns from.
TEST(NaplpsFontTest, ANarrowFieldTakesAFaceThatFitsAcross) {
  EXPECT_EQ(face_for(kNaplpsGridTwice, 6, 20), "misc-fixed 6x10");
  EXPECT_EQ(face_for(kNaplpsGridThrice, 9, 30), "misc-fixed 9x15");
}

}  // namespace
}  // namespace orc
