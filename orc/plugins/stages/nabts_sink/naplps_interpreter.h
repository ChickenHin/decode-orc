/*
 * File:        naplps_interpreter.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Run a NAPLPS presentation record into a resolved display list
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NAPLPS_INTERPRETER_H
#define ORC_NAPLPS_INTERPRETER_H

#include <orc/support/nabts_page.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "naplps_code_env.h"
#include "naplps_pdi.h"
#include "naplps_state.h"

namespace orc {

/**
 * @brief Runs a NAPLPS presentation record and emits what it drew
 *
 * One instance per record. The record is a program (CEA-516 §6.1, ANSI
 * X3.110-1983), and running it means walking the byte stream while three things
 * change underneath: which G-set a byte belongs to (NaplpsCodeEnvironment),
 * what the presentation attributes are (NaplpsState), and where the drawing
 * point is. Each drawing operation is emitted with the attributes that were in
 * force when it ran, so the display list needs none of that state to be
 * re-drawn.
 *
 * What is deliberately not done here is rasterisation. The primitives carry
 * their control points, their logical pel, their texture and their colours in
 * unit space; turning an arc into pixels, choosing a font, or stepping a
 * hatching pattern is the renderer's business. That keeps the MVP boundary
 * where the teletext viewer's is and keeps this testable against the standard
 * rather than against a screenshot.
 *
 * Two limits are enforced rather than assumed, because a recovered record has
 * lost packets in it and a decoder that trusts its input will hang on one:
 * macro nesting (kNaplpsMaxMacroDepth) and the shared storage budget
 * (kNaplpsSharedStorageBytes). Both are counted in the diagnostics.
 */
class NaplpsInterpreter {
 public:
  NaplpsInterpreter();

  /**
   * @brief Run |record| and return what it drew
   *
   * @param record Record data as CEA-516 §5.3 delivers it — the record header
   *               already removed, byte parity still in place
   *
   * Byte parity is stripped here rather than by the caller: §3.3 puts odd
   * parity in b8 of every data byte of a type-zero group, and a NAPLPS byte is
   * its low seven bits. A caller passing already-stripped bytes gets the same
   * result, since stripping twice is idempotent.
   *
   * Never throws. A record that runs out mid-sequence yields the primitives it
   * managed, with the shortfall in the diagnostics.
   */
  NabtsPageSnapshot run(const std::vector<uint8_t>& record);

  /// The state after the last run(), for a test that wants to inspect it.
  const NaplpsState& state() const { return state_; }

 private:
  /// Where bytes are being executed from: the record, or an expanding macro.
  struct Frame {
    const uint8_t* bytes = nullptr;
    size_t length = 0;
    size_t position = 0;
  };

  /// Where a definition in progress is collecting its bytes.
  enum class Collecting : uint8_t {
    kNothing,
    /// DEF MACRO / DEFT MACRO: stored, not executed (§6.2.2.1, §6.2.2.3).
    kMacro,
    /// DEFP MACRO: stored *and* executed (§6.2.2.2).
    kMacroExecuting,
    /// DEF DRCS: executed into a character buffer (§6.2.3).
    kDrcs,
    /// DEF TEXTURE: executed into a mask buffer (§6.2.4).
    kTextureMask,
  };

  // ---- The main loop -------------------------------------------------------

  /// Execute one byte from the top frame. Returns false when the frame is done.
  bool step();

  void execute_byte(uint8_t byte);
  void execute_c0(uint8_t byte);
  void execute_escape();
  void execute_c1(NaplpsC1 control);
  void execute_graphic(uint8_t byte);

  // ---- PDI ----------------------------------------------------------------

  /**
   * @brief Execute the PDI opcode |opcode| with the numeric data that follows
   *
   * The operand bytes are gathered first, because §5.3.2.2.5 makes the count
   * decide the meaning — short is zero-extended, long repeats the opcode.
   * §5.3.1 defines the run's end: "A PDI sequence is terminated by an opcode
   * introducing the next PDI sequence or by any other presentation layer code
   * not from the numeric data section of the same PDI set", with the
   * transparent controls of §6.1.4-6.1.6.1 explicitly not terminating it.
   */
  void execute_pdi(uint8_t opcode);

  /// Collect the numeric data bytes following the current position.
  std::vector<uint8_t> gather_operands();

  void pdi_reset(NaplpsOperandReader& reader);
  void pdi_domain(NaplpsOperandReader& reader);
  void pdi_text(NaplpsOperandReader& reader);
  void pdi_texture(NaplpsOperandReader& reader);
  void pdi_set_colour(NaplpsOperandReader& reader, size_t operand_bytes);
  void pdi_select_colour(NaplpsOperandReader& reader, size_t operand_bytes);
  void pdi_blink(NaplpsOperandReader& reader, size_t operand_bytes);
  void pdi_wait(NaplpsOperandReader& reader);
  void pdi_point(NaplpsPdi opcode, NaplpsOperandReader& reader);
  void pdi_line(NaplpsPdi opcode, NaplpsOperandReader& reader);
  void pdi_arc(NaplpsPdi opcode, NaplpsOperandReader& reader);
  void pdi_rect(NaplpsPdi opcode, NaplpsOperandReader& reader);
  void pdi_poly(NaplpsPdi opcode, NaplpsOperandReader& reader);
  void pdi_field(NaplpsOperandReader& reader);
  void pdi_incremental(NaplpsPdi opcode, NaplpsOperandReader& reader,
                       const std::vector<uint8_t>& operands);

  // ---- Emission ------------------------------------------------------------

  /// A primitive with the current attributes already filled in.
  NabtsPrimitive make_primitive(NabtsPrimitiveKind kind) const;

  /// Emit |primitive|, into the display list or into whatever definition buffer
  /// is collecting — §6.2.3 and §6.2.4 both redirect drawing rather than
  /// suppressing it.
  void emit(NabtsPrimitive primitive);

  /// Move the drawing point to |point|, taking the cursor with it if the move
  /// attribute says to (§5.3.2.3.7).
  void move_drawing_point(const NabtsPoint& point);
  /// Move the cursor, taking the drawing point with it if the move attribute
  /// says to.
  void move_cursor(const NabtsPoint& point);

  /// |point| resolved into the unit screen, counting a clamp if it needed one.
  NabtsPoint resolve(NabtsPoint point);
  /// |base| displaced by |delta|, resolved.
  NabtsPoint resolve_relative(const NabtsPoint& base, const NabtsPoint& delta);

  /// Advance the cursor one character along the character path (§5.3.2.3.3).
  void advance_cursor();

  // ---- Definitions ---------------------------------------------------------

  /// Start collecting for |what| at code |code|.
  void begin_definition(Collecting what, uint8_t code);
  /// Finish whatever is being collected.
  void end_definition();

  /// Rasterise |primitive| into the definition buffer, which is what §6.2.3
  /// means by "all drawing operations affect the DRCS storage buffer rather
  /// than the display area".
  void draw_into_definition(const NabtsPrimitive& primitive);

  /// Invoke the macro at |code| by pushing a frame for it.
  void invoke_macro(uint8_t code);

  NaplpsCodeEnvironment env_;
  NaplpsState state_;
  NabtsPageSnapshot snapshot_;

  /// Execution frames: the record at the bottom, expanding macros above it.
  std::vector<Frame> frames_;
  /// The record's own bytes, parity stripped, which frames_[0] points into.
  std::vector<uint8_t> record_;

  Collecting collecting_ = Collecting::kNothing;
  /// Bytes gathered for a macro definition.
  std::vector<uint8_t> definition_body_;
  uint8_t definition_code_ = 0;
  bool definition_is_transmit_ = false;
  /// The DRCS character or texture mask being written into.
  NabtsDrcsCharacter* drcs_target_ = nullptr;
  NabtsTextureMask* mask_target_ = nullptr;

  /// §6.2.3: the last DRCS code defined, so a DEF DRCS that terminates another
  /// takes "the next character of the DRCS G-set (ie, in the circular sequence
  /// 2/0, 2/1, ... 7/15, 2/0 ...)".
  uint8_t last_drcs_code_ = kNaplpsLastCode;
  bool have_last_drcs_code_ = false;

  /// The last graphic byte executed, which REPEAT (§6.2.7.2) repeats.
  uint8_t last_graphic_ = 0;
  bool have_last_graphic_ = false;
};

}  // namespace orc

#endif  // ORC_NAPLPS_INTERPRETER_H
