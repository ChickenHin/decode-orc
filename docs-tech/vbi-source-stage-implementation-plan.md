# VBI capture source stage — implementation plan

Phased implementation plan for the design in
[vbi-source-stage-design.md](./vbi-source-stage-design.md). Section references
(§n.n) below refer to that document. Normative output targets are defined by the
[CVBS File Format Specification](./cvbs-file-format-specification/README.md).

## Scope

**The deliverable is a source stage, nothing more.** The single requirement of
this implementation is to get the different raw VBI capture formats (§3.2) into
decode-orc — that is, presented internally as CVBS-domain frames that the rest
of the pipeline already understands. Everything downstream of the stage
boundary (teletext slicing and decoding, file export, archival) is existing
decode-orc functionality and appears in this plan only as validation of the
stage's output, never as a deliverable. In particular:

- The existing teletext decoder stages are the *acceptance oracle* (§1: the
  decoder, pointed at the output, produces the same packets as a native
  decode) — no decoder work is in scope.
- `.composite`/`.meta` file export already exists via `cvbs_sink`; a correct
  in-memory CVBS representation makes it work for free. The VBI-specific
  provenance sidecar (§7.2) is deferred beyond this plan.

## Architecture mapping

The design describes a file-to-file transform. In decode-orc it is a source
stage producing the internal CVBS representation; these decisions bind the
phases below:

- **Stage:** a new source stage plugin `vbi_source` in
  `orc/plugins/stages/vbi_source/`, `NodeType::SOURCE`, zero inputs, one output,
  modelled on `orc/plugins/stages/cvbs_source/`.
- **Output artifact:** a `VideoFrameRepresentation` in the normalised
  `CVBS_U10_4FSC` 10-bit domain — the same in-memory contract `cvbs_source`
  produces — so the existing teletext decoder stages consume it unchanged.
- **Frame synthesis is lazy (per requested frame).** The 21.6× full-frame
  expansion (§5.7) never lands on disk unless the user exports; therefore the
  in-DAG path is always logically `full-frame`, and the `vbi-only` file
  transport is deferred (recorded in §9 open questions, not implemented here).
- **No file output in this stage.** Users who want a `.composite` + `.meta`
  file connect the existing `cvbs_sink`; the stage's only product is the
  in-memory representation.
- **FLAC transport** uses libavformat/libavcodec (already project dependencies)
  rather than adding a libFLAC dependency.
- All external I/O goes behind an injected dependency interface
  (`IVBISourceStageDeps` pattern, as in `ICVBSSourceStageDeps`) so every unit
  test is fully mocked per `TESTING.md` and AGENTS.md §4.2.

**First milestone: FLAC-wrapped bt8x8 PAL carrying WST, end to end.**
Phases 1–6 deliver exactly one working path — `bt8x8-pal` preset, FLAC
transport, PAL geometry, WST timing — validated against the reference sample
`test-data/teletext/bt8x8 sample/0002.vbi.flac` (8-bit mono FLAC,
blocksize 65535, decoded size 24,117,706,752 bytes = exactly 368,007 ×
65,536-byte bt8x8 PAL frames, ≈4 h of capture). Every module in those phases is
still written against the generic container descriptor (§3.1) so that Phase 7
adds the remaining formats as data and narrow extensions, not rework. NTSC,
NABTS, family B (u16 TBC) inputs, and the non-bt8x8 card formats are all
Phase 7. `teletext.system` is carried on the artifact from day one; anything
other than WST fails with a clear error until Phase 7 (§4).

---

## Phase 1 — Container model, FLAC transport, bt8x8 reading

Goal: parse a FLAC-wrapped bt8x8 PAL capture into indexed line records with no
signal processing. Everything here is unit-testable with synthetic in-memory
byte streams; the real sample is touched only by `functional`-labelled tests.

### Task 1.1 — Source format descriptor and the `bt8x8-pal` preset

Implement the generic container descriptor (§3.1: `sample_rate`, `line_length`,
`valid_samples`, `sample_format`, `field_lines`, `field_range`,
`frame_trailer_bytes`, `capture_offset`, `first_field`, `tv_system`,
`tt_system`) with named presets as pure data expanding to the descriptor — one
code path. Ship only `bt8x8-pal` (35 468 950 Hz, 2048-sample records with 2044
valid, u8, 16 records/field, 4-byte frame trailer, §3.2/§3.3) plus the
`custom` expansion; the remaining §3.2 presets are Phase 7 data entries.

**Acceptance criteria**
- `bt8x8-pal` expands to exactly the §3.2 values including `valid_samples`
  2044 and `frame_trailer_bytes` 4.
- Adding a preset requires only a data entry, no code changes (demonstrated in
  Phase 7).
- Unit tests assert the expansion field-by-field; labelled `unit` + `sources`.

### Task 1.2 — Line-record reader and frame indexing

Implement the reader that turns a byte stream into
`(frame, field, record) → line record` tuples: fixed-stride record parsing,
`field_range` selection, padding exclusion via `valid_samples`, u8 sample
decode, and extraction of the bt8x8 per-frame counter from the frame trailer
(§3.3, §6.3). The byte source is an interface so tests never touch the
filesystem. The reader is generic over the descriptor; only the u8 decode path
needs to exist yet.

**Acceptance criteria**
- Synthetic-stream unit tests cover bt8x8 PAL record parsing, the 4-byte pad /
  frame-counter overlap, and `field_range` selection.
- Frame counter values are surfaced per frame in little-endian interpretation
  (the §9 endianness question is closed out in Phase 8).
- Records are never silently dropped or reordered (§1.1 provenance principle);
  a short final frame is a reported error, not a truncation.

### Task 1.3 — FLAC transport

Implement transparent FLAC unwrapping via libavformat: detect the `fLaC` magic,
stream-decode to the identical raw byte sequence, ignore the declared FLAC
sample rate entirely, take only `bps` as a sample-format hint (§3.3). Build a
seek-point index on first open so per-frame access and preview do not
repeatedly seek the FLAC stream. Streaming, not materialisation — the reference
sample alone is 10 GB compressed / 24 GB raw.

**Acceptance criteria**
- The parser from Task 1.2 receives bytes identical to the raw file, verified
  in a `functional`-labelled test against a bounded prefix of
  `test-data/teletext/bt8x8 sample/0002.vbi.flac` (first N frames, not the
  whole file; the harness must handle the space in the directory name).
- No timing value is ever derived from the FLAC header; unit test asserts the
  container descriptor is unaffected by the wrapper.
- Random access to frame *n* after index build does not re-decode from the
  file head.

### Task 1.4 — Configuration validation

Implement the fail-fast checks that need no signal processing: decoded stream
size is an exact multiple of the configured frame size (§8 — the reference
sample must factorise to exactly 368,007 frames), `field_range` length does not
exceed the standard's teletext line list (§5.1 — error, never truncate),
`capture_offset` must be 0 for TBC-derived formats (§5.3.3 — the rule is
enforced now even though those formats arrive in Phase 7), sample-format /
`valid_samples` consistency.

**Acceptance criteria**
- Each violation produces a distinct, actionable error message naming the
  offending parameter.
- Unit tests cover every rejection path; labelled `unit` + `sources`.

---

## Phase 2 — PAL geometry and u8 level mapping

Goal: correct placement and amplitude for the PAL/WST path, independent of any
real data. All numeric tables come straight from the design and the CVBS spec.

### Task 2.1 — PAL non-orthogonal line geometry

Implement `line_start(k) = ceil(k × 1135.0064)` and per-line `phase(k)` for
PAL (§2.3), with the orthogonal constant-stride case behind the same interface
(data for it lands in Phase 7). This module is the single source of truth for
line offsets — no other code may compute one.

**Acceptance criteria**
- PAL frame totals exactly 709,379 samples; 621 lines of 1135 and 4 of 1136 at
  frame lines 0, 156, 312, 468.
- `phase(k)` for frame lines 6, 21, 319, 334 matches the §2.3 table.
- All placement code in later phases indexes through this module (enforced by
  the Phase 5 assembled-frame-size runtime check plus review).

### Task 2.2 — Vertical line mapping for WST

Implement the record-to-frame-line lookup for PAL/WST: broadcast frame lines
7–22 and 320–335, per field, as an explicit table, not offset+stride (§5.1).
The NTSC/NABTS table is a Phase 7 data addition to the same structure.

**Acceptance criteria**
- Table lookup verified against §5.1 for both fields.
- Sources carrying fewer lines than the standard map contiguously from
  `field_range.start`; excess records were already rejected in Task 1.4.

### Task 2.3 — Level mapper for u8 card captures (family A)

Per-line level estimation (§5.4): logic-0 from the pre-CRI quiet region
(samples 0–117 for bt8x8 PAL), logic-1 from the larger of CRI peaks and the
framing-code `111` run, with the CRI/FRC amplitude ratio recorded as a
per-line bandwidth quality metric. Modes `per-line`, `rolling` (median over N
lines, per-line correction only on significant deviation), `fixed`. Map to the
§2.2 derived PAL levels (black → 256, logic 1 → 644).

**Acceptance criteria**
- On a synthetic clean line, mapped logic levels land within ±2 counts of
  256/644.
- On a synthetic VHS-blurred line (CRI at ~5 % amplitude, §5.3.6), the FRC path
  wins and output amplitude is full-scale — the 18× under-scale failure mode
  has a regression test.
- `rolling` mode does not propagate single-line gain noise (test with one
  outlier line in a run).

### Task 2.4 — Output clamping

Clamp all written samples to [4, 1019] (§2.2) as the final step of the sample
path, after resampling.

**Acceptance criteria**
- Values 0–3 and 1020–1023 never appear in any output (asserted by every
  Phase 5 frame test).
- A synthetic overshooting edge is clamped, not wrapped or rescaled.

---

## Phase 3 — 2:1 resampling and horizontal placement

Goal: land bt8x8's 8·fsc samples on the 4·fsc grid at the WST time offset with
sub-sample accuracy. The arbitrary-ratio polyphase case (saa7131) and the 1:1
passthrough (TBC-derived) are Phase 7.

### Task 3.1 — Half-band 2:1 decimator

Proper low-pass-then-decimate for 8·fsc → 4·fsc, not sample-dropping — WST at
6.9375 MHz sits close to the 8.87 MHz output Nyquist (§5.5).

**Acceptance criteria**
- A 6.9375 MHz test tone survives with < 0.5 dB loss; energy above output
  Nyquist is attenuated ≥ 60 dB.
- Filter is linear-phase; group delay is compensated so placement is unbiased.

### Task 3.2 — Fractional-delay placement

Fold the total fractional delay — WST nominal `t_offset` 10.3 µs (libzvbi
convention, §5.2), the calibrated `capture_offset` (Phase 4), and PAL
`phase(k)` from Task 2.1 — into the decimator's filter phase as one pass
(§5.2, §5.5). Design the delay interface so Phase 7's polyphase resampler
plugs into the same seam.

**Acceptance criteria**
- Data placed on a synthetic line lands at `t_offset × fs_out − phase(k)`
  within ±0.1 sample, verified per PAL teletext line including the four long
  lines.
- No signal conditioning beyond linear resampling — the §5.3.6 "do not clean
  anything up" rule; test asserts a blurred input waveform is preserved to
  within filter tolerance.

---

## Phase 4 — WST CRI/FRC lock and capture-offset calibration

Goal: `capture_offset: auto` (§5.3.4) for card captures — bt8x8's offset is
explicitly unreliable hardware folklore (~244 samples, §5.3.3) and must be
calibrated, not configured.

### Task 4.1 — CRI+FRC template generation

Generate the combined 24-bit template (16-bit clock run-in + 8-bit framing
code, WST `0xAAAAE4`, §5.3.2) as filtered NRZ at the configured bit rate in
source sample coordinates, parameterised by bit rate and sample rate only.
Leave a seam for a measured template (vhs-teletext `observed_crifc` style) for
VHS-sourced captures (§5.3.6) and for the NABTS pattern in Phase 7.

**Acceptance criteria**
- Template autocorrelation shows no 2-bit-period ambiguity (sidelobe well
  below main peak); unit test asserts the FRC breaks the alternation.
- The same generator produces a correct template at both 8·fsc (source-domain
  calibration) and 4·fsc.

### Task 4.2 — Correlation search with sub-sample refinement

Correlate normalised records against the template, threshold to reject
no-teletext lines, refine accepted peaks by parabolic interpolation (§5.3.4
steps 2–4).

**Acceptance criteria**
- On synthetic clean lines, recovered position is within ±0.1 sample of ground
  truth; on σ = 0.8-bit Gaussian-blurred lines, within ±1 sample (§5.3.6).
- Blank lines and noise-only lines fall below threshold; acceptance rate is
  reported.

### Task 4.3 — Global offset estimation and health reporting

Sample records spread across the file (not the head), take median/mode of
accepted positions, derive `capture_offset = t_offset − cri_position /
sample_rate`, and report the fitted value with its spread against per-format
thresholds (§5.3.4, §5.3.6). Detect monotonic drift and compute the implied
sample-rate correction `configured × (1 + d / (N × line_length))`. Apply the
offset globally, never per line.

**Acceptance criteria**
- Spread classification matches the §5.3.4 table; thresholds are a property of
  the format descriptor, not a global constant.
- Injected sample-rate error is detected and the suggested correction recovers
  the true rate within 1 ppm.
- Drift or excess spread stops the run with a clear diagnostic; it does not
  proceed with a bad fit.
- Records without teletext receive the same global offset and become ordinary
  blanking.
- `functional` test: calibration on the reference sample converges, and the
  fitted offset and its spread are recorded in the design document as measured
  values (§9). The sample measures 262.1 samples with 4.2 samples of spread,
  against the folkloric ~244 (§5.3.3); the fitted offset must be stable to
  under a sample across different samples of the file, and the lock must be at
  the correct bit alignment rather than a run-in period away from it.

### Task 4.4 — Independent cross-checks

Implement the cheap corroborations from §5.3.5 as warnings: burst-remnant
phase (bt8x8 PAL captures the burst tail in samples 0–34 of every record),
other VBI services in the captured range (VPS line 16, CC line 22 via the §5.2
service table), and data-end position vs `t_offset + 51.892 µs` for bit-rate
validation.

**Acceptance criteria**
- Each check runs only when applicable to the configured format and emits a
  warning (never an error) on disagreement, naming the two conflicting
  estimates.
- Unit tests exercise agree and disagree paths with synthetic lines.

---

## Phase 5 — PAL frame synthesis

Goal: structurally valid PAL CVBS frames around the recovered data lines.

### Task 5.1 — Line synthesiser

Per-line assembly per the §5.6 region table: sync (raised-cosine ~250 ns
edges, never a step), back porch, data region (resampled source or blanking),
front porch, at the §2.2 levels. Non-teletext lines become standards-correct
blank lines.

**Acceptance criteria**
- Region boundaries match §5.6 at 4·fsc PAL sample positions.
- Sync edges show no step transition (max per-sample delta bounded); no
  ringing injected into the data region.
- Every sample in [4, 1019].

### Task 5.2 — Colour burst synthesis

`synthesise_burst: true` default (§5.6): PAL ±135° swinging burst with the
4-frame Sc/H progression. Burst omission supported; burst preservation
(option 3) explicitly not implemented. NTSC burst is Phase 7.

**Acceptance criteria**
- Burst phase progression is coherent across frame boundaries over an 8-frame
  test sequence (verified analytically on the synthesised samples).
- Burst amplitude and window match the CVBS spec preset; disabling burst
  produces blanking in the burst window.

### Task 5.3 — Vertical interval synthesis

PAL equalising and broad pulses per the §5.6 SysParams-derived table,
including the first-field half-line pattern (1, 0.5).

**Acceptance criteria**
- Pulse widths, counts and positions match the table.
- Field 1 and field 2 sequences differ correctly; the half-line pattern
  identifies the field parity.

### Task 5.4 — Frame assembler with normative size check

Assemble both fields into stored frames through the Task 2.1 geometry module
only. Assert the assembled PAL frame is exactly 709,379 samples (§2.1) — a
runtime check in the stage, not only a test; this catches the constant-1135
mistake immediately (§8).

**Acceptance criteria**
- Size assertion fires as a stage error, not a crash, if geometry is violated.
- A 10-frame synthetic run produces frames at identical sizes with no
  cumulative drift.

### Task 5.5 — Field order, dropped frames and signal state

Implement `ordering.first_field` (default field-1-first, §6.1), bt8x8 counter
continuity tracking with `drops: preserve | pad` (§6.3), and automatic
`signal_state_preset` derivation: `STANDARD_TBC_LOCKED` only when burst is
synthesised and no drops were detected (or `pad` preserved the timeline);
otherwise `STANDARD_TBC_UNLOCKED` — a consequence of the run, never a user
flag (§2.4).

**Acceptance criteria**
- Injected counter gap with `preserve` downgrades to `UNLOCKED` and records
  every discontinuity; with `pad`, synthesised blank frames keep output frame
  *n* aligned with source frame *n* and the PAL burst sequence stays coherent
  across the pad.
- Padding frames are flagged as such in the per-frame provenance data
  (consumed by Phase 8).
- Field-order choice is recorded for `capture_notes`.

---

## Phase 6 — Stage plugin integration and first-milestone validation

Goal: a registered, parameterised, documented `vbi_source` stage passing all
architecture gates, decoding the reference sample end to end.

### Task 6.1 — Stage class and parameter surface

`VBISourceStage` implementing `DAGStage` + `ParameterizedStage` with the §4
configuration mapped to stage parameters (input path, format preset or custom
container fields, output preset, `synthesise_burst`, teletext system and
lines, `capture_offset` auto/explicit, levels mode, `first_field`, drops
policy), deps-injection constructor, and lazy per-frame execution returning
the CVBS-domain `VideoFrameRepresentation`.

**Acceptance criteria**
- Named presets and `custom` share one code path (§4); parameter defaults
  match the design's defaults.
- `teletext.system` values other than WST fail at configuration with an error
  stating only WST is currently implemented (§4); the system is carried on the
  output artifact for downstream stages.
- Frames are synthesised on demand and cached consistently with `cvbs_source`
  conventions; no retained raw-pointer frame handoff (VFR contract).
- Unit suite under `orc-tests/core/unit/stages/vbi_source/` with mocked deps;
  labelled `unit` + `sources`.

### Task 6.2 — Stage self-documentation

`instructions.md` in the plugin directory covering what-it-does, when-to-use,
every parameter, and the calibration/diagnostic behaviour; wired via
`ORC_STAGE_INSTRUCTIONS_MD` (AGENTS.md §9.1).

**Acceptance criteria**
- All parameters from Task 6.1 documented with ranges and defaults, including
  which presets are implemented (bt8x8-pal) vs pending.
- The LOCKED/UNLOCKED automatic downgrade and the vbi-only deferral are
  explained to the user.

### Task 6.3 — Contract and gate coverage

Registry, node discovery, parameter/default parity, and project-to-DAG wiring
coverage per the shared stage contract suites; SDK-only compliance.

**Acceptance criteria**
- `ctest -L sdk`, `ctest -L contracts`, `ctest -L sources`, and
  `ctest -R MVPArchitectureCheck` all pass.
- The plugin includes only public SDK headers (AGENTS.md §9).

### Task 6.4 — Preview capability

Implement `IStagePreviewCapability` so the existing preview dialogue renders
synthesised frames — this is the §8 "render a few frames' VBI region" check
for free.

**Acceptance criteria**
- Preview shows a structurally valid frame (sync, burst if enabled, data
  lines) for a mocked source in the stage unit tests' capability probe; live
  GUI sanity checked manually against the reference sample.

### Task 6.5 — First-milestone end-to-end validation

Validate the stage's output using the existing downstream pipeline as the
oracle — the design's success criterion (§1) on the reference sample:
`test-data/teletext/bt8x8 sample/0002.vbi.flac` → `vbi_source` → the existing
teletext decode produces the same packets as a reference decode (vhs-teletext
on the same file). This exercises no new downstream code; it proves the
ingested CVBS representation is correct. Automated tests use a bounded prefix;
a full-file decode is a manual milestone check, not CI.

**Acceptance criteria**
- Packet-for-packet parity with the vhs-teletext reference on the tested
  prefix; test labelled `functional`, excluded from the unit lane.
- `vbi_source → cvbs_sink` export of a few frames opens cleanly in
  `ld-analyse` with visible sync, burst and teletext lines (manual check,
  recorded in the PR).

---

## Phase 7 — Remaining formats and systems

Goal: extend the working PAL/WST path to the rest of the §3.2 table. Each task
leans on the seams left in Phases 1–5; none should require touching the bt8x8
path.

### Task 7.1 — Remaining named presets and readers

Add `bt8x8-ntsc` (1600 valid samples, 448-byte pad, §3.3), `cx88-pal`
(18 records/field, records 0 and 17 skipped), `saa7131-pal`, `tbc-pal`,
`tbc-vbi-pal`, `tbc-vbi-ntsc` as preset data, plus the u16le/s16le decode path
in the Task 1.2 reader.

**Acceptance criteria**
- Each preset expands to the §3.2 table values; synthetic-stream unit tests
  cover the NTSC padding exclusion, cx88 record skip, and u16le decode.
- No changes to the bt8x8-pal code path (presets are data, §4).

### Task 7.2 — Level mapper for u16 TBC captures (family B)

Two-point affine map through blanking and white from the ld-decode 16-bit
domain to the 10-bit CVBS domain (§5.4), reading `white16bIre` /
`black16bIre` / `blanking16bIre` from the `.tbc.json` or SQLite sidecar via
the deps interface, falling back to the §5.4 table with a logged notice when
the sidecar is absent (it will be, for cropped VBI-only files).

**Acceptance criteria**
- Mapping is affine through the shared physical levels, not a bit-shift; unit
  test asserts PAL blanking 16384 → 256 and white 54016 → 844 exactly.
- Sidecar values override the defaults; fallback path emits the notice.

### Task 7.3 — Polyphase resampler and 1:1 passthrough

Polyphase arbitrary-ratio resampler for saa7131 (17734475:27000000, exact
rational) plugging into the Task 3.2 fractional-delay seam, and the
special-cased straight-copy path for TBC-derived 4·fsc inputs (§5.5).

**Acceptance criteria**
- No drift over a frame at the exact rational ratio; placement accuracy
  matches the Task 3.2 criteria.
- A `tbc-vbi` input round-trips bit-exactly into the output data region.
- Family B formats assert `capture_offset = 0` and skip calibration entirely
  (§5.3.3 — already enforced by Task 1.4).

### Task 7.4 — NTSC geometry, synthesis and NABTS timing

NTSC orthogonal geometry data (910 samples/line), NABTS vertical mapping
(broadcast lines 10–21 / 273–284), NABTS CRI+FRC template (`0xAAAAE7`) and
timing (`t_offset` 10.48 µs), NTSC levels (240/632), NTSC line/burst/vertical
interval synthesis (2-frame A/B burst sequence, (0.5, 1) half-line pattern)
(§2.2, §5.1, §5.2, §5.6).

**Acceptance criteria**
- NTSC frame totals exactly 477,750 samples.
- All Phase 2–5 test suites gain NTSC/NABTS cases and pass.
- Whether NABTS decode is now permitted end-to-end depends solely on the
  downstream slicer's capability declaration; if no NABTS slicer exists yet,
  the Task 6.1 clear-error path still fires (§4).

---

## Phase 8 — Diagnostics, provenance and close-out

Goal: the user can trust a configuration before decoding, and every packet is
traceable to a source byte range.

### Task 8.1 — Pre-flight diagnostics

Run the §8 check table over the first few hundred frames at configuration
time, surfaced as stage observations: frame-size multiple, CRI peak strength,
CRI position spread, CRI detection fraction, level stability, counter
continuity, output frame size. Expose as a dry-run stage tool that prints the
report and stops. For non-bt8x8 sources, state explicitly that no drop signal
exists rather than implying continuity (§6.3).

**Acceptance criteria**
- Each check maps to the §8 "what a failure means" interpretation in its
  message.
- Dry-run touches only the sampled prefix, not the whole capture.

### Task 8.2 — Format auto-detection suggestion tool

The §8.1 ladder (FLAC magic → size factorisation → u8/u16 histogram →
line-length autocorrelation → CRI frequency under candidate rates → filename
hints) as a stage tool producing ranked, confidence-scored configuration
suggestions — never applied silently (§1.1).

**Acceptance criteria**
- Correctly ranks the true format first for synthetic samples of each §3.2
  preset, and identifies the reference sample as `bt8x8-pal` (`functional`);
  the CRI-frequency test discriminates PAL-rate from NTSC-rate bt8x8.
- Output is a suggested configuration the user must accept; nothing is
  auto-applied.

### Task 8.3 — Provenance on the output artifact

Attach the synthesis provenance to the stage's output representation: source
format, calibrated offset + method + spread, teletext system, field-order
method, burst flag, and the per-frame source frame index / counter / padding
flag (§7.2's content, carried in memory). Because the line mapping is a pure
function of the configuration (§1.1), this is all that is needed for a packet
at (frame, field, line) to resolve to a source byte range. Include a
§7.3-style human-readable summary string so any consumer (preview, logs, a
future export path) can display what was synthesised. Writing the
`.vbisource.meta` sidecar and `capture_notes` at export time is a `cvbs_sink`
concern and is out of scope for this plan (see Scope).

**Acceptance criteria**
- Per-frame provenance rows exist for every emitted frame including pad
  frames; unit test resolves a (frame, field, line) tuple back to the correct
  source byte range through the recorded mapping.
- The summary string contains the elements of the §7.3 example (source file,
  format, synthesised regions, offset + σ, field-order assumption, drop
  summary).
- No sink or export code is modified.

### Task 8.4 — Close out design verification items

Work through the §9 **[verify]** list against real captures: bt8x8 counter
endianness (measurable on the reference sample), cx88/saa7131 offsets, NABTS
amplitude, 10.198 vs 10.3 µs WST nominal (measurable from the Phase 4
calibration output on the reference sample). Update the design document with
measured values and adjust preset data (Task 1.1/7.1) where measurements
disagree.

**Acceptance criteria**
- Every §9 item is either resolved with a measurement recorded in the design
  doc, or explicitly re-recorded as open with what blocks it.
- Preset data changes ship with updated unit test expectations.
