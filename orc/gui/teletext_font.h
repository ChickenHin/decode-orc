/*
 * File:        teletext_font.h
 * Module:      orc-gui
 * Purpose:     The SAA5050 character generator's bitmaps, keyed by Unicode
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef TELETEXT_FONT_H
#define TELETEXT_FONT_H

#include <array>
#include <cstdint>

/**
 * @file
 * @brief The typeface a teletext page was drawn in
 *
 * A teletext page is not text set in whatever face the reading platform has.
 * The service transmitted character *codes* and the receiver's character
 * generator turned each one into a fixed 5 by 9 matrix of pixels, so the
 * letterforms are as much a part of what a page looked like as its colours
 * are — and a page drawn in a modern monospaced face does not look like
 * teletext at all, whatever else is right about it.
 *
 * The generator almost every receiver held was the Mullard SAA5050 series,
 * which is also what the BBC Micro put on screen in Mode 7. The bitmaps here
 * are that family's, from the Bedstead project
 * (<http://bjh21.me.uk/bedstead/>), which took them from the July 1982
 * datasheet code tables and checked the English ones against a real SAA5050.
 * They cover the SAA5050 to SAA5057, so the Cyrillic G0 sets of ETSI EN 300
 * 706 clause 15.6.4 to 15.6.6 are drawn from the generator that drew them
 * (the SAA5057) rather than being borrowed from elsewhere.
 *
 * Bedstead's bitmaps and code were dedicated to the public domain under CC0
 * 1.0 by Ben Harris, Simon Tatham and Marnanel Thurman, which this project's
 * GPL-3.0-or-later can carry. Upstream's own note on the typeface is worth
 * repeating: copyright in it is still owned by Mullard's corporate
 * successors, but under section 55 of the Copyright Designs and Patents Act
 * 1988 that copyright is no longer infringed by producing or using articles
 * designed for producing material in it.
 *
 * teletext_font.cpp is generated from upstream's bedstead.c by
 * `tools/generate_teletext_font.py`, which pins the revision it was taken
 * from — so the table can be checked against its source rather than trusted.
 * Four letters are the generator's own and are argued for there: the
 * Macedonian Ѓ, ѓ, Ќ and ќ that Table 38 needs and no chip in the family
 * held.
 *
 * A bitmap is only half of what the chip did; see teletext_glyph_painter.h
 * for the character rounding that turns one into what reached the screen.
 */

/// Columns of the matrix a character is drawn in.
constexpr int kTeletextGlyphColumns = 5;
/// Rows of it.
constexpr int kTeletextGlyphRows = 9;

/// Columns of the character rectangle the matrix sits in. The extra column is
/// blank and is what separates a character from the one beside it.
constexpr int kTeletextCellColumns = 6;
/// Rows of it. The extra row is blank and separates a row of text from the
/// next.
constexpr int kTeletextCellRows = 10;

/// Where the matrix sits in the character rectangle: hard against its bottom
/// right corner, leaving the blank column down the left and the blank row
/// along the top.
///
/// The bottom two rows of the matrix are the descender rows, so setting it
/// against the bottom is what puts a descender at the foot of the rectangle
/// and the gap between rows of text above the capitals rather than below the
/// descenders. Drawn a row higher — matrix against the top — every character
/// rides high in its own background, which is what a coloured row makes
/// obvious.
constexpr int kTeletextGlyphOriginColumn =
    kTeletextCellColumns - kTeletextGlyphColumns;
constexpr int kTeletextGlyphOriginRow = kTeletextCellRows - kTeletextGlyphRows;

/**
 * @brief One character's 5 by 9 matrix
 *
 * Row 0 is the top one. Within a row, bit (@ref kTeletextGlyphColumns - 1) is
 * the leftmost column, so a row's bits read left to right as the row is drawn.
 */
struct TeletextGlyph {
  std::array<uint8_t, kTeletextGlyphRows> rows;
};

/**
 * @brief The matrix for @p code, or nullptr where the face has none
 *
 * A code point the face does not hold is not an error. The viewer draws it in
 * the platform's own font instead, which is worse-looking but legible — the
 * alternative, drawing nothing, would read as a decoding fault.
 */
const TeletextGlyph* teletext_font_glyph(char32_t code);

#endif  // TELETEXT_FONT_H
