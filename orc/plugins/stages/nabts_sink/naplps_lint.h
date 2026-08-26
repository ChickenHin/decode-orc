/*
 * File:        naplps_lint.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Validate a NAPLPS presentation record against the grammar of
 *              ANSI X3.110-1983, without interpreting it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NAPLPS_LINT_H
#define ORC_NAPLPS_LINT_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "naplps_code_env.h"
#include "naplps_pdi.h"

namespace orc {

/**
 * @file
 * @brief A linter for the NAPLPS presentation language
 *
 * NAPLPS is a formally specified language (ANSI X3.110-1983, published as FIPS
 * PUB 121), so a record recovered from a damaged recording can be checked
 * against its own grammar: what a byte is allowed to be where it stands, how
 * many operand bytes an opcode wants, and the ranges the standard bounds. This
 * is deliberately *lint* rather than "error correction": the transport layers
 * below already do error correction in the coding-theory sense — Hamming 8/4
 * over the packet prefix and the record header, the product code over the data
 * block suffix, odd byte parity per CEA-516 §3.3 — and this is a different
 * kind of check entirely.
 *
 * Nothing here draws, and nothing here changes a byte. The linter reports what
 * it found and where; repairing it is a separate pass built on this one.
 *
 * The code environment and escape parsing are the interpreter's own
 * (NaplpsCodeEnvironment, naplps_parse_escape), so the shadow parse and the
 * interpreter cannot disagree about what a byte is.
 */

// ---------------------------------------------------------------------------
// Damage evidence
// ---------------------------------------------------------------------------

/// Why a byte is held in doubt.
enum NaplpsSuspectFlag : uint8_t {
  /// CEA-516 §3.3 gives every data byte of a type-zero group odd parity in b8,
  /// so a byte failing the check carries an odd number of wrong bits — usually
  /// exactly one. This also catches the 0x00 fillers a lost packet leaves,
  /// since zero has even parity.
  kNaplpsSuspectParity = 1u << 0,
  /// The bit detector was unsure of this byte (see NabtsDataGroup::confidence).
  kNaplpsSuspectConfidence = 1u << 1,
  /// The byte never arrived: it stands in for one a lost packet carried (see
  /// NabtsDataGroup::present). Held apart from the other two because it is a
  /// different kind of doubt — a byte that was received and is wrong has an odd
  /// number of bad bits and can be reasoned about, while a byte that was never
  /// received carries no information at all and nothing may be inferred from
  /// the value standing in its place.
  kNaplpsSuspectMissing = 1u << 2,
};

/**
 * @brief Detector confidence at or below which a byte is held in doubt
 *
 * Provisional, and deliberately low: parity is the primary signal and this only
 * adds bytes the detector itself nearly decided the other way. On the 0-255
 * scale of NabtsPacket::confidence this is the bottom quarter.
 */
constexpr uint8_t kNaplpsLowConfidence = 64;

/**
 * @brief Which bytes of a record are known or suspected damaged
 *
 * Built from the record as transmitted — byte parity still in place — and,
 * where the recovery kept it, the detector's per-byte confidence. An empty map
 * means nothing is known, and every byte reads as trustworthy; that is the
 * honest reading for a caller with no evidence to offer, not an assertion that
 * the record is clean.
 */
class NaplpsSuspectMap {
 public:
  NaplpsSuspectMap() = default;

  /**
   * @brief Build from |record| and whatever else the recovery kept
   *
   * @param record     Record data with byte parity in place (CEA-516 §3.3)
   * @param present    Per-byte arrival mask, index-aligned with @p record:
   *                   non-zero where the byte arrived, zero where it stands in
   *                   for one a lost packet carried (see
   *                   NabtsDataGroup::present). Empty where the recovery lost
   *                   nothing, or did not keep the mask — in which case a zero
   *                   byte is read as a filler anyway, that being what the
   *                   group assembler writes into a hole and a value no service
   *                   has reason to send inside a presentation record.
   * @param confidence Per-byte detector confidence, 0-255, index-aligned with
   *                   @p record. Empty where nothing measured it, which reads
   *                   as full confidence throughout — a detector with no way of
   *                   saying it is unsure has not said so.
   * @param floor      Confidence at or below which a byte is held in doubt
   */
  static NaplpsSuspectMap from_record(
      const std::vector<uint8_t>& record,
      const std::vector<uint8_t>& present = {},
      const std::vector<uint8_t>& confidence = {},
      uint8_t floor = kNaplpsLowConfidence);

  bool empty() const { return flags_.empty(); }
  size_t size() const { return flags_.size(); }

  /// Whether the byte at |offset| is held in doubt for any reason. An offset
  /// past the end is not: there is no byte there to doubt.
  bool suspect(size_t offset) const {
    return offset < flags_.size() && flags_[offset] != 0;
  }
  bool parity_failed(size_t offset) const {
    return offset < flags_.size() &&
           (flags_[offset] & kNaplpsSuspectParity) != 0;
  }
  bool low_confidence(size_t offset) const {
    return offset < flags_.size() &&
           (flags_[offset] & kNaplpsSuspectConfidence) != 0;
  }
  /// Whether the byte at |offset| never arrived. Nothing may be inferred from
  /// the value standing in its place, so a repair may work around it but must
  /// never try to correct it.
  bool missing(size_t offset) const {
    return offset < flags_.size() &&
           (flags_[offset] & kNaplpsSuspectMissing) != 0;
  }

  /// Bytes held in doubt, for the report.
  size_t suspect_count() const;

  /// Whether any byte in [offset, offset + length) is held in doubt.
  bool any_suspect(size_t offset, size_t length) const;

 private:
  std::vector<uint8_t> flags_;
};

// ---------------------------------------------------------------------------
// Spans
// ---------------------------------------------------------------------------

/**
 * @brief What part a byte plays in the record
 *
 * The kinds a repair pass has to tell apart: they are what decides which
 * replacement bytes are admissible at a position. A byte inside a PDI operand
 * run must come from columns 4 to 7 (§5.3.1); the final of an escape sequence
 * must be one Table 1 lists; a byte standing where an opcode is expected must
 * come from columns 2 to 3.
 */
enum class NaplpsSpanKind : uint8_t {
  /// A C0 control with an effect on the presentation: a format effector, a
  /// shift, CS, NSR, CAN, APS.
  kControl,
  /// X3.110 §6.1.4-6.1.6.1: a transmission or device control, or NUL. "No
  /// effect on the presentation layer", and legal anywhere — including inside
  /// a PDI sequence, which it does not terminate (§5.3.1).
  kTransparentControl,
  /// Bytes a control took after itself: the row and column of APS (§6.1.2.4)
  /// or of NSR (§6.1.6.5), the count byte of REPEAT (§6.2.7.2).
  kControlOperand,
  /// A whole escape sequence, ESC to final character (§4.3.2). For a malformed
  /// one this stops before the byte that broke it, which is then executed in
  /// its own right.
  kEscape,
  /// The byte naming what a DEF MACRO, DEF DRCS or DEF TEXTURE defines
  /// (§6.2.2.1, §6.2.3, §6.2.4).
  kDefinitionCode,
  /// One PDI opcode byte, columns 2 to 3 (§5.3.1).
  kPdiOpcode,
  /// A run of PDI numeric data bytes, columns 4 to 7 (§5.3.1). One span per
  /// contiguous run: a transparent control embedded in the operands splits the
  /// span without ending the PDI.
  kPdiOperand,
  /// A displayable character from one of the character G-sets.
  kGraphic,
  /// A byte from the macro G-set, which invokes the macro of that code (§5.5).
  kMacroInvocation,
  /// A byte from a slot designated to the null set, executed as a null
  /// operation (§4.3.2).
  kNullSet,
  /// A byte stored into a macro definition rather than executed (§6.2.2.1).
  kMacroBody,
};

/// Human-readable name of |kind|, for reports and test failures.
const char* naplps_span_kind_name(NaplpsSpanKind kind);

/// One classified run of record bytes.
struct NaplpsSpan {
  NaplpsSpanKind kind = NaplpsSpanKind::kGraphic;
  /// Offset of the first byte within the record as it was given to the linter.
  size_t offset = 0;
  size_t length = 0;
};

// ---------------------------------------------------------------------------
// Findings
// ---------------------------------------------------------------------------

/**
 * @brief What the linter can complain about
 *
 * Split by whether the standard is being broken or merely stretched — see
 * NaplpsLintSeverity, which is what a repair pass keys on. Every rule cites
 * the clause it enforces.
 */
enum class NaplpsLintRule : uint8_t {
  /// §4.3.2: an escape sequence whose byte after the intermediates is not a
  /// final character. The sequence is abandoned and that byte executed.
  kMalformedEscape,
  /// The record ended inside an escape sequence.
  kTruncatedEscape,
  /// §4.3.2: a designation naming a set outside Table 1, which designates the
  /// null set. Legal, and a service has no reason to do it.
  kNullDesignation,
  /// A PDI whose operand run is shorter than the opcode needs for even one
  /// execution (§5.3.2.2.5 zero-extends it, so it draws something wrong rather
  /// than nothing).
  kTruncatedPdi,
  /// A control whose own operand bytes ran out with the record — the row and
  /// column of an APS (§6.1.2.4), the count byte of a REPEAT (§6.2.7.2).
  kTruncatedControl,
  /// An operand run whose length is not a whole number of operand words of the
  /// size DOMAIN declared (§5.3.2.2). Legal — §5.3.2.2.5 makes a short run
  /// zero-extended and a long one a repeat — but a service emits whole words.
  kOperandRunMisaligned,
  /// §5.3.1: numeric data with no opcode in front of it, which has nothing to
  /// be an operand of and is discarded.
  kOrphanedOperand,
  /// §5.3.1: an absolute coordinate outside the unit screen, which "is
  /// considered to be in error".
  kCoordinateOutOfRange,
  /// A transparent control inside a PDI operand run, at a byte held in doubt —
  /// which is what the 0x00 filler of a lost packet looks like. §5.3.1 has it
  /// not terminate the sequence, so every operand bit after it shifts.
  kHoleInOperandRun,
  /// A byte held in doubt inside a PDI operand run. The run parses, but the
  /// geometry it describes cannot be trusted.
  kSuspectInOperandRun,
  /// §6.2.2.1: a DEF command whose code byte is not a graphic character, which
  /// makes "the entire command ... a null operation".
  kInvalidDefinitionCode,
  /// §6.2.4: a DEF TEXTURE naming something other than mask A to D (4/1-4/4).
  kInvalidTextureCode,
  /// A macro invoked that no DEF MACRO in this record or before it defined
  /// (§6.2.2).
  kUndefinedMacro,
  /// Macro expansion nested past the depth a receiver holds. §6.2.2.2 rules
  /// self-reference out for DEFP MACRO alone, so a recovered record can carry a
  /// macro that invokes itself; the expansion is abandoned at the bound.
  /// Reported at the invocation in the record that began the chain, the
  /// offending invocation itself being inside an expansion.
  kMacroRecursionTooDeep,
  /// The record ended with a definition still open (§6.2.5's END never came).
  kUnterminatedDefinition,
  /// §6.2.7.2: REPEAT with no count byte in 4/0-7/15 after it, or with no
  /// graphic character before it to repeat.
  kInvalidRepeat,
  /// §4.3.2: a single shift with no graphic character after it to shift.
  kDanglingSingleShift,
  /// Table D1 item 4: more vertices in one polygon or spline than a receiver is
  /// required to hold (kNaplpsMaxVertices). A receiver bound rather than a
  /// syntax rule, so the excess is dropped rather than the drawing refused.
  kVertexOverflow,
};

/// Whether a rule marks a broken stream or a stretched one.
enum class NaplpsLintSeverity : uint8_t {
  /// The standard is being broken, so something is wrong with the record —
  /// either the sender or, far more often here, the recovery. This is what a
  /// repair pass acts on.
  kError,
  /// Legal, but not what a service does. Evidence rather than proof.
  kWarning,
};

/// Severity of |rule|.
NaplpsLintSeverity naplps_lint_severity(NaplpsLintRule rule);
/// Short human-readable name of |rule|, for reports and test failures.
const char* naplps_lint_rule_name(NaplpsLintRule rule);

/// One thing the linter found, and where.
struct NaplpsLintFinding {
  NaplpsLintRule rule = NaplpsLintRule::kMalformedEscape;
  /// Offset of the first byte at fault, within the record as given.
  size_t offset = 0;
  /// Bytes at fault. One for a single bad byte; the whole run for a finding
  /// about a run.
  size_t length = 1;
  /// The byte that introduced what is at fault — the opcode of a truncated
  /// PDI, the code of an undefined macro — or zero where none applies. Kept so
  /// a report can say *which* PDI without the reader going back to the record.
  uint8_t context = 0;
};

/**
 * @brief Everything one lint pass found
 *
 * @ref findings is capped (see kNaplpsMaxLintFindings) because a record
 * assembled from a badly damaged carousel can fault on nearly every byte, and
 * a bounded list is what a report and a repair pass can both use. The counts
 * are not capped, so @ref total always says how bad it really was.
 */
struct NaplpsLintFindings {
  std::vector<NaplpsLintFinding> findings;
  /// Findings per rule, uncapped.
  std::map<NaplpsLintRule, uint32_t> counts;
  uint32_t errors = 0;
  uint32_t warnings = 0;
  /// Findings the cap dropped from @ref findings.
  uint32_t dropped = 0;

  uint32_t total() const { return errors + warnings; }
  bool clean() const { return total() == 0; }

  /// Count of |rule|, or zero.
  uint32_t count(NaplpsLintRule rule) const;

  /// Human-readable summary for the stage report. Empty for a clean record.
  std::string summary() const;
};

/**
 * @brief How grammatical one reading of a record is
 *
 * Fewer errors first, then fewer warnings. Errors are never traded for
 * warnings, because only an error is proof that something is wrong.
 *
 * Shared by everything that has to choose between two readings of the same
 * record — the repair pass weighing a substitution, the vote's tie-break
 * weighing a candidate byte — so that the two cannot come to different
 * conclusions about which reading is the better one.
 */
struct NaplpsLintGrade {
  uint32_t errors = 0;
  uint32_t warnings = 0;

  bool operator<(const NaplpsLintGrade& other) const {
    if (errors != other.errors) {
      return errors < other.errors;
    }
    return warnings < other.warnings;
  }
  bool operator==(const NaplpsLintGrade& other) const {
    return errors == other.errors && warnings == other.warnings;
  }
};

/// Grade of |findings|.
inline NaplpsLintGrade naplps_lint_grade(const NaplpsLintFindings& findings) {
  return NaplpsLintGrade{findings.errors, findings.warnings};
}

/// Findings retained per record. A record is at most 1904 bytes (CEA-516
/// §8.4.2.5) but a linked message concatenates up to 128 of them, so a cap
/// keeps a pathological message from carrying tens of thousands of findings.
constexpr size_t kNaplpsMaxLintFindings = 512;

/**
 * @brief One PDI as it was actually laid out in the record
 *
 * How a run of numeric data divides into operands depends on the DOMAIN format
 * in force where the opcode stood (§5.3.2.2), which nothing downstream can work
 * out for itself — the format is a property of the presentation state, not of
 * the record. Recorded here so a repair pass can cut a damaged run at a real
 * operand boundary instead of guessing one.
 *
 * The run occupies the record bytes from @ref operand_offsets.front() to
 * @ref run_end, and every byte in that range is either one of
 * @ref operand_offsets or one of @ref embedded_controls: §5.3.1 lets nothing
 * else stand inside a PDI sequence without ending it.
 */
struct NaplpsPdiObservation {
  size_t opcode_offset = 0;
  uint8_t opcode = 0;
  /// Bytes one operand word occupies, or zero where this opcode's operands have
  /// no word structure to divide by (a fixed-format, colour or incremental
  /// run, all of which are legal at any length).
  size_t word_bytes = 0;
  /// Bytes one complete execution occupies: one word, or the group of words the
  /// opcode takes per execution, which §5.3.2.2.5 repeats as a whole.
  size_t block_bytes = 0;
  /// Record offsets of the numeric data bytes gathered, ascending.
  std::vector<size_t> operand_offsets;
  /// Record offsets of transparent controls standing inside the run. §5.3.1
  /// has them not terminate it, so this is where a lost packet's filler sits —
  /// and so where bytes went missing from the middle of the operands.
  std::vector<size_t> embedded_controls;
  /// One past the last byte the run consumed.
  size_t run_end = 0;
};

/// What one lint pass made of one record.
struct NaplpsLintResult {
  /// Every byte of the record, classified, in order and without gaps or
  /// overlaps — see NaplpsLinter for what "every byte" means when a macro
  /// expands.
  std::vector<NaplpsSpan> spans;
  /// Every PDI the record executed, in order.
  std::vector<NaplpsPdiObservation> pdis;
  NaplpsLintFindings findings;

  /// The span covering |offset|, or nullptr where none does.
  const NaplpsSpan* span_at(size_t offset) const;
};

// ---------------------------------------------------------------------------
// The linter
// ---------------------------------------------------------------------------

/**
 * @brief Lints NAPLPS presentation records against X3.110
 *
 * One instance per *service*, not per record, exactly as NaplpsInterpreter is:
 * macros and G-set designations persist across records unless a reset clears
 * them, so a page invoking a macro its channel's Support Record defined
 * (CEA-516 §5.2.7.9) must be linted by an instance that has already seen that
 * Support Record. Linting it alone would report every such invocation as an
 * undefined macro.
 *
 * The walk mirrors NaplpsInterpreter::step() byte for byte, macro expansion
 * included, so what the linter says a byte is, is what the interpreter will do
 * with it. Two things are deliberately not mirrored, because neither can change
 * how a byte is read:
 *
 *   - Nothing is drawn, so there is no display list, no colour map and no DRCS
 *     buffer. The presentation attributes are tracked only where a later byte's
 *     meaning depends on them, which is the DOMAIN operand format (§5.3.2.2)
 *     and the code environment (§4.3).
 *   - The shared storage budget of CEA-516 §8.6.1 is not enforced, so a macro
 *     this accepts may be one the interpreter refused for want of room. The
 *     effect is that kUndefinedMacro under-reports rather than over-reports,
 *     which is the right way round for a finding a repair pass acts on; the
 *     interpreter counts its own refusals in NabtsDecodeDiagnostics.
 *
 * Spans and findings describe the record's own bytes. A macro *expansion* is
 * walked for its effect on the state but reports nothing: its bytes were
 * already classified where they were defined (as NaplpsSpanKind::kMacroBody),
 * and a macro invoked twenty times would otherwise be faulted twenty times at
 * offsets that are not in the record at all.
 *
 * Never throws. Thread safety: none; confine an instance to one thread.
 *
 * Copyable, and deliberately so: a repair pass forks the service state to try a
 * candidate byte without committing what that candidate would define. The copy
 * is only meaningful *between* lints — during one, the frame stack points into
 * the record buffer, and a copy of that would point into the original's.
 */
class NaplpsLinter {
 public:
  NaplpsLinter();

  /**
   * @brief The general reset of CEA-516 §8.5
   *
   * Everything to its defaults, macros included. Call before the first record
   * of an independent presentation; do not call between records that share
   * state.
   */
  void reset_decoder();

  /**
   * @brief Lint |record|, computing the damage evidence from parity alone
   *
   * @param record Record data as CEA-516 §5.3 delivers it — the record header
   *               already removed, byte parity still in place. Parity is
   *               stripped internally, exactly as NaplpsInterpreter::run()
   *               does, so a caller passing already-stripped bytes gets the
   *               same classification with no parity findings.
   */
  NaplpsLintResult lint(const std::vector<uint8_t>& record);

  /// Lint |record| against damage evidence the caller has assembled, which may
  /// know more than parity does — see NaplpsSuspectMap.
  NaplpsLintResult lint(const std::vector<uint8_t>& record,
                        const NaplpsSuspectMap& suspects);

  /// Macros defined so far, for a test that wants to inspect the state.
  size_t defined_macros() const { return macros_.size(); }

 private:
  /// Where bytes are being read from: the record, or an expanding macro.
  struct Frame {
    const uint8_t* bytes = nullptr;
    size_t length = 0;
    size_t position = 0;
  };

  /// What a definition in progress is collecting, mirroring the interpreter's
  /// own states (§6.2.2 to §6.2.4).
  enum class Collecting : uint8_t {
    kNothing,
    kMacro,
    kMacroExecuting,
    kDrcs,
    kTextureMask,
  };

  /// One stored macro body. Transmit macros (§6.2.2.3) are stored and never
  /// expanded, which is what the interpreter does with them.
  struct Macro {
    std::vector<uint8_t> body;
    bool transmit = false;
  };

  // ---- The walk ------------------------------------------------------------

  bool step();
  void execute_byte(uint8_t byte, size_t offset);
  void execute_c0(uint8_t byte, size_t offset);
  void execute_escape(size_t offset);
  void execute_c1(NaplpsC1 control, size_t escape_offset);
  void execute_graphic(uint8_t byte, size_t offset);
  void execute_pdi(uint8_t opcode, size_t offset);

  /// Collect the numeric data bytes after a PDI opcode, recording where each
  /// came from and where any transparent control stood among them. Mirrors
  /// NaplpsInterpreter::gather_operands().
  void gather_operands(std::vector<uint8_t>& operands,
                       std::vector<size_t>& offsets,
                       std::vector<size_t>& controls);

  /// Check one gathered operand run against the grammar of |opcode|.
  void check_operands(uint8_t opcode, const std::vector<uint8_t>& operands,
                      const std::vector<size_t>& offsets, size_t opcode_offset);

  void begin_definition(Collecting what, uint8_t code);
  void end_definition();
  void invoke_macro(uint8_t code, size_t offset);

  // ---- Reporting -----------------------------------------------------------

  /// Whether the byte being walked is the record's own, rather than one of an
  /// expanding macro. Spans and findings are only for the record.
  bool in_record() const { return frames_.size() == 1; }

  void add_span(NaplpsSpanKind kind, size_t offset, size_t length);
  void add_finding(NaplpsLintRule rule, size_t offset, size_t length = 1,
                   uint8_t context = 0);
  /// Record a finding whose offset is known to be the record's own even though
  /// the walk is not there: one raised after the frames have run out, or one
  /// raised inside an expansion but attributed to the invocation that began it.
  void add_finding_at(NaplpsLintRule rule, size_t offset, size_t length = 1,
                      uint8_t context = 0);

  NaplpsCodeEnvironment env_;
  /// The operand format DOMAIN established (§5.3.2.2), which decides how many
  /// bytes an operand word is and so how a numeric run divides up.
  NaplpsOperandFormat format_;
  /// The drawing point, tracked only to resolve relative coordinates far enough
  /// to range-check the absolute ones. Clamped exactly as the interpreter
  /// clamps (naplps_clamp_to_unit_screen), so the two stay in step.
  NabtsPoint drawing_point_;

  std::map<uint8_t, Macro> macros_;

  std::vector<Frame> frames_;
  /// The record's own bytes, parity stripped, which frames_[0] points into.
  std::vector<uint8_t> record_;

  Collecting collecting_ = Collecting::kNothing;
  std::vector<uint8_t> definition_body_;
  uint8_t definition_code_ = 0;
  bool definition_is_transmit_ = false;
  size_t defining_frame_index_ = 0;
  /// Offset the open definition started at, so an unterminated one can say
  /// where it began.
  size_t definition_offset_ = 0;

  /// Whether a graphic character has been displayed, which REPEAT needs
  /// (§6.2.7.2).
  bool have_last_graphic_ = false;
  /// Offset of the single shift awaiting its character, if one is.
  size_t single_shift_offset_ = 0;
  bool single_shift_open_ = false;

  /// Offset in the record of the macro invocation that began the expansion
  /// currently running, so a fault raised deep inside one can be reported
  /// against a byte the reader can actually look at.
  size_t expansion_root_offset_ = 0;

  const NaplpsSuspectMap* suspects_ = nullptr;
  NaplpsLintResult* result_ = nullptr;
};

}  // namespace orc

#endif  // ORC_NAPLPS_LINT_H
