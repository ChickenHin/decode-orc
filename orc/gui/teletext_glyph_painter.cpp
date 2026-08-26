/*
 * File:        teletext_glyph_painter.cpp
 * Module:      orc-gui
 * Purpose:     SAA5050 character rounding, and the shape it makes of a glyph
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_glyph_painter.h"

namespace {

// Whether the character rectangle of |glyph| has a pixel at |column|, |row|.
// Outside the rectangle is empty, which is what makes the rounding stop at a
// character's edge instead of reaching into its neighbour — the chip rounds
// one character at a time.
bool glyph_pixel(const TeletextGlyph& glyph, int column, int row) {
  const int matrix_column = column - kTeletextGlyphOriginColumn;
  const int matrix_row = row - kTeletextGlyphOriginRow;
  if (matrix_column < 0 || matrix_column >= kTeletextGlyphColumns ||
      matrix_row < 0 || matrix_row >= kTeletextGlyphRows) {
    return false;
  }
  const int bit = kTeletextGlyphColumns - 1 - matrix_column;
  return ((glyph.rows[static_cast<size_t>(matrix_row)] >> bit) & 1) != 0;
}

void set_rounded(TeletextRoundedCell& cell, int column, int row) {
  cell[static_cast<size_t>(row) * static_cast<size_t>(kTeletextRoundedColumns) +
       static_cast<size_t>(column)] = true;
}

}  // namespace

TeletextRoundedCell teletext_rounded_cell(const TeletextGlyph& glyph) {
  TeletextRoundedCell rounded{};
  for (int row = 0; row < kTeletextCellRows; ++row) {
    for (int column = 0; column < kTeletextCellColumns; ++column) {
      const int left = column * 2;
      const int top = row * 2;
      if (glyph_pixel(glyph, column, row)) {
        set_rounded(rounded, left, top);
        set_rounded(rounded, left + 1, top);
        set_rounded(rounded, left, top + 1);
        set_rounded(rounded, left + 1, top + 1);
        continue;
      }
      // An empty pixel gains a corner sub-pixel for each diagonal that meets
      // in it: the two pixels either side of the corner filled and the pixel
      // across the corner empty. Naming them makes the four cases read as the
      // one rule they are.
      const bool west = glyph_pixel(glyph, column - 1, row);
      const bool east = glyph_pixel(glyph, column + 1, row);
      const bool north = glyph_pixel(glyph, column, row - 1);
      const bool south = glyph_pixel(glyph, column, row + 1);
      if (west && north && !glyph_pixel(glyph, column - 1, row - 1)) {
        set_rounded(rounded, left, top);
      }
      if (east && north && !glyph_pixel(glyph, column + 1, row - 1)) {
        set_rounded(rounded, left + 1, top);
      }
      if (west && south && !glyph_pixel(glyph, column - 1, row + 1)) {
        set_rounded(rounded, left, top + 1);
      }
      if (east && south && !glyph_pixel(glyph, column + 1, row + 1)) {
        set_rounded(rounded, left + 1, top + 1);
      }
    }
  }
  return rounded;
}

QPainterPath teletext_glyph_path(char32_t code) {
  QPainterPath path;
  const TeletextGlyph* const glyph = teletext_font_glyph(code);
  if (glyph == nullptr) {
    return path;
  }

  const TeletextRoundedCell rounded = teletext_rounded_cell(*glyph);
  for (int row = 0; row < kTeletextRoundedRows; ++row) {
    int run_start = -1;
    for (int column = 0; column <= kTeletextRoundedColumns; ++column) {
      const bool filled = column < kTeletextRoundedColumns &&
                          teletext_rounded_at(rounded, column, row);
      if (filled && run_start < 0) {
        run_start = column;
      } else if (!filled && run_start >= 0) {
        path.addRect(QRectF(run_start, row, column - run_start, 1));
        run_start = -1;
      }
    }
  }
  return path;
}
