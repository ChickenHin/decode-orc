# NAPLPS Linter and Lint-Directed Repair — Implementation Plan

## Purpose

NAPLPS is a formally specified language (FIPS PUB 121 / ANSI X3.110-1983), so a
recovered presentation record can be checked — and in many cases repaired —
against its own grammar: opcode/operand structure, escape-sequence syntax,
operand lengths declared by DOMAIN, and the value ranges the standard bounds.
Captures from tape and disc arrive with known-damaged bytes; combining that
damage evidence with the grammar allows corrections that neither signal
supports alone.

This plan adds a NAPLPS **linter** — a syntax validator over the presentation
byte stream — and a **lint-directed repair** pass to the `nabts_sink` stage.
"Lint" is the deliberate term: the stage already performs error *correction*
in the transport sense (Hamming 8/4 prefixes and headers, the suffix product
code, odd byte parity), and this pass is a different thing — validation of the
language itself, with repairs applied the way a linter applies fixes. The
repair changes how a record *reads*, not what was *recovered*, so the
reader-facing switch lives in the records viewer (alongside the receiver
dropdown, which already embodies this split); the one lint application that
does change recovered data — the lint-assisted vote of Phase 4 — is governed
by a stage parameter.

## Authoritative specifications (in-tree)

- NAPLPS presentation syntax: [FIPS-121.md](analogue-video-specifications/docs/teletext/FIPS-121/FIPS-121.md)
  (the full X3.110-1983 text; PDI set §5.3, code extension §4.3, control sets §6)
- NABTS transport and record structure: [CEA-516_S-2013.md](analogue-video-specifications/docs/teletext/CEA-516_S-2013/CEA-516_S-2013.md)
  (byte parity §3.3, data groups §4, records §5)

## Where the pass sits

Record data flows: packets → data groups → messages → record catalogue (vote
across damaged copies) → `NaplpsInterpreter::run()`. The repair pass runs
**between the catalogue and the interpreter** — on the voted record data, with
byte parity still in place — in `nabts_interpret_records()`
(`orc/plugins/stages/nabts_sink/nabts_record_catalogue.cpp`), which is the
single choke point both the trigger path (`nabts_sink_deps.cpp`) and the
catalogue viewer (`nabts_catalogue_view.cpp`) already go through.

Hard constraints:

- The exported `.t33` packet stream and `.rec` record files are **never**
  modified by the repair pass: they are the capture as transmitted (for `.rec`,
  as voted). Repair affects only interpretation — the catalogue viewer, the
  caption cues the SRT export reads off the presented pages, and any rendering.
  The one deliberate exception is Phase 4's lint-assisted vote, which improves
  the voted record data itself and is therefore recovery-side, under a stage
  parameter of its own.
- The pass never alters a byte that is parity-clean **and** grammatically
  admissible in context. Repairs are gated on independent damage evidence.
- Every intervention is counted in diagnostics; a page must be inspectable as
  "how much of this was guessed at".

## Where the switches live

Lint-directed repair is a property of the reading, not of the recovery — the
same records, read more charitably. The catalogue browser contract already
models exactly this with `CatalogueViewOption`
(`orc/sdk/include/orc/stage/tooling/catalogue_results.h`): "Not a filter and
not a setting: every option shows the same items, drawn differently", which is
how the receiver resolution is offered to the reader today. The repair switch
therefore belongs in the records viewer, where turning it on and off is an
instant A/B comparison over the same catalogue, not a parameter round trip.

Two derived outputs still need a policy at trigger time:

- **SRT caption export** — built by interpreting records in the trigger path
  (`NabtsSinkDeps::write_captions()`), so it is affected by repair. Captions
  are always exported with repair applied: the SRT is itself a derived reading
  aid, the uncorrected data remains in `.t33`/`.rec`, and the report states
  how many repairs the caption records took.
- **Voted record data** (Phase 4) — lint assistance in the vote changes the
  bytes `.rec` exports and everything downstream, so it is a stage parameter,
  not a viewer choice.

## Damage evidence available at the repair point

1. **Byte parity** — CEA-516 §3.3 gives every data byte of a type-zero group
   odd parity in b8. A parity-failed byte has an odd number of wrong bits
   (usually exactly one). Recomputable at any time since the record data
   retains parity until `NaplpsInterpreter::run()` strips it.
2. **Lost-packet holes** — held open as 0x00 fillers by the group assembler
   (`NabtsDataGroup::present`). 0x00 fails odd parity, so holes are
   parity-suspect even after the `present` mask is no longer to hand.
3. **Detector confidence** — per-byte 0–255 from the bit detector, carried as
   far as the vote (`NabtsRecordCopy::confidence`) and, since Phase 4, past it
   on `NabtsCataloguedRecord::data_confidence` along with the arrival mask
   `data_present`. Both are empty for a record kept from a copy that arrived
   whole, which falls back to the parity-only reading.

## Grammar signals the language provides

- **Opcode/operand discrimination** — X3.110 §5.3.1: within the PDI set, b7
  alone separates opcodes (columns 2–3) from numeric data (columns 4–7). An
  operand run's length is meaningful: short is zero-extended, long repeats the
  opcode (§5.3.2.2.5), so runs whose length is not a multiple of the
  DOMAIN-declared word size are suspicious even though they are technically
  legal.
- **Escape-sequence syntax** — §4.3.2: ESC I…I F with intermediates in 2/0–2/15
  and finals in 3/0–7/14; designation pairs from Table 1; two-character C1
  forms with finals in columns 4–5. A malformed sequence at a suspect byte has
  a small candidate set of valid repairs.
- **Declared operand formats** — DOMAIN (§5.3.2.2) sets the operand word size,
  which decides how a numeric run divides up; coordinate words carry fixed bit
  layouts (Figures 10–12).
- **Value ranges** — and here the language is less help than it first appears,
  because most of its parameter fields are *self-bounding*: the field is exactly
  as wide as its legal range, so no bit pattern can name a value outside it, and
  a range check would be dead code. DOMAIN's operand lengths are a two-bit field
  for one to four bytes and a three-bit field for one to eight (Tables 4 and 5);
  a colour-map address is left-justified in its operand and truncated to the
  four bits the 16-entry map needs (§5.3.2.6.1); a colour value beyond three
  bits per gun is explicitly truncated (§5.3.2.5.1). None of these can be
  violated.

  What is genuinely checkable is the handful of fields whose legal range is
  narrower than their encoding: absolute coordinates against the unit screen,
  0 ≤ v < 1 (§5.3.1) — the substantial one, since a coordinate is a signed
  fraction in [-1, 1) and half that range is invalid where the standard demands
  an absolute position; the texture-mask selector, which must name mask A to D
  (§6.2.4); a definition code, which must be a graphic character (§6.2.2.1); a
  REPEAT count byte, which must come from columns 4 to 7 (§6.2.7.2); and the
  polygon and spline vertex bound of Table D1 item 4.

  A relative coordinate cannot be checked this way at all: it is a displacement,
  legal at any value the encoding can express, and whether the point it resolves
  to lies on screen depends on where the drawing point had got to. Checking
  those would mean restating the interpreter's geometry alongside it, where the
  two could drift; the interpreter already counts its own resolved clamps in
  `NabtsDecodeDiagnostics::out_of_range_coordinates`.
- **Structural sequences** — definitions (DEF MACRO/DRCS/TEXTURE) must be
  terminated by another DEF or END (§6.2.2–§6.2.5); SS2/SS3 must be followed
  by a graphic character; a macro invocation must name a defined macro.

The interpreter already tolerates much of this (clamping, skip-whole escape
handling, truncation counts in `NabtsDecodeDiagnostics`). The linter differs
in that it acts *before* interpretation and can use damage evidence to choose
a correction rather than merely surviving the error.

---

## Phase 1 — Linter (detection only)

New module `naplps_lint.h/.cpp` in `orc/plugins/stages/nabts_sink/`,
reusing `NaplpsCodeEnvironment` and `naplps_parse_escape()` so the shadow
parse and the interpreter can never disagree about what a byte is.

### Task 1.1 — Shadow lexer

Walk a record (parity in place) and partition it into classified spans:
C0 control, escape sequence, C1 control, PDI opcode, numeric-data run,
graphic character, definition body, hole filler. Track the same code
environment state the interpreter does (designations, locking/single shifts,
open definitions), without drawing anything.

**Acceptance criteria**
- Classifying then discarding spans consumes exactly the bytes
  `NaplpsInterpreter` would (byte positions verified against interpreter
  behaviour in tests using shared fixtures).
- Handles all `NaplpsEscapeKind` outcomes, transparent controls inside PDI
  operand runs (§5.3.1), and definitions collected across DEF/END.
- Unit tests (`unit` + `sinks` labels) in
  `orc-tests/core/unit/stages/nabts_sink/naplps_lint_test.cpp`; no
  filesystem or other external dependencies.

### Task 1.2 — Suspect-byte map

Build a per-byte suspicion mask for a record: parity failure (includes hole
fillers), and — where supplied — low detector confidence. Confidence
plumbing from the vote is Task 4.1; this task defines the interface so the
mask degrades gracefully to parity-only.

**Acceptance criteria**
- 0x00 hole fillers and parity-failed bytes flagged; parity-clean bytes not.
- Interface accepts an optional confidence vector without requiring one.
- Unit tests cover parity-only and parity-plus-confidence inputs.

### Task 1.3 — Findings taxonomy and lint report

Emit a `NaplpsLintFindings` structure from a lint pass: counts and byte
offsets per rule, with a `summary()` string in the style of
`NabtsGroupStats::summary()` for the stage report. The rules are the
structural ones (malformed escape, escape truncated by the record end,
designation of a set outside Table 1, truncated PDI, control cut short of its
own bytes, operand run not a whole number of words, numeric data with no
opcode, definition code outside the graphic range, texture mask other than A
to D, undefined macro, macro recursion past the depth bound, definition left
open at record end, REPEAT with nothing to repeat, dangling single shift), the
range ones (absolute coordinate outside the unit screen, vertex overflow), and
the damage-evidence ones (hole inside an operand run, suspect byte inside an
operand run).

Each rule carries a severity, because a repair pass must not treat the two
alike: `kError` is the standard being broken, so something really is wrong;
`kWarning` is legal but not what a service does — §4.3.2's null designation,
§5.3.2.2.5's short and long operand runs, and Table D1 item 4's vertex bound,
which bounds what a *receiver* must hold rather than what a sender may send.

The retained offsets are capped (`kNaplpsMaxLintFindings`) while the counts
are not: a record assembled from a badly damaged carousel can fault on nearly
every byte, and a bounded list is what a report and a repair pass can both
use.

**Acceptance criteria**
- A clean, spec-conformant stream yields zero findings (fixture streams from
  existing interpreter tests reused).
- Each violation class is individually constructible and detected in a unit
  test.
- No behavioural change to interpretation in this phase.

---

## Phase 2 — Lint-directed repair engine

New module `naplps_lint_repair.h/.cpp`. Input: record bytes (parity in
place) + suspect map. Output: repaired byte vector + `NaplpsRepairSummary`
counters. Deterministic, single bounded pass; never loops on its own output.

### Task 2.1 — Parity-guided single-bit repair

For each parity-failed byte, generate the eight single-bit-flip candidates
(a b8 flip means the seven-bit payload was already right) and grade each by
re-linting the record with it in place. Grade on findings — fewer errors
first, then fewer warnings — and apply the correction only where one candidate
alone attains the best grade *and* that grade is strictly better than leaving
the byte alone; otherwise leave it and count it undecided. Grading by re-lint
rather than by a per-position admissibility table is what makes the check
right: at most positions the grammar admits both continuing and ending a
construct, so what distinguishes a correction is the faults it removes
downstream, not the column the byte lands in.

**A byte that never arrived is not a byte with a bit wrong in it.** Parity says
an odd number of bits are wrong, which is a statement about something that was
received; the filler standing in for a lost packet carries no information, and
"correcting" it writes a plausible byte into a gap. The suspect map therefore
separates *missing* from *corrupted* (from the recovery's `present` mask, or
failing that from the zero filler the group assembler writes), and substitution
skips the missing ones entirely — the structural pass works around them
instead.

Bytes are weighed one at a time, so two faults that mask each other — where
neither correction is an improvement until the other has been made too — are
both declined. This corrects independent damage, not conspiracies; searching
corrections in combination would cost the square of the record length and
would mostly find coincidences.

**Acceptance criteria**
- Single-bit corruptions injected into fixture streams at each structural
  position class are repaired to the original byte.
- A parity-failed byte with multiple admissible candidates is left unchanged
  and counted.
- Parity-clean bytes are never modified (asserted in tests).

### Task 2.2 — Hole and truncation resynchronisation

A hole inside a PDI operand run misaligns every operand word after it — not
because the zero filler is there, since §5.3.1 makes NUL a transparent control
that operand gathering skips, but because the real bytes it stands in for are
gone. The surviving numeric bytes close up, and the words after the gap are
assembled from the wrong six-bit groups.

On a hole or an unrepairable suspect byte inside an operand run, cut the run
back to the last whole *execution* before the damage — the group of words the
opcode consumes per execution, not merely one word, or a two-word opcode is
left with a half-argument. Whole executions after the damage are retained as a
repeat of the opcode (§5.3.2.2.5) when what follows the gap divides evenly.

Every edit is a byte substitution in place, never an insertion or a deletion,
so the record keeps its length and a caller's parallel `present` and
`confidence` arrays stay index for index with the data. Bytes taken out of a
run are overwritten with the null operation of §6.1.6.4 — which, unlike the NUL
a lost packet leaves, is *not* one of the transparent controls §5.3.1 lets
stand inside a PDI, so it ends the sequence where it stands and draws nothing.
A retained tail is re-anchored by writing the opcode again into the last byte
so nulled, which is not an invention: §5.3.2.2.5 already makes a long run a
repeat of that same opcode.

**Acceptance criteria**
- A LINE/ARC/RECT/POLY sequence with a mid-operand hole no longer produces a
  primitive derived from misaligned operand bits; primitives before the
  damage are unaffected.
- Resynchronisation never consumes past the record end and never enters an
  infinite loop (fuzz-style unit test over random corruptions, seeded
  deterministically).
- Counters distinguish PDIs truncated-by-repair from operand words retained
  by realignment.

### Task 2.3 — Range and sequence repairs

Bounded-value repairs where the standard gives a range narrower than the
encoding and the byte is suspect — see "Value ranges" above for why that is a
short list rather than every parameter the language has. A coordinate word
that fails the absolute unit-screen check and contains a suspect byte is
dropped rather than clamped when the repair pass is on, since its value is
noise rather than a sender's mistake (the interpreter's existing clamp stays
the behaviour for a clean word, which is a sender's mistake). A suspect
texture-mask selector outside 4/1–4/4, definition code outside the graphic
range, or REPEAT count outside columns 4–7 is repaired where exactly one
single-bit candidate lands inside — which the substitution pass above already
does, those being findings like any other. How often it succeeds depends on how
narrow the range is: a texture-mask selector has four legal values and is
usually decidable, while a definition code has ninety-six and usually is not,
several corrections naming equally legal macros.

A definition left open at record end needs no repair: `NaplpsInterpreter::run()`
already closes one rather than dropping it, on the grounds that the bytes which
did arrive defined something. The linter reports it and the repair leaves it
alone.

**Acceptance criteria**
- Each repair class has a positive test (repair applied to a suspect byte)
  and a negative test (identical value on a parity-clean byte left alone,
  interpreter's existing behaviour preserved).
- Repairs compose: a stream with several damage classes repairs all of them
  in one pass.

### Task 2.4 — Diagnostics integration

Extend `NabtsDecodeDiagnostics`
(`orc/plugins/stages/common/vbi-services/nabts_page.h`) with the repair
counters (bytes repaired, PDIs resynchronised, values range-repaired,
suspect bytes left undecided), populated when the pass ran, zero otherwise.

**Acceptance criteria**
- Counters flow through `NabtsPageSnapshot` untouched by the interpreter.
- Existing diagnostics fields and their meanings are unchanged.

---

## Phase 3 — Reader-facing toggle and plumbing

### Task 3.1 — Catalogue browser toggle contract

The browser schema currently offers one re-asking control: the view-option
dropdown (`CatalogueSchema::view_options`, round-tripped as an opaque id
through `ICatalogueResults::catalogue(view_option)`). Extend the contract so
a schema can also offer re-asking **toggles**: label, tooltip, opaque id and
default state, with the active toggle ids round-tripped to the stage the same
way the option id is. The host's generic browser renders them as checkboxes
beside the view dropdown; a toggle change re-asks for the catalogue exactly
as an option change does.

This is an SDK stage-tooling contract change, so it needs an ABI record: the
new virtual takes a vtable slot and every plugin implementing the interface
must be rebuilt. Check `orc/sdk/abi_history.yaml` before bumping — where the
branch has already bumped `kStagePluginHostAbiVersion` for another change to
the same header, this belongs folded into that entry rather than added as a
new one, since one bump covers everything unreleased. Regenerate the docs
blocks, update the `host_abi_version` note in
`docs/technical/plugin-architecture.md`, and extend the stage-tools section of
`docs/technical/plugin-sdk.md` — the gates in [AGENTS.md](../AGENTS.md) §9
enforce the first two. The browser contract id stays as it is: the ABI version
is what actually gates loading, and the id did not move for the previous
vtable addition to this header either.

Rejected alternative (recorded so it is not re-litigated): doubling the
receiver dropdown into a raw/repaired cross-product needs no SDK change,
but eight entries conflate two independent axes and every future toggle
doubles the list again.

**Acceptance criteria**
- A stage declaring no toggles gets today's browser unchanged; the default
  `catalogue()` overloads keep older stages source-compatible.
- Toggle state round-trips opaquely; an unrecognised id is ignored the way
  an unrecognised view option is.
- GUI browser test (tier 3, offscreen) covers rendering and re-ask on
  toggle; SDK docs and ABI history updated in the same PR.

### Task 3.2 — The "Syntax repair" toggle in the records viewer

`build_nabts_catalogue()` gains the repair switch; the schema offers one
toggle, labelled **Syntax repair** — "lint" is the right word inside the
codebase and jargon to a reader browsing teletext pages, while "error
correction" is the transport layer's — default **on** (repairs only engage on bytes independently known
damaged, every intervention is counted, and off is one click away for the
as-transmitted reading). `NabtsSinkStage::catalogue_for()` keys its cache on
(receiver mode, repair state) as it keys on mode today. Support records
and More chains are repaired consistently, so state carried between records
comes from the same reading of the stream.

**Acceptance criteria**
- With the toggle off, interpretation output is byte-identical to today
  (regression test over existing fixtures).
- With it on, a fixture with injected damage renders the repaired page; the
  raw record data on `NabtsCataloguedRecord` remains as voted/transmitted.
- Page payloads surface the repair counters (the diagnostics of Task 2.4) so
  a reader can see how much of the page was guessed at.

### Task 3.3 — Caption export policy, report and documentation

The SRT export always interprets with repair applied (see "Where the
switches live"). Add a lint findings/repair section to the stage report,
aggregated over every presentation record the run catalogued by a
`nabts_lint_records()` sweep — the repair itself happens at interpretation
time, which the trigger path does not otherwise do, so the totals are computed
once beside the catalogue rather than scraped from a path that may not run.
Then update
`orc/plugins/stages/nabts_sink/instructions.md`: the NABTS Records tool
section documents the toggle, and "What a marginal tape looks like" gains a
paragraph on what lint repair can and cannot recover.

**Acceptance criteria**
- Caption cues from a damaged fixture reflect the repaired reading; the
  report states the repair totals.
- Report renders totals when findings exist and stays quiet otherwise.
- `instructions.md` updated in the same PR ([AGENTS.md](../AGENTS.md) §9.1).

---

## Phase 4 — Lint-assisted voting (second-order correction)

The vote in `nabts_vote_record()` decides each byte position
independently. The linter adds a cross-byte signal: among parity-clean
candidates of equal standing, the one that keeps the stream grammatical is
likelier right. Unlike the interpretation-time repair, this changes the voted
record data itself — what `.rec` exports and what every reading starts from —
so it is recovery-side and carries a stage parameter, not a viewer toggle.

### Task 4.1 — Retain damage evidence past the vote

Carry what the recovery knew about each byte onto `NabtsCataloguedRecord`,
so Phase 2's suspect map is better than parity-only for voted records.

Two masks rather than one suspect vector, because
`NaplpsSuspectMap::from_record()` already takes exactly these two and because
they are different kinds of statement: `data_present` says whether the byte
arrived at all — the one thing no repair may reason from — and
`data_confidence` says how sure the recovery ended up being of a byte that
did. The second is where the vote's own doubt is recorded: the mean
confidence of the copies that voted for the winning value, reduced by the
share of the weight that voted against it, and **zero** where the weights
left the position level, because a byte settled by a tie-break is a byte
nothing actually chose.

**Acceptance criteria**
- Vectors are index-aligned with `data`, empty when a single intact copy was
  kept.
- Memory bound documented against `kMaxCataloguedRecords` ×
  `kNabtsMaxGroupBytes` (under 4 MB, against the 31 MB the retained copies
  themselves already cost).

### Task 4.2 — Lint tie-break in the vote, under a stage parameter

Where a position's leading candidates are within `kNabtsVoteTiePercent` of
one another in weight — inside the winner's own parity class, since weight
behind a byte the parity gate has ruled out is not weight against a clean
one — put the position to the grammar instead of to recency. The existing
rules stay primary: this only re-decides what the current vote settles by
recency.

Graded by **re-linting the whole record** with each candidate in place, not
by a lookahead window: the record is the unit the grammar works on, and a
window would have to guess where a candidate's consequences stop. The bound
is on how many positions are asked about (`kNabtsMaxAdjudicatedPositions`,
32) and how many candidates each may offer (`kNabtsMaxVoteCandidates`, 4),
which caps the work at a fixed number of lint passes per record; the vote
itself stays linear in record length and copy count, and nothing beyond the
first pass runs at all for a record with no level positions. Grading is
`NaplpsLintGrade` — the same errors-then-warnings comparison the repair pass
uses, moved into `naplps_lint.h` so the two cannot disagree about which
reading is better — and a candidate is taken only where it uniquely attains
the best grade *and* beats leaving the position alone, which is Phase 2's
discipline for the same reason.

Two orderings matter. Support Records are voted first per Data Channel and
read into a `NaplpsLinter` the channel's other records are then graded
against, exactly as `nabts_interpret_records()` presents them first — a page
invoking a macro §5.2.7.9 defined there is otherwise graded as invoking an
undefined one. A Support Record is itself graded from a general reset,
because that is how §8.5 has it presented. And the tie-break is asked only
about presentation records: an application record's data is function
descriptors (§7.2.2), and grading it as NAPLPS would be reading it in a
language it is not written in.

Governed by a BOOL stage parameter `grammar_assisted_vote` (default **true**;
named for what it does rather than for the pass it borrows, so it is not read
as the render-side linter having leaked into the recovery)
in `NabtsSinkStage::get_parameter_descriptors()`, carried through
`parse_config()` into `NabtsSinkOptions` and on to
`NabtsRecordCatalogue::records()`. The library default is **off** and the
stage's is on: a caller that has not asked for the grammar gets the vote it
always had. The report gains a `Record vote:` line saying how many positions
were level and how many the grammar settled.

**Acceptance criteria**
- Existing vote unit tests pass unchanged for all non-tie cases, and for all
  cases with the parameter off.
- A constructed tie where one candidate breaks PDI structure and the other
  does not resolves to the grammatical one. The two candidates must differ
  by **two** bits, or the parity gate settles the position before the
  grammar is reached and the test proves nothing.
- Bound documented; vote complexity remains linear in record length for the
  non-tie path.
- Parameter round-trip and descriptor/default parity covered in
  `nabts_sink_stage_test.cpp`.

### Task 4.3 — Functional validation

Functional tests (label `functional`) in
`orc-tests/core/functional/stages/nabts_sink/nabts_sink_pipeline_test.cpp`,
over the NBC EP transfer — the marginal one, where 7 % of record bytes fail
parity and the grammar has something to work with:

- repair: the lint sweep's own before/after totals, asserting errors and
  faulted records only come down and that the evidence reached the repair
  pass at all, plus the viewer toggle exercised end to end on real data
  (same records either way, different reading).
- voting: the same window recovered twice, with `grammar_assisted_vote` on
  and off. Which positions are level is a property of the copies, so both
  runs must report the same contested count; only the off-run may report
  nothing adjudicated. Skips rather than passing vacuously where the window
  left no position level.

**Acceptance criteria**
- Tests skip (not fail) without reference media.
- Not added to the unit-test CI lane.
