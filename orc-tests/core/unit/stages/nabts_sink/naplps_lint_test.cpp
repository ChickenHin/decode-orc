/*
 * File:        naplps_lint_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NAPLPS linter — span classification and
 *              grammar findings (X3.110 §4.3, §5.3, §6)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_lint.h"

#include <gtest/gtest.h>

#include <cstdint>
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

/// Lint with no damage evidence at all, which is how a grammar-only test wants
/// it: an empty suspect map holds nothing in doubt.
NaplpsLintResult lint_grammar(const std::vector<uint8_t>& record) {
  NaplpsLinter linter;
  return linter.lint(record, NaplpsSuspectMap{});
}

/// Offsets covered by spans, in order, so a test can assert the tiling.
size_t span_coverage(const NaplpsLintResult& result) {
  size_t covered = 0;
  for (const NaplpsSpan& span : result.spans) {
    covered += span.length;
  }
  return covered;
}

/// Whether the spans tile [0, length) exactly: ascending, contiguous, no gaps
/// and no overlaps.
::testing::AssertionResult SpansTile(const NaplpsLintResult& result,
                                     size_t length) {
  size_t expected = 0;
  for (const NaplpsSpan& span : result.spans) {
    if (span.offset != expected) {
      return ::testing::AssertionFailure()
             << "span " << naplps_span_kind_name(span.kind) << " starts at "
             << span.offset << ", expected " << expected;
    }
    if (span.length == 0) {
      return ::testing::AssertionFailure()
             << "zero-length span at " << span.offset;
    }
    expected = span.offset + span.length;
  }
  if (expected != length) {
    return ::testing::AssertionFailure()
           << "spans cover " << expected << " bytes of " << length;
  }
  return ::testing::AssertionSuccess();
}

/// Count of spans of |kind|.
size_t count_kind(const NaplpsLintResult& result, NaplpsSpanKind kind) {
  size_t count = 0;
  for (const NaplpsSpan& span : result.spans) {
    if (span.kind == kind) {
      ++count;
    }
  }
  return count;
}

// The PDI set is designated into G1 by default (§4.3.1.3), so SO invokes it.
constexpr uint8_t kSo = 0x0E;
constexpr uint8_t kSi = 0x0F;
constexpr uint8_t kEsc = 0x1B;

/// A numeric data byte carrying |payload| in b6-b1 (§5.3.1, Figure 10).
constexpr uint8_t numeric(uint8_t payload) {
  return static_cast<uint8_t>(0x40 | (payload & 0x3F));
}

////////////////////////////////////////////////////////////////////////////////
// Span classification (§4.3, §5.3.1)
////////////////////////////////////////////////////////////////////////////////

TEST(NaplpsLinter, ClassifiesAnEmptyRecordAsNothingAtAll) {
  const NaplpsLintResult result = lint_grammar({});
  EXPECT_TRUE(result.spans.empty());
  EXPECT_TRUE(result.findings.clean());
  EXPECT_TRUE(result.findings.summary().empty());
}

// §4.3.1.3: G0 holds the primary character set and is invoked, so plain text
// bytes are graphic characters.
TEST(NaplpsLinter, ClassifiesPrimarySetBytesAsGraphicCharacters) {
  const std::vector<uint8_t> record = with_parity({'A', 'B', 'C'});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_TRUE(SpansTile(result, record.size()));
  EXPECT_EQ(count_kind(result, NaplpsSpanKind::kGraphic), 3u);
  EXPECT_TRUE(result.findings.clean());
}

// §5.3.1: within the PDI set b7 clear is an opcode and b7 set is numeric data.
// One opcode and its operand run are two spans, not one.
TEST(NaplpsLinter, SplitsAPdiIntoItsOpcodeAndItsOperandRun) {
  // SO invokes G1 (the PDI set), then POINT SET ABS with one three-byte
  // coordinate word — the default multi-value length of §5.3.2.2.3.
  const std::vector<uint8_t> record = with_parity(
      {kSo, code(2, 4), numeric(0x10), numeric(0x20), numeric(0x30)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_TRUE(SpansTile(result, record.size()));
  ASSERT_EQ(result.spans.size(), 3u);
  EXPECT_EQ(result.spans[0].kind, NaplpsSpanKind::kControl);
  EXPECT_EQ(result.spans[1].kind, NaplpsSpanKind::kPdiOpcode);
  EXPECT_EQ(result.spans[2].kind, NaplpsSpanKind::kPdiOperand);
  EXPECT_EQ(result.spans[2].length, 3u);
  EXPECT_TRUE(result.findings.clean());
}

// §5.3.1: "A PDI sequence is terminated by an opcode introducing the next PDI
// sequence or by any other presentation layer code not from the numeric data
// section of the same PDI set."
TEST(NaplpsLinter, EndsAnOperandRunAtTheNextOpcode) {
  const std::vector<uint8_t> record =
      with_parity({kSo, code(2, 4), numeric(0x10), numeric(0x20), numeric(0x30),
                   code(2, 6), numeric(0x01), numeric(0x02), numeric(0x03)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_TRUE(SpansTile(result, record.size()));
  EXPECT_EQ(count_kind(result, NaplpsSpanKind::kPdiOpcode), 2u);
  EXPECT_EQ(count_kind(result, NaplpsSpanKind::kPdiOperand), 2u);
}

// §4.3.2: ESC 2/8 4/2 designates the primary set into G0 — three bytes, one
// span.
TEST(NaplpsLinter, ClassifiesAWholeEscapeSequenceAsOneSpan) {
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(2, 8), code(4, 2), 'A'});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_TRUE(SpansTile(result, record.size()));
  ASSERT_EQ(result.spans.size(), 2u);
  EXPECT_EQ(result.spans[0].kind, NaplpsSpanKind::kEscape);
  EXPECT_EQ(result.spans[0].length, 3u);
  EXPECT_EQ(result.spans[1].kind, NaplpsSpanKind::kGraphic);
  EXPECT_TRUE(result.findings.clean());
}

// §6.1.2.4: APS takes the two bytes after it as a row and column address.
TEST(NaplpsLinter, GivesApsItsTwoAddressBytes) {
  constexpr uint8_t kAps = 0x1C;
  const std::vector<uint8_t> record =
      with_parity({kAps, code(2, 8), code(2, 8), 'A'});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_TRUE(SpansTile(result, record.size()));
  ASSERT_EQ(result.spans.size(), 3u);
  EXPECT_EQ(result.spans[0].kind, NaplpsSpanKind::kControl);
  EXPECT_EQ(result.spans[1].kind, NaplpsSpanKind::kControlOperand);
  EXPECT_EQ(result.spans[1].length, 2u);
  EXPECT_EQ(result.spans[2].kind, NaplpsSpanKind::kGraphic);
}

// §6.1.2.4: "If either of the characters following the APS character is a C0 or
// C1 control, the APS is ignored and the C0 or C1 control is executed" — so
// neither byte is consumed by the APS.
TEST(NaplpsLinter, LeavesAControlAfterApsToBeExecuted) {
  constexpr uint8_t kAps = 0x1C;
  constexpr uint8_t kApd = 0x0A;
  const std::vector<uint8_t> record = with_parity({kAps, kApd, 'A'});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_TRUE(SpansTile(result, record.size()));
  ASSERT_EQ(result.spans.size(), 3u);
  EXPECT_EQ(result.spans[0].kind, NaplpsSpanKind::kControl);  // APS
  EXPECT_EQ(result.spans[1].kind, NaplpsSpanKind::kControl);  // APD
  EXPECT_EQ(result.spans[2].kind, NaplpsSpanKind::kGraphic);
}

// §5.3.1: the transparent controls "do not terminate PDI sequences", so one
// embedded in the operands splits the span without ending the PDI.
TEST(NaplpsLinter, KeepsAPdiOpenAcrossAnEmbeddedTransparentControl) {
  constexpr uint8_t kSyn = 0x16;  // §6.1.4, a transmission control
  const std::vector<uint8_t> record = with_parity(
      {kSo, code(2, 4), numeric(0x10), kSyn, numeric(0x20), numeric(0x30)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_TRUE(SpansTile(result, record.size()));
  EXPECT_EQ(count_kind(result, NaplpsSpanKind::kTransparentControl), 1u);
  // Two contiguous runs of numeric data, split by the control between them.
  EXPECT_EQ(count_kind(result, NaplpsSpanKind::kPdiOperand), 2u);
  // The three operand bytes still made one whole coordinate word, so the PDI
  // is complete.
  EXPECT_EQ(result.findings.count(NaplpsLintRule::kTruncatedPdi), 0u);
}

// §6.2.2.1: a macro definition's bytes are stored rather than executed.
TEST(NaplpsLinter, ClassifiesAMacroBodyAsStoredRatherThanExecuted) {
  // ESC 4/0 (DEF MACRO), code 2/1, body "AB", ESC 4/5 (END).
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(4, 0), code(2, 1), 'A', 'B', kEsc, code(4, 5)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_TRUE(SpansTile(result, record.size()));
  EXPECT_EQ(count_kind(result, NaplpsSpanKind::kDefinitionCode), 1u);
  EXPECT_EQ(count_kind(result, NaplpsSpanKind::kMacroBody), 2u);
  // The body was stored, not displayed.
  EXPECT_EQ(count_kind(result, NaplpsSpanKind::kGraphic), 0u);
  EXPECT_TRUE(result.findings.clean());
}

// A macro expansion is walked for its effect on the state but classifies
// nothing: its bytes were already classified where they were defined, and an
// expansion has no offsets in the record at all.
TEST(NaplpsLinter, ReportsNoSpansForAMacroExpansion) {
  // Define macro 2/1 as "AB", then invoke it: ESC 2/9 7/10 designates the macro
  // set into G1, SO invokes it, and 2/1 calls the macro.
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(4, 0), code(2, 1), 'A', 'B', kEsc, code(4, 5),
                   kEsc, code(2, 9), code(7, 10), kSo, code(2, 1)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_TRUE(SpansTile(result, record.size()));
  EXPECT_EQ(count_kind(result, NaplpsSpanKind::kMacroInvocation), 1u);
  // The expansion drew two characters, and neither is a byte of the record at
  // the point of invocation.
  EXPECT_EQ(count_kind(result, NaplpsSpanKind::kGraphic), 0u);
  EXPECT_TRUE(result.findings.clean());
}

TEST(NaplpsLinter, FindsTheSpanCoveringAnOffset) {
  const std::vector<uint8_t> record = with_parity(
      {kSo, code(2, 4), numeric(0x10), numeric(0x20), numeric(0x30)});
  const NaplpsLintResult result = lint_grammar(record);

  ASSERT_NE(result.span_at(0), nullptr);
  EXPECT_EQ(result.span_at(0)->kind, NaplpsSpanKind::kControl);
  ASSERT_NE(result.span_at(1), nullptr);
  EXPECT_EQ(result.span_at(1)->kind, NaplpsSpanKind::kPdiOpcode);
  for (size_t offset = 2; offset < record.size(); ++offset) {
    ASSERT_NE(result.span_at(offset), nullptr) << "at " << offset;
    EXPECT_EQ(result.span_at(offset)->kind, NaplpsSpanKind::kPdiOperand);
  }
  EXPECT_EQ(result.span_at(record.size()), nullptr);
}

////////////////////////////////////////////////////////////////////////////////
// Consumption parity with the interpreter
////////////////////////////////////////////////////////////////////////////////

// The linter's whole value rests on its walking the record the way the
// interpreter does. Where the two disagree about how many bytes a command takes
// they disagree about everything after it, so the character count is the check:
// a mis-consumed byte either swallows a character or invents one.
TEST(NaplpsLinter, ClassifiesAsManyCharactersAsTheInterpreterDraws) {
  constexpr uint8_t kAps = 0x1C;
  const std::vector<uint8_t> record = with_parity({
      // Two characters, an APS with its two address bytes, two more.
      'A',
      'B',
      kAps,
      code(2, 8),
      code(2, 8),
      'C',
      'D',
      // A designation, a PDI with operands, and a character after it.
      kEsc,
      code(2, 8),
      code(4, 2),
      kSo,
      code(2, 4),
      numeric(0x10),
      numeric(0x20),
      numeric(0x30),
      kSi,
      'E',
      // A macro defined and never invoked: stored, so drawn zero times.
      kEsc,
      code(4, 0),
      code(2, 1),
      'X',
      'Y',
      kEsc,
      code(4, 5),
      // One last character.
      'F',
  });

  const NaplpsLintResult result = lint_grammar(record);
  EXPECT_TRUE(SpansTile(result, record.size()));

  NaplpsInterpreter interpreter;
  interpreter.reset_decoder();
  const NabtsPageSnapshot page = interpreter.run(record);

  size_t characters = 0;
  for (const NabtsPrimitive& primitive : page.primitives) {
    if (primitive.kind == NabtsPrimitiveKind::kCharacter) {
      ++characters;
    }
  }
  EXPECT_EQ(count_kind(result, NaplpsSpanKind::kGraphic), characters);
  EXPECT_EQ(characters, 6u);  // A B C D E F
  EXPECT_EQ(span_coverage(result), record.size());
}

// The interpreter reads every byte of a well-formed record, so the linter must
// account for every byte too.
TEST(NaplpsLinter, AccountsForEveryByteTheInterpreterReads) {
  const std::vector<uint8_t> record = with_parity(
      {kSo, code(2, 1), numeric(0x0A), code(2, 4), numeric(0x10), numeric(0x20),
       numeric(0x30), kSi, 'A', kEsc, code(4, 8), 'B'});

  const NaplpsLintResult result = lint_grammar(record);
  EXPECT_TRUE(SpansTile(result, record.size()));

  NaplpsInterpreter interpreter;
  interpreter.reset_decoder();
  const NabtsPageSnapshot page = interpreter.run(record);
  EXPECT_EQ(page.diagnostics.bytes_read, record.size());
}

////////////////////////////////////////////////////////////////////////////////
// Findings: the structural rules
////////////////////////////////////////////////////////////////////////////////

// §4.3.2: "The occurrence of any other bit combination in an escape sequence
// shall cause the partial escape sequence to be terminated and ignored, and
// that bit combination shall be executed."
TEST(NaplpsLinter, ReportsAnEscapeBrokenByAByteOutsideTheSyntax) {
  // A C0 control where the final character should be.
  const std::vector<uint8_t> record = with_parity({kEsc, code(2, 8), 0x0A});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kMalformedEscape), 1u);
  EXPECT_EQ(naplps_lint_severity(NaplpsLintRule::kMalformedEscape),
            NaplpsLintSeverity::kError);
  // The offending byte is not consumed by the sequence: it is executed.
  EXPECT_TRUE(SpansTile(result, record.size()));
  ASSERT_GE(result.spans.size(), 2u);
  EXPECT_EQ(result.spans[0].length, 2u);  // ESC and the intermediate only
}

TEST(NaplpsLinter, ReportsAnEscapeCutShortByTheEndOfTheRecord) {
  const std::vector<uint8_t> record = with_parity({'A', kEsc, code(2, 8)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kTruncatedEscape), 1u);
  EXPECT_TRUE(SpansTile(result, record.size()));
}

// §4.3.2: a designation naming a set outside Table 1 designates the null set.
TEST(NaplpsLinter, ReportsADesignationOfASetOutsideTheStandard) {
  // ESC 2/8 with a final character Table 1 does not list.
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(2, 8), code(3, 0)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kNullDesignation), 1u);
  // Legal, so a warning rather than an error.
  EXPECT_EQ(naplps_lint_severity(NaplpsLintRule::kNullDesignation),
            NaplpsLintSeverity::kWarning);
  EXPECT_EQ(result.findings.errors, 0u);
}

// §5.3.2.2.5 zero-extends a short operand, so a PDI missing most of its
// coordinate still draws — somewhere wrong.
TEST(NaplpsLinter, ReportsAPdiWithTooFewOperandBytes) {
  // SET LINE ABS needs two three-byte coordinate words; this carries one.
  const std::vector<uint8_t> record = with_parity(
      {kSo, code(2, 10), numeric(0x10), numeric(0x20), numeric(0x30)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kTruncatedPdi), 1u);
  ASSERT_FALSE(result.findings.findings.empty());
  EXPECT_EQ(result.findings.findings.front().context, code(2, 10));
}

// §5.3.2.2.5 makes a long run a repeat of the opcode, so a run that is not a
// whole number of words is legal — and not what a service sends.
TEST(NaplpsLinter, ReportsAnOperandRunThatIsNotAWholeNumberOfWords) {
  const std::vector<uint8_t> record =
      with_parity({kSo, code(2, 4), numeric(0x10), numeric(0x20), numeric(0x30),
                   numeric(0x01)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kOperandRunMisaligned), 1u);
  EXPECT_EQ(naplps_lint_severity(NaplpsLintRule::kOperandRunMisaligned),
            NaplpsLintSeverity::kWarning);
  ASSERT_FALSE(result.findings.findings.empty());
  // The finding points at the leftover byte, not at the opcode.
  EXPECT_EQ(result.findings.findings.front().offset, 5u);
}

// §5.3.1: numeric data with no opcode in front of it has nothing to be an
// operand of.
TEST(NaplpsLinter, ReportsNumericDataWithNoOpcode) {
  const std::vector<uint8_t> record = with_parity({kSo, numeric(0x10)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kOrphanedOperand), 1u);
  EXPECT_TRUE(SpansTile(result, record.size()));
}

// §6.2.2.1: "If the character following the DEF MACRO control is not in this
// range, the entire command ... is in error and is executed as a null
// operation."
TEST(NaplpsLinter, ReportsADefinitionCodeOutsideTheGraphicRange) {
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(4, 0), 0x0A, 'A'});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kInvalidDefinitionCode), 1u);
  // The offending byte is left to be executed rather than swallowed.
  EXPECT_TRUE(SpansTile(result, record.size()));
}

// §6.2.4: the texture mask code must name mask A to D, 4/1 to 4/4.
TEST(NaplpsLinter, ReportsATextureMaskOtherThanAToD) {
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(4, 4), code(5, 0), kEsc, code(4, 5)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kInvalidTextureCode), 1u);
}

TEST(NaplpsLinter, ReportsAnInvocationOfAMacroNothingDefined) {
  // Designate the macro set into G1, invoke it, and call a macro never defined.
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(2, 9), code(7, 10), kSo, code(2, 1)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kUndefinedMacro), 1u);
  ASSERT_FALSE(result.findings.findings.empty());
  EXPECT_EQ(result.findings.findings.front().context, code(2, 1));
}

// A macro the channel's Support Record defined is defined for the pages that
// follow it (CEA-516 §5.2.7.9), so one linter instance carries it across
// records exactly as one interpreter instance does.
TEST(NaplpsLinter, CarriesMacrosBetweenRecordsOfTheSameService) {
  const std::vector<uint8_t> support =
      with_parity({kEsc, code(4, 0), code(2, 1), 'A', kEsc, code(4, 5)});
  const std::vector<uint8_t> page =
      with_parity({kEsc, code(2, 9), code(7, 10), kSo, code(2, 1)});

  NaplpsLinter linter;
  linter.reset_decoder();
  EXPECT_TRUE(linter.lint(support, NaplpsSuspectMap{}).findings.clean());
  const NaplpsLintResult result = linter.lint(page, NaplpsSuspectMap{});
  EXPECT_EQ(result.findings.count(NaplpsLintRule::kUndefinedMacro), 0u);

  // A fresh linter has never seen the Support Record, and says so.
  NaplpsLinter fresh;
  EXPECT_EQ(fresh.lint(page, NaplpsSuspectMap{})
                .findings.count(NaplpsLintRule::kUndefinedMacro),
            1u);
}

TEST(NaplpsLinter, ReportsADefinitionLeftOpenAtTheEndOfTheRecord) {
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(4, 0), code(2, 1), 'A', 'B'});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kUnterminatedDefinition), 1u);
}

// §6.2.7.2: REPEAT repeats the last graphic character, and takes a count byte
// from columns 4 to 7.
TEST(NaplpsLinter, ReportsARepeatWithNothingToRepeat) {
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(4, 6), numeric(0x03)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kInvalidRepeat), 1u);
}

TEST(NaplpsLinter, AcceptsARepeatAfterACharacter) {
  const std::vector<uint8_t> record =
      with_parity({'A', kEsc, code(4, 6), numeric(0x03)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_TRUE(result.findings.clean());
  EXPECT_TRUE(SpansTile(result, record.size()));
  EXPECT_EQ(count_kind(result, NaplpsSpanKind::kControlOperand), 1u);
}

// §4.3.2 makes a single shift affect the next character; one still pending when
// the record runs out never got one.
TEST(NaplpsLinter, ReportsASingleShiftWithNoCharacterAfterIt) {
  constexpr uint8_t kSs2 = 0x19;
  const std::vector<uint8_t> record = with_parity({'A', kSs2});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kDanglingSingleShift), 1u);
}

////////////////////////////////////////////////////////////////////////////////
// Findings: the range rules (§5.3.1)
////////////////////////////////////////////////////////////////////////////////

// §5.3.1 makes a coordinate a two's-complement binary fraction in [-1, 1), and
// a drawing outside the unit screen "is considered to be in error". A negative
// absolute coordinate is therefore out of range.
TEST(NaplpsLinter, ReportsAnAbsoluteCoordinateOutsideTheUnitScreen) {
  // POINT SET ABS with the sign bit of X set: the first numeric byte's b6 is
  // the MSB of X, which Figure 11 weights -1.
  const std::vector<uint8_t> record = with_parity(
      {kSo, code(2, 4), numeric(0x20), numeric(0x00), numeric(0x00)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kCoordinateOutOfRange), 1u);
  EXPECT_EQ(naplps_lint_severity(NaplpsLintRule::kCoordinateOutOfRange),
            NaplpsLintSeverity::kError);
  ASSERT_FALSE(result.findings.findings.empty());
  // The finding covers the whole coordinate word, which is what a repair pass
  // has to reconsider.
  EXPECT_EQ(result.findings.findings.front().offset, 2u);
  EXPECT_EQ(result.findings.findings.front().length, 3u);
}

TEST(NaplpsLinter, AcceptsAnAbsoluteCoordinateInsideTheUnitScreen) {
  const std::vector<uint8_t> record = with_parity(
      {kSo, code(2, 4), numeric(0x09), numeric(0x09), numeric(0x09)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kCoordinateOutOfRange), 0u);
}

// A relative coordinate is a displacement, which Figure 11 bounds by
// construction — the negative value that fails an absolute check is ordinary
// here.
TEST(NaplpsLinter, DoesNotRangeCheckARelativeCoordinate) {
  // POINT SET REL (2/5) with the same bits that failed as an absolute.
  const std::vector<uint8_t> record = with_parity(
      {kSo, code(2, 5), numeric(0x20), numeric(0x00), numeric(0x00)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kCoordinateOutOfRange), 0u);
}

// §5.3.2.2.6: "the new length of the multi-value operands, as set in byte 1,
// applies to the multi-value logical pel size operand of that DOMAIN command",
// and to everything after it — so a DOMAIN changes how later runs divide up.
TEST(NaplpsLinter, DividesLaterOperandRunsOnTheLengthDomainDeclared) {
  // DOMAIN byte 1 with b5 b4 b3 = 000 sets the multi-value length to one byte
  // (§5.3.2.2.3 Table 5), after which a single numeric byte is a whole
  // coordinate word.
  const std::vector<uint8_t> record =
      with_parity({kSo, code(2, 1), numeric(0x00), code(2, 4), numeric(0x09)});
  const NaplpsLintResult result = lint_grammar(record);

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kTruncatedPdi), 0u);
  EXPECT_EQ(result.findings.count(NaplpsLintRule::kOperandRunMisaligned), 0u);

  // Without the DOMAIN the same POINT would be two bytes short of the
  // three-byte default word.
  const std::vector<uint8_t> undeclared =
      with_parity({kSo, code(2, 4), numeric(0x09)});
  EXPECT_EQ(
      lint_grammar(undeclared).findings.count(NaplpsLintRule::kTruncatedPdi),
      1u);
}

////////////////////////////////////////////////////////////////////////////////
// Damage evidence (CEA-516 §3.3)
////////////////////////////////////////////////////////////////////////////////

TEST(NaplpsSuspectMap, HoldsNothingInDoubtWhenItIsEmpty) {
  const NaplpsSuspectMap map;
  EXPECT_TRUE(map.empty());
  EXPECT_FALSE(map.suspect(0));
  EXPECT_FALSE(map.any_suspect(0, 100));
  EXPECT_EQ(map.suspect_count(), 0u);
}

// §3.3 gives every data byte odd parity in b8, so a byte failing the check
// carries an odd number of wrong bits.
TEST(NaplpsSuspectMap, FlagsBytesThatFailOddParity) {
  std::vector<uint8_t> record = with_parity({'A', 'B', 'C'});
  record[1] ^= 0x01;  // one bit wrong, so parity now fails

  const NaplpsSuspectMap map = NaplpsSuspectMap::from_record(record);
  EXPECT_FALSE(map.suspect(0));
  EXPECT_TRUE(map.suspect(1));
  EXPECT_TRUE(map.parity_failed(1));
  EXPECT_FALSE(map.low_confidence(1));
  EXPECT_FALSE(map.suspect(2));
  EXPECT_EQ(map.suspect_count(), 1u);
}

// A lost packet leaves 0x00 fillers, and zero has even parity — so a hole is
// suspect without anything having to say it is one. It is held apart from a
// byte that arrived wrong, because nothing may be inferred from a value
// standing in for one that never came.
TEST(NaplpsSuspectMap, FlagsTheZeroFillerALostPacketLeavesAsMissing) {
  std::vector<uint8_t> record = with_parity({'A', 'B', 'C'});
  record[1] = 0x00;

  const NaplpsSuspectMap map = NaplpsSuspectMap::from_record(record);
  EXPECT_TRUE(map.parity_failed(1));
  EXPECT_TRUE(map.missing(1));
  EXPECT_FALSE(map.missing(0));
}

// Where the recovery kept the arrival mask it is believed over the heuristic:
// a byte that did arrive is not missing however it reads.
TEST(NaplpsSuspectMap, BelievesTheArrivalMaskTheRecoveryKept) {
  std::vector<uint8_t> record = with_parity({'A', 'B', 'C'});
  record[1] = 0x00;
  const std::vector<uint8_t> present = {1, 1, 0};

  const NaplpsSuspectMap map = NaplpsSuspectMap::from_record(record, present);
  EXPECT_FALSE(map.missing(1));  // it arrived; it is merely wrong
  EXPECT_TRUE(map.parity_failed(1));
  EXPECT_TRUE(map.missing(2));  // it never arrived, whatever it reads as
}

TEST(NaplpsSuspectMap, FlagsBytesTheDetectorWasUnsureOf) {
  const std::vector<uint8_t> record = with_parity({'A', 'B', 'C'});
  const std::vector<uint8_t> confidence = {255, 10, 255};

  const NaplpsSuspectMap map =
      NaplpsSuspectMap::from_record(record, {}, confidence);
  EXPECT_FALSE(map.suspect(0));
  EXPECT_TRUE(map.suspect(1));
  EXPECT_TRUE(map.low_confidence(1));
  EXPECT_FALSE(map.parity_failed(1));
}

// A copy nothing measured is read as full confidence throughout, which is the
// same reading the record catalogue's vote gives it.
TEST(NaplpsSuspectMap, TreatsAnUnmeasuredCopyAsFullyConfident) {
  const std::vector<uint8_t> record = with_parity({'A', 'B', 'C'});
  const NaplpsSuspectMap map = NaplpsSuspectMap::from_record(record, {}, {});
  EXPECT_EQ(map.suspect_count(), 0u);
}

// §5.3.1 has a transparent control not terminate a PDI, so the 0x00 a lost
// packet leaves is swallowed into the operands — and every operand bit after it
// shifts by six.
TEST(NaplpsLinter, ReportsALostByteInsideAnOperandRun) {
  std::vector<uint8_t> record = with_parity(
      {kSo, code(2, 4), numeric(0x10), numeric(0x20), numeric(0x30)});
  record[3] = 0x00;  // the hole a lost packet leaves

  NaplpsLinter linter;
  const NaplpsLintResult result =
      linter.lint(record, NaplpsSuspectMap::from_record(record));

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kHoleInOperandRun), 1u);
  EXPECT_EQ(naplps_lint_severity(NaplpsLintRule::kHoleInOperandRun),
            NaplpsLintSeverity::kError);
  // Two bytes of numeric data left, which is short of a three-byte word.
  EXPECT_EQ(result.findings.count(NaplpsLintRule::kTruncatedPdi), 1u);
}

TEST(NaplpsLinter, ReportsADamagedByteInsideAnOperandRun) {
  std::vector<uint8_t> record = with_parity(
      {kSo, code(2, 4), numeric(0x10), numeric(0x20), numeric(0x30)});
  record[3] ^= 0x01;  // still a numeric byte, but parity now fails

  NaplpsLinter linter;
  const NaplpsLintResult result =
      linter.lint(record, NaplpsSuspectMap::from_record(record));

  EXPECT_EQ(result.findings.count(NaplpsLintRule::kSuspectInOperandRun), 1u);
  // The run still parses, so this is evidence rather than proof.
  EXPECT_EQ(naplps_lint_severity(NaplpsLintRule::kSuspectInOperandRun),
            NaplpsLintSeverity::kWarning);
}

// The default overload computes the evidence from parity alone, which is what a
// caller with no confidence data to offer gets.
TEST(NaplpsLinter, ComputesDamageEvidenceFromParityWhenGivenNone) {
  std::vector<uint8_t> record = with_parity(
      {kSo, code(2, 4), numeric(0x10), numeric(0x20), numeric(0x30)});
  record[4] ^= 0x02;

  NaplpsLinter linter;
  EXPECT_EQ(
      linter.lint(record).findings.count(NaplpsLintRule::kSuspectInOperandRun),
      1u);
}

////////////////////////////////////////////////////////////////////////////////
// Reporting
////////////////////////////////////////////////////////////////////////////////

TEST(NaplpsLintFindings, SummarisesNothingForACleanRecord) {
  const std::vector<uint8_t> record = with_parity({'A', 'B', 'C'});
  EXPECT_TRUE(lint_grammar(record).findings.summary().empty());
}

TEST(NaplpsLintFindings, SummarisesWhatItFoundAndHowBad) {
  const std::vector<uint8_t> record = with_parity({kEsc, code(2, 8), 0x0A});
  const NaplpsLintResult result = lint_grammar(record);

  const std::string summary = result.findings.summary();
  EXPECT_NE(summary.find("NAPLPS lint"), std::string::npos);
  EXPECT_NE(
      summary.find(naplps_lint_rule_name(NaplpsLintRule::kMalformedEscape)),
      std::string::npos);
  EXPECT_EQ(result.findings.errors, 1u);
  EXPECT_EQ(result.findings.warnings, 0u);
  EXPECT_EQ(result.findings.total(), 1u);
}

// A record assembled from a badly damaged carousel can fault on nearly every
// byte, so the offsets are capped while the counts are not.
TEST(NaplpsLintFindings, CapsTheOffsetsItKeepsButNotTheCounts) {
  // Every byte an orphaned operand: numeric data with no opcode in front of it.
  std::vector<uint8_t> plain;
  plain.reserve(kNaplpsMaxLintFindings + 51);
  plain.push_back(kSo);
  plain.resize(plain.size() + kNaplpsMaxLintFindings + 50, numeric(0x01));
  const std::vector<uint8_t> record = with_parity(plain);

  const NaplpsLintResult result = lint_grammar(record);
  EXPECT_EQ(result.findings.findings.size(), kNaplpsMaxLintFindings);
  EXPECT_EQ(result.findings.count(NaplpsLintRule::kOrphanedOperand),
            kNaplpsMaxLintFindings + 50);
  EXPECT_EQ(result.findings.dropped, 50u);
  EXPECT_TRUE(SpansTile(result, record.size()));
}

////////////////////////////////////////////////////////////////////////////////
// Robustness
////////////////////////////////////////////////////////////////////////////////

// A recovered record is arbitrary bytes as far as the linter is concerned, and
// it has to account for all of them without running away.
TEST(NaplpsLinter, ClassifiesEveryByteOfEveryValue) {
  for (int lead = 0; lead <= 0xFF; ++lead) {
    std::vector<uint8_t> record;
    record.push_back(static_cast<uint8_t>(lead));
    record.push_back(teletext_odd_parity_encode('A'));
    record.push_back(teletext_odd_parity_encode('B'));

    const NaplpsLintResult result = lint_grammar(record);
    EXPECT_TRUE(SpansTile(result, record.size())) << "lead byte " << lead;
  }
}

// A macro that invokes itself is what a recovered record can easily contain,
// and the depth bound is what stops it running for ever.
TEST(NaplpsLinter, StopsAMacroThatInvokesItself) {
  // DEF MACRO 2/1, body = designate macro set into G1, SO, 2/1 (itself), END.
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(4, 0), code(2, 1), kEsc, code(2, 9), code(7, 10),
                   kSo, code(2, 1), kEsc, code(4, 5),
                   // Now invoke it.
                   kEsc, code(2, 9), code(7, 10), kSo, code(2, 1)});

  const NaplpsLintResult result = lint_grammar(record);
  EXPECT_TRUE(SpansTile(result, record.size()));
  // The recursion is refused at the depth bound rather than followed, and
  // reported against the invocation in the record that began the chain — the
  // one that overran is inside an expansion, at no offset a reader could look
  // up.
  EXPECT_EQ(result.findings.count(NaplpsLintRule::kMacroRecursionTooDeep), 1u);
  ASSERT_FALSE(result.findings.findings.empty());
  const NaplpsLintFinding& finding = result.findings.findings.front();
  EXPECT_EQ(finding.rule, NaplpsLintRule::kMacroRecursionTooDeep);
  EXPECT_EQ(finding.offset, record.size() - 1);
}

}  // namespace
}  // namespace orc
