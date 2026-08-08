/*
 * File:        nabts_page.h
 * Module:      decode-orc Plugin SDK (support)
 * Purpose:     A decoded NAPLPS presentation record as a resolved display list
 *              in unit space
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_PAGE_H
#define ORC_NABTS_PAGE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace orc {

/**
 * @file
 * @brief The decode/render boundary for a NABTS presentation record
 *
 * A NABTS presentation record is NAPLPS — CEA-516 §6.1, which is ANSI
 * X3.110-1983 / ITU-T T.101 Data Syntax III — and NAPLPS is a drawing language
 * rather than a character grid. There is no equivalent of the World System
 * Teletext 40x25 cell array to decode into: a page is a program of geometric
 * primitives, and what it looks like depends on where it is drawn.
 *
 * So the decoder resolves rather than renders. It runs the program, tracks the
 * presentation state the standard defines, and emits each drawing operation
 * with the attributes that were in force when it executed — in the unit
 * Cartesian space of X3.110 §5.3.1, before any mapping to pixels. Rasterising
 * that is the host's business, which keeps the MVP boundary exactly where the
 * teletext viewer's is: decode below, render above.
 *
 * Coordinates are unit space throughout: 0 <= x < 1 and 0 <= y < 1, y upwards
 * from the bottom left. The display area is the lower 0,78125 of it (Table D1
 * item 10 and T.101 Table II-3), and the strip above is border that Table D1
 * does not guarantee is visible.
 */

/// The unit screen's y extent that Table D1 item 10 guarantees is visible.
constexpr double kNabtsDisplayAreaHeight = 0.78125;

/// X3.110 §5.3.2.6: colour modes 0, 1 and 2, which decide whether a drawing
/// colour is a value or a colour-map address, and whether a background colour
/// applies.
enum class NabtsColourMode : uint8_t {
  /// Colour value used directly; no background colour (§5.3.2.5).
  kDirect = 0,
  /// Drawing colour is a map address; foreground pixels only.
  kMapped = 1,
  /// Drawing and background colours are both map addresses.
  kMappedWithBackground = 2,
};

/**
 * @brief One colour, as the GRB system of X3.110 §5.3.1 expresses it
 *
 * Three bits per gun is what Table D1 item 5(4) requires — "sixteen
 * simultaneous colours out of a set of 512 obtained by allocating three bits
 * each to G R & B" — so each component is 0 to 7. Held as components rather
 * than a packed value because the transmission order (green, red, blue, by
 * decreasing luminance) is not the order anything renders in.
 */
struct NabtsColour {
  uint8_t green = 0;
  uint8_t red = 0;
  uint8_t blue = 0;
  /// X3.110 §5.3.2.5: a SET COLOR with no operand sets the transparent colour,
  /// which shows lower planes through — the analogue video signal, in a
  /// captioning application. Rendered as black where there is nothing behind.
  bool transparent = false;

  bool operator==(const NabtsColour& other) const {
    return green == other.green && red == other.red && blue == other.blue &&
           transparent == other.transparent;
  }
};

/// Colour-map entries Table D1 item 5(4) requires, and T.101 Table II-3
/// tabulates.
constexpr size_t kNabtsColourMapEntries = 16;

/// X3.110 §5.3.2.4.2 Table 12.
enum class NabtsLineTexture : uint8_t {
  kSolid = 0,
  kDotted = 1,
  kDashed = 2,
  kDottedDashed = 3,
};

/// X3.110 §5.3.2.4.4 Table 13. The four programmable masks are defined by DEF
/// TEXTURE (§6.2.4) and carried in NabtsPageSnapshot::texture_masks.
enum class NabtsTexturePattern : uint8_t {
  kSolid = 0,
  kVerticalHatch = 1,
  kHorizontalHatch = 2,
  kCrossHatch = 3,
  kMaskA = 4,
  kMaskB = 5,
  kMaskC = 6,
  kMaskD = 7,
};

/// X3.110 §5.3.2.3.2 Table 6: rotation counterclockwise about the character
/// field origin.
enum class NabtsCharRotation : uint8_t {
  kNone = 0,
  k90 = 1,
  k180 = 2,
  k270 = 3,
};

/// X3.110 §5.3.2.3.3 Table 7: the direction the cursor advances after a
/// character.
enum class NabtsCharPath : uint8_t {
  kRight = 0,
  kLeft = 1,
  kUp = 2,
  kDown = 3,
};

/// X3.110 §5.3.2.3.8 Table 11.
enum class NabtsCursorStyle : uint8_t {
  kUnderscore = 0,
  kBlock = 1,
  kCrossHair = 2,
  kCustom = 3,
};

/// A point in unit space.
struct NabtsPoint {
  double x = 0.0;
  double y = 0.0;
};

/// A size in unit space. May be negative, which reflects the thing it sizes —
/// X3.110 §5.3.2.2.6 for the logical pel, §5.3.2.3.9 for a character field.
struct NabtsSize {
  double dx = 0.0;
  double dy = 0.0;
};

/// What a display-list entry draws.
enum class NabtsPrimitiveKind : uint8_t {
  /// A visible point, sized by the logical pel (§5.3.3.1).
  kPoint,
  /// A straight line between two points (§5.3.3.2).
  kLine,
  /// A circular arc through three points, or a spline through more
  /// (§5.3.3.3). |points| holds them in order: start, intermediate, end.
  kArc,
  /// An axis-aligned rectangle from an origin and a size (§5.3.3.4).
  kRectangle,
  /// A closed polygon through |points| (§5.3.3.5).
  kPolygon,
  /// A run of colours deposited raster-sequentially in the active field
  /// (§5.3.3.6.3 INCREMENTAL POINT). |incremental_colours| holds them.
  kIncrementalPoints,
  /// One character of a G-set, at |origin| in the character field of |size|.
  kCharacter,
};

/**
 * @brief One drawing operation, with the state that was in force when it ran
 *
 * Self-contained by design: a renderer walks the list front to back and needs
 * no state of its own. That costs a few dozen bytes per entry and buys a
 * display list that can be drawn, re-drawn, clipped or scaled without
 * re-running the interpreter.
 */
struct NabtsPrimitive {
  NabtsPrimitiveKind kind = NabtsPrimitiveKind::kPoint;

  /// Points the primitive needs, in unit space, already resolved from whatever
  /// mix of absolute and relative coordinates the record used.
  std::vector<NabtsPoint> points;

  /// Origin for a rectangle or a character; the first of |points| otherwise.
  NabtsPoint origin;
  /// Extent for a rectangle, or the character field for a character.
  NabtsSize size;

  /// Filled rather than outlined (the "filled" forms of ARC, RECTANGLE and
  /// POLYGON, and INCREMENTAL POLYGON).
  bool filled = false;
  /// X3.110 §5.3.2.4.3: a filled figure whose outline is drawn solid in
  /// nominal black, or in the background colour in colour mode 2.
  bool highlighted = false;

  /// Logical pel in force (§5.3.2.2.6), which is what gives a line its width.
  NabtsSize logical_pel;
  NabtsLineTexture line_texture = NabtsLineTexture::kSolid;
  NabtsTexturePattern texture_pattern = NabtsTexturePattern::kSolid;
  /// Step-and-repeat size for the programmable masks (§5.3.2.4.5).
  NabtsSize texture_mask_size;

  NabtsColourMode colour_mode = NabtsColourMode::kDirect;
  /// Drawing colour, resolved through the colour map where the mode says to.
  NabtsColour colour;
  /// Background colour; meaningful only in colour mode 2.
  NabtsColour background;
  /// X3.110 §6.2.8.1: this primitive is part of a blink process, alternating
  /// between |colour| and nominal black (or the background in colour mode 2).
  bool blinking = false;

  /// kIncrementalPoints only: the colour specifications of §5.3.3.6.3, in
  /// raster order within the active field. Values in colour mode 0 and colour
  /// map addresses in modes 1 and 2, which |colour_mode| distinguishes.
  std::vector<uint8_t> incremental_colours;

  // ---- kCharacter only ----------------------------------------------------

  /// The character's code position within its G-set, 0x20 to 0x7F.
  uint8_t character = 0;
  /// Which G-set the character came from, so a renderer knows which repertoire
  /// to look it up in.
  enum class Repertoire : uint8_t {
    kPrimary,        ///< X3.110 §7.1, the 94 ASCII-positioned graphics
    kSupplementary,  ///< §7.2, accents and symbols
    kMosaic,         ///< §5.4, the six-element block graphics
    kDrcs,           ///< §5.6, defined by DEF DRCS into |drcs_index|
  } repertoire = Repertoire::kPrimary;
  /// Index into NabtsPageSnapshot::drcs when |repertoire| is kDrcs.
  uint8_t drcs_index = 0;

  NabtsCharRotation rotation = NabtsCharRotation::kNone;
  /// Reverse video (§6.2.7.4): the character shape is left undrawn and the
  /// field around it is filled instead.
  bool reverse_video = false;
  /// Underline mode (§6.2.7.15).
  bool underlined = false;
};

/**
 * @brief A DRCS character's storage buffer (X3.110 §6.2.3)
 *
 * DEF DRCS runs presentation code into a buffer rather than onto the screen,
 * and every element the code writes comes on unless it was written in nominal
 * black. The buffer's aspect ratio is the character field's at the moment DEF
 * DRCS arrived, so it is carried with it.
 *
 * The elements are the resolved bitmap rather than the code that made it,
 * because a renderer scaling a DRCS character into a character field needs the
 * former and could not use the latter.
 */
struct NabtsDrcsCharacter {
  /// Code position this was defined at, 0x20 to 0x7F.
  uint8_t code = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  /// Row-major, |width| * |height| entries, true for an element that is on.
  /// The first row is the bottom of the buffer, matching unit space.
  std::vector<bool> elements;

  bool defined() const {
    return width > 0 && height > 0 &&
           elements.size() == static_cast<size_t>(width) * height;
  }
};

/// One programmable texture mask (X3.110 §6.2.4). Table D1 item 5(3)(b)
/// requires 16x16 stored elements, and item 11 keeps that storage outside the
/// shared budget.
struct NabtsTextureMask {
  uint16_t width = 0;
  uint16_t height = 0;
  std::vector<bool> elements;

  bool defined() const {
    return width > 0 && height > 0 &&
           elements.size() == static_cast<size_t>(width) * height;
  }
};

/// Programmable texture masks A to D.
constexpr size_t kNabtsTextureMaskCount = 4;

/// What the interpreter could not do with the record it was given.
struct NabtsDecodeDiagnostics {
  /// Bytes consumed, which on a truncated record is less than it was given.
  uint64_t bytes_read = 0;
  /// Escape sequences naming a set this does not implement, or a null set
  /// (X3.110 §4.3.2). Skipped whole, so the parser stays in step.
  uint64_t unknown_designations = 0;
  /// Control functions recognised but with no effect on a display list — the
  /// transmission and device controls of §6.1.4 and §6.1.5, and the interactive
  /// controls Table D1 marks not applicable to teletext.
  uint64_t ignored_controls = 0;
  /// PDI sequences whose operands ran out before the opcode had what it needed.
  uint64_t truncated_pdis = 0;
  /// Coordinates outside the unit screen, which §5.3.1 makes an error whose
  /// handling is implementation-dependent. Counted and clamped.
  uint64_t out_of_range_coordinates = 0;
  /// Macro invocations that could not run: undefined, or nested past the depth
  /// bound.
  uint64_t unresolved_macros = 0;
  /// Definitions refused because the shared macro and DRCS budget of CEA-516
  /// §8.6.1 and T.101 Table II-3 — 3072 bytes — was full.
  uint64_t storage_refusals = 0;
  /// Bytes of the shared budget in use at the end of the record.
  uint64_t storage_used = 0;
};

/**
 * @brief A decoded NAPLPS presentation record
 *
 * The display list is in execution order, which is also back-to-front paint
 * order: NAPLPS has no z-ordering, so a later primitive covers an earlier one.
 *
 * A value type with no decoder types in its interface, so it crosses the
 * plugin boundary and reaches a presenter unchanged.
 */
struct NabtsPageSnapshot {
  std::vector<NabtsPrimitive> primitives;

  /// The colour map as it stood at the end of the record. A renderer needs it
  /// for colour modes 1 and 2, where a map change is retroactive: §5.3.2.5 has
  /// a write "immediately reflected in the colour of all pixels whose
  /// associated colour map address points to the entry that has been changed",
  /// so the final map is the one that was on screen.
  NabtsColour colour_map[kNabtsColourMapEntries];

  /// DRCS characters the record defined, ascending by code position.
  std::vector<NabtsDrcsCharacter> drcs;
  /// Programmable texture masks A to D.
  NabtsTextureMask texture_masks[kNabtsTextureMaskCount];

  /// Whether the record drew anything at all. A record that only defined
  /// macros or DRCS is legitimately empty.
  bool empty() const { return primitives.empty(); }

  NabtsDecodeDiagnostics diagnostics;
};

/// The default colour map of X3.110 §5.3.2.5.2, as T.101 Table II-3 tabulates
/// it for Data Syntax III: a uniform grey ramp in the low half and eight hues
/// equally spaced around the hue circle in the high half.
void nabts_default_colour_map(NabtsColour (&map)[kNabtsColourMapEntries]);

/// Nominal black and nominal white, which several rules of the standard name
/// specifically (§5.3.2.4.3 highlight, §6.2.3 DRCS elements, §6.2.8.1 blink).
constexpr NabtsColour kNabtsNominalBlack{0, 0, 0, false};
constexpr NabtsColour kNabtsNominalWhite{7, 7, 7, false};

}  // namespace orc

#endif  // ORC_NABTS_PAGE_H
