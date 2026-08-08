/*
 * File:        naplps_interpreter_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NAPLPS interpreter: PDI execution, attribute
 *              inheritance, macros, DRCS and the storage budget
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_interpreter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace orc {
namespace {

/// A code position in the standard's column/row notation.
constexpr uint8_t code(int column, int row) {
  return static_cast<uint8_t>((column << 4) | row);
}

/// A numeric-data byte of the PDI set carrying |payload| in b6-b1 (§5.3.1).
constexpr uint8_t numeric(uint8_t payload) {
  return static_cast<uint8_t>(0x40 | (payload & 0x3F));
}

/// A one-byte coordinate word: three bits of X then three of Y (Figure 11).
constexpr uint8_t coord1(uint8_t x, uint8_t y) {
  return numeric(static_cast<uint8_t>(((x & 0x7) << 3) | (y & 0x7)));
}

/**
 * @brief Builds a presentation record byte by byte
 *
 * Every record starts by invoking the PDI set, because that is what a record
 * drawing anything does: §4.3.1.3 designates PDI into G1, and SO invokes G1.
 */
class Record {
 public:
  /// SO — invoke G1, which by default holds the PDI set.
  Record& pdi() { return byte(0x0E); }
  /// SI — invoke G0, the primary character set.
  Record& text_mode() { return byte(0x0F); }

  Record& byte(uint8_t value) {
    bytes_.push_back(value);
    return *this;
  }
  Record& add(std::initializer_list<uint8_t> values) {
    for (const uint8_t value : values) {
      bytes_.push_back(value);
    }
    return *this;
  }
  /// ESC followed by |rest| — a designation, locking shift or C1 control.
  Record& escape(std::initializer_list<uint8_t> rest) {
    bytes_.push_back(0x1B);
    return add(rest);
  }

  const std::vector<uint8_t>& data() const { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
};

NabtsPageSnapshot run(const Record& record) {
  NaplpsInterpreter interpreter;
  return interpreter.run(record.data());
}

////////////////////////////////////////////////////////////////////////////////////////////
// The reset state (T.101 Table II-3)
////////////////////////////////////////////////////////////////////////////////////////////

// Table II-3's default presentation parameters, field by field, for the ones a
// display list can carry. This is the test the plan asked for: "a freshly reset
// interpreter matches Table II-3 field for field".
TEST(NaplpsInterpreter, AFreshInterpreterMatchesTableII3) {
  NaplpsInterpreter interpreter;
  interpreter.run({});
  const NaplpsState& state = interpreter.state();

  // current-text-position: Table II-3 says "lower left corner", which is the
  // corner of the *character field* rather than of the screen. X3.110 says so
  // three times over — §5.3.2.9.3 sends a reset cursor to "its home position
  // (top left character position in the display area)", §6.1.2.6 and §6.1.2.8
  // home CS and APH to "the upper left character position in the display area,
  // in which the top of the character field coincides with the top boundary",
  // and §6.1.6.5(6) numbers NSR's rows from "the upper leftmost character
  // position" — and Table II-3 itself gives the other two data syntaxes an
  // "upper left corner". Reading it as the bottom of the screen puts every
  // record that opens with text and line feeds, which is how the reference
  // ExtraVision service writes, on the bottom row with every line feed clamped.
  EXPECT_DOUBLE_EQ(state.cursor.x, 0.0);
  EXPECT_DOUBLE_EQ(state.cursor.y,
                   kNabtsDisplayAreaHeight - state.text.character_field.dy);

  // current-foreground-colour: colour = white, mode = direct.
  EXPECT_EQ(state.colour.mode(), NabtsColourMode::kDirect);
  EXPECT_EQ(state.colour.drawing_colour(), kNabtsNominalWhite);

  // flash-blink-state: off.
  EXPECT_FALSE(state.blinking);

  // basic-char-size-state: dx = 1/40, dy = 1/128 ... the table's own figure for
  // dy is 1/128, but X3.110 §5.3.2.3.9 and §6.2.7.8 both give 5/128 for the
  // default and for NORMAL TEXT, and Table II-3's geometric-control-1-state
  // gives the texture mask — "the default character field size" per §5.3.2.4.5
  // — as 1/40, 5/128. So 5/128 is the value two independent statements agree on
  // and 1/128 is a typo in the table.
  EXPECT_DOUBLE_EQ(state.text.character_field.dx, 1.0 / 40.0);
  EXPECT_DOUBLE_EQ(state.text.character_field.dy, 5.0 / 128.0);

  // cursor-control-state: off (invisible).
  EXPECT_FALSE(state.text.cursor_visible);

  // geometric-control-1-state: line texture solid, texture pattern solid,
  // texture mask 1/40 by 5/128, highlight off, logical pel 0,0.
  EXPECT_EQ(state.texture.line_texture, NabtsLineTexture::kSolid);
  EXPECT_EQ(state.texture.pattern, NabtsTexturePattern::kSolid);
  EXPECT_DOUBLE_EQ(state.texture.mask_size.dx, 1.0 / 40.0);
  EXPECT_DOUBLE_EQ(state.texture.mask_size.dy, 5.0 / 128.0);
  EXPECT_FALSE(state.texture.highlight);
  EXPECT_DOUBLE_EQ(state.domain.logical_pel.dx, 0.0);
  EXPECT_DOUBLE_EQ(state.domain.logical_pel.dy, 0.0);

  // general-text-state: char rotation 0, char path right, char spacing 1,
  // cursor underscore, interrow single space.
  EXPECT_EQ(state.text.rotation, NabtsCharRotation::kNone);
  EXPECT_EQ(state.text.path, NabtsCharPath::kRight);
  EXPECT_EQ(state.text.intercharacter_spacing, 0u);
  EXPECT_EQ(state.text.interrow_spacing, 0u);
  EXPECT_EQ(state.text.cursor_style, NabtsCursorStyle::kUnderscore);

  // DRCS-definition-state and macro-definition-state: none defined.
  EXPECT_EQ(state.storage_used(), 0u);

  // Macro-Seg-Memory-Limit and DRCS-Memory-Limit: 3072 bytes, shared.
  EXPECT_EQ(kNaplpsSharedStorageBytes, 3072u);

  // Default operand lengths: §5.3.2.2.2 one byte, §5.3.2.2.3 three bytes,
  // §5.3.2.2.4 two-dimensional.
  EXPECT_EQ(state.domain.format.single_value_bytes, 1u);
  EXPECT_EQ(state.domain.format.multi_value_bytes, 3u);
  EXPECT_FALSE(state.domain.format.three_dimensional);
}

TEST(NaplpsInterpreter, AnEmptyRecordDrawsNothing) {
  const NabtsPageSnapshot snapshot = run(Record{});
  EXPECT_TRUE(snapshot.empty());
  EXPECT_EQ(snapshot.diagnostics.bytes_read, 0u);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Geometric primitives
////////////////////////////////////////////////////////////////////////////////////////////

// §5.3.3.1.4: POINT (Absolute, Visible) "sets the drawing point to the absolute
// coordinates specified and draws a point".
TEST(NaplpsInterpreter, DrawsAnAbsolutePoint) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      // One-byte multi-value operands, so a coordinate is one byte.
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b010, 0b011));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const NabtsPrimitive& point = snapshot.primitives[0];
  EXPECT_EQ(point.kind, NabtsPrimitiveKind::kPoint);
  EXPECT_DOUBLE_EQ(point.origin.x, 0.5);   // 010 over three bits
  EXPECT_DOUBLE_EQ(point.origin.y, 0.75);  // 011
}

// §5.3.3.1.2: POINT SET (Absolute, Invisible) "sets the drawing point ... A
// point is not drawn."
TEST(NaplpsInterpreter, PointSetMovesTheDrawingPointWithoutDrawing) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .byte(coord1(0b010, 0b010));

  NaplpsInterpreter interpreter;
  const NabtsPageSnapshot snapshot = interpreter.run(record.data());
  EXPECT_TRUE(snapshot.primitives.empty());
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.x, 0.5);
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.y, 0.5);
}

// §5.3.3.2.2: LINE (Absolute) — "The start point is the current drawing point.
// The end point is specified in absolute coordinates." And §5.3.3.2.1: "At the
// completion of drawing a line, the drawing point is coincident with the end
// point."
TEST(NaplpsInterpreter, DrawsALineFromTheDrawingPointAndLeavesItAtTheEnd) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .byte(coord1(0b001, 0b001))
      .byte(static_cast<uint8_t>(NaplpsPdi::kLineAbs))
      .byte(coord1(0b011, 0b010));

  NaplpsInterpreter interpreter;
  const NabtsPageSnapshot snapshot = interpreter.run(record.data());
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const NabtsPrimitive& line = snapshot.primitives[0];
  EXPECT_EQ(line.kind, NabtsPrimitiveKind::kLine);
  ASSERT_EQ(line.points.size(), 2u);
  EXPECT_DOUBLE_EQ(line.points[0].x, 0.25);
  EXPECT_DOUBLE_EQ(line.points[1].x, 0.75);
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.x, 0.75);
}

// §5.3.3.4.2: RECTANGLE (Outlined) takes the width and height as a coordinate
// word; §5.3.3.4.1 leaves the drawing point "altered in x only".
TEST(NaplpsInterpreter, DrawsARectangleAndAdvancesTheDrawingPointInXOnly) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .byte(coord1(0b010, 0b001));

  NaplpsInterpreter interpreter;
  const NabtsPageSnapshot snapshot = interpreter.run(record.data());
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const NabtsPrimitive& rect = snapshot.primitives[0];
  EXPECT_EQ(rect.kind, NabtsPrimitiveKind::kRectangle);
  EXPECT_TRUE(rect.filled);
  EXPECT_DOUBLE_EQ(rect.size.dx, 0.5);
  EXPECT_DOUBLE_EQ(rect.size.dy, 0.25);
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.x, 0.5);
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.y, 0.0);
}

// §5.3.3.5.1: a polygon's vertices are relative displacements from the previous
// vertex, with "implicit closure between the start point and the last vertex".
TEST(NaplpsInterpreter, DrawsAPolygonFromRelativeVertices) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPolyFilled))
      .byte(coord1(0b010, 0b000))   // +0,5 in x
      .byte(coord1(0b000, 0b010))   // +0,5 in y
      .byte(coord1(0b110, 0b000));  // -0,5 in x

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const NabtsPrimitive& poly = snapshot.primitives[0];
  EXPECT_EQ(poly.kind, NabtsPrimitiveKind::kPolygon);
  EXPECT_TRUE(poly.filled);
  // Start plus three vertices; the closure back to the start is implicit and so
  // is not a fourth point.
  ASSERT_EQ(poly.points.size(), 4u);
  EXPECT_DOUBLE_EQ(poly.points[1].x, 0.5);
  EXPECT_DOUBLE_EQ(poly.points[2].y, 0.5);
  EXPECT_DOUBLE_EQ(poly.points[3].x, 0.0);
}

// §5.3.3.3.1: "an arc is drawn from a start point to an end point through an
// intermediate point on the arc", the intermediate relative to the start and
// the end relative to the intermediate.
TEST(NaplpsInterpreter, DrawsAnArcThroughThreeControlPoints) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kArcOutlined))
      .byte(coord1(0b001, 0b001))
      .byte(coord1(0b001, 0b111));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const NabtsPrimitive& arc = snapshot.primitives[0];
  EXPECT_EQ(arc.kind, NabtsPrimitiveKind::kArc);
  EXPECT_FALSE(arc.filled);
  ASSERT_EQ(arc.points.size(), 3u) << "start, intermediate, end";
}

// §5.3.2.2.5: "If an operand following an opcode is longer than the length
// previously specified ... it is taken as an indication to repeat the execution
// of the opcode with the subsequent numeric data taken as new operands."
TEST(NaplpsInterpreter, RepeatsAnOpcodeWhoseOperandRunIsLong) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      .byte(coord1(0b010, 0b010))
      .byte(coord1(0b011, 0b011));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.x, 0.25);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.x, 0.5);
  EXPECT_DOUBLE_EQ(snapshot.primitives[2].origin.x, 0.75);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Attributes
////////////////////////////////////////////////////////////////////////////////////////////

// §5.3.2.2.2 Table 4 and §5.3.2.2.3 Table 5, and the note in §5.3.2.2.6 that
// the new multi-value length "applies to the multi-value logical pel size
// operand of that DOMAIN command".
TEST(NaplpsInterpreter, DomainSetsTheOperandLengthsAndItsOwnPelUsesTheNewOne) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      // b2 b1 = 01 → two-byte single values; b5 b4 b3 = 000 → one-byte
      // multi-values; b6 = 0 → two-dimensional.
      .byte(numeric(0b000001))
      // Read on the new one-byte length: dx = 001 = +0,25, dy = 001.
      .byte(coord1(0b001, 0b001));

  NaplpsInterpreter interpreter;
  interpreter.run(record.data());
  const NaplpsState& state = interpreter.state();
  EXPECT_EQ(state.domain.format.single_value_bytes, 2u);
  EXPECT_EQ(state.domain.format.multi_value_bytes, 1u);
  EXPECT_DOUBLE_EQ(state.domain.logical_pel.dx, 0.25);
  EXPECT_DOUBLE_EQ(state.domain.logical_pel.dy, 0.25);
}

// §5.3.2.4: the texture attributes are inherited by every subsequent primitive,
// which is what makes the display list self-contained.
TEST(NaplpsInterpreter, APrimitiveCarriesTheAttributesInForceWhenItRan) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kTexture))
      // b2 b1 = 10 dashed; b3 = 1 highlight; b6 b5 b4 = 011 cross-hatching.
      .byte(numeric(0b011110))
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .byte(coord1(0b001, 0b001))
      // Then change the texture and draw again.
      .byte(static_cast<uint8_t>(NaplpsPdi::kTexture))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .byte(coord1(0b001, 0b001));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_EQ(snapshot.primitives[0].line_texture, NabtsLineTexture::kDashed);
  EXPECT_TRUE(snapshot.primitives[0].highlighted);
  EXPECT_EQ(snapshot.primitives[0].texture_pattern,
            NabtsTexturePattern::kCrossHatch);
  // The second inherited the new state, not the old — so the first kept its
  // own.
  EXPECT_EQ(snapshot.primitives[1].line_texture, NabtsLineTexture::kSolid);
  EXPECT_FALSE(snapshot.primitives[1].highlighted);
}

// §5.3.2.6: no operand selects colour mode 0, one selects mode 1, two select
// mode 2 — and in modes 1 and 2 a SET COLOR writes the map at the selected
// address, which a later primitive then draws in.
TEST(NaplpsInterpreter, AColourMapWriteIsVisibleToTheNextPrimitive) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      // SELECT COLOR with one operand: mode 1, drawing address from the high
      // four bits of the one-byte operand — 0011xx → address 3.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      // SET COLOR writes entry 3. One byte gives two bits per gun: GRB = 100,
      // GRB = 000 → green 10, red 10, blue 00, i.e. 2 of the 3 a two-bit
      // operand can express.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b110000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].colour_mode, NabtsColourMode::kMapped);
  // §5.3.2.5.1: "For each primary, the maximum color fraction attainable, given
  // the number of bits specified in the color value operand, shall be
  // interpreted as full intensity and intermediate values shall be equally
  // distributed between zero and full intensity." Over two bits that is
  // 0, 7/3, 14/3, 7 — so 2 of 3 is 5 of 7, not the 4 a shift-and-zero-fill
  // would give. The clause matters: zero-filling makes white unreachable in a
  // one-byte operand, and the reference ExtraVision service sets white with
  // exactly one byte.
  EXPECT_EQ(snapshot.primitives[0].colour.green, 5u);
  EXPECT_EQ(snapshot.primitives[0].colour.red, 5u);
  EXPECT_EQ(snapshot.primitives[0].colour.blue, 0u);
  // And the map the renderer is handed carries it, because §5.3.2.5 makes a map
  // change retroactive for every pixel already pointing at that entry.
  EXPECT_EQ(snapshot.colour_map[3].green, 5u);
}

// The regression the ExtraVision logo exposed. DOMAIN may declare a multi-value
// length of three bytes and the service still send SET COLOR one byte at a
// time; §5.3.2.5.1 zero-fills the rest. Requiring the declared length dropped
// the write silently, so the CBS eye was drawn in whatever colour came before
// it — the page background — and vanished.
TEST(NaplpsInterpreter, AShortColourOperandStillSetsTheColour) {
  Record record;
  record
      .pdi()
      // DOMAIN with a three-byte multi-value length.
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000010))
      // A one-byte SET COLOR: all six payload bits set is full intensity.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b111111))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].colour, kNabtsNominalWhite)
      << "a one-byte SET COLOR was dropped, so the primitive kept the previous "
         "colour";
}

// The same clause from the other end: a colour operand longer than the map can
// hold "is truncated and only the most significant bits are used".
TEST(NaplpsInterpreter, ALongColourOperandIsTruncatedToTheMostSignificantBits) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000010))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      // Three bytes: green takes b6 and b3 of each, so 1,0 1,0 1,0 = 0b101010
      // over six bits, truncated to its top three = 0b101 = 5.
      .byte(numeric(0b100000))
      .byte(numeric(0b100000))
      .byte(numeric(0b100000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].colour.green, 5u);
  EXPECT_EQ(snapshot.primitives[0].colour.red, 0u);
  EXPECT_EQ(snapshot.primitives[0].colour.blue, 0u);
}

// §5.3.2.5.1: "If no operand follows a SET COLOR opcode, the transparent color
// is set", which in a captioning application is what lets the video through.
TEST(NaplpsInterpreter, SetColourWithNoOperandSelectsTransparent) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_TRUE(snapshot.primitives[0].colour.transparent);
}

// §5.3.2.9: RESET is selective, and byte 2 b6 is the bit that clears the DRCS
// set. A RESET with no operands is "as if it had been sent with bits b6 to b1
// in both bytes set equal to 0" — that is, it does nothing.
TEST(NaplpsInterpreter, ResetWithNoOperandsChangesNothing) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000001))  // two-byte single values
      .byte(static_cast<uint8_t>(NaplpsPdi::kReset));

  NaplpsInterpreter interpreter;
  interpreter.run(record.data());
  // The DOMAIN change survived, because RESET was told to reset nothing.
  EXPECT_EQ(interpreter.state().domain.format.single_value_bytes, 2u);
}

TEST(NaplpsInterpreter, ResetByte1Bit1RestoresTheDomainDefaults) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000001))
      .byte(static_cast<uint8_t>(NaplpsPdi::kReset))
      .byte(numeric(0b000001));  // byte 1 b1 = 1

  NaplpsInterpreter interpreter;
  interpreter.run(record.data());
  EXPECT_EQ(interpreter.state().domain.format.single_value_bytes, 1u);
}

// §5.3.2.9.2 Table 15: clearing the display area means everything drawn before
// it is gone, which for a display list means the primitives are dropped.
TEST(NaplpsInterpreter, ResetClearingTheDisplayAreaDropsWhatWasDrawn) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      .byte(static_cast<uint8_t>(NaplpsPdi::kReset))
      // Byte 1 b6 b5 b4 = 001: display area to nominal black.
      .byte(numeric(0b001000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b010, 0b010));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u) << "the pre-clear point survived";
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.x, 0.5);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Text
////////////////////////////////////////////////////////////////////////////////////////////

// A character from the primary set is emitted at the cursor in the current
// character field, and the cursor advances along the character path
// (§5.3.2.3.3).
TEST(NaplpsInterpreter, EmitsCharactersAndAdvancesAlongTheCharacterPath) {
  Record record;
  record.text_mode().add({'A', 'B', 'C'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(snapshot.primitives[i].kind, NabtsPrimitiveKind::kCharacter);
    EXPECT_EQ(snapshot.primitives[i].repertoire,
              NabtsPrimitive::Repertoire::kPrimary);
    EXPECT_DOUBLE_EQ(snapshot.primitives[i].size.dx, 1.0 / 40.0);
  }
  EXPECT_EQ(snapshot.primitives[0].character, 'A');
  EXPECT_EQ(snapshot.primitives[2].character, 'C');
  // Default intercharacter spacing is 1, so the fields abut (§5.3.2.3.4).
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.x, 1.0 / 40.0);
  EXPECT_DOUBLE_EQ(snapshot.primitives[2].origin.x, 2.0 / 40.0);
}

////////////////////////////////////////////////////////////////////////////////////////////
// The format effectors (§6.1.2)
////////////////////////////////////////////////////////////////////////////////////////////

// The regression this section exists for. §6.1.2 defines four *different*
// movements relative to the character path, and an implementation that treats
// them all as "advance along the path" draws every row of a page on top of the
// last: the ExtraVision news pages came out with each line overprinting the one
// before it, one character out of step.
TEST(NaplpsInterpreter, APRThenAPDStartsANewRowRatherThanOverprinting) {
  // CS first: the reset cursor is at the lower left corner (T.101 Table II-3),
  // where there is no room below to move into. A record that draws rows homes
  // the cursor first, which is what CS does (§6.1.2.6).
  Record record;
  record.text_mode()
      .byte(0x0C)  // CS — home to the top left of the display area
      .add({'A', 'B'})
      .byte(0x0D)  // APR — back to the first character position of the row
      .byte(0x0A)  // APD — down one row
      .add({'C'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  const NabtsPoint& first = snapshot.primitives[0].origin;
  const NabtsPoint& third = snapshot.primitives[2].origin;
  // Back to the left edge...
  EXPECT_DOUBLE_EQ(third.x, first.x);
  // ...and one row down, which is the whole point.
  EXPECT_LT(third.y, first.y);
  EXPECT_DOUBLE_EQ(third.y, first.y - 5.0 / 128.0);
}

// §6.1.2.3: APD moves "a distance equal to the interrow space lying
// perpendicular to the character path in a direction perpendicular to the
// character path (-90 degrees)". Default interrow spacing is 1, so that is one
// character field height.
TEST(NaplpsInterpreter, APDMovesDownOneRowAndLeavesTheColumnAlone) {
  Record record;
  record.text_mode().byte(0x0C).add({'A'}).byte(0x0A).add({'B'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  // The character before it advanced one field along the path; APD adds no
  // horizontal movement of its own.
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.x, 1.0 / 40.0);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.y,
                   snapshot.primitives[0].origin.y - 5.0 / 128.0);
}

// §6.1.2.5: APU is the same movement 180 degrees round.
TEST(NaplpsInterpreter, APUMovesUpOneRow) {
  Record record;
  record.text_mode()
      .byte(0x0C)
      .add({'A'})
      .byte(0x0A)  // down, so there is room to come back up
      .byte(0x0B)  // APU
      .add({'B'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.y,
                   snapshot.primitives[0].origin.y);
}

// §6.1.2.1: APB moves back along the character path, so the character after it
// lands on the one before — which is how a record overstrikes.
TEST(NaplpsInterpreter, APBMovesBackAlongTheCharacterPath) {
  Record record;
  record.text_mode().byte(0x0C).add({'A', 'B'}).byte(0x08).add({'C'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  // 'B' was drawn at one field in and the cursor left at two; APB puts it back
  // to one, so 'C' lands on 'B'.
  EXPECT_DOUBLE_EQ(snapshot.primitives[2].origin.x,
                   snapshot.primitives[1].origin.x);
  EXPECT_DOUBLE_EQ(snapshot.primitives[2].origin.y,
                   snapshot.primitives[1].origin.y);
}

// The regression the ExtraVision service exposed. A record that simply writes
// text with CR and LF — no CS, no APS, no NSR — must run *down* the screen from
// the top. Homing the cursor at the bottom left clamps every line feed, and the
// whole page piles onto one row.
TEST(NaplpsInterpreter, ARecordThatOpensWithTextStartsAtTheTopOfTheScreen) {
  Record record;
  record.text_mode()
      .add({'A'})
      .add({0x0D, 0x0A})  // APR APD — carriage return, line feed
      .add({'B'})
      .add({0x0D, 0x0A})
      .add({'C'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  const double field = snapshot.primitives[0].size.dy;
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.y,
                   kNabtsDisplayAreaHeight - field);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.y,
                   kNabtsDisplayAreaHeight - 2.0 * field);
  EXPECT_DOUBLE_EQ(snapshot.primitives[2].origin.y,
                   kNabtsDisplayAreaHeight - 3.0 * field);
  // Nothing was clamped back into the screen on the way.
  EXPECT_EQ(snapshot.diagnostics.out_of_range_coordinates, 0u);
}

// §6.1.6.5(6): NSR "can be used as an alternative means to position the
// cursor" — the two bytes after it are a row and a column when both come from
// columns 4 to 7, and they are consumed rather than drawn. The reference
// ExtraVision records open with exactly this, and executing the pair as text
// left the cursor wherever it was and put two stray glyphs on the page.
TEST(NaplpsInterpreter, NSRConsumesItsRowAndColumnAddress) {
  Record record;
  // NSR, row 2, column 3 — both bytes from column 4 of the in-use table.
  record.text_mode().byte(0x1F).byte(0x42).byte(0x43).add({'X'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u) << "the address bytes were drawn";
  EXPECT_EQ(snapshot.primitives[0].character, 'X');
  const double dx = snapshot.primitives[0].size.dx;
  const double dy = snapshot.primitives[0].size.dy;
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.x, 3.0 * dx);
  // Row 0 is the *upper* leftmost character position here — the opposite end
  // from APS's own numbering in §6.1.2.4.
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.y,
                   kNabtsDisplayAreaHeight - 3.0 * dy);
}

// §6.1.6.5(6): "If the two bytes are from columns 2 and 3 (or columns 10 and
// 11), they are ignored" — consumed and discarded, so nothing is drawn for
// them and the cursor stays where the reset put it.
TEST(NaplpsInterpreter, NSRIgnoresAnAddressFromColumnsTwoAndThree) {
  Record record;
  record.text_mode().byte(0x1F).add({'!', '"'}).add({'X'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u) << "the ignored bytes were drawn";
  EXPECT_EQ(snapshot.primitives[0].character, 'X');
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.x, 0.0);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.y,
                   kNabtsDisplayAreaHeight - snapshot.primitives[0].size.dy);
}

// A C0 control after NSR "terminates the NSR sequence" and is executed, so it
// must not be swallowed as half an address.
TEST(NaplpsInterpreter, NSRLeavesAFollowingControlToBeExecuted) {
  Record record;
  // NSR, then APD (line feed), then a character: the line feed must move the
  // cursor down a row rather than being read as a row address.
  record.text_mode().byte(0x1F).byte(0x0A).add({'X'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_DOUBLE_EQ(
      snapshot.primitives[0].origin.y,
      kNabtsDisplayAreaHeight - 2.0 * snapshot.primitives[0].size.dy);
}

// §6.1.2.2: APF is the forward movement, i.e. a tab of one character position.
TEST(NaplpsInterpreter, APFSkipsOneCharacterPosition) {
  Record record;
  record.text_mode().byte(0x0C).add({'A'}).byte(0x09).add({'B'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  // One field for the character, one for the APF.
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.x, 2.0 / 40.0);
}

// All four are defined *relative to the character path*, so a record that
// turned the path through 90 degrees gets its rows turned with it (§5.3.2.3.3
// Table 7 and §5.3.2.3.5). With the path running down the screen, -90 degrees
// from it is to the left.
TEST(NaplpsInterpreter, TheFormatEffectorsFollowTheCharacterPath) {
  // TEXT byte 1 carries the character path in b4-b3 (§5.3.2.3.1); Table 7
  // code 2 is "up". Started from the reset cursor at the lower left, so both
  // the advance and the APD move away from the edges rather than into them.
  Record record;
  record.pdi()
      .byte(code(2, 2))                             // TEXT (§5.3.2.3, Fig. 13)
      .byte(numeric(static_cast<uint8_t>(2 << 2)))  // path = up
      .byte(numeric(0))
      .text_mode()
      .add({'A'})
      .byte(0x0A)  // APD: -90 degrees from an upward path, i.e. to the right
      .add({'B'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  // 'A' advanced the cursor up the screen — the path — by the field height,
  // and APD then stepped across the path by the field width rather than
  // further along it.
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.y,
                   snapshot.primitives[0].origin.y + 5.0 / 128.0);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.x,
                   snapshot.primitives[0].origin.x + 1.0 / 40.0);
}

// §6.2.7.6-10: each C1 text control sets the character field to a stated size.
TEST(NaplpsInterpreter, TheC1TextControlsSetTheStatedCharacterFieldSizes) {
  struct Case {
    uint8_t final_byte;
    double dx;
    double dy;
  };
  const Case cases[] = {
      {code(4, 10), 1.0 / 80.0, 5.0 / 128.0},  // SMALL TEXT §6.2.7.6
      {code(4, 11), 1.0 / 32.0, 3.0 / 64.0},   // MEDIUM TEXT §6.2.7.7
      {code(4, 12), 1.0 / 40.0, 5.0 / 128.0},  // NORMAL TEXT §6.2.7.8
      {code(4, 13), 1.0 / 40.0, 5.0 / 64.0},   // DOUBLE HEIGHT §6.2.7.9
      {code(4, 15), 1.0 / 20.0, 5.0 / 64.0},   // DOUBLE SIZE §6.2.7.10
  };

  for (const Case& test_case : cases) {
    Record record;
    record.text_mode().escape({test_case.final_byte}).byte('X');
    const NabtsPageSnapshot snapshot = run(record);
    ASSERT_EQ(snapshot.primitives.size(), 1u) << +test_case.final_byte;
    EXPECT_DOUBLE_EQ(snapshot.primitives[0].size.dx, test_case.dx)
        << +test_case.final_byte;
    EXPECT_DOUBLE_EQ(snapshot.primitives[0].size.dy, test_case.dy)
        << +test_case.final_byte;
  }
}

// §6.2.7.4 and §6.2.7.15: reverse video and underline are text attributes a
// character inherits.
TEST(NaplpsInterpreter, ReverseVideoAndUnderlineAreCarriedOnTheCharacter) {
  Record record;
  record.text_mode()
      .escape({code(4, 8)})  // REVERSE VIDEO
      .escape({code(5, 9)})  // UNDERLINE START
      .byte('A')
      .escape({code(4, 9)})   // NORMAL VIDEO
      .escape({code(5, 10)})  // UNDERLINE STOP
      .byte('B');

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_TRUE(snapshot.primitives[0].reverse_video);
  EXPECT_TRUE(snapshot.primitives[0].underlined);
  EXPECT_FALSE(snapshot.primitives[1].reverse_video);
  EXPECT_FALSE(snapshot.primitives[1].underlined);
}

// §6.2.7.2: REPEAT repeats the preceding character a stated number of
// additional times.
TEST(NaplpsInterpreter, RepeatEmitsTheStatedNumberOfAdditionalCharacters) {
  Record record;
  record.text_mode()
      .byte('X')
      .escape({code(4, 6)})  // REPEAT
      .byte(numeric(3));     // three more

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 4u) << "one plus three repeats";
  for (const NabtsPrimitive& primitive : snapshot.primitives) {
    EXPECT_EQ(primitive.character, 'X');
  }
}

// §4.3.2: a single shift lasts one character, so the character after it comes
// from the locked set again.
TEST(NaplpsInterpreter, ASingleShiftAppliesToExactlyOneCharacter) {
  Record record;
  record.text_mode()
      .byte('A')
      .byte(0x19)  // SS2 — G2, the supplementary set
      .byte('B')
      .byte('C');

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  EXPECT_EQ(snapshot.primitives[0].repertoire,
            NabtsPrimitive::Repertoire::kPrimary);
  EXPECT_EQ(snapshot.primitives[1].repertoire,
            NabtsPrimitive::Repertoire::kSupplementary);
  EXPECT_EQ(snapshot.primitives[2].repertoire,
            NabtsPrimitive::Repertoire::kPrimary);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Macros (§6.2.2)
////////////////////////////////////////////////////////////////////////////////////////////

// The plan's acceptance: "a macro defined and invoked expands to the same
// display list as its inline form".
TEST(NaplpsInterpreter, AnInvokedMacroExpandsToItsInlineForm) {
  // The body: switch to text and draw two characters.
  const std::vector<uint8_t> body = {0x0F, 'H', 'i'};

  Record inline_form;
  inline_form.add({body[0], body[1], body[2]});
  const NabtsPageSnapshot expected = run(inline_form);

  Record macro_form;
  macro_form
      // DEF MACRO at name 2/0, body, END.
      .escape({code(4, 0)})
      .byte(code(2, 0))
      .add({body[0], body[1], body[2]})
      .escape({code(4, 5)})
      // Designate the macro set into G1 and invoke it, then name 2/0.
      .escape({code(2, 9), code(7, 10)})
      .byte(0x0E)
      .byte(code(2, 0));
  const NabtsPageSnapshot actual = run(macro_form);

  ASSERT_EQ(actual.primitives.size(), expected.primitives.size());
  for (size_t i = 0; i < expected.primitives.size(); ++i) {
    EXPECT_EQ(actual.primitives[i].character, expected.primitives[i].character)
        << "primitive " << i;
    EXPECT_DOUBLE_EQ(actual.primitives[i].origin.x,
                     expected.primitives[i].origin.x)
        << "primitive " << i;
  }
  EXPECT_EQ(actual.diagnostics.unresolved_macros, 0u);
}

// §6.2.2.1: "A null macro definition ... causes that macro to be deleted."
TEST(NaplpsInterpreter, ANullMacroDefinitionDeletesTheMacro) {
  Record record;
  record.escape({code(4, 0)})
      .byte(code(2, 0))
      .add({0x0F, 'X'})
      .escape({code(4, 5)})
      // Redefine it with nothing between the name and END.
      .escape({code(4, 0)})
      .byte(code(2, 0))
      .escape({code(4, 5)})
      // Invoke it: there is nothing there now.
      .escape({code(2, 9), code(7, 10)})
      .byte(0x0E)
      .byte(code(2, 0));

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_TRUE(snapshot.primitives.empty());
  EXPECT_EQ(snapshot.diagnostics.unresolved_macros, 1u);
}

// §6.2.2.1 lists DEF MACRO among the controls that terminate a definition, so
// one definition can follow another with no END between them.
// The second SO before the second macro name is not decoration. A macro's
// effects on the presentation state persist past its expansion — §6.2.3 says so
// of a DRCS definition in as many words, "Any changes to the state of the
// receiving device, such as character field size, in-use G-sets, etc, shall
// persist", and a macro is no different — so the SI inside the first macro's
// body leaves the primary set invoked. Naming the second macro without
// re-invoking the macro set draws '!' instead, which is what an earlier version
// of this test did.
TEST(NaplpsInterpreter, OneMacroDefinitionTerminatesThePrevious) {
  Record record;
  record.escape({code(4, 0)})
      .byte(code(2, 0))
      .add({0x0F, 'A'})
      .escape({code(4, 0)})  // terminates the first and opens the second
      .byte(code(2, 1))
      .add({0x0F, 'B'})
      .escape({code(4, 5)})
      .escape({code(2, 9), code(7, 10)})
      .byte(0x0E)
      .byte(code(2, 0))
      .byte(0x0E)  // the first macro left G0 invoked; go back to the macro set
      .byte(code(2, 1));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_EQ(snapshot.primitives[0].character, 'A');
  EXPECT_EQ(snapshot.primitives[1].character, 'B');
}

// The property the test above depends on, stated on its own: a macro's changes
// to the in-use table outlive its expansion.
TEST(NaplpsInterpreter, AMacrosStateChangesPersistAfterItReturns) {
  Record record;
  record
      // A macro whose whole body is "invoke G0".
      .escape({code(4, 0)})
      .byte(code(2, 0))
      .byte(0x0F)
      .escape({code(4, 5)})
      // Invoke the macro set, run the macro, then send a graphic byte with no
      // invocation of its own.
      .escape({code(2, 9), code(7, 10)})
      .byte(0x0E)
      .byte(code(2, 0))
      .byte('Z');

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u)
      << "the macro's SI did not outlive its expansion";
  EXPECT_EQ(snapshot.primitives[0].character, 'Z');
  EXPECT_EQ(snapshot.primitives[0].repertoire,
            NabtsPrimitive::Repertoire::kPrimary);
}

// §6.2.2.1: the definition is stored and *not* executed, so a DEF MACRO body
// draws nothing at definition time.
TEST(NaplpsInterpreter, DefMacroStoresWithoutExecuting) {
  Record record;
  record.escape({code(4, 0)})
      .byte(code(2, 0))
      .add({0x0F, 'X'})
      .escape({code(4, 5)});

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_TRUE(snapshot.primitives.empty());
}

// §6.2.2.2: DEFP MACRO is the exception — its body is "simultaneously executed
// and stored".
TEST(NaplpsInterpreter, DefpMacroExecutesAsItStores) {
  Record record;
  record.escape({code(4, 1)})
      .byte(code(2, 0))
      .add({0x0F, 'X'})
      .escape({code(4, 5)});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].character, 'X');
}

// §6.2.2.3: a transmit macro "when called, [is] not executed, but [is]
// transmitted in their entirety to the host". There is no host here.
TEST(NaplpsInterpreter, ATransmitMacroDrawsNothingWhenInvoked) {
  Record record;
  record
      .escape({code(4, 2)})  // DEFT MACRO
      .byte(code(2, 0))
      .add({0x0F, 'X'})
      .escape({code(4, 5)})
      .escape({code(2, 9), code(7, 10)})
      .byte(0x0E)
      .byte(code(2, 0));

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_TRUE(snapshot.primitives.empty());
}

// The standard bounds macro nesting only by memory, so a self-invoking macro is
// expressible. It is capped and counted rather than followed.
TEST(NaplpsInterpreter, BoundsMacroRecursionRatherThanFollowingIt) {
  Record record;
  record.escape({code(4, 0)})
      .byte(code(2, 0))
      // Body: designate and invoke the macro set, then invoke itself.
      .escape({code(2, 9), code(7, 10)})
      .byte(0x0E)
      .byte(code(2, 0))
      .escape({code(4, 5)})
      .escape({code(2, 9), code(7, 10)})
      .byte(0x0E)
      .byte(code(2, 0));

  const NabtsPageSnapshot snapshot = run(record);
  // It stopped, which is the point of the test, and said so.
  EXPECT_GT(snapshot.diagnostics.unresolved_macros, 0u);
}

////////////////////////////////////////////////////////////////////////////////////////////
// DRCS and the storage budget
////////////////////////////////////////////////////////////////////////////////////////////

// §6.2.3: the presentation code following DEF DRCS is executed into a storage
// buffer rather than onto the screen, and every element it writes comes on.
TEST(NaplpsInterpreter, DefDrcsDrawsIntoABufferRatherThanTheDisplay) {
  Record record;
  record
      .escape({code(4, 3)})  // DEF DRCS
      .byte(code(2, 0))
      .pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .byte(coord1(0b011, 0b011))
      .escape({code(4, 5)});  // END

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_TRUE(snapshot.primitives.empty())
      << "a DRCS definition must not draw on the display";
  ASSERT_EQ(snapshot.drcs.size(), 1u);
  EXPECT_EQ(snapshot.drcs[0].code, code(2, 0));
  EXPECT_TRUE(snapshot.drcs[0].defined());
  // Something was switched on.
  const auto on = std::count(snapshot.drcs[0].elements.begin(),
                             snapshot.drcs[0].elements.end(), true);
  EXPECT_GT(on, 0);
}

// §6.2.3: "When the current DRCS downloading operation is terminated by another
// DEF DRCS command, the next character of the DRCS G-set (ie, in the circular
// sequence 2/0, 2/1, ... 7/15, 2/0 ...) is defined by the presentation layer
// code immediately following this new DEF DRCS command."
TEST(NaplpsInterpreter, ADefDrcsTerminatingAnotherAdvancesThroughTheGSet) {
  Record record;
  record.escape({code(4, 3)})
      .byte(code(2, 0))
      .pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .byte(coord1(0b011, 0b011))
      // A second DEF DRCS with a code, which is the ordinary form.
      .escape({code(4, 3)})
      .byte(code(2, 1))
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .byte(coord1(0b011, 0b011))
      .escape({code(4, 5)});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.drcs.size(), 2u);
  EXPECT_EQ(snapshot.drcs[0].code, code(2, 0));
  EXPECT_EQ(snapshot.drcs[1].code, code(2, 1));
}

// Table D1 item 5(3)(b): the four programmable texture masks are 16 by 16
// stored elements, and item 11 keeps that storage outside the shared budget.
TEST(NaplpsInterpreter, DefTextureDefinesA16By16Mask) {
  Record record;
  record
      .escape({code(4, 4)})  // DEF TEXTURE
      .byte(code(4, 1))      // mask A
      .pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .byte(coord1(0b011, 0b011))
      .escape({code(4, 5)});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_TRUE(snapshot.texture_masks[0].defined());
  EXPECT_EQ(snapshot.texture_masks[0].width, 16u);
  EXPECT_EQ(snapshot.texture_masks[0].height, 16u);
  // The mask storage is not part of the shared budget.
  EXPECT_EQ(snapshot.diagnostics.storage_used, 0u);
}

// §6.2.4: "If bits b7 to b1 of the character following the DEF TEXTURE control
// are not in the range 4/1 to 4/4, the entire command ... is executed as a null
// operation."
TEST(NaplpsInterpreter, DefTextureRefusesAMaskNameOutsideTheRange) {
  Record record;
  record.escape({code(4, 4)}).byte(code(5, 0)).escape({code(4, 5)});

  const NabtsPageSnapshot snapshot = run(record);
  for (const NabtsTextureMask& mask : snapshot.texture_masks) {
    EXPECT_FALSE(mask.defined());
  }
}

// The plan's acceptance: "the storage budget is enforced rather than exceeded".
// CEA-516 §8.6.1 and T.101 Table II-3 both give 3072 bytes, shared between
// macro definitions and DRCS.
TEST(NaplpsInterpreter, EnforcesTheSharedStorageBudget) {
  // A macro body just under the whole budget, then another that cannot fit.
  const size_t big = kNaplpsSharedStorageBytes - 8;

  Record record;
  record.escape({code(4, 0)}).byte(code(2, 0));
  for (size_t i = 0; i < big; ++i) {
    record.byte('A');
  }
  record.escape({code(4, 0)}).byte(code(2, 1));
  for (size_t i = 0; i < 64; ++i) {
    record.byte('B');
  }
  record.escape({code(4, 5)});

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_LE(snapshot.diagnostics.storage_used, kNaplpsSharedStorageBytes)
      << "the budget was exceeded rather than enforced";
  EXPECT_GT(snapshot.diagnostics.storage_refusals, 0u)
      << "the overrun was accepted silently";
}

////////////////////////////////////////////////////////////////////////////////////////////
// Robustness on a recovered record
////////////////////////////////////////////////////////////////////////////////////////////

// §5.3.1: the transmission and device controls and NUL "may be embedded within
// any presentation layer sequence without affecting that sequence" — so one in
// the middle of a PDI's operands must not terminate it.
TEST(NaplpsInterpreter, AnEmbeddedNullDoesNotTerminateAnOpenPdi) {
  Record with_null;
  with_null.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      .byte(0x00)  // NUL
      .byte(coord1(0b010, 0b010))
      .byte(0x11)  // DC1
      .byte(coord1(0b011, 0b011));

  const NabtsPageSnapshot snapshot = run(with_null);
  EXPECT_EQ(snapshot.primitives.size(), 3u)
      << "an embedded transparent control broke the operand run";
  EXPECT_GT(snapshot.diagnostics.ignored_controls, 0u);
}

// A record that ends mid-sequence yields what it managed. This is the normal
// case for a recovered record: the group lost its last packet.
TEST(NaplpsInterpreter, ATruncatedRecordYieldsWhatItManaged) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      // An arc with no coordinate data at all behind it.
      .byte(static_cast<uint8_t>(NaplpsPdi::kArcOutlined));

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_EQ(snapshot.primitives.size(), 1u) << "the point before it survived";
  EXPECT_GT(snapshot.diagnostics.truncated_pdis, 0u);
}

// §5.3.3.3.1: "If the end point is omitted, it is taken to be coincident with
// the start point and a circle is drawn." A single coordinate block after ARC
// is the compact encoding of a circle, not a record that ran out — reading it
// as a truncation drops every circle a service draws that way.
TEST(NaplpsInterpreter, AnArcWithNoEndPointIsACircle) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .byte(coord1(0b001, 0b001))
      .byte(static_cast<uint8_t>(NaplpsPdi::kArcOutlined))
      .byte(coord1(0b011, 0b011));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const NabtsPrimitive& arc = snapshot.primitives[0];
  EXPECT_EQ(arc.kind, NabtsPrimitiveKind::kArc);
  // Start, the intermediate point that gives the diameter, and an end point
  // supplied as coincident with the start — which is what makes it a circle.
  ASSERT_EQ(arc.points.size(), 3u);
  EXPECT_DOUBLE_EQ(arc.points[2].x, arc.points[0].x);
  EXPECT_DOUBLE_EQ(arc.points[2].y, arc.points[0].y);
  EXPECT_NE(arc.points[1].x, arc.points[0].x);
  EXPECT_EQ(snapshot.diagnostics.truncated_pdis, 0u);
}

// CEA-516 §3.3 puts odd parity in b8 of every data byte, so the interpreter
// strips it — and a caller that stripped it already gets the same answer.
TEST(NaplpsInterpreter, StripsByteParityAndIsIdempotentAboutIt) {
  Record record;
  record.text_mode().add({'A', 'B'});

  std::vector<uint8_t> with_parity;
  for (const uint8_t byte : record.data()) {
    // Set b8 on every byte, as a type-zero group's odd parity may.
    with_parity.push_back(static_cast<uint8_t>(byte | 0x80));
  }

  NaplpsInterpreter stripped;
  NaplpsInterpreter unstripped;
  const NabtsPageSnapshot a = stripped.run(record.data());
  const NabtsPageSnapshot b = unstripped.run(with_parity);

  ASSERT_EQ(a.primitives.size(), b.primitives.size());
  for (size_t i = 0; i < a.primitives.size(); ++i) {
    EXPECT_EQ(a.primitives[i].character, b.primitives[i].character);
  }
}

// §5.3.1 makes an out-of-screen coordinate an error; this clips it and counts
// it rather than wrapping or rejecting the PDI.
TEST(NaplpsInterpreter, ClipsACoordinateOffTheUnitScreenAndCountsIt) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .byte(coord1(0b001, 0b001))
      // A relative move of -0,5 in x from +0,25, which lands off the screen.
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointRel))
      .byte(coord1(0b110, 0b000));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.x, 0.0)
      << "clipped, not wrapped";
  EXPECT_GT(snapshot.diagnostics.out_of_range_coordinates, 0u);
}

// §4.3.2: a null set "is a set in which all code positions are executed as null
// operations", so a record that designates one draws nothing from it.
TEST(NaplpsInterpreter, ANullSetDrawsNothing) {
  Record record;
  record
      // Designate an unlisted final character into G0, then invoke it.
      .escape({code(2, 8), code(6, 5)})
      .text_mode()
      .add({'A', 'B', 'C'});

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_TRUE(snapshot.primitives.empty());
  EXPECT_GT(snapshot.diagnostics.unknown_designations, 0u);
}

}  // namespace
}  // namespace orc
