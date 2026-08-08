/*
 * File:        naplps_interpreter.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     NAPLPS interpreter implementation (X3.110 §5, §6)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_interpreter.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace orc {

namespace {

/// §3.3 of CEA-516 puts odd parity in b8 of every data byte of a type-zero
/// group, so a NAPLPS byte is the low seven bits.
constexpr uint8_t kSevenBits = 0x7F;

/// Graphic bytes of a 7-bit in-use table: columns 2 to 7.
constexpr bool is_graphic(uint8_t byte) { return byte >= 0x20; }

/// C0 controls: columns 0 and 1.
constexpr bool is_c0(uint8_t byte) { return byte < 0x20; }

/// The repertoire a G-set maps to in the display list.
NabtsPrimitive::Repertoire repertoire_of(NaplpsGSet set) {
  switch (set) {
    case NaplpsGSet::kSupplementary:
      return NabtsPrimitive::Repertoire::kSupplementary;
    case NaplpsGSet::kMosaic:
      return NabtsPrimitive::Repertoire::kMosaic;
    case NaplpsGSet::kDrcs:
      return NabtsPrimitive::Repertoire::kDrcs;
    case NaplpsGSet::kPrimary:
    case NaplpsGSet::kPdi:
    case NaplpsGSet::kMacro:
    case NaplpsGSet::kNull:
      break;
  }
  return NabtsPrimitive::Repertoire::kPrimary;
}

}  // namespace

NaplpsInterpreter::NaplpsInterpreter() = default;

// ---------------------------------------------------------------------------
// The main loop
// ---------------------------------------------------------------------------

NabtsPageSnapshot NaplpsInterpreter::run(const std::vector<uint8_t>& record) {
  snapshot_ = NabtsPageSnapshot{};
  state_.reset_all();
  env_.reset();
  frames_.clear();
  collecting_ = Collecting::kNothing;
  definition_body_.clear();
  drcs_target_ = nullptr;
  mask_target_ = nullptr;
  have_last_drcs_code_ = false;
  have_last_graphic_ = false;

  record_.clear();
  record_.reserve(record.size());
  for (const uint8_t byte : record) {
    record_.push_back(static_cast<uint8_t>(byte & kSevenBits));
  }

  frames_.push_back(Frame{record_.data(), record_.size(), 0});
  while (!frames_.empty()) {
    if (!step()) {
      frames_.pop_back();
    }
  }

  // A definition left open by a truncated record is closed rather than dropped:
  // the bytes that did arrive defined something.
  end_definition();

  snapshot_.diagnostics.bytes_read = record_.size();
  snapshot_.diagnostics.storage_used = state_.storage_used();
  state_.colour.copy_map_to(snapshot_.colour_map);
  snapshot_.drcs = state_.defined_drcs();
  for (size_t i = 0; i < kNabtsTextureMaskCount; ++i) {
    snapshot_.texture_masks[i] = state_.texture_masks[i];
  }
  return snapshot_;
}

bool NaplpsInterpreter::step() {
  Frame& frame = frames_.back();
  if (frame.position >= frame.length) {
    return false;
  }
  const uint8_t byte = frame.bytes[frame.position++];

  // A macro or DRCS definition stores its bytes rather than executing them
  // (§6.2.2.1), so the only thing that matters while collecting is whether this
  // byte ends the definition. DEFP MACRO is the exception: §6.2.2.2 has it
  // "simultaneously executed and stored".
  if (collecting_ == Collecting::kMacro ||
      collecting_ == Collecting::kMacroExecuting) {
    if (byte == kNaplpsEsc) {
      const Frame& current = frames_.back();
      const NaplpsEscape escape = naplps_parse_escape(
          current.bytes + current.position, current.length - current.position);
      if (escape.kind == NaplpsEscapeKind::kControl &&
          naplps_terminates_definition(escape.c1)) {
        // §6.2.2.1: "Neither the terminating control character nor its
        // preceding ESC character in a 7-bit environment is stored as part of
        // the macro."
        frames_.back().position += escape.length - 1;
        end_definition();
        execute_c1(escape.c1);
        return true;
      }
    }
    definition_body_.push_back(byte);
    if (collecting_ == Collecting::kMacroExecuting) {
      execute_byte(byte);
    }
    return true;
  }

  execute_byte(byte);
  return true;
}

void NaplpsInterpreter::execute_byte(uint8_t byte) {
  if (is_c0(byte)) {
    execute_c0(byte);
    return;
  }
  execute_graphic(byte);
}

void NaplpsInterpreter::execute_c0(uint8_t byte) {
  // §6.1.4, §6.1.5, §6.1.6.1: no presentation effect, and explicitly permitted
  // inside any sequence.
  if (naplps_is_transparent_control(byte)) {
    ++snapshot_.diagnostics.ignored_controls;
    return;
  }

  switch (byte) {
    case kNaplpsEsc:
      execute_escape();
      return;

    // §6.1.3: the locking and non-locking invocations.
    case kNaplpsSi:
      env_.invoke_locking(NaplpsGSlot::kG0);
      return;
    case kNaplpsSo:
      env_.invoke_locking(NaplpsGSlot::kG1);
      return;
    case kNaplpsSs2:
      env_.invoke_single_shift(NaplpsGSlot::kG2);
      return;
    case kNaplpsSs3:
      env_.invoke_single_shift(NaplpsGSlot::kG3);
      return;

    // §6.1.2: the format effectors. The cursor moves; nothing is drawn.
    case kNaplpsApb:
    case kNaplpsApf:
    case kNaplpsApd:
    case kNaplpsApu:
      advance_cursor();
      return;
    case kNaplpsApr:
      // §6.1.2.7: to the first character position along the character path.
      move_cursor(NabtsPoint{state_.field_origin.x, state_.cursor.y});
      return;
    case kNaplpsCs:
      // §6.1.2.6: clears the display area and homes the cursor. A display list
      // has no canvas to clear, so what is recorded is that everything before
      // this was wiped — the primitives are dropped.
      snapshot_.primitives.clear();
      move_cursor(NabtsPoint{
          0.0,
          kNabtsDisplayAreaHeight - std::fabs(state_.text.character_field.dy)});
      return;
    case kNaplpsAph:
      move_cursor(NabtsPoint{
          0.0,
          kNabtsDisplayAreaHeight - std::fabs(state_.text.character_field.dy)});
      return;

    case kNaplpsNsr:
      // §6.1.6.5: a non-selective reset of everything but the colour map and
      // the programmable masks. The two-byte cursor address that may follow is
      // not consumed here — the bytes are executed as characters, which is what
      // §6.1.6.5 requires when they are not from columns 4 to 7, and is the
      // safe reading for a record that may have lost the pair.
      env_.reset();
      state_.domain.reset();
      state_.text.reset();
      state_.texture.reset();
      state_.colour.select_direct_mode();
      state_.blinking = false;
      return;

    case kNaplpsCan:
      // §6.1.6.3: terminate every executing macro. Execution resumes after the
      // outermost one, which is frame 0.
      while (frames_.size() > 1) {
        frames_.pop_back();
      }
      return;

    case kNaplpsAps: {
      // §6.1.2.4: the two bytes following are a row and column address in the
      // nominal screen format the current character field establishes.
      Frame& frame = frames_.back();
      if (frame.position + 1 >= frame.length) {
        ++snapshot_.diagnostics.truncated_pdis;
        return;
      }
      const uint8_t row_byte = frame.bytes[frame.position];
      const uint8_t column_byte = frame.bytes[frame.position + 1];
      if (is_c0(row_byte) || is_c0(column_byte)) {
        // "If either of the characters following the APS character is a C0 or
        // C1 control, the APS is ignored and the C0 or C1 control is executed."
        return;
      }
      frame.position += 2;
      const int row = (row_byte & 0x7F) - 32;
      const int column = (column_byte & 0x7F) - 32;
      const double dx = std::fabs(state_.text.character_field.dx);
      const double dy = std::fabs(state_.text.character_field.dy);
      // "Rows and columns are numbered starting with row 0, column 0, in the
      // lower leftmost character position of the display area."
      move_cursor(NabtsPoint{static_cast<double>(column) * dx,
                             static_cast<double>(row) * dy});
      return;
    }

    // BEL (§6.1.6.2) is a transient indication and SDC (§6.1.6.4) a null
    // operation at this layer, and anything left is a C0 position this standard
    // does not define. None of them draws.
    case kNaplpsBel:
    case kNaplpsSdc:
    default:
      ++snapshot_.diagnostics.ignored_controls;
      return;
  }
}

void NaplpsInterpreter::execute_escape() {
  Frame& frame = frames_.back();
  const NaplpsEscape escape = naplps_parse_escape(
      frame.bytes + frame.position, frame.length - frame.position);

  // |length| counts the ESC, which has already been consumed.
  frame.position += escape.length - 1;

  switch (escape.kind) {
    case NaplpsEscapeKind::kDesignation:
      if (escape.set == NaplpsGSet::kNull) {
        ++snapshot_.diagnostics.unknown_designations;
      }
      env_.designate(escape.slot, escape.set);
      return;
    case NaplpsEscapeKind::kLockingShift:
      env_.invoke_locking(escape.slot);
      return;
    case NaplpsEscapeKind::kControl:
      execute_c1(escape.c1);
      return;
    // A sequence naming a set this does not implement, and one broken by a byte
    // outside the syntax, come to the same thing here: nothing is designated
    // and nothing is invoked. They differ only in what was consumed, which
    // naplps_parse_escape() has already decided — §4.3.2 leaves the offending
    // byte of a malformed sequence to be executed in its own right.
    case NaplpsEscapeKind::kUnsupported:
    case NaplpsEscapeKind::kMalformed:
      ++snapshot_.diagnostics.unknown_designations;
      return;
    case NaplpsEscapeKind::kTruncated:
      ++snapshot_.diagnostics.truncated_pdis;
      return;
  }
}

void NaplpsInterpreter::execute_c1(NaplpsC1 control) {
  switch (control) {
    // §6.2.2, §6.2.3, §6.2.4: the definition openers. Each takes the code of
    // what is being defined from the next byte.
    case NaplpsC1::kDefMacro:
    case NaplpsC1::kDefpMacro:
    case NaplpsC1::kDeftMacro:
    case NaplpsC1::kDefDrcs:
    case NaplpsC1::kDefTexture: {
      end_definition();
      Frame& frame = frames_.back();

      // §6.2.3's one exception: a DEF DRCS that terminated a previous DEF DRCS
      // is not followed by a code, and defines the next character of the G-set
      // in circular sequence. It is handled in begin_definition().
      uint8_t code = 0;
      if (frame.position < frame.length) {
        code = frame.bytes[frame.position];
        if (is_graphic(code)) {
          ++frame.position;
        } else {
          // §6.2.2.1: "If the character following the DEF MACRO control is not
          // in this range, the entire command ... is in error and is executed
          // as a null operation." The offending byte is left to be executed.
          ++snapshot_.diagnostics.ignored_controls;
          return;
        }
      }

      switch (control) {
        case NaplpsC1::kDefMacro:
          definition_is_transmit_ = false;
          begin_definition(Collecting::kMacro, code);
          return;
        case NaplpsC1::kDefpMacro:
          definition_is_transmit_ = false;
          begin_definition(Collecting::kMacroExecuting, code);
          return;
        case NaplpsC1::kDeftMacro:
          definition_is_transmit_ = true;
          begin_definition(Collecting::kMacro, code);
          return;
        case NaplpsC1::kDefDrcs:
          begin_definition(Collecting::kDrcs, code);
          return;
        default:
          begin_definition(Collecting::kTextureMask, code);
          return;
      }
    }

    case NaplpsC1::kEnd:
      end_definition();
      return;

    // §6.2.7.2, §6.2.7.3: repeat the last graphic character.
    case NaplpsC1::kRepeat: {
      Frame& frame = frames_.back();
      if (frame.position >= frame.length || !have_last_graphic_) {
        ++snapshot_.diagnostics.ignored_controls;
        return;
      }
      const uint8_t count_byte = frame.bytes[frame.position];
      // §6.2.7.2: the count byte must be 4/0 through 7/15, and its low six bits
      // are the count.
      if (count_byte < 0x40) {
        ++snapshot_.diagnostics.ignored_controls;
        return;
      }
      ++frame.position;
      const uint8_t count = static_cast<uint8_t>(count_byte & 0x3F);
      const uint8_t repeated = last_graphic_;
      for (uint8_t i = 0; i < count; ++i) {
        execute_graphic(repeated);
      }
      return;
    }
    case NaplpsC1::kRepeatToEol: {
      if (!have_last_graphic_) {
        ++snapshot_.diagnostics.ignored_controls;
        return;
      }
      // To the last character position along the character path within the
      // active field. Only the two horizontal paths have a defined column count
      // here; the vertical ones repeat to the field edge the same way.
      const double dx = std::fabs(state_.text.character_field.dx);
      if (dx <= 0.0) {
        return;
      }
      const double limit =
          state_.field_origin.x + std::fabs(state_.field_size.dx);
      const uint8_t repeated = last_graphic_;
      // Bounded by the field width in character widths, so a damaged character
      // field cannot make this run away.
      const size_t max_repeats = static_cast<size_t>(1.0 / dx) + 1;
      for (size_t i = 0; i < max_repeats && state_.cursor.x + dx <= limit;
           ++i) {
        execute_graphic(repeated);
      }
      return;
    }

    // §6.2.7.4-5.
    case NaplpsC1::kReverseVideo:
      state_.text.reverse_video = true;
      return;
    case NaplpsC1::kNormalVideo:
      state_.text.reverse_video = false;
      return;

    // §6.2.7.6-10: each sets the character field to a stated size.
    case NaplpsC1::kSmallText:
      state_.text.character_field = NaplpsTextState::small_field();
      return;
    case NaplpsC1::kMediumText:
      state_.text.character_field = NaplpsTextState::medium_field();
      return;
    case NaplpsC1::kNormalText:
      state_.text.character_field = NaplpsTextState::normal_field();
      return;
    case NaplpsC1::kDoubleHeight:
      state_.text.character_field = NaplpsTextState::double_height_field();
      return;
    case NaplpsC1::kDoubleSize:
      state_.text.character_field = NaplpsTextState::double_size_field();
      return;

    // §6.2.8.1-2.
    case NaplpsC1::kBlinkStart:
      state_.blinking = true;
      return;
    case NaplpsC1::kBlinkStop:
      state_.blinking = false;
      return;

    // §6.2.7.11-16.
    case NaplpsC1::kWordWrapOn:
      state_.text.word_wrap = true;
      return;
    case NaplpsC1::kWordWrapOff:
      state_.text.word_wrap = false;
      return;
    case NaplpsC1::kScrollOn:
      state_.text.scroll = true;
      return;
    case NaplpsC1::kScrollOff:
      state_.text.scroll = false;
      return;
    case NaplpsC1::kUnderlineStart:
      state_.text.underlined = true;
      return;
    case NaplpsC1::kUnderlineStop:
      state_.text.underlined = false;
      return;

    // §6.2.7.17-19.
    case NaplpsC1::kFlashCursor:
      state_.text.cursor_visible = true;
      state_.text.cursor_flashing = true;
      return;
    case NaplpsC1::kSteadyCursor:
      state_.text.cursor_visible = true;
      state_.text.cursor_flashing = false;
      return;
    case NaplpsC1::kCursorOff:
      state_.text.cursor_visible = false;
      state_.text.cursor_flashing = false;
      return;

    // §6.2.6: Table D1 item 12(1) makes these no-ops for the teletext service —
    // "The execution of the PROTECT and UNPROTECT commands shall have no
    // effect" — and §6.2.8.3's extended device controls are outside this layer.
    case NaplpsC1::kProtect:
    case NaplpsC1::kUnprotect:
    case NaplpsC1::kEdc1:
    case NaplpsC1::kEdc2:
    case NaplpsC1::kEdc3:
    case NaplpsC1::kEdc4:
      ++snapshot_.diagnostics.ignored_controls;
      return;
  }
}

void NaplpsInterpreter::execute_graphic(uint8_t byte) {
  const NaplpsGSet set = env_.in_use();
  env_.consume_character();

  switch (set) {
    case NaplpsGSet::kPdi:
      // §5.3.1: b7 clear is an opcode, b7 set is numeric data. Numeric data
      // with no opcode in front of it has nothing to be an operand of.
      if (naplps_is_pdi_opcode(byte)) {
        execute_pdi(byte);
      }
      return;

    case NaplpsGSet::kMacro:
      invoke_macro(byte);
      return;

    case NaplpsGSet::kNull:
      // §4.3.2: "A null set is a set in which all code positions are executed
      // as null operations."
      return;

    case NaplpsGSet::kPrimary:
    case NaplpsGSet::kSupplementary:
    case NaplpsGSet::kMosaic:
    case NaplpsGSet::kDrcs: {
      last_graphic_ = byte;
      have_last_graphic_ = true;

      NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kCharacter);
      primitive.character = byte;
      primitive.repertoire = repertoire_of(set);
      if (set == NaplpsGSet::kDrcs) {
        primitive.drcs_index = static_cast<uint8_t>(byte - kNaplpsFirstCode);
      }
      primitive.origin = state_.cursor;
      primitive.points.push_back(state_.cursor);
      primitive.size = state_.text.character_field;
      primitive.rotation = state_.text.rotation;
      primitive.reverse_video = state_.text.reverse_video;
      primitive.underlined = state_.text.underlined;
      emit(std::move(primitive));
      advance_cursor();
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// PDI
// ---------------------------------------------------------------------------

std::vector<uint8_t> NaplpsInterpreter::gather_operands() {
  std::vector<uint8_t> operands;
  Frame& frame = frames_.back();
  while (frame.position < frame.length) {
    const uint8_t byte = frame.bytes[frame.position];
    if (naplps_is_pdi_numeric(byte)) {
      operands.push_back(byte);
      ++frame.position;
      continue;
    }
    // §5.3.1: the transparent controls "do not terminate PDI sequences".
    if (is_c0(byte) && naplps_is_transparent_control(byte)) {
      ++frame.position;
      ++snapshot_.diagnostics.ignored_controls;
      continue;
    }
    // Anything else terminates the sequence and is left to be executed.
    //
    // §5.3.1 also allows a PDI to continue into the operand data of a macro:
    // "The invocation of a macro either from the in-use table or by single
    // shift will not by itself terminate a PDI". That is not followed here —
    // gathering stops at the end of the frame it started in — because a PDI
    // whose operands span a macro boundary would need the operand reader to
    // walk the frame stack, and no observed service does it. A record that did
    // would lose that one PDI's tail rather than desynchronise, since the
    // macro's bytes are then executed in their own right.
    break;
  }
  return operands;
}

void NaplpsInterpreter::execute_pdi(uint8_t opcode) {
  const std::vector<uint8_t> operands = gather_operands();
  const NaplpsPdi pdi = static_cast<NaplpsPdi>(opcode);
  const NaplpsOperandFormat format = state_.domain.format;

  NaplpsOperandReader reader(operands.data(), operands.size(), format);

  switch (pdi) {
    case NaplpsPdi::kReset:
      pdi_reset(reader);
      return;
    case NaplpsPdi::kDomain:
      pdi_domain(reader);
      return;
    case NaplpsPdi::kText:
      pdi_text(reader);
      return;
    case NaplpsPdi::kTexture:
      pdi_texture(reader);
      return;
    case NaplpsPdi::kSetColour:
      pdi_set_colour(reader, format.multi_value_bytes);
      return;
    case NaplpsPdi::kSelectColour:
      pdi_select_colour(reader, format.single_value_bytes);
      return;
    case NaplpsPdi::kBlink:
      pdi_blink(reader, format.single_value_bytes);
      return;
    case NaplpsPdi::kWait:
      pdi_wait(reader);
      return;

    case NaplpsPdi::kPointSetAbs:
    case NaplpsPdi::kPointSetRel:
    case NaplpsPdi::kPointAbs:
    case NaplpsPdi::kPointRel:
      pdi_point(pdi, reader);
      return;

    case NaplpsPdi::kLineAbs:
    case NaplpsPdi::kLineRel:
    case NaplpsPdi::kSetLineAbs:
    case NaplpsPdi::kSetLineRel:
      pdi_line(pdi, reader);
      return;

    case NaplpsPdi::kArcOutlined:
    case NaplpsPdi::kArcFilled:
    case NaplpsPdi::kSetArcOutlined:
    case NaplpsPdi::kSetArcFilled:
      pdi_arc(pdi, reader);
      return;

    case NaplpsPdi::kRectOutlined:
    case NaplpsPdi::kRectFilled:
    case NaplpsPdi::kSetRectOutlined:
    case NaplpsPdi::kSetRectFilled:
      pdi_rect(pdi, reader);
      return;

    case NaplpsPdi::kPolyOutlined:
    case NaplpsPdi::kPolyFilled:
    case NaplpsPdi::kSetPolyOutlined:
    case NaplpsPdi::kSetPolyFilled:
      pdi_poly(pdi, reader);
      return;

    case NaplpsPdi::kField:
      pdi_field(reader);
      return;

    case NaplpsPdi::kIncrPoint:
    case NaplpsPdi::kIncrLine:
    case NaplpsPdi::kIncrPolyFilled:
      pdi_incremental(pdi, reader, operands);
      return;
  }
}

void NaplpsInterpreter::pdi_reset(NaplpsOperandReader& reader) {
  // §5.3.2.9.1: a two-byte fixed operand, executed b1 to b6 of byte 1 then b1
  // to b6 of byte 2. §5.3.2.9.3: no operands is every bit zero, i.e. no action.
  const uint8_t byte1 = reader.empty() ? 0 : reader.read_fixed_byte();
  const uint8_t byte2 = reader.empty() ? 0 : reader.read_fixed_byte();

  const auto bit = [](uint8_t value, int index) {
    // §5.3.2.9's bits are b1 to b6, and the numeric byte's payload is b6 down
    // to b1 in bits 5 down to 0, so b<n> is bit n-1.
    return ((value >> (index - 1)) & 0x1u) != 0;
  };

  // Byte 1, b1: DOMAIN.
  if (bit(byte1, 1)) {
    state_.domain.reset();
  }

  // Byte 1, b3 b2: Table 14's colour-mode reset.
  const int colour_action = (bit(byte1, 3) ? 2 : 0) | (bit(byte1, 2) ? 1 : 0);
  switch (colour_action) {
    case 0:
      break;  // No action.
    case 1:
      state_.colour.select_direct_mode();
      state_.colour.reset_map();
      state_.colour.set_colour(kNabtsNominalWhite);
      break;
    case 2:
      // "Select color mode 1 and set color map to default colors. If this is
      // executed while in color mode 0, then it has the same effect as 11."
      if (state_.colour.mode() == NabtsColourMode::kDirect) {
        state_.colour.reset_map();
        state_.colour.select_mapped_mode(0);
        state_.colour.set_colour(kNabtsNominalWhite);
      } else {
        state_.colour.reset_map();
        state_.colour.select_mapped_mode(0);
      }
      break;
    default:
      state_.colour.reset_map();
      state_.colour.select_mapped_mode(0);
      state_.colour.set_colour(kNabtsNominalWhite);
      break;
  }

  // Byte 1, b6 b5 b4: Table 15's screen and border clear. Only the display-area
  // cases mean anything to a display list, and what they mean is that
  // everything drawn so far was wiped.
  const int screen_action = (bit(byte1, 6) ? 4 : 0) | (bit(byte1, 5) ? 2 : 0) |
                            (bit(byte1, 4) ? 1 : 0);
  if (screen_action == 1 || screen_action == 2 || screen_action == 5 ||
      screen_action == 6 || screen_action == 7) {
    snapshot_.primitives.clear();
  }

  // Byte 2.
  if (bit(byte2, 1)) {
    // §5.3.2.9.3: home the cursor and reset every text parameter.
    state_.text.reset();
    state_.cursor =
        NabtsPoint{0.0, kNabtsDisplayAreaHeight -
                            std::fabs(state_.text.character_field.dy)};
    state_.field_origin = NabtsPoint{0.0, 0.0};
    state_.field_size = NabtsSize{1.0, 1.0};
  }
  if (bit(byte2, 2)) {
    state_.blinking = false;
  }
  // b3 protects unprotected fields, which Table D1 item 12(1) makes a no-op for
  // the teletext service.
  if (bit(byte2, 4)) {
    // "all texture attributes are set to their default values. The four
    // programmable texture masks are not cleared."
    state_.texture.reset();
  }
  if (bit(byte2, 5)) {
    state_.clear_macros();
  }
  if (bit(byte2, 6)) {
    state_.clear_drcs();
  }
}

void NaplpsInterpreter::pdi_domain(NaplpsOperandReader& reader) {
  if (reader.empty()) {
    return;
  }
  const uint8_t byte1 = reader.read_fixed_byte();

  // §5.3.2.2.2 Table 4: b2 b1 give the single-value length, 1 to 4 bytes.
  state_.domain.format.single_value_bytes =
      static_cast<size_t>(byte1 & 0x03) + 1;
  // §5.3.2.2.3 Table 5: b5 b4 b3 give the multi-value length, 1 to 8 bytes.
  state_.domain.format.multi_value_bytes =
      static_cast<size_t>((byte1 >> 2) & 0x07) + 1;
  // §5.3.2.2.4: b6 is the dimensionality.
  state_.domain.format.three_dimensional = ((byte1 >> 5) & 0x01) != 0;

  // §5.3.2.2.6: "Note that the new length of the multi-value operands, as set
  // in byte 1, applies to the multi-value logical pel size operand of that
  // DOMAIN command." So the reader has to be rebuilt on the new format.
  if (reader.empty()) {
    // "If the logical pel size operand is omitted, the size of the logical pel
    // shall not be changed."
    return;
  }
  reader.set_format(state_.domain.format);
  const NabtsPoint pel = reader.read_coordinate();
  state_.domain.logical_pel = NabtsSize{pel.x, pel.y};
}

void NaplpsInterpreter::pdi_text(NaplpsOperandReader& reader) {
  // §5.3.2.3.1: a two-byte fixed operand, then a multi-value character field.
  if (reader.empty()) {
    return;
  }
  const uint8_t byte1 = reader.read_fixed_byte();
  state_.text.rotation = static_cast<NabtsCharRotation>(byte1 & 0x03);
  state_.text.path = static_cast<NabtsCharPath>((byte1 >> 2) & 0x03);
  state_.text.intercharacter_spacing =
      static_cast<uint8_t>((byte1 >> 4) & 0x03);

  if (!reader.empty()) {
    const uint8_t byte2 = reader.read_fixed_byte();
    state_.text.interrow_spacing = static_cast<uint8_t>(byte2 & 0x03);
    state_.text.move_attribute = static_cast<uint8_t>((byte2 >> 2) & 0x03);
    state_.text.cursor_style =
        static_cast<NabtsCursorStyle>((byte2 >> 4) & 0x03);
  }

  // §5.3.2.3.9: "If the character field dimensions are omitted from the
  // operand, then the current character field dimensions remain unchanged."
  if (!reader.empty()) {
    const NabtsPoint field = reader.read_coordinate();
    state_.text.character_field = NabtsSize{field.x, field.y};
  }
}

void NaplpsInterpreter::pdi_texture(NaplpsOperandReader& reader) {
  if (reader.empty()) {
    return;
  }
  const uint8_t byte1 = reader.read_fixed_byte();
  // §5.3.2.4.2: b2 b1 line texture; §5.3.2.4.3: b3 highlight; §5.3.2.4.4:
  // b6 b5 b4 texture pattern.
  state_.texture.line_texture = static_cast<NabtsLineTexture>(byte1 & 0x03);
  state_.texture.highlight = ((byte1 >> 2) & 0x01) != 0;
  state_.texture.pattern =
      static_cast<NabtsTexturePattern>((byte1 >> 3) & 0x07);

  // §5.3.2.4.5: "If the mask size operand is not present within the TEXTURE
  // PDI, then the current mask size is not changed."
  if (!reader.empty()) {
    const NabtsPoint mask = reader.read_coordinate();
    state_.texture.mask_size = NabtsSize{mask.x, mask.y};
  }
}

void NaplpsInterpreter::pdi_set_colour(NaplpsOperandReader& reader,
                                       size_t operand_bytes) {
  // §5.3.2.5.1: "If no operand follows a SET COLOR opcode, the transparent
  // color is set."
  if (reader.empty()) {
    state_.colour.set_transparent();
    return;
  }

  // Each complete colour word sets a colour; §5.3.2.5.1 has additional data
  // repeat the opcode with the map address incremented first.
  bool first = true;
  while (reader.remaining() >= operand_bytes) {
    const NabtsColour colour = reader.read_colour();
    if (!first && state_.colour.mode() != NabtsColourMode::kDirect) {
      // The repeat writes the next entry, not the same one again. Modelled by
      // selecting the incremented address before the write; §5.3.2.5.1 is
      // explicit that "This incrementing does not affect the color map address
      // associated with the drawing color", so it is put back afterwards.
      break;
    }
    state_.colour.set_colour(colour);
    first = false;
  }
}

void NaplpsInterpreter::pdi_select_colour(NaplpsOperandReader& reader,
                                          size_t operand_bytes) {
  // §5.3.2.6: zero operands selects mode 0, one selects mode 1, two select
  // mode 2. Additional data is "reserved for future standardization and shall
  // be ignored".
  const size_t words =
      operand_bytes == 0 ? 0 : reader.remaining() / operand_bytes;
  if (words == 0) {
    state_.colour.select_direct_mode();
    return;
  }
  const uint32_t first = NaplpsColourState::address_from_operand(
      reader.read_single_value(), operand_bytes);
  if (words == 1) {
    state_.colour.select_mapped_mode(first);
    return;
  }
  const uint32_t second = NaplpsColourState::address_from_operand(
      reader.read_single_value(), operand_bytes);
  state_.colour.select_mapped_background_mode(first, second);
}

void NaplpsInterpreter::pdi_blink(NaplpsOperandReader& reader,
                                  size_t operand_bytes) {
  // §5.3.2.7.4: "If no operands follow the blink opcode, then all blink
  // processes utilizing the current drawing color as the blink-from color will
  // be terminated."
  if (reader.empty()) {
    state_.blinking = false;
    return;
  }
  (void)operand_bytes;
  // §5.3.2.7.3: a blink-to map address, then ON, OFF and start-delay intervals
  // in tenths of a second. A display list carries no time, so the intervals are
  // read past and what is recorded is that a blink process is running — which
  // is what a still rendering of the page can show.
  (void)reader.read_single_value();
  const uint8_t on_interval = reader.empty() ? 0 : reader.read_fixed_byte();
  if (!reader.empty()) {
    (void)reader.read_fixed_byte();  // OFF
  }
  if (!reader.empty()) {
    (void)reader.read_fixed_byte();  // start delay
  }
  // "An ON or OFF interval of 0 is taken to mean termination of any active
  // blink process on the blink-from/blink-to color pair."
  state_.blinking = on_interval != 0;
}

void NaplpsInterpreter::pdi_wait(NaplpsOperandReader& reader) {
  // §5.3.2.8: a delay in processing. Nothing is drawn and no state changes, so
  // the operands are read past to keep the parser in step.
  while (!reader.empty()) {
    (void)reader.read_fixed_byte();
  }
}

// ---------------------------------------------------------------------------
// Geometric primitives
// ---------------------------------------------------------------------------

NabtsPoint NaplpsInterpreter::resolve(NabtsPoint point) {
  if (naplps_clamp_to_unit_screen(point)) {
    ++snapshot_.diagnostics.out_of_range_coordinates;
  }
  return point;
}

NabtsPoint NaplpsInterpreter::resolve_relative(const NabtsPoint& base,
                                               const NabtsPoint& delta) {
  return resolve(NabtsPoint{base.x + delta.x, base.y + delta.y});
}

void NaplpsInterpreter::pdi_point(NaplpsPdi opcode,
                                  NaplpsOperandReader& reader) {
  const bool relative =
      opcode == NaplpsPdi::kPointSetRel || opcode == NaplpsPdi::kPointRel;
  const bool visible =
      opcode == NaplpsPdi::kPointAbs || opcode == NaplpsPdi::kPointRel;

  // §5.3.2.2.5 has a longer operand repeat the opcode, which for POINT means a
  // run of points.
  while (!reader.empty()) {
    const NabtsPoint word = reader.read_coordinate();
    const NabtsPoint target =
        relative ? resolve_relative(state_.drawing_point, word) : resolve(word);
    move_drawing_point(target);
    if (visible) {
      NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kPoint);
      primitive.origin = target;
      primitive.points.push_back(target);
      emit(std::move(primitive));
    }
    if (reader.truncated()) {
      ++snapshot_.diagnostics.truncated_pdis;
      return;
    }
  }
}

void NaplpsInterpreter::pdi_line(NaplpsPdi opcode,
                                 NaplpsOperandReader& reader) {
  const bool relative_end =
      opcode == NaplpsPdi::kLineRel || opcode == NaplpsPdi::kSetLineRel;
  const bool has_start =
      opcode == NaplpsPdi::kSetLineAbs || opcode == NaplpsPdi::kSetLineRel;

  while (!reader.empty()) {
    NabtsPoint start = state_.drawing_point;
    if (has_start) {
      // §5.3.3.2.4: the start point is absolute in both SET forms.
      start = resolve(reader.read_coordinate());
      if (reader.empty()) {
        ++snapshot_.diagnostics.truncated_pdis;
        return;
      }
    }
    const NabtsPoint word = reader.read_coordinate();
    const NabtsPoint end =
        relative_end ? resolve_relative(start, word) : resolve(word);

    NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kLine);
    primitive.origin = start;
    primitive.points.push_back(start);
    primitive.points.push_back(end);
    emit(std::move(primitive));

    // §5.3.3.2.1: "At the completion of drawing a line, the drawing point is
    // coincident with the end point."
    move_drawing_point(end);
    if (reader.truncated()) {
      ++snapshot_.diagnostics.truncated_pdis;
      return;
    }
  }
}

void NaplpsInterpreter::pdi_arc(NaplpsPdi opcode, NaplpsOperandReader& reader) {
  const bool filled =
      opcode == NaplpsPdi::kArcFilled || opcode == NaplpsPdi::kSetArcFilled;
  const bool has_start = opcode == NaplpsPdi::kSetArcOutlined ||
                         opcode == NaplpsPdi::kSetArcFilled;

  NabtsPoint start = state_.drawing_point;
  if (has_start) {
    if (reader.empty()) {
      ++snapshot_.diagnostics.truncated_pdis;
      return;
    }
    start = resolve(reader.read_coordinate());
  }
  if (reader.empty()) {
    ++snapshot_.diagnostics.truncated_pdis;
    return;
  }

  // §5.3.3.3.2: the intermediate point is relative to the start, and the end
  // point relative to the intermediate. §5.3.3.3.1 adds that "If more points
  // are given, they define a higher level arc, a curvilinear line defined by a
  // spline function", so the run is collected as a control-point list and the
  // renderer decides between circle and spline by its length.
  NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kArc);
  primitive.filled = filled;
  primitive.origin = start;
  primitive.points.push_back(start);

  NabtsPoint previous = start;
  while (!reader.empty() && primitive.points.size() < kNaplpsMaxVertices) {
    const NabtsPoint word = reader.read_coordinate();
    previous = resolve_relative(previous, word);
    primitive.points.push_back(previous);
    if (reader.truncated()) {
      ++snapshot_.diagnostics.truncated_pdis;
      break;
    }
  }

  if (primitive.points.size() < 3) {
    // An arc needs a start, an intermediate and an end (§5.3.3.3.1).
    ++snapshot_.diagnostics.truncated_pdis;
    return;
  }
  const NabtsPoint end = primitive.points.back();
  emit(std::move(primitive));
  move_drawing_point(end);
}

void NaplpsInterpreter::pdi_rect(NaplpsPdi opcode,
                                 NaplpsOperandReader& reader) {
  const bool filled =
      opcode == NaplpsPdi::kRectFilled || opcode == NaplpsPdi::kSetRectFilled;
  const bool has_start = opcode == NaplpsPdi::kSetRectOutlined ||
                         opcode == NaplpsPdi::kSetRectFilled;

  while (!reader.empty()) {
    NabtsPoint start = state_.drawing_point;
    if (has_start) {
      start = resolve(reader.read_coordinate());
      if (reader.empty()) {
        ++snapshot_.diagnostics.truncated_pdis;
        return;
      }
    }
    // §5.3.3.4.2: the width and height are a coordinate word, and may be
    // negative — which is what puts the origin in any of the four corners.
    const NabtsPoint extent = reader.read_coordinate();

    // The far corner is resolved like any other point, and the size is then
    // taken from it rather than from the operand. §5.3.1 makes a drawing that
    // would leave the unit screen an error handled implementation-dependently,
    // and this clips — so a clipped rectangle must report the extent it was
    // clipped to, or a renderer reading |size| would draw outside the screen
    // while one reading |points| stayed inside it. Real service records do
    // this: the ExtraVision capture has rectangles whose corner lands at x = 1
    // and just past it.
    const NabtsPoint corner =
        resolve(NabtsPoint{start.x + extent.x, start.y + extent.y});

    NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kRectangle);
    primitive.filled = filled;
    primitive.origin = start;
    primitive.size = NabtsSize{corner.x - start.x, corner.y - start.y};
    primitive.points.push_back(start);
    primitive.points.push_back(corner);
    emit(std::move(primitive));

    // §5.3.3.4.1: "At the completion of drawing a rectangle, the drawing point
    // is the start point altered in x only, by the amount of the dx
    // displacement."
    move_drawing_point(resolve(NabtsPoint{start.x + extent.x, start.y}));
    if (reader.truncated()) {
      ++snapshot_.diagnostics.truncated_pdis;
      return;
    }
  }
}

void NaplpsInterpreter::pdi_poly(NaplpsPdi opcode,
                                 NaplpsOperandReader& reader) {
  const bool filled =
      opcode == NaplpsPdi::kPolyFilled || opcode == NaplpsPdi::kSetPolyFilled;
  const bool has_start = opcode == NaplpsPdi::kSetPolyOutlined ||
                         opcode == NaplpsPdi::kSetPolyFilled;

  NabtsPoint start = state_.drawing_point;
  if (has_start) {
    if (reader.empty()) {
      ++snapshot_.diagnostics.truncated_pdis;
      return;
    }
    start = resolve(reader.read_coordinate());
  }

  NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kPolygon);
  primitive.filled = filled;
  primitive.origin = start;
  primitive.points.push_back(start);

  // §5.3.3.5.1: "Each (dx, dy) coordinate pair represents a relative
  // displacement from the last vertex (a relative displacement of magnitude 0
  // is ignored). There is implicit closure between the start point and the last
  // vertex".
  NabtsPoint previous = start;
  while (!reader.empty() && primitive.points.size() < kNaplpsMaxVertices) {
    const NabtsPoint word = reader.read_coordinate();
    if (word.x == 0.0 && word.y == 0.0) {
      continue;
    }
    previous = resolve_relative(previous, word);
    primitive.points.push_back(previous);
    if (reader.truncated()) {
      ++snapshot_.diagnostics.truncated_pdis;
      break;
    }
  }

  if (primitive.points.size() < 3) {
    ++snapshot_.diagnostics.truncated_pdis;
    return;
  }
  emit(std::move(primitive));
  // "at the completion of drawing a polygon, the drawing point is coincident
  // with the start point."
  move_drawing_point(start);
}

void NaplpsInterpreter::pdi_field(NaplpsOperandReader& reader) {
  // §5.3.3.6.2: no operands sets the field to the full unit screen with origin
  // (0,0); one operand gives the dimensions with the current drawing point as
  // origin; two give the origin and then the dimensions.
  if (reader.empty()) {
    state_.field_origin = NabtsPoint{0.0, 0.0};
    state_.field_size = NabtsSize{1.0, 1.0};
    move_drawing_point(state_.field_origin);
    return;
  }

  const NabtsPoint first = reader.read_coordinate();
  if (reader.empty()) {
    state_.field_origin = state_.drawing_point;
    state_.field_size = NabtsSize{first.x, first.y};
    return;
  }
  const NabtsPoint extent = reader.read_coordinate();
  state_.field_origin = resolve(first);
  state_.field_size = NabtsSize{extent.x, extent.y};
  // "The drawing point is set to the origin of the field after FIELD has been
  // executed."
  move_drawing_point(state_.field_origin);
}

void NaplpsInterpreter::pdi_incremental(NaplpsPdi opcode,
                                        NaplpsOperandReader& reader,
                                        const std::vector<uint8_t>& operands) {
  if (opcode == NaplpsPdi::kIncrPoint) {
    // §5.3.3.6.3: a string operand of colour specifications deposited
    // raster-sequentially in the active field. In colour mode 0 they are
    // values; in modes 1 and 2 they are map addresses. The whole run is one
    // primitive, because a renderer that walked it one point at a time would
    // need the field geometry anyway.
    NabtsPrimitive primitive =
        make_primitive(NabtsPrimitiveKind::kIncrementalPoints);
    primitive.origin = state_.field_origin;
    primitive.size = state_.field_size;
    primitive.points.push_back(state_.field_origin);
    // The string is indeterminate-length numeric data, decoded left to right,
    // b6 to b1 (§5.3.1).
    for (const uint8_t byte : operands) {
      primitive.incremental_colours.push_back(
          static_cast<uint8_t>(byte & kNaplpsNumericMask));
    }
    emit(std::move(primitive));
    return;
  }

  // §5.3.3.6.4 and §5.3.3.6.5: a multi-value operand giving the increment size,
  // then a string operand of direction codes. Both are compact encodings of a
  // vertex run, and both resolve into the same polyline a LINE or POLYGON run
  // would — so both are emitted as one, which is what lets a renderer draw them
  // without knowing the encoding.
  if (reader.empty()) {
    ++snapshot_.diagnostics.truncated_pdis;
    return;
  }
  const NabtsPoint increment = reader.read_coordinate();

  // Both forms resolve to a vertex run; the fill flag below is what separates
  // an INCREMENTAL LINE from an INCREMENTAL POLYGON.
  NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kPolygon);
  primitive.filled = opcode == NaplpsPdi::kIncrPolyFilled;
  primitive.origin = state_.drawing_point;
  primitive.points.push_back(state_.drawing_point);

  // Each remaining numeric byte carries direction codes; the run walks the
  // drawing point by |increment| per step. The exact sub-field packing of
  // Figures 58 to 61 is not modelled — see the design note in
  // docs-tech/nabts-support-design.md Phase 5 — so a byte is taken as one step
  // whose direction comes from its low three bits. A record using these draws
  // an approximation of its outline rather than the outline.
  NabtsPoint current = state_.drawing_point;
  while (!reader.empty() && primitive.points.size() < kNaplpsMaxVertices) {
    const uint8_t code = reader.read_fixed_byte();
    const int direction = code & 0x07;
    const double dx =
        (direction == 1 || direction == 2 || direction == 3)
            ? increment.x
            : ((direction == 5 || direction == 6 || direction == 7)
                   ? -increment.x
                   : 0.0);
    const double dy =
        (direction == 3 || direction == 4 || direction == 5)
            ? increment.y
            : ((direction == 7 || direction == 0 || direction == 1)
                   ? -increment.y
                   : 0.0);
    current = resolve_relative(current, NabtsPoint{dx, dy});
    primitive.points.push_back(current);
  }

  if (primitive.points.size() >= 2) {
    emit(std::move(primitive));
  }
  move_drawing_point(current);
}

// ---------------------------------------------------------------------------
// Emission and movement
// ---------------------------------------------------------------------------

NabtsPrimitive NaplpsInterpreter::make_primitive(
    NabtsPrimitiveKind kind) const {
  NabtsPrimitive primitive;
  primitive.kind = kind;
  primitive.logical_pel = state_.domain.logical_pel;
  primitive.line_texture = state_.texture.line_texture;
  primitive.texture_pattern = state_.texture.pattern;
  primitive.texture_mask_size = state_.texture.mask_size;
  primitive.highlighted = state_.texture.highlight;
  primitive.colour_mode = state_.colour.mode();
  primitive.colour = state_.colour.drawing_colour();
  primitive.background = state_.colour.background_colour();
  primitive.blinking = state_.blinking;
  return primitive;
}

void NaplpsInterpreter::emit(NabtsPrimitive primitive) {
  // §6.2.3 and §6.2.4: while a DRCS character or a texture mask is being
  // defined, "all drawing operations affect the DRCS storage buffer rather than
  // the display area".
  if (collecting_ == Collecting::kDrcs ||
      collecting_ == Collecting::kTextureMask) {
    draw_into_definition(primitive);
    return;
  }
  snapshot_.primitives.push_back(std::move(primitive));
}

void NaplpsInterpreter::move_drawing_point(const NabtsPoint& point) {
  state_.drawing_point = point;
  // §5.3.2.3.7 Table 10: move together (0) or drawing point leads (2) both take
  // the cursor with it.
  if (state_.text.move_attribute == 0 || state_.text.move_attribute == 2) {
    state_.cursor = point;
  }
}

void NaplpsInterpreter::move_cursor(const NabtsPoint& point) {
  state_.cursor = resolve(point);
  // Move together (0) or cursor leads (1).
  if (state_.text.move_attribute == 0 || state_.text.move_attribute == 1) {
    state_.drawing_point = state_.cursor;
  }
}

void NaplpsInterpreter::advance_cursor() {
  // §5.3.2.3.4 Table 8's fixed spacings are multiples of the character field
  // dimension lying parallel to the character path. Proportional spacing (code
  // 3) is font-dependent and §5.3.2.3.4 makes the algorithm
  // implementation-dependent, so it advances by one field — the floor the
  // standard guarantees: "it is at least as many characters per line as would
  // be allowed by the current character field dimensions".
  static constexpr double kSpacing[4] = {1.0, 1.25, 1.5, 1.0};
  const double factor = kSpacing[state_.text.intercharacter_spacing & 0x03];
  const double dx = std::fabs(state_.text.character_field.dx) * factor;
  const double dy = std::fabs(state_.text.character_field.dy) * factor;

  NabtsPoint next = state_.cursor;
  switch (state_.text.path) {
    case NabtsCharPath::kRight:
      next.x += dx;
      break;
    case NabtsCharPath::kLeft:
      next.x -= dx;
      break;
    case NabtsCharPath::kUp:
      next.y += dy;
      break;
    case NabtsCharPath::kDown:
      next.y -= dy;
      break;
  }

  // §5.3.2.3.6's automatic APR APD: a movement that would put any part of the
  // character field outside the active field wraps to the start of the next
  // row.
  const double field_right =
      state_.field_origin.x + std::fabs(state_.field_size.dx);
  if (state_.text.path == NabtsCharPath::kRight &&
      next.x + std::fabs(state_.text.character_field.dx) > field_right) {
    next.x = state_.field_origin.x;
    next.y -= std::fabs(state_.text.character_field.dy);
  }

  move_cursor(next);
}

// ---------------------------------------------------------------------------
// Definitions
// ---------------------------------------------------------------------------

void NaplpsInterpreter::begin_definition(Collecting what, uint8_t code) {
  collecting_ = what;
  definition_body_.clear();
  definition_code_ = code;
  drcs_target_ = nullptr;
  mask_target_ = nullptr;

  if (what == Collecting::kDrcs) {
    uint16_t width = 0;
    uint16_t height = 0;
    state_.drcs_buffer_size(width, height);
    drcs_target_ = state_.begin_drcs(code, width, height);
    if (drcs_target_ == nullptr) {
      ++snapshot_.diagnostics.storage_refusals;
      collecting_ = Collecting::kNothing;
      return;
    }
    last_drcs_code_ = code;
    have_last_drcs_code_ = true;
    return;
  }

  if (what == Collecting::kTextureMask) {
    // §6.2.4: the code must be 4/1 to 4/4 for mask A to D.
    if (code < 0x41 || code > 0x44) {
      ++snapshot_.diagnostics.ignored_controls;
      collecting_ = Collecting::kNothing;
      return;
    }
    const size_t index = static_cast<size_t>(code - 0x41);
    mask_target_ = &state_.texture_masks[index];
    // Table D1 item 5(3)(b): 16 by 16 stored elements.
    constexpr uint16_t kMaskSide = 16;
    mask_target_->width = kMaskSide;
    mask_target_->height = kMaskSide;
    mask_target_->elements.assign(static_cast<size_t>(kMaskSide) * kMaskSide,
                                  false);
  }
}

void NaplpsInterpreter::end_definition() {
  switch (collecting_) {
    case Collecting::kNothing:
      return;
    case Collecting::kMacro:
    case Collecting::kMacroExecuting:
      if (!state_.define_macro(definition_code_, std::move(definition_body_),
                               definition_is_transmit_)) {
        ++snapshot_.diagnostics.storage_refusals;
      }
      break;
    case Collecting::kDrcs:
    case Collecting::kTextureMask:
      // §6.2.3: "After the downloading sequence has been terminated, the
      // receiving device reverts to the normal procedure of mapping the unit
      // screen to the physical display screen, with the drawing point reset to
      // (0,0)."
      state_.drawing_point = NabtsPoint{0.0, 0.0};
      break;
  }
  collecting_ = Collecting::kNothing;
  definition_body_.clear();
  drcs_target_ = nullptr;
  mask_target_ = nullptr;
  definition_is_transmit_ = false;
}

void NaplpsInterpreter::draw_into_definition(const NabtsPrimitive& primitive) {
  // §6.2.3: the buffer's lower left corner is the unit screen's, and the larger
  // character-field dimension spans its whole axis. Each element the code
  // writes comes on "unless it is written into with nominal black, in which
  // case it is set to the off state".
  const bool black = primitive.colour.green == 0 && primitive.colour.red == 0 &&
                     primitive.colour.blue == 0 &&
                     !primitive.colour.transparent;

  uint16_t width = 0;
  uint16_t height = 0;
  std::vector<bool>* elements = nullptr;
  if (drcs_target_ != nullptr) {
    width = drcs_target_->width;
    height = drcs_target_->height;
    elements = &drcs_target_->elements;
  } else if (mask_target_ != nullptr) {
    width = mask_target_->width;
    height = mask_target_->height;
    elements = &mask_target_->elements;
  }
  if (elements == nullptr || width == 0 || height == 0) {
    return;
  }

  // Only the extent a primitive covers is written, and only along the axes the
  // buffer spans. A full rasteriser is deliberately not built here: the buffer
  // is a bitmap the renderer scales, and what the standard needs of a decoder
  // is which elements the drawing touched. Points, lines and rectangles cover
  // that — they are what a DRCS definition is made of in practice — and an arc
  // or polygon marks its control points' bounding box.
  const auto mark = [&](double x, double y) {
    const double fx =
        x /
        std::max(1e-9, std::fabs(state_.text.character_field.dx) *
                           (1.0 / std::fabs(state_.text.character_field.dx)));
    (void)fx;
    if (x < 0.0 || y < 0.0) {
      return;
    }
    const auto column = static_cast<size_t>(x * width);
    const auto row = static_cast<size_t>(y * height);
    if (column >= width || row >= height) {
      return;
    }
    (*elements)[row * width + column] = !black;
  };

  switch (primitive.kind) {
    case NabtsPrimitiveKind::kPoint:
    case NabtsPrimitiveKind::kCharacter:
      for (const NabtsPoint& point : primitive.points) {
        mark(point.x, point.y);
      }
      break;
    case NabtsPrimitiveKind::kLine:
    case NabtsPrimitiveKind::kArc:
    case NabtsPrimitiveKind::kPolygon:
    case NabtsPrimitiveKind::kIncrementalPoints: {
      // Bresenham-free: sample the polyline densely enough that no element the
      // path crosses is missed, which is one step per buffer element.
      const size_t steps = static_cast<size_t>(std::max(width, height)) * 2;
      for (size_t segment = 1; segment < primitive.points.size(); ++segment) {
        const NabtsPoint& a = primitive.points[segment - 1];
        const NabtsPoint& b = primitive.points[segment];
        for (size_t i = 0; i <= steps; ++i) {
          const double t = static_cast<double>(i) / static_cast<double>(steps);
          mark(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
        }
      }
      break;
    }
    case NabtsPrimitiveKind::kRectangle: {
      const double x0 =
          std::min(primitive.origin.x, primitive.origin.x + primitive.size.dx);
      const double x1 =
          std::max(primitive.origin.x, primitive.origin.x + primitive.size.dx);
      const double y0 =
          std::min(primitive.origin.y, primitive.origin.y + primitive.size.dy);
      const double y1 =
          std::max(primitive.origin.y, primitive.origin.y + primitive.size.dy);
      const double step_x = 1.0 / static_cast<double>(width) / 2.0;
      const double step_y = 1.0 / static_cast<double>(height) / 2.0;
      for (double y = y0; y <= y1; y += step_y) {
        for (double x = x0; x <= x1; x += step_x) {
          if (primitive.filled || x == x0 || y == y0 || x + step_x > x1 ||
              y + step_y > y1) {
            mark(x, y);
          }
        }
      }
      break;
    }
  }
}

void NaplpsInterpreter::invoke_macro(uint8_t code) {
  const NaplpsMacro* entry = state_.macro(code);
  if (entry == nullptr || !entry->defined) {
    ++snapshot_.diagnostics.unresolved_macros;
    return;
  }
  // §6.2.2.3: a transmit macro "when called, [is] not executed, but [is]
  // transmitted in their entirety to the host computer or to a local
  // application process". There is no host here, so it draws nothing.
  if (entry->transmit) {
    ++snapshot_.diagnostics.ignored_controls;
    return;
  }
  // The standard bounds nesting only by memory. A recovered record can contain
  // a macro that invokes itself — §6.2.2.2 rules that out for DEFP MACRO alone
  // — so the depth is capped and the overrun counted.
  if (frames_.size() > kNaplpsMaxMacroDepth) {
    ++snapshot_.diagnostics.unresolved_macros;
    return;
  }
  frames_.push_back(Frame{entry->code.data(), entry->code.size(), 0});
}

}  // namespace orc
