/*
 * File:        naplps_lint_repair.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Repair a damaged NAPLPS record where its own grammar says what
 *              a byte had to have been (ANSI X3.110-1983)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NAPLPS_LINT_REPAIR_H
#define ORC_NAPLPS_LINT_REPAIR_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "naplps_interpreter.h"
#include "naplps_lint.h"
#include "vbi-services/nabts_page.h"

namespace orc {

/**
 * @file
 * @brief Lint-directed repair
 *
 * The linter says where a record breaks its own grammar; this decides what to
 * do about it. The rule the whole file follows is that **doubt may stop this
 * pass acting, but only proof may license it to act**. Five things have to hold
 * before a byte is changed:
 *
 *   1. The byte is *known* to be wrong: it failed the odd parity CEA-516 §3.3
 *      puts on every data byte. A byte the recovery merely felt unsure of —
 *      a close vote between copies, a detector reporting low confidence — is
 *      not evidence that anything is wrong with it.
 *   2. Changing it makes the record *more* grammatical, and no other change to
 *      the same byte does as well.
 *   3. The byte still puts marks on the page afterwards. One that draws — an
 *      opcode, an operand, a character, a macro invocation — may not become
 *      one that does not, however much more grammatical that would make the
 *      record.
 *   4. The byte does not decide how the bytes after it are read, and does not
 *      come to. Escape sequences, locking shifts, DOMAIN and RESET reach the
 *      whole record from where they stand.
 *   5. The page it draws afterwards is still recognisably the page it drew
 *      before. One bit of damage cannot account for repainting a page.
 *
 * Rules 3 to 5 are what keep rule 2 honest, because the grade is not a measure
 * of whether a page is right — only of whether it parses.
 *
 *   - Silence is always grammatical. Knocking b7 off an operand moves it out
 *     of columns 4 to 7, which ends its run (§5.3.1) and takes every fault
 *     after it away together with the drawing.
 *   - So is reading the rest of the record as something else. One bit turns an
 *     ESC into an ordinary control, and the designation it introduced becomes
 *     characters printed on the page; one bit moves a designation from one
 *     G-set slot to another, and every byte after it means something different.
 *   - And so is drawing the whole page in the wrong colour. An operand read
 *     as SET COLOUR draws more than the record ever asked for, all of it
 *     invisible.
 *
 * Each of those looks like a large improvement to a grader that counts faults,
 * and each of them is a page replaced rather than repaired. Rules 3 and 4 rule
 * out the two classes cheaply, from the part the byte plays; rule 5 is the
 * backstop, and asks the interpreter what actually changed on screen.
 *
 * The cost is real: a control byte damaged into a character cannot be restored,
 * and neither can a damaged escape sequence. Both are rarer than the damage the
 * rules prevent, and both fail safe — the page reads as it arrived.
 *
 * A byte that parses cleanly is never touched however odd it looks, and a byte
 * that is known to be wrong is left alone unless the grammar picks out one
 * answer. The failure mode is therefore always "declined to guess", which is
 * what a reader wants from something standing between them and the recording.
 *
 * Bytes are weighed one at a time, so two faults that mask each other — where
 * neither correction is an improvement until the other has been made too — are
 * both declined. This corrects independent damage, not conspiracies: searching
 * corrections in combination would cost the square of the record length and
 * would mostly find coincidences. The structural pass below still works around
 * what is left, and the counters say how much was left.
 *
 * Nothing here lengthens or shortens the record. Every repair is a byte
 * substitution in place, so a caller's parallel arrays — the per-byte present
 * and confidence masks the recovery carries — stay index for index with the
 * data, and an offset a reader is shown still means what it meant.
 */

// ---------------------------------------------------------------------------
// What a repair pass did
// ---------------------------------------------------------------------------

struct NaplpsRepairSummary {
  /// Whether a pass ran at all. Distinguishes "repaired nothing" from "was
  /// never asked", which read the same in the counters alone.
  bool ran = false;

  /// Bytes the evidence held in doubt, for any reason at all.
  uint32_t suspect_bytes = 0;
  /// Doubted bytes that also failed their parity, which is what a substitution
  /// needs before it may be considered at all. The rest are reported and left.
  uint32_t bytes_offered = 0;
  /// Offered bytes past the inspection cap, never considered for substitution.
  uint32_t suspect_bytes_uninspected = 0;

  /// Single-byte substitutions applied, the grammar having picked out one
  /// answer.
  uint32_t bytes_repaired = 0;
  /// Offered bytes where more than one substitution would have improved the
  /// record, so none was made.
  uint32_t bytes_ambiguous = 0;
  /// Changes the grammar did pick out but which would have redrawn more of the
  /// page than one bit of damage can account for, so were refused (§5.3.1's
  /// grammar has nothing to say about this — the interpreter does).
  uint32_t changes_declined_by_reach = 0;

  /// Operand runs cut back to a whole operand boundary because bytes had gone
  /// missing from the middle of them.
  uint32_t pdis_resynchronised = 0;
  /// Whole operand words after the damage kept by re-anchoring the opcode
  /// (§5.3.2.2.5), rather than discarded with it.
  uint32_t operand_words_retained = 0;
  /// Coordinate words dropped for naming a point outside the unit screen with
  /// a byte the evidence doubts (§5.3.1).
  uint32_t coordinate_words_dropped = 0;
  /// Bytes overwritten with the null operation of §6.1.6.4 to take them out of
  /// an operand run without moving anything after them.
  uint32_t bytes_nulled = 0;

  /// What the linter made of the record before and after.
  uint32_t errors_before = 0;
  uint32_t errors_after = 0;
  uint32_t warnings_before = 0;
  uint32_t warnings_after = 0;

  /// What the record *drew* before and after, from the same service state.
  /// The fault counts say whether the record parses better; only these say
  /// whether the page a reader is shown was altered, which is the question a
  /// reader is actually asking. Both zero where the pass changed nothing, the
  /// record never having been run.
  uint32_t primitives_before = 0;
  uint32_t primitives_after = 0;
  /// Whether the page draws differently — any primitive added, dropped or
  /// altered, not merely a different number of them.
  bool drawing_changed = false;

  /// Changes made to the record, of every kind.
  uint32_t total_repairs() const {
    return bytes_repaired + pdis_resynchronised + coordinate_words_dropped;
  }

  /// Human-readable summary for the stage report. Empty for a pass that
  /// changed nothing and had nothing to complain about.
  std::string summary() const;
};

/// A repaired record and the account of what was done to it.
struct NaplpsRepairResult {
  /// The record with the repairs applied, always the same length as the input.
  std::vector<uint8_t> data;
  NaplpsRepairSummary summary;
  /// The linter's verdict on @ref data, which is what a reader should be shown
  /// — the findings that survived the repair.
  NaplpsLintFindings findings;
};

/**
 * @brief Suspect bytes one record offers to substitution
 *
 * Each one costs a lint pass per candidate, and a record damaged past this many
 * bytes is past what byte-level repair is the right tool for — the structural
 * pass still runs over the whole of it. The count is reported either way, so a
 * capped run says so rather than looking clean.
 */
constexpr size_t kNaplpsMaxInspectedSuspectBytes = 128;

/**
 * @brief The share of a page one change may redraw
 *
 * One flipped bit is one damaged instruction. It can lose a line, move a
 * vertex, or restore a run that was being discarded — but a change that leaves
 * this much of what the page was drawing altered or gone has not corrected an
 * instruction, it has made the record say something else. That is what a
 * repainted page looks like from the inside, and the fault counts cannot see
 * it: the replacement page parses beautifully.
 *
 * Read as a denominator: a quarter. Additions are not counted against a change
 * — a repair that puts back an opcode the damage had orphaned restores a whole
 * run of drawing at once, and that is exactly what it is for.
 */
constexpr size_t kNaplpsMaxRedrawnDenominator = 4;

/**
 * @brief Drawings one change may alter however small the page
 *
 * A share alone would forbid every repair to a page with only a few drawings
 * on it: correcting an instruction alters the drawing it takes part in, so a
 * page of one line has a quarter of nothing to spend. A coordinate word can be
 * the end of one drawing and the start of the next, so two is what a single
 * correction is allowed outright — and on a page with any real content the
 * share is what binds instead.
 */
constexpr size_t kNaplpsMinRedrawnPrimitives = 2;

/**
 * @brief Repairs NAPLPS records against the grammar of X3.110
 *
 * One instance per *service*, for the reason NaplpsLinter is: a page invoking a
 * macro its channel's Support Record defined has to be repaired by something
 * that has already seen that Support Record, or every such invocation looks
 * like damage. Feed it the records in the order a receiver would present them,
 * and its state advances as the repaired records run.
 *
 * Never throws. Thread safety: none; confine an instance to one thread.
 */
class NaplpsLintRepairer {
 public:
  NaplpsLintRepairer();

  /// The general reset of CEA-516 §8.5, as NaplpsLinter::reset_decoder().
  void reset_decoder();

  /// Repair |record|, taking the damage evidence from parity alone.
  NaplpsRepairResult repair(const std::vector<uint8_t>& record);

  /// Repair |record| against evidence the caller has assembled, which may know
  /// more than parity does — see NaplpsSuspectMap.
  NaplpsRepairResult repair(const std::vector<uint8_t>& record,
                            const NaplpsSuspectMap& suspects);

 private:
  /// Lint |data| from the committed service state without advancing it, so a
  /// candidate's macros and designations do not outlive the trial.
  NaplpsLintResult trial_lint(const std::vector<uint8_t>& data,
                              const NaplpsSuspectMap& suspects) const;

  /**
   * @brief What |data| draws, from the committed service state
   *
   * The same discipline as trial_lint(), and for a stronger reason: running a
   * candidate defines its macros and writes its colour map, and none of that
   * may reach the next record on the strength of a trial. Asked once per byte
   * inspected and once per cut weighed, never per candidate — the grammar
   * narrows the field first, and this settles what is left of it.
   */
  std::vector<NabtsPrimitive> trial_render(
      const std::vector<uint8_t>& data) const;

  /// Task one: try each single-bit correction of every byte known to be wrong,
  /// and take one only where it alone improves the record.
  void substitute_bytes(const NaplpsSuspectMap& suspects,
                        std::vector<uint8_t>& data,
                        NaplpsRepairSummary& summary) const;

  /// Task two and three: cut operand runs back to a whole operand boundary
  /// where bytes went missing from the middle, and drop coordinate words that
  /// name a point outside the unit screen with a byte the evidence doubts.
  void resynchronise_runs(const NaplpsSuspectMap& suspects,
                          std::vector<uint8_t>& data,
                          NaplpsRepairSummary& summary) const;

  /**
   * @brief Cut one operand run at a whole operand boundary
   *
   * @param pdi         The run, as the linter observed it
   * @param cut_index   Index into @c pdi.operand_offsets of the first operand
   *                    byte that cannot be trusted
   * @param resume_index Index of the first operand byte after the damage,
   *                    which is kept when what follows it is a whole number of
   *                    executions
   *
   * The operand bytes up to the last whole execution before @p cut_index stay
   * as they are. Everything from there to @p resume_index is overwritten with
   * the null operation of §6.1.6.4, which — unlike the NUL a lost packet leaves
   * — is not one of the transparent controls §5.3.1 lets stand inside a PDI, so
   * it ends the sequence where it stands and draws nothing.
   *
   * Where the bytes after the damage are a whole number of executions they are
   * kept, by writing the opcode again into the last byte nulled. §5.3.2.2.5
   * makes a run longer than one execution a repeat of the opcode, so a record
   * that read "opcode, six words" and lost a word from the middle becomes
   * "opcode, two words, null, opcode, three words" — which draws exactly what
   * the surviving words describe.
   *
   * The cut is weighed before it is made, as a substitution is: it is applied
   * only where it leaves the record more grammatical than it found it, and only
   * where the page it leaves is still the page that was there. The grade alone
   * is a weak test here — nulling bytes cannot make a record less grammatical,
   * so it only catches the cut that buys nothing at all. The page comparison is
   * what has teeth: a cut is meant to drop the operands one damaged run could
   * not read, and one run is a small part of a page.
   *
   * @return whether anything was changed
   */
  bool cut_run(const NaplpsPdiObservation& pdi, size_t cut_index,
               size_t resume_index, const NaplpsSuspectMap& suspects,
               std::vector<uint8_t>& data, NaplpsRepairSummary& summary) const;

  /// The service state, advanced by each repaired record as it is finished.
  /// Two of them, because the two questions asked of a candidate are asked of
  /// different machines: the linter says whether the record parses, and the
  /// interpreter says what it draws. Both advance together on the record that
  /// is finally committed, so both stand where the next record will find them.
  NaplpsLinter linter_;
  NaplpsInterpreter interpreter_;
};

/**
 * @brief Record a repair pass in a decoded page's diagnostics
 *
 * The interpreter builds its own diagnostics and knows nothing of the repair,
 * so the counters are stamped onto the snapshot afterwards by whoever ran both.
 * A page whose record was never repaired keeps the zeroes it was built with,
 * which is the honest reading: nothing was guessed at.
 */
void naplps_stamp_repair_diagnostics(const NaplpsRepairSummary& summary,
                                     NabtsDecodeDiagnostics& diagnostics);

}  // namespace orc

#endif  // ORC_NAPLPS_LINT_REPAIR_H
