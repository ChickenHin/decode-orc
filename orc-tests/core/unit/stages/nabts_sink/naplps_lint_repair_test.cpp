/*
 * File:        naplps_lint_repair_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for lint-directed NAPLPS repair (X3.110 §5.3, §6.1)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_lint_repair.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "naplps_interpreter.h"
#include "vbi-services/teletext_page_decoder.h"

namespace orc {
namespace {

/// A code position in the standard's column/row notation.
constexpr uint8_t code(int column, int row) {
  return static_cast<uint8_t>((column << 4) | row);
}

/// A numeric data byte carrying |payload| in b6-b1 (§5.3.1, Figure 10).
constexpr uint8_t numeric(uint8_t payload) {
  return static_cast<uint8_t>(0x40 | (payload & 0x3F));
}

constexpr uint8_t kSo = 0x0E;
constexpr uint8_t kEsc = 0x1B;
/// §6.1.6.4: the null operation a repair writes to take a byte out of a run.
constexpr uint8_t kSdc = 0x1A;

/// CEA-516 §3.3's odd parity in b8, which is how a record arrives.
std::vector<uint8_t> with_parity(const std::vector<uint8_t>& bytes) {
  std::vector<uint8_t> out;
  out.reserve(bytes.size());
  for (const uint8_t byte : bytes) {
    out.push_back(
        teletext_odd_parity_encode(static_cast<uint8_t>(byte & 0x7F)));
  }
  return out;
}

/// The seven-bit payloads of |bytes|, which is what the interpreter reads.
std::vector<uint8_t> payloads(const std::vector<uint8_t>& bytes) {
  std::vector<uint8_t> out;
  out.reserve(bytes.size());
  for (const uint8_t byte : bytes) {
    out.push_back(static_cast<uint8_t>(byte & 0x7F));
  }
  return out;
}

NaplpsRepairResult repair(const std::vector<uint8_t>& record) {
  NaplpsLintRepairer repairer;
  return repairer.repair(record);
}

/**
 * @brief |record| with one bit of the byte at |offset| flipped
 *
 * Which is exactly what a single-bit error looks like on the wire: the payload
 * changes and the odd parity CEA-516 §3.3 put on the byte no longer matches it,
 * so the recovery knows the byte is wrong without knowing which bit.
 */
std::vector<uint8_t> flip_bit(std::vector<uint8_t> record, size_t offset,
                              uint8_t mask) {
  record[offset] = static_cast<uint8_t>(record[offset] ^ mask);
  return record;
}

/// Primitives of |record| as the interpreter draws them.
std::vector<NabtsPrimitive> draw(const std::vector<uint8_t>& record) {
  NaplpsInterpreter interpreter;
  interpreter.reset_decoder();
  return interpreter.run(record).primitives;
}

/**
 * @brief One three-byte coordinate word carrying |x| and |y| (Figure 11)
 *
 * Each byte holds one three-bit field of each component, X in b6-b4 and Y in
 * b3-b1, the first byte carrying the most significant. Nine bits per component
 * at the default multi-value length of §5.3.2.2.3, read as a two's-complement
 * binary fraction — so a value below 0x100 leaves the sign bit clear and names
 * a point inside the unit screen, which is what an absolute coordinate must do.
 */
std::vector<uint8_t> coordinate(uint16_t x, uint16_t y) {
  std::vector<uint8_t> out;
  for (int field = 2; field >= 0; --field) {
    const uint8_t shift = static_cast<uint8_t>(field * 3);
    const uint8_t packed = static_cast<uint8_t>((((x >> shift) & 0x7u) << 3) |
                                                ((y >> shift) & 0x7u));
    out.push_back(numeric(packed));
  }
  return out;
}

/// |head| followed by every word of |words|, with parity applied.
std::vector<uint8_t> pdi_record(
    const std::vector<uint8_t>& head,
    const std::vector<std::vector<uint8_t>>& words) {
  std::vector<uint8_t> plain = head;
  for (const std::vector<uint8_t>& word : words) {
    plain.insert(plain.end(), word.begin(), word.end());
  }
  return with_parity(plain);
}

/// A record that draws two lines: SO invokes the PDI set, then LINE ABS (2/8)
/// with two absolute coordinate words. §5.3.2.2.5 repeats the opcode for the
/// second word, so one word is one line drawn.
std::vector<uint8_t> two_line_record() {
  return pdi_record({kSo, code(2, 8)},
                    {coordinate(0x040, 0x050), coordinate(0x0C0, 0x0D0)});
}

////////////////////////////////////////////////////////////////////////////////
// Task 2.1 — parity-guided single-bit repair
////////////////////////////////////////////////////////////////////////////////

TEST(NaplpsLintRepair, LeavesACleanRecordExactlyAsItArrived) {
  const std::vector<uint8_t> record = two_line_record();
  const NaplpsRepairResult result = repair(record);

  EXPECT_EQ(result.data, record);
  EXPECT_TRUE(result.summary.ran);
  EXPECT_EQ(result.summary.suspect_bytes, 0u);
  EXPECT_EQ(result.summary.total_repairs(), 0u);
  EXPECT_EQ(result.summary.errors_before, 0u);
  EXPECT_EQ(result.summary.errors_after, 0u);
}

// §5.3.1 puts numeric data in columns 4 to 7, so b7 is what makes a byte an
// operand. Knock b7 out of an operand and the PDI ends early; the grammar then
// admits exactly one correction, because no other single bit puts the byte back
// in the numeric columns.
TEST(NaplpsLintRepair, RestoresAnOperandByteKnockedOutOfItsColumn) {
  const std::vector<uint8_t> record = two_line_record();
  // b7 is what puts a byte in the numeric columns, so clearing it drops the
  // operand out of the run (§5.3.1).
  const std::vector<uint8_t> damaged = flip_bit(record, 3, 0x40);

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.bytes_repaired, 1u);
  EXPECT_EQ(result.data, record);
  EXPECT_LT(result.summary.errors_after, result.summary.errors_before);
}

// The correction is the flip of any one of the eight bits, the parity bit
// included: a byte whose payload was right and whose parity bit alone was hit
// needs no change to what it means.
TEST(NaplpsLintRepair, LeavesAByteWhoseOnlyDamagedBitIsTheParityBit) {
  const std::vector<uint8_t> record = two_line_record();
  const std::vector<uint8_t> damaged = flip_bit(record, 3, 0x80);

  const NaplpsRepairResult result = repair(damaged);

  // The payload still parses, so nothing is improved by changing it and
  // nothing is changed. The record reads as it always did.
  EXPECT_EQ(payloads(result.data), payloads(record));
  EXPECT_EQ(result.summary.bytes_repaired, 0u);
  EXPECT_EQ(result.summary.suspect_bytes, 1u);
}

// A value bit inside an operand leaves the byte in its own column, so the
// grammar has nothing to say about which bit was hit. Six corrections would
// parse equally well, and guessing between them would invent a page.
TEST(NaplpsLintRepair, DeclinesAByteWhoseDamageTheGrammarCannotDecide) {
  const std::vector<uint8_t> record = two_line_record();
  const std::vector<uint8_t> damaged =
      flip_bit(record, 3, 0x01);  // a value bit

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.bytes_repaired, 0u);
  EXPECT_EQ(payloads(result.data), payloads(damaged));
}

// The one thing a repair pass must never do is change a byte the recovery had
// no reason to doubt.
TEST(NaplpsLintRepair, NeverChangesAByteTheEvidenceDoesNotDoubt) {
  // A record the linter faults — an operand run that is not a whole number of
  // words — but every byte of which arrived with its parity intact.
  std::vector<uint8_t> plain = {kSo, code(2, 8)};
  const std::vector<uint8_t> word = coordinate(0x040, 0x050);
  plain.insert(plain.end(), word.begin(), word.end());
  plain.push_back(numeric(0x01));  // one byte more than a whole word
  const std::vector<uint8_t> record = with_parity(plain);

  const NaplpsRepairResult result = repair(record);

  EXPECT_GT(result.summary.warnings_before, 0u);
  EXPECT_EQ(result.data, record);
  EXPECT_EQ(result.summary.total_repairs(), 0u);
  EXPECT_EQ(result.summary.suspect_bytes, 0u);
}

// §5.3.1: numeric data with no opcode in front of it is discarded, so a
// corrupted opcode costs the whole PDI. Putting it back is the repair that buys
// the most.
TEST(NaplpsLintRepair, RestoresAPdiOpcodeCorruptedIntoNumericData) {
  const std::vector<uint8_t> record = two_line_record();
  // Setting b7 on the opcode makes it read as an operand (§5.3.1).
  const std::vector<uint8_t> damaged = flip_bit(record, 1, 0x40);

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.bytes_repaired, 1u);
  EXPECT_EQ(payloads(result.data), payloads(record));
  EXPECT_EQ(draw(result.data).size(), draw(record).size());
}

////////////////////////////////////////////////////////////////////////////////
// Task 2.2 — hole and truncation resynchronisation
////////////////////////////////////////////////////////////////////////////////

// A lost packet leaves 0x00 fillers where its bytes should have been. §6.1.4
// makes NUL a transparent control, which §5.3.1 lets stand inside a PDI without
// ending it — so the operands close up over the gap and every word after it is
// read from the wrong six-bit groups.
TEST(NaplpsLintRepair,
     ResynchronisesAnOperandRunWithBytesMissingFromTheMiddle) {
  const std::vector<uint8_t> record = two_line_record();
  std::vector<uint8_t> damaged = record;
  damaged[3] = 0x00;  // one byte of the first coordinate word lost

  const std::vector<NabtsPrimitive> before = draw(damaged);
  const NaplpsRepairResult result = repair(damaged);
  const std::vector<NabtsPrimitive> after = draw(result.data);

  EXPECT_EQ(result.summary.pdis_resynchronised, 1u);
  EXPECT_EQ(result.data.size(), record.size());

  // Before the repair the run read as two words assembled from the surviving
  // five bytes plus zero-extension: geometry the sender never described.
  ASSERT_FALSE(before.empty());
  // After it, the trailing whole word is kept by repeating the opcode
  // (§5.3.2.2.5) and the broken leading word is gone.
  EXPECT_EQ(result.summary.operand_words_retained, 1u);
  ASSERT_EQ(after.size(), 1u);
  // The surviving line is the one the intact second word describes.
  const std::vector<uint8_t> tail_only =
      pdi_record({kSo, code(2, 8)}, {coordinate(0x0C0, 0x0D0)});
  const std::vector<NabtsPrimitive> expected = draw(tail_only);
  ASSERT_EQ(expected.size(), 1u);
  EXPECT_DOUBLE_EQ(after.front().points.back().x,
                   expected.front().points.back().x);
  EXPECT_DOUBLE_EQ(after.front().points.back().y,
                   expected.front().points.back().y);
}

// The bytes a repair takes out of a run are overwritten with the null operation
// of §6.1.6.4 rather than removed, so nothing after them moves and every offset
// a reader has been shown still means what it meant.
TEST(NaplpsLintRepair, TakesBytesOutOfARunWithoutMovingAnythingAfterThem) {
  const std::vector<uint8_t> record = two_line_record();
  std::vector<uint8_t> damaged = record;
  damaged[3] = 0x00;

  const NaplpsRepairResult result = repair(damaged);

  ASSERT_EQ(result.data.size(), record.size());
  // The bytes before the damage are untouched.
  EXPECT_EQ(result.data[0], record[0]);
  EXPECT_EQ(result.data[1], record[1]);
  // The broken leading word and the filler became null operations, bar the one
  // byte carrying the repeated opcode.
  EXPECT_EQ(result.data[2] & 0x7F, kSdc);
  EXPECT_EQ(result.data[3] & 0x7F, kSdc);
  EXPECT_EQ(result.data[4] & 0x7F, code(2, 8));  // the opcode, said again
  // The intact trailing word is exactly as it arrived.
  EXPECT_EQ(result.data[5], record[5]);
  EXPECT_EQ(result.data[6], record[6]);
  EXPECT_EQ(result.data[7], record[7]);
}

// A gap that happens to take whole operand words with it leaves everything
// after it reading correctly, so there is nothing to resynchronise.
TEST(NaplpsLintRepair, LeavesARunWhoseGapFellOnAnOperandBoundary) {
  // A hole three bytes wide — one whole coordinate word — after the first.
  const std::vector<uint8_t> record = two_line_record();
  std::vector<uint8_t> damaged = record;
  damaged[5] = 0x00;
  damaged[6] = 0x00;
  damaged[7] = 0x00;

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.pdis_resynchronised, 0u);
  // The surviving word still draws what it always described.
  ASSERT_EQ(draw(result.data).size(), 1u);
}

// Where the bytes after the damage are not a whole number of executions there
// is nothing to re-anchor them to, and keeping them would draw a shape
// assembled from the wrong bytes.
TEST(NaplpsLintRepair, DiscardsATailThatDoesNotRealign) {
  // SET LINE ABS (2/10) takes two coordinate words per execution (§5.3.3.2.4),
  // so a six-byte block. Four words, with the gap in the second block: one and
  // a half words survive after it, which no repeat of the opcode can consume.
  const std::vector<uint8_t> record = pdi_record(
      {kSo, code(2, 10)}, {coordinate(0x010, 0x020), coordinate(0x030, 0x040),
                           coordinate(0x050, 0x060), coordinate(0x070, 0x080)});
  std::vector<uint8_t> damaged = record;
  damaged[10] = 0x00;

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.pdis_resynchronised, 1u);
  EXPECT_EQ(result.summary.operand_words_retained, 0u);
  // The first whole execution — two words, six bytes — is kept.
  EXPECT_EQ(result.data[2], record[2]);
  EXPECT_EQ(result.data[7], record[7]);
  // Everything from the damage on is a null operation.
  for (size_t offset = 8; offset < result.data.size(); ++offset) {
    EXPECT_EQ(result.data[offset] & 0x7F, kSdc) << "at " << offset;
  }
}

////////////////////////////////////////////////////////////////////////////////
// Task 2.3 — range and sequence repairs
////////////////////////////////////////////////////////////////////////////////

// §5.3.1 makes a coordinate a two's-complement fraction in [-1, 1), so the sign
// bit of an absolute coordinate puts it outside the unit screen. Where one
// single-bit correction brings it back, that is the correction.
TEST(NaplpsLintRepair, RestoresACoordinateWhoseSignBitWasHit) {
  const std::vector<uint8_t> record = two_line_record();
  // b6 of the first numeric byte is the most significant bit of X, which
  // Figure 11 weights -1.
  const std::vector<uint8_t> damaged = flip_bit(record, 2, 0x20);

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.bytes_repaired, 1u);
  EXPECT_EQ(payloads(result.data), payloads(record));
}

// Where no single correction brings the coordinate back, the word is dropped
// rather than clamped: §5.3.1 leaves the handling open, and clamping is the
// right answer for a sender's mistake but not for noise.
TEST(NaplpsLintRepair, DropsACoordinateWordThatCannotBeBroughtBackInRange) {
  // Two coordinate words, the first with the sign bit of *both* components set,
  // which no single correction can clear. Its parity is stale, so the recovery
  // doubts it.
  std::vector<uint8_t> damaged = pdi_record(
      {kSo, code(2, 8)}, {coordinate(0x140, 0x150), coordinate(0x0C0, 0x0D0)});
  damaged[2] = static_cast<uint8_t>(damaged[2] ^ 0x80);
  damaged[3] = static_cast<uint8_t>(damaged[3] ^ 0x80);

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.coordinate_words_dropped, 1u);
  EXPECT_EQ(result.summary.operand_words_retained, 1u);
  // The intact second word is kept, re-anchored by repeating the opcode.
  EXPECT_EQ(result.data[4] & 0x7F, code(2, 8));
  ASSERT_EQ(draw(result.data).size(), 1u);
}

// A coordinate the sender really did put outside the unit screen is its own
// mistake, and the interpreter's existing clamp is the right answer for it.
TEST(NaplpsLintRepair, LeavesAnOutOfRangeCoordinateThatArrivedIntact) {
  const std::vector<uint8_t> record =
      pdi_record({kSo, code(2, 8)}, {coordinate(0x140, 0x050)});

  const NaplpsRepairResult result = repair(record);

  EXPECT_EQ(result.summary.coordinate_words_dropped, 0u);
  EXPECT_EQ(result.data, record);
  // The interpreter still clamps it, exactly as before.
  EXPECT_EQ(draw(result.data).size(), 1u);
}

// §6.2.4: the texture mask code must name mask A to D, 4/1 to 4/4. The linter
// faults anything else, so a single-bit correction that lands inside is the one
// the grammar picks.
TEST(NaplpsLintRepair, RestoresATextureMaskCodeKnockedOutOfRange) {
  // ESC 4/4 (DEF TEXTURE), code 4/2 (mask B), END.
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(4, 4), code(4, 2), kEsc, code(4, 5)});
  const std::vector<uint8_t> damaged = flip_bit(record, 2, 0x10);  // 4/2 -> 5/2

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.bytes_repaired, 1u);
  EXPECT_EQ(payloads(result.data), payloads(record));
}

// §6.2.2.1 requires only that a DEF MACRO's code byte be a graphic character,
// and there are ninety-six of those. A code knocked out of that range therefore
// has several corrections that put it back, all of them naming a perfectly
// legal macro — so the grammar cannot say which macro was meant, and the
// repair declines. The narrow ranges are the repairable ones; this is what the
// wide ones look like.
TEST(NaplpsLintRepair, DeclinesADefinitionCodeWithSeveralValidCorrections) {
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(4, 0), code(2, 1), 'A', kEsc, code(4, 5)});
  const std::vector<uint8_t> damaged = flip_bit(record, 2, 0x20);  // into C0

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.bytes_repaired, 0u);
  EXPECT_EQ(result.summary.bytes_ambiguous, 1u);
  EXPECT_EQ(result.data, damaged);
}

// The interpreter already closes a definition a truncated record left open, so
// there is nothing for a repair to do about one — see NaplpsInterpreter::run().
// The linter reports it and the repair leaves it alone.
TEST(NaplpsLintRepair, LeavesADefinitionTheInterpreterAlreadyCloses) {
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(4, 0), code(2, 1), 'A', 'B'});

  const NaplpsRepairResult result = repair(record);

  EXPECT_EQ(result.data, record);
  EXPECT_EQ(result.findings.count(NaplpsLintRule::kUnterminatedDefinition), 1u);
}

TEST(NaplpsLintRepair, RepairsSeveralKindsOfDamageInOneRecord) {
  // A LINE ABS with one word, a line feed to end the sequence (§5.3.1), then a
  // POINT ABS (2/6) with one word.
  constexpr uint8_t kApd = 0x0A;
  std::vector<uint8_t> plain = {kSo, code(2, 8)};
  const std::vector<uint8_t> first = coordinate(0x040, 0x050);
  const std::vector<uint8_t> second = coordinate(0x0C0, 0x0D0);
  plain.insert(plain.end(), first.begin(), first.end());
  plain.push_back(kApd);
  plain.push_back(code(2, 6));
  plain.insert(plain.end(), second.begin(), second.end());
  const std::vector<uint8_t> record = with_parity(plain);

  // An operand knocked out of its column, and an opcode knocked into one.
  const std::vector<uint8_t> damaged =
      flip_bit(flip_bit(record, 3, 0x40), 6, 0x40);

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.bytes_repaired, 2u);
  EXPECT_EQ(payloads(result.data), payloads(record));
  EXPECT_EQ(result.summary.errors_after, 0u);
}

// Two faults in the same operand run can mask each other: with the second still
// wrong, correcting the first buys nothing the grammar can see, and the other
// way about. Both are declined rather than guessed at, and the structural pass
// salvages what it can of the run.
TEST(NaplpsLintRepair, DeclinesTwoFaultsInOneRunThatMaskEachOther) {
  std::vector<uint8_t> plain = {kSo, code(2, 8)};
  const std::vector<uint8_t> first = coordinate(0x040, 0x050);
  const std::vector<uint8_t> second = coordinate(0x0C0, 0x0D0);
  plain.insert(plain.end(), first.begin(), first.end());
  plain.push_back(code(2, 6));
  plain.insert(plain.end(), second.begin(), second.end());
  const std::vector<uint8_t> record = with_parity(plain);

  const std::vector<uint8_t> damaged =
      flip_bit(flip_bit(record, 3, 0x40), 5, 0x40);

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.bytes_repaired, 0u);
  // What survives is the intact trailing word, re-anchored to its opcode.
  EXPECT_EQ(result.summary.pdis_resynchronised, 1u);
  EXPECT_LE(result.summary.errors_after, result.summary.errors_before);
}

////////////////////////////////////////////////////////////////////////////////
// Service state
////////////////////////////////////////////////////////////////////////////////

// A page invoking a macro its channel's Support Record defined (CEA-516
// §5.2.7.9) has to be repaired by something that has already seen that Support
// Record, or every such invocation looks like damage.
TEST(NaplpsLintRepair, CarriesServiceStateBetweenRecords) {
  const std::vector<uint8_t> support =
      with_parity({kEsc, code(4, 0), code(2, 1), 'A', kEsc, code(4, 5)});
  const std::vector<uint8_t> page =
      with_parity({kEsc, code(2, 9), code(7, 10), kSo, code(2, 1)});

  NaplpsLintRepairer repairer;
  EXPECT_EQ(repairer.repair(support).summary.errors_after, 0u);
  const NaplpsRepairResult result = repairer.repair(page);
  EXPECT_EQ(result.findings.count(NaplpsLintRule::kUndefinedMacro), 0u);
  EXPECT_EQ(result.data, page);
}

// A candidate byte considered and rejected must leave nothing of itself behind:
// a trial that defined a macro would make the next record's invocation of it
// look resolvable when it is not.
TEST(NaplpsLintRepair, DoesNotCommitTheStateOfACandidateItRejected) {
  // A record whose damaged byte, under some candidate corrections, would open a
  // macro definition. Whatever is tried, only the chosen record's state stands.
  const std::vector<uint8_t> damaged = flip_bit(
      with_parity({kEsc, code(4, 0), code(2, 1), 'A', kEsc, code(4, 5)}), 2,
      0x04);

  NaplpsLintRepairer repairer;
  const NaplpsRepairResult first = repairer.repair(damaged);

  // Whichever macro code the repaired record defined, exactly one was defined:
  // invoking a different one is still undefined.
  const std::vector<uint8_t> page =
      with_parity({kEsc, code(2, 9), code(7, 10), kSo, code(7, 15)});
  const NaplpsRepairResult second = repairer.repair(page);
  EXPECT_EQ(second.findings.count(NaplpsLintRule::kUndefinedMacro), 1u);
  EXPECT_GE(first.summary.suspect_bytes, 1u);
}

////////////////////////////////////////////////////////////////////////////////
// Task 2.4 — diagnostics
////////////////////////////////////////////////////////////////////////////////

// The interpreter is never told a record was repaired, so it leaves the repair
// counters at the zeroes it built them with — which is the honest reading for a
// page presented as transmitted.
TEST(NaplpsLintRepair, LeavesTheRepairCountersZeroWhenNothingRanThem) {
  NaplpsInterpreter interpreter;
  interpreter.reset_decoder();
  const NabtsPageSnapshot page = interpreter.run(two_line_record());

  EXPECT_EQ(page.diagnostics.repaired_bytes, 0u);
  EXPECT_EQ(page.diagnostics.resynchronised_pdis, 0u);
  EXPECT_EQ(page.diagnostics.dropped_coordinate_words, 0u);
  EXPECT_EQ(page.diagnostics.undecided_suspect_bytes, 0u);
}

TEST(NaplpsLintRepair, StampsWhatTheRepairDidOntoADecodedPage) {
  NaplpsRepairSummary summary;
  summary.ran = true;
  summary.bytes_repaired = 3;
  summary.pdis_resynchronised = 2;
  summary.coordinate_words_dropped = 1;
  summary.bytes_ambiguous = 4;

  NabtsPageSnapshot page;
  page.diagnostics.bytes_read = 42;
  naplps_stamp_repair_diagnostics(summary, page.diagnostics);

  EXPECT_EQ(page.diagnostics.repaired_bytes, 3u);
  EXPECT_EQ(page.diagnostics.resynchronised_pdis, 2u);
  EXPECT_EQ(page.diagnostics.dropped_coordinate_words, 1u);
  EXPECT_EQ(page.diagnostics.undecided_suspect_bytes, 4u);
  // Everything the interpreter measured is left as it found it.
  EXPECT_EQ(page.diagnostics.bytes_read, 42u);
}

TEST(NaplpsLintRepair, StampsNothingForAPassThatNeverRan) {
  const NaplpsRepairSummary summary;  // ran == false
  NabtsPageSnapshot page;
  page.diagnostics.repaired_bytes = 7;
  naplps_stamp_repair_diagnostics(summary, page.diagnostics);
  EXPECT_EQ(page.diagnostics.repaired_bytes, 7u);
}

TEST(NaplpsRepairSummary, SummarisesNothingForAPassThatNeverRan) {
  EXPECT_TRUE(NaplpsRepairSummary{}.summary().empty());
}

TEST(NaplpsRepairSummary, SaysWhatItDidAndHowMuchWasInDoubt) {
  const std::vector<uint8_t> record = two_line_record();
  const std::vector<uint8_t> damaged = flip_bit(record, 3, 0x40);

  const std::string summary = repair(damaged).summary.summary();
  EXPECT_NE(summary.find("NAPLPS repair"), std::string::npos);
  EXPECT_NE(summary.find("in doubt"), std::string::npos);
}

////////////////////////////////////////////////////////////////////////////////
// Robustness
////////////////////////////////////////////////////////////////////////////////

// A recovered record is arbitrary bytes as far as the repairer is concerned. It
// must always terminate, always keep the record's length, and never leave it
// worse than it found it.
TEST(NaplpsLintRepair, SurvivesArbitraryCorruptionOfARealRecord) {
  const std::vector<uint8_t> record = with_parity(
      {kSo,           code(2, 1),    numeric(0x00), code(2, 8), numeric(0x04),
       code(2, 10),   numeric(0x08), numeric(0x0C), 0x0F,       'A',
       'B',           kEsc,          code(4, 0),    code(2, 1), 'C',
       kEsc,          code(4, 5),    kSo,           code(2, 4), numeric(0x10),
       numeric(0x14), numeric(0x18)});

  std::mt19937 rng(20260811u);  // fixed, so a failure is reproducible
  std::uniform_int_distribution<size_t> offset_of(0, record.size() - 1);
  std::uniform_int_distribution<int> bit_of(0, 7);

  for (int trial = 0; trial < 500; ++trial) {
    std::vector<uint8_t> damaged = record;
    const int hits = 1 + (trial % 4);
    for (int hit = 0; hit < hits; ++hit) {
      const size_t offset = offset_of(rng);
      damaged[offset] =
          static_cast<uint8_t>(damaged[offset] ^ (1u << bit_of(rng)));
    }

    const NaplpsRepairResult result = repair(damaged);
    ASSERT_EQ(result.data.size(), record.size()) << "trial " << trial;
    // Repair never makes the grammar worse: that is the whole contract.
    EXPECT_LE(result.summary.errors_after, result.summary.errors_before)
        << "trial " << trial;
    // And whatever it produced, the interpreter can run it without complaint.
    (void)draw(result.data);
  }
}

TEST(NaplpsLintRepair, HandlesAnEmptyRecord) {
  const NaplpsRepairResult result = repair({});
  EXPECT_TRUE(result.data.empty());
  EXPECT_TRUE(result.summary.ran);
  EXPECT_EQ(result.summary.total_repairs(), 0u);
}

// Beyond a certain amount of damage, byte-level repair is not the right tool
// and the cost of trying is real. The cap is reported rather than hidden.
TEST(NaplpsLintRepair, ReportsSuspectBytesItDidNotInspect) {
  std::vector<uint8_t> damaged;
  damaged.reserve(kNaplpsMaxInspectedSuspectBytes + 20);
  damaged.push_back(teletext_odd_parity_encode(kSo));
  // Zero fails odd parity, so every one of these is a byte in doubt.
  damaged.resize(damaged.size() + kNaplpsMaxInspectedSuspectBytes + 20, 0x00);

  const NaplpsRepairResult result = repair(damaged);
  EXPECT_EQ(result.summary.suspect_bytes, kNaplpsMaxInspectedSuspectBytes + 20);
  EXPECT_EQ(result.summary.suspect_bytes_uninspected, 20u);
  EXPECT_EQ(result.data.size(), damaged.size());
}

}  // namespace
}  // namespace orc
