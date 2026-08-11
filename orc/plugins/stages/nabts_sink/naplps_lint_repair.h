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

#include "naplps_lint.h"
#include "vbi-services/nabts_page.h"

namespace orc {

/**
 * @file
 * @brief Lint-directed repair
 *
 * The linter says where a record breaks its own grammar; this decides what to
 * do about it. Two signals have to agree before anything is changed:
 *
 *   1. The recovery independently doubts the byte — it failed the odd parity
 *      CEA-516 §3.3 puts on every data byte, or it stands where a lost packet
 *      left a filler, or the detector was unsure of it (NaplpsSuspectMap).
 *   2. Changing it makes the record *more* grammatical, and no other change to
 *      the same byte does as well.
 *
 * A byte that parses cleanly is never touched however odd it looks, and a byte
 * the evidence doubts is left alone unless the grammar picks out one answer.
 * The failure mode is therefore always "declined to guess", which is what a
 * reader wants from something standing between them and the recording.
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

  /// Bytes the evidence held in doubt.
  uint32_t suspect_bytes = 0;
  /// Suspect bytes past the inspection cap, never considered for substitution.
  uint32_t suspect_bytes_uninspected = 0;

  /// Single-byte substitutions applied, the grammar having picked out one
  /// answer.
  uint32_t bytes_repaired = 0;
  /// Suspect bytes where more than one substitution would have improved the
  /// record, so none was made.
  uint32_t bytes_ambiguous = 0;

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

  /// Task one: try each single-bit correction of every suspect byte, and take
  /// one only where it alone improves the record.
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
   * @return whether anything was changed
   */
  bool cut_run(const NaplpsPdiObservation& pdi, size_t cut_index,
               size_t resume_index, std::vector<uint8_t>& data,
               NaplpsRepairSummary& summary) const;

  /// The service state, advanced by each repaired record as it is finished.
  NaplpsLinter linter_;
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
