/*
 * File:        naplps_lint.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     NAPLPS linter implementation (X3.110 §4.3, §5.3, §6)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_lint.h"

#include <fmt/format.h>

#include <algorithm>
#include <utility>

#include "vbi-services/teletext_page_decoder.h"

namespace orc {

namespace {

/// CEA-516 §3.3 puts odd parity in b8 of every data byte of a type-zero group,
/// so a NAPLPS byte is the low seven bits.
constexpr uint8_t kSevenBits = 0x7F;

/// Graphic bytes of a 7-bit in-use table: columns 2 to 7.
constexpr bool is_graphic(uint8_t byte) { return byte >= 0x20; }

/// C0 controls: columns 0 and 1.
constexpr bool is_c0(uint8_t byte) { return byte < 0x20; }

/// §6.2.4: the four programmable texture masks are named 4/1 to 4/4.
constexpr uint8_t kFirstTextureMaskCode = 0x41;
constexpr uint8_t kLastTextureMaskCode = 0x44;

/// §6.2.7.2: REPEAT's count byte comes from columns 4 to 7.
constexpr uint8_t kFirstRepeatCountCode = 0x40;

/// The bound on nesting the interpreter enforces, mirrored so the two agree on
/// which invocations resolve (naplps_state.h kNaplpsMaxMacroDepth).
constexpr size_t kMaxMacroDepth = 8;

/// Words an absolute_mask can describe. A group longer than this has no
/// absolute words past the eighth in any opcode the standard defines.
constexpr size_t kMaxMaskedWords = 8;

/**
 * @brief How an opcode's numeric data divides into operands
 *
 * X3.110 gives each PDI a fixed operand shape (§5.3.2, §5.3.3), and §5.3.2.2
 * makes the *size* of an operand word a property of the presentation state
 * rather than of the opcode. Together they say how a gathered run of numeric
 * bytes should divide up — which is what makes a run that does not divide
 * evenly worth reporting.
 */
enum class OperandClass : uint8_t {
  /// No operands defined.
  kNone,
  /// Fixed-format bytes only (§5.3.1), any number of which is legal.
  kFixed,
  /// Operands of more than one shape in a repeating group that no single word
  /// size describes — BLINK's address-plus-three-intervals (§5.3.2.7.3).
  kIrregular,
  /// Single-value operands (§5.3.2.2.2).
  kSingleValue,
  /// Colour operands (§5.3.2.5.1), which are explicitly allowed to be short.
  kColour,
  /// Coordinate words (§5.3.2.2.3, Figure 11).
  kCoordinate,
  /// The incremental group's raw bit stream (§5.3.3.6), which has no word
  /// structure at all.
  kIncremental,
};

struct PdiGrammar {
  OperandClass operand_class = OperandClass::kNone;
  /// Fixed-format bytes before the word operands begin.
  uint8_t fixed_lead = 0;
  /// Words one complete execution of the opcode needs.
  uint8_t minimum_words = 0;
  /// Words per repeated execution (§5.3.2.2.5), or 0 where the run is a
  /// variable-length list rather than a repeating group.
  uint8_t group_words = 0;
  /// Bit i set: word i is an absolute coordinate, and so must lie within the
  /// unit screen. Indexed within the group where there is one, and from the
  /// start of the run where there is not.
  uint8_t absolute_mask = 0;
};

/**
 * @brief The operand shape of |opcode|
 *
 * Only the absolute coordinate words are marked. A relative word is a
 * displacement, which Figure 11 makes a two's-complement value in [-1, 1) — it
 * cannot be out of range on its own, and whether the point it resolves to is
 * depends on where the drawing point had got to. Tracking that would mean
 * restating the interpreter's geometry here, where it could drift; the
 * interpreter counts the resolved clamps itself
 * (NabtsDecodeDiagnostics::out_of_range_coordinates).
 */
PdiGrammar grammar_of(NaplpsPdi opcode) {
  switch (opcode) {
    // §5.3.2.9.1: a two-byte fixed operand, and no operands means no action.
    case NaplpsPdi::kReset:
      return {OperandClass::kFixed, 0, 0, 0, 0};
    // §5.3.2.2: one fixed byte, then an optional logical pel size — a size
    // rather than a position, so not range-checked.
    case NaplpsPdi::kDomain:
      return {OperandClass::kCoordinate, 1, 0, 0, 0};
    // §5.3.2.3.1: two fixed bytes, then an optional character field size.
    case NaplpsPdi::kText:
      return {OperandClass::kCoordinate, 2, 0, 0, 0};
    // §5.3.2.4: one fixed byte, then optional mask parameters.
    case NaplpsPdi::kTexture:
      return {OperandClass::kCoordinate, 1, 0, 0, 0};
    // §5.3.2.5.1 allows a short colour operand, so no alignment is expected.
    case NaplpsPdi::kSetColour:
      return {OperandClass::kColour, 0, 0, 0, 0};
    // §5.3.2.6: zero, one or two single-value map addresses.
    case NaplpsPdi::kSelectColour:
      return {OperandClass::kSingleValue, 0, 0, 1, 0};
    // §5.3.2.7.3: an address then three interval bytes, repeating.
    case NaplpsPdi::kBlink:
      return {OperandClass::kIrregular, 0, 0, 0, 0};
    // §5.3.2.8: fixed bytes of delay.
    case NaplpsPdi::kWait:
      return {OperandClass::kFixed, 0, 0, 0, 0};

    // §5.3.3.1: one coordinate per point, repeating.
    case NaplpsPdi::kPointSetAbs:
    case NaplpsPdi::kPointAbs:
      return {OperandClass::kCoordinate, 0, 1, 1, 0x1};
    case NaplpsPdi::kPointSetRel:
    case NaplpsPdi::kPointRel:
      return {OperandClass::kCoordinate, 0, 1, 1, 0x0};

    // §5.3.3.2: the plain forms start at the drawing point and carry only an
    // end point; the SET forms carry an absolute start (§5.3.3.2.4) first.
    case NaplpsPdi::kLineAbs:
      return {OperandClass::kCoordinate, 0, 1, 1, 0x1};
    case NaplpsPdi::kLineRel:
      return {OperandClass::kCoordinate, 0, 1, 1, 0x0};
    case NaplpsPdi::kSetLineAbs:
      return {OperandClass::kCoordinate, 0, 2, 2, 0x3};
    case NaplpsPdi::kSetLineRel:
      return {OperandClass::kCoordinate, 0, 2, 2, 0x1};

    // §5.3.3.3: a start, then a list of relative control points of any length.
    case NaplpsPdi::kArcOutlined:
    case NaplpsPdi::kArcFilled:
      return {OperandClass::kCoordinate, 0, 1, 0, 0x0};
    case NaplpsPdi::kSetArcOutlined:
    case NaplpsPdi::kSetArcFilled:
      return {OperandClass::kCoordinate, 0, 2, 0, 0x1};

    // §5.3.3.4: an extent, which is a size; the SET forms prefix an absolute
    // corner.
    case NaplpsPdi::kRectOutlined:
    case NaplpsPdi::kRectFilled:
      return {OperandClass::kCoordinate, 0, 1, 1, 0x0};
    case NaplpsPdi::kSetRectOutlined:
    case NaplpsPdi::kSetRectFilled:
      return {OperandClass::kCoordinate, 0, 2, 2, 0x1};

    // §5.3.3.5: a vertex list. A polygon needs three points, one of which is
    // the drawing point unless the SET form supplies it.
    case NaplpsPdi::kPolyOutlined:
    case NaplpsPdi::kPolyFilled:
      return {OperandClass::kCoordinate, 0, 2, 0, 0x0};
    case NaplpsPdi::kSetPolyOutlined:
    case NaplpsPdi::kSetPolyFilled:
      return {OperandClass::kCoordinate, 0, 3, 0, 0x1};

    // §5.3.3.6.2: an absolute origin and an extent.
    case NaplpsPdi::kField:
      return {OperandClass::kCoordinate, 0, 2, 0, 0x1};

    // §5.3.3.6.3-5: a bit stream whose meaning is set by the FIELD before it.
    case NaplpsPdi::kIncrPoint:
    case NaplpsPdi::kIncrLine:
    case NaplpsPdi::kIncrPolyFilled:
      return {OperandClass::kIncremental, 0, 0, 0, 0};
  }
  return {};
}

/// Bytes one operand word of |grammar| occupies under |format|, or zero where
/// the class has no word structure to divide by.
size_t word_bytes(const PdiGrammar& grammar,
                  const NaplpsOperandFormat& format) {
  switch (grammar.operand_class) {
    case OperandClass::kSingleValue:
      return format.single_value_bytes;
    case OperandClass::kCoordinate:
      return format.multi_value_bytes;
    case OperandClass::kNone:
    case OperandClass::kFixed:
    case OperandClass::kIrregular:
    case OperandClass::kColour:
    case OperandClass::kIncremental:
      break;
  }
  return 0;
}

/// Whether word |index| of a run under |grammar| is an absolute coordinate.
bool word_is_absolute(const PdiGrammar& grammar, size_t index) {
  const size_t masked =
      grammar.group_words > 0 ? index % grammar.group_words : index;
  if (masked >= kMaxMaskedWords) {
    return false;
  }
  return ((grammar.absolute_mask >> masked) & 0x1u) != 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Damage evidence
// ---------------------------------------------------------------------------

NaplpsSuspectMap NaplpsSuspectMap::from_record(
    const std::vector<uint8_t>& record, const std::vector<uint8_t>& present,
    const std::vector<uint8_t>& confidence, uint8_t floor) {
  NaplpsSuspectMap out;
  out.flags_.assign(record.size(), 0);
  for (size_t i = 0; i < record.size(); ++i) {
    uint8_t flags = 0;
    if (!teletext_odd_parity_valid(record[i])) {
      flags |= kNaplpsSuspectParity;
    }
    // A byte the group assembler is holding a hole open for, or — where no mask
    // was kept — the zero filler it writes into one. Either way nothing arrived
    // here, so there is no odd number of wrong bits to reason about.
    if ((i < present.size() && present[i] == 0) ||
        (present.empty() && record[i] == 0)) {
      flags |= kNaplpsSuspectMissing;
    }
    // A copy nothing measured is read as full confidence throughout, which is
    // the only honest reading for a detector with no way of saying it is
    // unsure — the same reading the record catalogue's vote gives it.
    if (i < confidence.size() && confidence[i] <= floor) {
      flags |= kNaplpsSuspectConfidence;
    }
    out.flags_[i] = flags;
  }
  return out;
}

size_t NaplpsSuspectMap::suspect_count() const {
  size_t count = 0;
  for (const uint8_t flags : flags_) {
    if (flags != 0) {
      ++count;
    }
  }
  return count;
}

bool NaplpsSuspectMap::any_suspect(size_t offset, size_t length) const {
  for (size_t i = offset; i < offset + length; ++i) {
    if (suspect(i)) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Naming
// ---------------------------------------------------------------------------

const char* naplps_span_kind_name(NaplpsSpanKind kind) {
  switch (kind) {
    case NaplpsSpanKind::kControl:
      return "control";
    case NaplpsSpanKind::kTransparentControl:
      return "transparent control";
    case NaplpsSpanKind::kControlOperand:
      return "control operand";
    case NaplpsSpanKind::kEscape:
      return "escape sequence";
    case NaplpsSpanKind::kDefinitionCode:
      return "definition code";
    case NaplpsSpanKind::kPdiOpcode:
      return "PDI opcode";
    case NaplpsSpanKind::kPdiOperand:
      return "PDI operand";
    case NaplpsSpanKind::kGraphic:
      return "graphic character";
    case NaplpsSpanKind::kMacroInvocation:
      return "macro invocation";
    case NaplpsSpanKind::kNullSet:
      return "null set";
    case NaplpsSpanKind::kMacroBody:
      return "macro body";
  }
  return "unknown";
}

const char* naplps_lint_rule_name(NaplpsLintRule rule) {
  switch (rule) {
    case NaplpsLintRule::kMalformedEscape:
      return "malformed escape sequence";
    case NaplpsLintRule::kTruncatedEscape:
      return "escape sequence cut short by the end of the record";
    case NaplpsLintRule::kNullDesignation:
      return "designation of a set outside the standard";
    case NaplpsLintRule::kTruncatedPdi:
      return "PDI with too few operand bytes";
    case NaplpsLintRule::kTruncatedControl:
      return "control cut short of the bytes it takes";
    case NaplpsLintRule::kOperandRunMisaligned:
      return "operand run not a whole number of words";
    case NaplpsLintRule::kOrphanedOperand:
      return "numeric data with no opcode";
    case NaplpsLintRule::kCoordinateOutOfRange:
      return "absolute coordinate outside the unit screen";
    case NaplpsLintRule::kHoleInOperandRun:
      return "lost byte inside an operand run";
    case NaplpsLintRule::kSuspectInOperandRun:
      return "damaged byte inside an operand run";
    case NaplpsLintRule::kInvalidDefinitionCode:
      return "definition code outside the graphic range";
    case NaplpsLintRule::kInvalidTextureCode:
      return "texture mask other than A to D";
    case NaplpsLintRule::kUndefinedMacro:
      return "invocation of an undefined macro";
    case NaplpsLintRule::kMacroRecursionTooDeep:
      return "macro expansion nested past the depth a receiver holds";
    case NaplpsLintRule::kUnterminatedDefinition:
      return "definition left open at the end of the record";
    case NaplpsLintRule::kInvalidRepeat:
      return "REPEAT with nothing to repeat";
    case NaplpsLintRule::kDanglingSingleShift:
      return "single shift with no character after it";
    case NaplpsLintRule::kVertexOverflow:
      return "more vertices than a receiver must hold";
  }
  return "unknown";
}

NaplpsLintSeverity naplps_lint_severity(NaplpsLintRule rule) {
  switch (rule) {
    // Legal under the standard, and not what a service does. §4.3.2 defines
    // the null designation, §5.3.2.2.5 defines both the short and the long
    // operand run, and Table D1 item 4 bounds what a *receiver* must hold
    // rather than what a sender may transmit.
    case NaplpsLintRule::kNullDesignation:
    case NaplpsLintRule::kOperandRunMisaligned:
    case NaplpsLintRule::kSuspectInOperandRun:
    case NaplpsLintRule::kVertexOverflow:
      return NaplpsLintSeverity::kWarning;
    default:
      break;
  }
  return NaplpsLintSeverity::kError;
}

// ---------------------------------------------------------------------------
// Findings
// ---------------------------------------------------------------------------

uint32_t NaplpsLintFindings::count(NaplpsLintRule rule) const {
  const auto it = counts.find(rule);
  return it == counts.end() ? 0u : it->second;
}

std::string NaplpsLintFindings::summary() const {
  if (clean()) {
    return {};
  }
  std::string out = fmt::format("NAPLPS lint: {} error(s), {} warning(s)\n",
                                errors, warnings);
  // counts is ordered by rule, so a report of two runs lists them the same way.
  for (const auto& [rule, count] : counts) {
    out += fmt::format("  {:5}  {}\n", count, naplps_lint_rule_name(rule));
  }
  if (dropped > 0) {
    out +=
        fmt::format("  (offsets recorded for the first {}; {} more counted)\n",
                    findings.size(), dropped);
  }
  return out;
}

const NaplpsSpan* NaplpsLintResult::span_at(size_t offset) const {
  // Spans are emitted in ascending offset order and never overlap, so the one
  // covering an offset is the last that starts at or before it.
  const auto it = std::upper_bound(
      spans.begin(), spans.end(), offset,
      [](size_t value, const NaplpsSpan& span) { return value < span.offset; });
  if (it == spans.begin()) {
    return nullptr;
  }
  const NaplpsSpan& span = *(it - 1);
  if (offset < span.offset + span.length) {
    return &span;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// The linter
// ---------------------------------------------------------------------------

NaplpsLinter::NaplpsLinter() { reset_decoder(); }

void NaplpsLinter::reset_decoder() {
  env_.reset();
  format_ = NaplpsOperandFormat{};
  macros_.clear();
  frames_.clear();
  record_.clear();
  collecting_ = Collecting::kNothing;
  definition_body_.clear();
  definition_code_ = 0;
  definition_is_transmit_ = false;
  defining_frame_index_ = 0;
  definition_offset_ = 0;
  have_last_graphic_ = false;
  single_shift_open_ = false;
  single_shift_offset_ = 0;
}

NaplpsLintResult NaplpsLinter::lint(const std::vector<uint8_t>& record) {
  return lint(record, NaplpsSuspectMap::from_record(record));
}

NaplpsLintResult NaplpsLinter::lint(const std::vector<uint8_t>& record,
                                    const NaplpsSuspectMap& suspects) {
  NaplpsLintResult result;
  result_ = &result;
  suspects_ = &suspects;

  // Only the per-record transients start fresh, exactly as
  // NaplpsInterpreter::run() has it: macros and the code environment carry over
  // from whatever ran before, because that is what a receiver does.
  frames_.clear();
  collecting_ = Collecting::kNothing;
  definition_body_.clear();
  defining_frame_index_ = 0;
  definition_offset_ = 0;
  single_shift_open_ = false;

  record_.clear();
  record_.reserve(record.size());
  for (const uint8_t byte : record) {
    record_.push_back(static_cast<uint8_t>(byte & kSevenBits));
  }

  frames_.push_back(Frame{record_.data(), record_.size(), 0});
  while (!frames_.empty()) {
    if (!step()) {
      frames_.pop_back();
      if (!frames_.empty() && defining_frame_index_ >= frames_.size()) {
        defining_frame_index_ = frames_.size() - 1;
      }
    }
  }

  // Reported once the frames have run out, so add_finding_at rather than
  // add_finding: the walk is no longer in the record, but these offsets are the
  // record's own.
  if (collecting_ != Collecting::kNothing) {
    add_finding_at(NaplpsLintRule::kUnterminatedDefinition, definition_offset_,
                   1, definition_code_);
  }
  // §4.3.2 makes a single shift affect the next character; one still pending
  // when the record runs out never got one.
  if (single_shift_open_) {
    add_finding_at(NaplpsLintRule::kDanglingSingleShift, single_shift_offset_);
  }
  end_definition();

  result_ = nullptr;
  suspects_ = nullptr;
  return result;
}

bool NaplpsLinter::step() {
  const size_t frame_index = frames_.size() - 1;
  if (frames_[frame_index].position >= frames_[frame_index].length) {
    return false;
  }
  const size_t offset = frames_[frame_index].position;
  const uint8_t byte =
      frames_[frame_index].bytes[frames_[frame_index].position];
  ++frames_[frame_index].position;

  const bool from_defining_frame = frame_index == defining_frame_index_;

  // A macro definition stores its bytes rather than executing them (§6.2.2.1);
  // DEFP MACRO is the exception, being "simultaneously executed and stored"
  // (§6.2.2.2). Either way the only thing that ends it is a terminator.
  if (collecting_ == Collecting::kMacro ||
      collecting_ == Collecting::kMacroExecuting) {
    if (from_defining_frame) {
      if (byte == kNaplpsEsc) {
        const Frame& current = frames_[frame_index];
        const NaplpsEscape escape =
            naplps_parse_escape(current.bytes + current.position,
                                current.length - current.position);
        if (escape.kind == NaplpsEscapeKind::kControl &&
            naplps_terminates_definition(escape.c1)) {
          // §6.2.2.1: neither the terminating control nor its ESC is stored.
          frames_[frame_index].position += escape.length - 1;
          add_span(NaplpsSpanKind::kEscape, offset, escape.length);
          end_definition();
          execute_c1(escape.c1, offset);
          return true;
        }
      }
      if (collecting_ == Collecting::kMacro) {
        add_span(NaplpsSpanKind::kMacroBody, offset, 1);
        definition_body_.push_back(byte);
        return true;
      }
      // DEFP MACRO: execute the byte, then store it together with whatever
      // lookahead its execution consumed from this frame.
      execute_byte(byte, offset);
      const Frame& defining = frames_[defining_frame_index_];
      definition_body_.insert(definition_body_.end(), defining.bytes + offset,
                              defining.bytes + defining.position);
      return true;
    }
    // A macro expanding inside a DEFP MACRO body: executed, not stored.
    execute_byte(byte, offset);
    return true;
  }

  execute_byte(byte, offset);
  return true;
}

void NaplpsLinter::execute_byte(uint8_t byte, size_t offset) {
  if (is_c0(byte)) {
    execute_c0(byte, offset);
    return;
  }
  execute_graphic(byte, offset);
}

void NaplpsLinter::execute_c0(uint8_t byte, size_t offset) {
  if (naplps_is_transparent_control(byte)) {
    add_span(NaplpsSpanKind::kTransparentControl, offset, 1);
    return;
  }

  switch (byte) {
    case kNaplpsEsc:
      execute_escape(offset);
      return;

    case kNaplpsSi:
      add_span(NaplpsSpanKind::kControl, offset, 1);
      env_.invoke_locking(NaplpsGSlot::kG0);
      single_shift_open_ = false;
      return;
    case kNaplpsSo:
      add_span(NaplpsSpanKind::kControl, offset, 1);
      env_.invoke_locking(NaplpsGSlot::kG1);
      single_shift_open_ = false;
      return;
    case kNaplpsSs2:
      add_span(NaplpsSpanKind::kControl, offset, 1);
      env_.invoke_single_shift(NaplpsGSlot::kG2);
      single_shift_open_ = true;
      single_shift_offset_ = offset;
      return;
    case kNaplpsSs3:
      add_span(NaplpsSpanKind::kControl, offset, 1);
      env_.invoke_single_shift(NaplpsGSlot::kG3);
      single_shift_open_ = true;
      single_shift_offset_ = offset;
      return;

    case kNaplpsNsr: {
      add_span(NaplpsSpanKind::kControl, offset, 1);
      // §6.1.6.5 items (1) and (2): the code environment and DOMAIN go back to
      // their defaults, which is what decides how the bytes after this are
      // read. The rest of the reset is presentation state the linter has no
      // use for.
      env_.reset();
      format_ = NaplpsOperandFormat{};
      single_shift_open_ = false;

      // §6.1.6.5(6): two bytes from columns 4 to 7 are a row and column address
      // and are consumed; two from columns 2 and 3 are consumed and ignored;
      // anything else is left to be executed in its own right.
      Frame& frame = frames_.back();
      if (frame.position + 1 < frame.length) {
        const uint8_t row_byte = frame.bytes[frame.position];
        const uint8_t column_byte = frame.bytes[frame.position + 1];
        const auto is_address = [](uint8_t value) {
          return value >= 0x40 && value <= 0x7F;
        };
        const auto is_ignored = [](uint8_t value) {
          return value >= 0x20 && value < 0x40;
        };
        if ((is_address(row_byte) && is_address(column_byte)) ||
            (is_ignored(row_byte) && is_ignored(column_byte))) {
          add_span(NaplpsSpanKind::kControlOperand, frame.position, 2);
          frame.position += 2;
        }
      }
      return;
    }

    case kNaplpsCan:
      add_span(NaplpsSpanKind::kControl, offset, 1);
      // §6.1.6.3: terminate every executing macro. Execution resumes in the
      // record, which is frame 0.
      while (frames_.size() > 1) {
        frames_.pop_back();
      }
      if (defining_frame_index_ >= frames_.size()) {
        defining_frame_index_ = frames_.size() - 1;
      }
      return;

    case kNaplpsAps: {
      add_span(NaplpsSpanKind::kControl, offset, 1);
      // §6.1.2.4: the two bytes following are a row and column address, unless
      // either is a control — in which case "the APS is ignored and the C0 or
      // C1 control is executed".
      Frame& frame = frames_.back();
      if (frame.position + 1 >= frame.length) {
        add_finding(NaplpsLintRule::kTruncatedControl, offset, 1, byte);
        return;
      }
      if (is_c0(frame.bytes[frame.position]) ||
          is_c0(frame.bytes[frame.position + 1])) {
        return;
      }
      add_span(NaplpsSpanKind::kControlOperand, frame.position, 2);
      frame.position += 2;
      return;
    }

    default:
      // The format effectors, CS, APH, BEL and SDC: none of them changes how a
      // later byte is read, and none of them takes an operand.
      add_span(NaplpsSpanKind::kControl, offset, 1);
      return;
  }
}

void NaplpsLinter::execute_escape(size_t offset) {
  Frame& frame = frames_.back();
  const NaplpsEscape escape = naplps_parse_escape(
      frame.bytes + frame.position, frame.length - frame.position);

  // |length| counts the ESC, which has already been consumed.
  frame.position += escape.length - 1;
  add_span(NaplpsSpanKind::kEscape, offset, escape.length);

  switch (escape.kind) {
    case NaplpsEscapeKind::kDesignation:
      if (escape.set == NaplpsGSet::kNull) {
        add_finding(NaplpsLintRule::kNullDesignation, offset, escape.length);
      }
      env_.designate(escape.slot, escape.set);
      return;
    case NaplpsEscapeKind::kLockingShift:
      env_.invoke_locking(escape.slot);
      single_shift_open_ = false;
      return;
    case NaplpsEscapeKind::kControl:
      execute_c1(escape.c1, offset);
      return;
    case NaplpsEscapeKind::kUnsupported:
      // A well-formed sequence naming something outside Table 1. §4.3.2 makes
      // it a null designation, which is already reported as its own rule when
      // the parser can say which slot; here it cannot, so it is the same
      // finding at the sequence.
      add_finding(NaplpsLintRule::kNullDesignation, offset, escape.length);
      return;
    case NaplpsEscapeKind::kMalformed:
      add_finding(NaplpsLintRule::kMalformedEscape, offset, escape.length);
      return;
    case NaplpsEscapeKind::kTruncated:
      add_finding(NaplpsLintRule::kTruncatedEscape, offset, escape.length);
      return;
  }
}

void NaplpsLinter::execute_c1(NaplpsC1 control, size_t escape_offset) {
  switch (control) {
    case NaplpsC1::kDefMacro:
    case NaplpsC1::kDefpMacro:
    case NaplpsC1::kDeftMacro:
    case NaplpsC1::kDefDrcs:
    case NaplpsC1::kDefTexture: {
      const Collecting terminated = collecting_;
      end_definition();

      // §6.2.3's one exception: a DEF DRCS terminating a previous DEF DRCS
      // takes no code byte, the next character of the circular sequence being
      // implied. Consuming one here would eat the first byte of the definition.
      //
      // Which character of the sequence it is does not matter to the linter —
      // no byte is read differently for it — so the code is carried over rather
      // than advanced, and only the byte the interpreter does not consume is
      // mirrored.
      if (control == NaplpsC1::kDefDrcs && terminated == Collecting::kDrcs) {
        begin_definition(Collecting::kDrcs, definition_code_);
        definition_offset_ = escape_offset;
        return;
      }

      Frame& frame = frames_.back();
      uint8_t code = 0;
      if (frame.position < frame.length) {
        code = frame.bytes[frame.position];
        if (!is_graphic(code)) {
          // §6.2.2.1: "If the character following the DEF MACRO control is not
          // in this range, the entire command ... is in error and is executed
          // as a null operation." The offending byte is left to be executed.
          add_finding(NaplpsLintRule::kInvalidDefinitionCode, frame.position, 1,
                      code);
          return;
        }
        add_span(NaplpsSpanKind::kDefinitionCode, frame.position, 1);
        ++frame.position;
      }

      switch (control) {
        case NaplpsC1::kDefMacro:
          definition_is_transmit_ = false;
          begin_definition(Collecting::kMacro, code);
          break;
        case NaplpsC1::kDefpMacro:
          definition_is_transmit_ = false;
          begin_definition(Collecting::kMacroExecuting, code);
          break;
        case NaplpsC1::kDeftMacro:
          definition_is_transmit_ = true;
          begin_definition(Collecting::kMacro, code);
          break;
        case NaplpsC1::kDefDrcs:
          begin_definition(Collecting::kDrcs, code);
          break;
        default:
          // §6.2.4: the code must name mask A to D.
          if (code < kFirstTextureMaskCode || code > kLastTextureMaskCode) {
            add_finding(NaplpsLintRule::kInvalidTextureCode, escape_offset, 1,
                        code);
            return;
          }
          begin_definition(Collecting::kTextureMask, code);
          break;
      }
      definition_offset_ = escape_offset;
      return;
    }

    case NaplpsC1::kEnd:
      end_definition();
      return;

    case NaplpsC1::kRepeat: {
      Frame& frame = frames_.back();
      if (frame.position >= frame.length || !have_last_graphic_) {
        add_finding(NaplpsLintRule::kInvalidRepeat, escape_offset);
        return;
      }
      const uint8_t count_byte = frame.bytes[frame.position];
      // §6.2.7.2: the count byte must be 4/0 through 7/15.
      if (count_byte < kFirstRepeatCountCode) {
        add_finding(NaplpsLintRule::kInvalidRepeat, frame.position, 1,
                    count_byte);
        return;
      }
      add_span(NaplpsSpanKind::kControlOperand, frame.position, 1);
      ++frame.position;
      return;
    }

    case NaplpsC1::kRepeatToEol:
      if (!have_last_graphic_) {
        add_finding(NaplpsLintRule::kInvalidRepeat, escape_offset);
      }
      return;

    default:
      // Every remaining C1 control sets a presentation attribute and takes no
      // operand, so none of them changes how a later byte is read.
      return;
  }
}

void NaplpsLinter::execute_graphic(uint8_t byte, size_t offset) {
  const NaplpsGSet set = env_.in_use();
  env_.consume_character();
  single_shift_open_ = false;

  switch (set) {
    case NaplpsGSet::kPdi:
      // §5.3.1: b7 clear is an opcode, b7 set is numeric data.
      if (naplps_is_pdi_opcode(byte)) {
        add_span(NaplpsSpanKind::kPdiOpcode, offset, 1);
        execute_pdi(byte, offset);
        return;
      }
      // Numeric data with no opcode in front of it has nothing to be an
      // operand of, and the interpreter discards it.
      add_span(NaplpsSpanKind::kPdiOperand, offset, 1);
      add_finding(NaplpsLintRule::kOrphanedOperand, offset, 1, byte);
      return;

    case NaplpsGSet::kMacro:
      add_span(NaplpsSpanKind::kMacroInvocation, offset, 1);
      invoke_macro(byte, offset);
      return;

    case NaplpsGSet::kNull:
      // §4.3.2: "A null set is a set in which all code positions are executed
      // as null operations."
      add_span(NaplpsSpanKind::kNullSet, offset, 1);
      return;

    case NaplpsGSet::kPrimary:
    case NaplpsGSet::kSupplementary:
    case NaplpsGSet::kMosaic:
    case NaplpsGSet::kDrcs:
      add_span(NaplpsSpanKind::kGraphic, offset, 1);
      have_last_graphic_ = true;
      return;
  }
}

void NaplpsLinter::gather_operands(std::vector<uint8_t>& operands,
                                   std::vector<size_t>& offsets,
                                   std::vector<size_t>& controls) {
  Frame& frame = frames_.back();

  // One span per contiguous run, closed as the run ends rather than after the
  // whole gather, so a hole inside the operands splits the span the way it
  // splits the operand words — and the spans still come out in offset order.
  size_t run_start = 0;
  size_t run_length = 0;
  const auto close_run = [&]() {
    if (run_length > 0) {
      add_span(NaplpsSpanKind::kPdiOperand, run_start, run_length);
      run_length = 0;
    }
  };

  while (frame.position < frame.length) {
    const size_t position = frame.position;
    const uint8_t byte = frame.bytes[position];
    if (naplps_is_pdi_numeric(byte)) {
      if (run_length == 0) {
        run_start = position;
      }
      ++run_length;
      operands.push_back(byte);
      offsets.push_back(position);
      ++frame.position;
      continue;
    }
    // §5.3.1: the transparent controls "do not terminate PDI sequences". They
    // are also exactly what a lost packet leaves behind, so a suspect one here
    // is a hole that has silently shifted every operand bit after it.
    if (is_c0(byte) && naplps_is_transparent_control(byte)) {
      close_run();
      add_span(NaplpsSpanKind::kTransparentControl, position, 1);
      controls.push_back(position);
      if (suspects_ != nullptr && suspects_->suspect(position)) {
        add_finding(NaplpsLintRule::kHoleInOperandRun, position, 1, byte);
      }
      ++frame.position;
      continue;
    }
    // Anything else terminates the sequence and is left to be executed.
    break;
  }
  close_run();
}

void NaplpsLinter::execute_pdi(uint8_t opcode, size_t offset) {
  std::vector<uint8_t> operands;
  std::vector<size_t> offsets;
  std::vector<size_t> controls;
  gather_operands(operands, offsets, controls);

  const NaplpsPdi pdi = static_cast<NaplpsPdi>(opcode);

  // DOMAIN and RESET are the two opcodes that change how the bytes after them
  // are read, so their effect is applied whether or not this record is the one
  // being reported on.
  if (pdi == NaplpsPdi::kDomain && !operands.empty()) {
    const uint8_t byte1 =
        static_cast<uint8_t>(operands.front() & kNaplpsNumericMask);
    // §5.3.2.2.2 Table 4 and §5.3.2.2.3 Table 5. Both fields are exactly as
    // wide as their legal range — two bits for one to four bytes, three for one
    // to eight — so neither can name a length outside it, and there is nothing
    // here to range-check.
    format_.single_value_bytes = static_cast<size_t>(byte1 & 0x03) + 1;
    format_.multi_value_bytes = static_cast<size_t>((byte1 >> 2) & 0x07) + 1;
    format_.three_dimensional = ((byte1 >> 5) & 0x01) != 0;
  } else if (pdi == NaplpsPdi::kReset && operands.size() >= 2) {
    const uint8_t byte1 =
        static_cast<uint8_t>(operands[0] & kNaplpsNumericMask);
    const uint8_t byte2 =
        static_cast<uint8_t>(operands[1] & kNaplpsNumericMask);
    if ((byte1 & 0x01) != 0) {
      format_ = NaplpsOperandFormat{};
    }
    // §5.3.2.9.3 byte 2 b5: the macro storage is cleared, so an invocation
    // after this one really is undefined.
    if ((byte2 & 0x10) != 0) {
      macros_.clear();
    }
  }

  // Recorded before the checks, and with the format as it now stands: DOMAIN
  // §5.3.2.2.6 applies its own new multi-value length to its own logical pel
  // operand, so the format above is the one this run divides on.
  if (in_record() && result_ != nullptr) {
    const PdiGrammar grammar = grammar_of(pdi);
    NaplpsPdiObservation observation;
    observation.opcode_offset = offset;
    observation.opcode = opcode;
    observation.word_bytes = word_bytes(grammar, format_);
    observation.block_bytes =
        observation.word_bytes *
        std::max<size_t>(1, static_cast<size_t>(grammar.group_words));
    observation.operand_offsets = offsets;
    observation.embedded_controls = controls;
    observation.run_end = frames_.front().position;
    result_->pdis.push_back(std::move(observation));
  }

  check_operands(opcode, operands, offsets, offset);
}

void NaplpsLinter::check_operands(uint8_t opcode,
                                  const std::vector<uint8_t>& operands,
                                  const std::vector<size_t>& offsets,
                                  size_t opcode_offset) {
  if (!in_record()) {
    return;
  }

  const PdiGrammar grammar = grammar_of(static_cast<NaplpsPdi>(opcode));
  const size_t word = word_bytes(grammar, format_);

  // A byte the recovery doubts anywhere in the run makes the geometry it
  // describes untrustworthy, however well it parses. Reported once for the run
  // rather than once per byte: the run is what draws.
  if (suspects_ != nullptr && !offsets.empty()) {
    for (const size_t position : offsets) {
      if (suspects_->suspect(position)) {
        add_finding(NaplpsLintRule::kSuspectInOperandRun, position,
                    offsets.back() - position + 1, opcode);
        break;
      }
    }
  }

  if (word == 0) {
    // No word structure to divide by: a fixed-format, colour, irregular or
    // incremental operand run, all of which are legal at any length.
    return;
  }

  if (operands.size() < grammar.fixed_lead) {
    add_finding(NaplpsLintRule::kTruncatedPdi, opcode_offset, 1, opcode);
    return;
  }
  const size_t word_bytes_available = operands.size() - grammar.fixed_lead;
  const size_t words = word_bytes_available / word;

  if (words < grammar.minimum_words) {
    // §5.3.2.2.5 zero-extends what is missing, so a PDI short of a whole
    // operand draws something wrong rather than nothing at all.
    add_finding(NaplpsLintRule::kTruncatedPdi, opcode_offset, 1, opcode);
  } else if (word_bytes_available % word != 0) {
    add_finding(NaplpsLintRule::kOperandRunMisaligned,
                offsets[grammar.fixed_lead + words * word],
                word_bytes_available % word, opcode);
  }

  if (grammar.operand_class != OperandClass::kCoordinate) {
    return;
  }

  // Table D1 item 4 bounds the vertices a receiver must hold. The excess is
  // dropped rather than the drawing refused, so this is a warning.
  if (words > kNaplpsMaxVertices) {
    add_finding(NaplpsLintRule::kVertexOverflow, opcode_offset, 1, opcode);
  }

  if (grammar.absolute_mask == 0) {
    // Every word is a displacement or a size, and Figure 11 bounds those by
    // construction — there is nothing here a range check could catch.
    return;
  }

  NaplpsOperandReader reader(operands.data(), operands.size(), format_);
  for (uint8_t i = 0; i < grammar.fixed_lead; ++i) {
    (void)reader.read_fixed_byte();
  }
  for (size_t index = 0; index < words; ++index) {
    const NabtsPoint point = reader.read_coordinate();
    if (!word_is_absolute(grammar, index)) {
      continue;
    }
    // §5.3.1: a coordinate that would put the drawing outside the unit screen
    // "is considered to be in error".
    if (!naplps_in_unit_screen(point.x) || !naplps_in_unit_screen(point.y)) {
      add_finding(NaplpsLintRule::kCoordinateOutOfRange,
                  offsets[grammar.fixed_lead + index * word], word, opcode);
    }
  }
}

void NaplpsLinter::begin_definition(Collecting what, uint8_t code) {
  collecting_ = what;
  definition_body_.clear();
  definition_code_ = code;
  defining_frame_index_ = frames_.empty() ? 0 : frames_.size() - 1;
}

void NaplpsLinter::end_definition() {
  if (collecting_ == Collecting::kMacro ||
      collecting_ == Collecting::kMacroExecuting) {
    Macro& macro = macros_[definition_code_];
    macro.body = std::move(definition_body_);
    macro.transmit = definition_is_transmit_;
  }
  collecting_ = Collecting::kNothing;
  definition_body_.clear();
  definition_is_transmit_ = false;
}

void NaplpsLinter::invoke_macro(uint8_t code, size_t offset) {
  // §6.2.2.2: a macro is undefined during its own definition, so a reference to
  // itself is a null operation rather than an unresolved invocation.
  if (collecting_ == Collecting::kMacroExecuting && code == definition_code_) {
    return;
  }
  const auto it = macros_.find(code);
  if (it == macros_.end()) {
    add_finding(NaplpsLintRule::kUndefinedMacro, offset, 1, code);
    return;
  }
  // §6.2.2.3: a transmit macro is transmitted rather than executed, so it
  // expands into nothing here.
  if (it->second.transmit) {
    return;
  }
  // The standard bounds nesting only by memory, and a recovered record can
  // contain a macro that invokes itself. The overrun happens inside an
  // expansion, so it is reported against the invocation in the record that
  // began the chain.
  if (frames_.size() > kMaxMacroDepth) {
    add_finding_at(NaplpsLintRule::kMacroRecursionTooDeep,
                   expansion_root_offset_, 1, code);
    return;
  }
  if (in_record()) {
    expansion_root_offset_ = offset;
  }
  frames_.push_back(Frame{it->second.body.data(), it->second.body.size(), 0});
}

void NaplpsLinter::add_span(NaplpsSpanKind kind, size_t offset, size_t length) {
  if (!in_record() || result_ == nullptr || length == 0) {
    return;
  }
  result_->spans.push_back(NaplpsSpan{kind, offset, length});
}

void NaplpsLinter::add_finding(NaplpsLintRule rule, size_t offset,
                               size_t length, uint8_t context) {
  if (!in_record()) {
    return;
  }
  add_finding_at(rule, offset, length, context);
}

void NaplpsLinter::add_finding_at(NaplpsLintRule rule, size_t offset,
                                  size_t length, uint8_t context) {
  if (result_ == nullptr) {
    return;
  }
  NaplpsLintFindings& findings = result_->findings;
  ++findings.counts[rule];
  if (naplps_lint_severity(rule) == NaplpsLintSeverity::kError) {
    ++findings.errors;
  } else {
    ++findings.warnings;
  }
  if (findings.findings.size() >= kNaplpsMaxLintFindings) {
    ++findings.dropped;
    return;
  }
  findings.findings.push_back(
      NaplpsLintFinding{rule, offset, std::max<size_t>(1, length), context});
}

}  // namespace orc
