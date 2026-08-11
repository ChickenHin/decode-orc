/*
 * File:        naplps_lint_repair.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Lint-directed NAPLPS repair implementation (X3.110 §5.3, §6.1)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_lint_repair.h"

#include <fmt/format.h>

#include <algorithm>
#include <utility>

#include "naplps_code_env.h"
#include "vbi-services/teletext_page_decoder.h"

namespace orc {

namespace {

/// Bits in a data byte, b1 to b7 of the code position and the parity bit b8
/// CEA-516 §3.3 puts above them. A single-bit error is a flip of any one of
/// them, so a correction is a flip of any one of them.
constexpr int kBitsPerByte = 8;

/**
 * @brief The byte a repair writes where a byte must stop meaning something
 *
 * X3.110 §6.1.6.4 makes 1/10 a null operation at the presentation layer, and it
 * is not one of the transparent controls §6.1.4 to §6.1.6.1 let stand inside a
 * PDI sequence — so unlike the NUL a lost packet leaves, writing it ends the
 * operand run where it stands and draws nothing at all.
 *
 * That combination is what lets a repair take bytes out of a run without
 * moving anything after them: the record keeps its length, and every offset a
 * reader has been shown still means what it meant.
 */
uint8_t null_operation_byte() { return teletext_odd_parity_encode(kNaplpsSdc); }

/// |value| as a data byte, with the odd parity of CEA-516 §3.3.
uint8_t as_data_byte(uint8_t value) {
  return teletext_odd_parity_encode(static_cast<uint8_t>(value & 0x7F));
}

}  // namespace

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

std::string NaplpsRepairSummary::summary() const {
  if (!ran) {
    return {};
  }
  if (total_repairs() == 0 && suspect_bytes == 0 && errors_before == 0) {
    return {};
  }

  std::string out = fmt::format(
      "NAPLPS repair: {} byte(s) corrected, {} run(s) resynchronised, {} "
      "coordinate word(s) dropped\n",
      bytes_repaired, pdis_resynchronised, coordinate_words_dropped);
  out += fmt::format("  Evidence:  {} byte(s) in doubt", suspect_bytes);
  if (bytes_ambiguous > 0) {
    out += fmt::format(", {} the grammar could not decide", bytes_ambiguous);
  }
  if (suspect_bytes_uninspected > 0) {
    out += fmt::format(", {} past the inspection limit",
                       suspect_bytes_uninspected);
  }
  out += "\n";
  out += fmt::format(
      "  Findings:  {} error(s) and {} warning(s) before, {} "
      "and {} after\n",
      errors_before, warnings_before, errors_after, warnings_after);
  if (operand_words_retained > 0) {
    out += fmt::format(
        "  Retained:  {} operand word(s) kept past the damage by repeating "
        "their opcode\n",
        operand_words_retained);
  }
  return out;
}

void naplps_stamp_repair_diagnostics(const NaplpsRepairSummary& summary,
                                     NabtsDecodeDiagnostics& diagnostics) {
  if (!summary.ran) {
    return;
  }
  diagnostics.repaired_bytes = summary.bytes_repaired;
  diagnostics.resynchronised_pdis = summary.pdis_resynchronised;
  diagnostics.dropped_coordinate_words = summary.coordinate_words_dropped;
  diagnostics.undecided_suspect_bytes = summary.bytes_ambiguous;
}

// ---------------------------------------------------------------------------
// The repairer
// ---------------------------------------------------------------------------

NaplpsLintRepairer::NaplpsLintRepairer() { reset_decoder(); }

void NaplpsLintRepairer::reset_decoder() { linter_.reset_decoder(); }

NaplpsLintResult NaplpsLintRepairer::trial_lint(
    const std::vector<uint8_t>& data, const NaplpsSuspectMap& suspects) const {
  // The service state is forked rather than advanced: a candidate byte may
  // define a macro or designate a set, and none of that may outlive the trial
  // that considered it. Copying between lints is safe — see NaplpsLinter.
  NaplpsLinter scratch = linter_;
  return scratch.lint(data, suspects);
}

NaplpsRepairResult NaplpsLintRepairer::repair(
    const std::vector<uint8_t>& record) {
  return repair(record, NaplpsSuspectMap::from_record(record));
}

NaplpsRepairResult NaplpsLintRepairer::repair(
    const std::vector<uint8_t>& record, const NaplpsSuspectMap& suspects) {
  NaplpsRepairResult result;
  result.data = record;
  result.summary.ran = true;

  const NaplpsLintResult before = trial_lint(result.data, suspects);
  result.summary.errors_before = before.findings.errors;
  result.summary.warnings_before = before.findings.warnings;

  substitute_bytes(suspects, result.data, result.summary);
  resynchronise_runs(suspects, result.data, result.summary);

  // The last pass is the committing one: the service state advances as the
  // *repaired* record ran, which is what the record after this one will be
  // linted and repaired against.
  const NaplpsLintResult after = linter_.lint(result.data, suspects);
  result.findings = after.findings;
  result.summary.errors_after = after.findings.errors;
  result.summary.warnings_after = after.findings.warnings;
  return result;
}

void NaplpsLintRepairer::substitute_bytes(const NaplpsSuspectMap& suspects,
                                          std::vector<uint8_t>& data,
                                          NaplpsRepairSummary& summary) const {
  std::vector<size_t> inspect;
  for (size_t offset = 0; offset < data.size(); ++offset) {
    if (!suspects.suspect(offset)) {
      continue;
    }
    ++summary.suspect_bytes;
    if (inspect.size() < kNaplpsMaxInspectedSuspectBytes) {
      inspect.push_back(offset);
    } else {
      ++summary.suspect_bytes_uninspected;
    }
  }
  if (inspect.empty()) {
    return;
  }

  for (const size_t offset : inspect) {
    // A byte that never arrived is not a byte with a bit wrong in it. Parity
    // says an odd number of bits are wrong, which is a statement about
    // something that was received; the filler standing in for a lost packet
    // carries no information at all, and "correcting" it would be writing a
    // plausible byte into a gap rather than recovering one. The structural pass
    // works around these instead.
    if (suspects.missing(offset)) {
      continue;
    }
    // Recomputed rather than carried, because an earlier repair in this same
    // pass may have changed what the record's remaining faults are.
    const NaplpsLintGrade baseline =
        naplps_lint_grade(trial_lint(data, suspects).findings);

    const uint8_t original = data[offset];
    uint8_t chosen = original;
    NaplpsLintGrade best;
    size_t attained_best = 0;

    for (int bit = 0; bit < kBitsPerByte; ++bit) {
      const uint8_t candidate = static_cast<uint8_t>(original ^ (1u << bit));
      data[offset] = candidate;
      const NaplpsLintGrade trial =
          naplps_lint_grade(trial_lint(data, suspects).findings);
      data[offset] = original;

      if (attained_best == 0 || trial < best) {
        best = trial;
        chosen = candidate;
        attained_best = 1;
      } else if (trial == best) {
        ++attained_best;
      }
    }

    // Strictly better than leaving it alone, not merely as good: a change that
    // buys nothing had no evidence for it beyond the parity failure, and the
    // parity failure alone does not say which bit was hit.
    if (!(best < baseline)) {
      continue;
    }
    if (attained_best == 1) {
      data[offset] = chosen;
      ++summary.bytes_repaired;
    } else {
      // Two corrections leave the record equally grammatical, and the grammar
      // is all there is to go on. Guessing between them would be inventing a
      // page rather than recovering one.
      ++summary.bytes_ambiguous;
    }
  }
}

void NaplpsLintRepairer::resynchronise_runs(
    const NaplpsSuspectMap& suspects, std::vector<uint8_t>& data,
    NaplpsRepairSummary& summary) const {
  const NaplpsLintResult state = trial_lint(data, suspects);

  // Which coordinate words the linter faulted for naming a point outside the
  // unit screen, so a run carrying one can be cut at it.
  std::vector<size_t> out_of_range;
  for (const NaplpsLintFinding& finding : state.findings.findings) {
    if (finding.rule == NaplpsLintRule::kCoordinateOutOfRange) {
      out_of_range.push_back(finding.offset);
    }
  }

  for (const NaplpsPdiObservation& pdi : state.pdis) {
    if (pdi.block_bytes == 0 || pdi.operand_offsets.empty()) {
      continue;  // No word structure to cut on.
    }
    const size_t total = pdi.operand_offsets.size();

    // A lost packet's filler standing among the operands means bytes went
    // missing from the middle of the run. The filler itself is skipped when the
    // operands are gathered (§5.3.1), so it shifts nothing by being there — but
    // the real bytes it stands in for are gone, and every word after the gap is
    // read from the wrong six-bit groups because of it.
    bool holed = false;
    size_t hole_index = total;
    for (const size_t control : pdi.embedded_controls) {
      if (!suspects.suspect(control)) {
        continue;  // A control the sender really did embed.
      }
      const auto it = std::upper_bound(pdi.operand_offsets.begin(),
                                       pdi.operand_offsets.end(), control);
      hole_index = static_cast<size_t>(it - pdi.operand_offsets.begin());
      holed = true;
      break;
    }

    // Only worth cutting where the surviving words really are misaligned. A gap
    // that happens to fall on an execution boundary took whole words with it
    // and left everything after it reading correctly.
    if (holed && hole_index % pdi.block_bytes != 0 &&
        cut_run(pdi, hole_index, hole_index, data, summary)) {
      ++summary.pdis_resynchronised;
      continue;
    }

    // A coordinate word naming a point outside the unit screen, at least one of
    // whose bytes the evidence doubts. §5.3.1 makes the coordinate an error and
    // leaves the handling open; the interpreter clamps it, which is right for a
    // sender's mistake and wrong for noise. Dropped rather than clamped, so the
    // page shows what arrived rather than a point invented for it.
    for (const size_t word_offset : out_of_range) {
      const auto it = std::lower_bound(pdi.operand_offsets.begin(),
                                       pdi.operand_offsets.end(), word_offset);
      if (it == pdi.operand_offsets.end() || *it != word_offset) {
        continue;  // Not this run's.
      }
      const size_t word_index =
          static_cast<size_t>(it - pdi.operand_offsets.begin());
      const size_t word_end = word_index + pdi.word_bytes;
      if (word_end > total) {
        continue;
      }
      if (!suspects.any_suspect(word_offset, pdi.operand_offsets[word_end - 1] -
                                                 word_offset + 1)) {
        continue;  // A clean word out of range is the sender's own mistake.
      }
      if (cut_run(pdi, word_index, word_end, data, summary)) {
        ++summary.coordinate_words_dropped;
      }
      break;
    }
  }
}

bool NaplpsLintRepairer::cut_run(const NaplpsPdiObservation& pdi,
                                 size_t cut_index, size_t resume_index,
                                 std::vector<uint8_t>& data,
                                 NaplpsRepairSummary& summary) const {
  const size_t total = pdi.operand_offsets.size();
  const size_t block = pdi.block_bytes;
  if (block == 0 || cut_index > total || resume_index > total ||
      resume_index < cut_index) {
    return false;
  }

  // Everything up to the last whole execution before the damage is kept: those
  // words were read from the right six-bit groups and describe what the sender
  // drew.
  const size_t keep = (cut_index / block) * block;
  if (keep >= total) {
    return false;  // Nothing before the damage to cut back to.
  }

  // The tail is kept only where it is a whole number of executions, so the
  // re-anchored opcode gets exactly the operands it expects. Anything else and
  // the tail would draw a shape assembled from the wrong bytes, which is the
  // very thing being repaired.
  //
  // Where the damage is a gap, the bytes just past it are usually the tail of
  // the word the gap broke, and are no more use than the gap itself — so the
  // resume point is the earliest one at or after |resume_index| that leaves
  // whole executions behind it, rather than |resume_index| exactly.
  size_t resume = total;
  for (size_t candidate = resume_index; candidate < total; ++candidate) {
    if ((total - candidate) % block == 0) {
      resume = candidate;
      break;
    }
  }
  const size_t tail = total - resume;
  const bool retain = tail > 0;

  const size_t null_from = pdi.operand_offsets[keep];
  const size_t null_to = retain ? pdi.operand_offsets[resume] : pdi.run_end;
  if (null_to <= null_from || null_to > data.size()) {
    return false;
  }

  for (size_t offset = null_from; offset < null_to; ++offset) {
    data[offset] = null_operation_byte();
    ++summary.bytes_nulled;
  }

  if (retain) {
    // §5.3.2.2.5: numeric data beyond one execution "is taken as an indication
    // to repeat the execution of the opcode with the subsequent numeric data
    // taken as new operands". Writing the opcode again into the last byte
    // nulled is therefore not an invention — it is the same instruction the
    // run already carried implicitly, said again where the damage broke it.
    data[null_to - 1] = as_data_byte(pdi.opcode);
    --summary.bytes_nulled;
    summary.operand_words_retained +=
        static_cast<uint32_t>(tail / pdi.word_bytes);
  }
  return true;
}

}  // namespace orc
