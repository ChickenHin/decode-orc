/*
 * File:        teletext_glyph_painter.h
 * Module:      orc-gui
 * Purpose:     SAA5050 character rounding, and the shape it makes of a glyph
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef TELETEXT_GLYPH_PAINTER_H
#define TELETEXT_GLYPH_PAINTER_H

#include <QPainterPath>
#include <array>

#include "teletext_font.h"

/**
 * @file
 * @brief What the character generator put on the screen, not just in its ROM
 *
 * The SAA5050 does not draw its 5 by 9 matrix as it stands. It doubles the
 * matrix to 10 by 18 for an interlaced display and, on the way, smooths the
 * diagonals: wherever two pixels meet corner to corner with the corner itself
 * empty, the sub-pixel in that corner is filled in. The datasheet calls it
 * character rounding, and it is why teletext letterforms look drawn rather
 * than plotted — a capital A has sloped sides on a screen and stepped ones in
 * the ROM.
 *
 * The rule, over a 2 by 2 clump of pixels containing a diagonal:
 *
 *     . #  ->  . . # #        # .  ->  # # . .
 *     # .      . # # #        . #      # # # .
 *              # # . .                 . . # #
 *              # # . .                 . . # #
 *
 * It is applied at every occurrence, including overlapping ones, so a long
 * diagonal comes out as an even stair of half-pixels while everything else is
 * left as it was.
 */

/// The character rectangle at the resolution rounding produces: the 6 by 10
/// rectangle doubled on both axes.
constexpr int kTeletextRoundedColumns = 2 * kTeletextCellColumns;
constexpr int kTeletextRoundedRows = 2 * kTeletextCellRows;

/// A rounded character rectangle, row-major from its top left.
using TeletextRoundedCell =
    std::array<bool, static_cast<size_t>(kTeletextRoundedColumns) *
                         static_cast<size_t>(kTeletextRoundedRows)>;

/// The sub-pixel @p column, @p row of a rounded cell.
inline bool teletext_rounded_at(const TeletextRoundedCell& cell, int column,
                                int row) {
  return cell[static_cast<size_t>(row) *
                  static_cast<size_t>(kTeletextRoundedColumns) +
              static_cast<size_t>(column)];
}

/// Round @p glyph the way the character generator does, placing it in the
/// character rectangle as teletext_font.h describes.
TeletextRoundedCell teletext_rounded_cell(const TeletextGlyph& glyph);

/**
 * @brief The outline of @p code in a character rectangle
 * kTeletextRoundedColumns wide and kTeletextRoundedRows tall
 *
 * One sub-pixel is one unit, with the origin at the rectangle's top left, so a
 * caller scales the path to whatever size it is drawing a cell at rather than
 * asking for one. Empty where the face has no glyph for @p code, and for a
 * character that is all background — both of which draw nothing.
 *
 * The path is built from the rounded cell rather than from the matrix, so it
 * is what the chip put on screen. Runs of adjacent sub-pixels are merged into
 * single rectangles: it is one filled shape either way, and the seams between
 * abutting rectangles are what would otherwise show under a fractional scale.
 */
QPainterPath teletext_glyph_path(char32_t code);

#endif  // TELETEXT_GLYPH_PAINTER_H
