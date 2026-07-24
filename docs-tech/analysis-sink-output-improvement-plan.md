# Analysis Sink Output Improvement Plan

Resolves:

- [#213 — Analysis sink CSV files are confusing](https://github.com/simoninns/decode-orc/issues/213)
- [#214 — Dropout report](https://github.com/simoninns/decode-orc/issues/214)
- [#216 — Dropout analysis must include Dropout Map stage modifications](https://github.com/simoninns/decode-orc/issues/216)

## Problem Statement

The three analysis sinks (`dropout_analysis_sink`, `snr_analysis_sink`,
`burst_level_analysis_sink`) bucket their measurements into ~1000 display
points so the GUI graphs render quickly, then write **the same bucketed data**
to CSV. This conflates two distinct outputs — an interactive overview graph and
a dataset for detailed offline analysis — and produces CSVs that are hard to
interpret:

- **Inconsistent bucketing semantics.** The dropout sink analyses every frame,
  then **sums** values into bins labelled with the *last* frame of the bin
  (`orc/plugins/stages/dropout_analysis_sink/dropout_analysis_sink_deps.cpp:151-186`).
  The SNR and burst sinks instead **sparse-sample** one frame per bucket up
  front and label buckets with the *centre* frame
  (`orc/plugins/stages/snr_analysis_sink/snr_analysis_sink_deps.cpp:66-150`,
  `orc/plugins/stages/burst_level_analysis_sink/burst_level_analysis_sink_deps.cpp:63-71`).
  A `frame_number` column therefore means three different things across the
  three CSVs, and dropout values are per-bin sums whose bin width depends on
  recording length.
- **Silently dropped rows.** Rows with `has_data == false` are skipped
  (`dropout_analysis_sink_deps.cpp:209-215`), so a dropout CSV has gaps that
  are indistinguishable from "no dropouts" vs "not analysed".
- **Unlabelled units and missing-value handling.** SNR CSV emits the literal
  string `nan` for absent metrics (`snr_analysis_sink_deps.cpp:201-203`);
  column headers carry no bucket-width or sampling information.
- **No per-dropout detail exists anywhere** (issue #214): the dropout sink
  aggregates to per-frame totals before any output is produced, so the
  location of dropouts within a frame is lost.
- **Dropout analysis can miss Dropout Map edits** (issue #216): the sink reads
  `get_dropout_hints()` from its DAG input
  (`dropout_analysis_sink_deps.cpp:80`); `dropout_map` edits only exist inside
  the `DropoutMappedFrameRepresentation` wrapper
  (`orc/plugins/stages/dropout_map/dropout_map_stage.cpp:49-113`), so a sink
  not wired downstream of the `dropout_map` stage — or holding stale cached
  results after a map edit — reports the original sidecar hints only.

## Target Architecture

Separate **measurement** from **presentation**:

1. Sinks capture a canonical per-frame dataset — one record per *actually
   analysed* frame, carrying that frame's true frame number. No bucket
   arithmetic occurs during capture. Analysis density is controlled by an
   explicit sampling parameter instead of an implicit display-bucket count.
2. A single shared decimation utility reduces a per-frame series to ≤N display
   points for graphing, with documented aggregate semantics (sum for counts,
   mean/min/max for levels) and explicit bucket start/end frames. Only the
   graph path uses it.
3. CSV output is always written from the canonical per-frame dataset with
   self-describing headers, consistent missing-value handling, and a
   documented schema.
4. A new per-dropout detail report provides frame/line/sample-level dropout
   location data.
5. Dropout analysis is defined to operate on the dropout hints visible at the
   sink's input, which includes any upstream `dropout_map` modifications, and
   re-triggering after a map edit is verified to reflect the edit.

References: result interfaces
`orc/sdk/include/orc/stage/analysis_sink_results.h`, stats structs
`orc/sdk/include/orc/stage/common_types.h:224-283`, GUI data path
`orc/presenters/src/render_presenter.cpp:900-1000` and
`orc/gui/mainwindow_coordinator_callbacks.cpp:475-510`, dialogs
`orc/gui/dropoutanalysisdialog.cpp`, `orc/gui/snranalysisdialog.cpp`,
`orc/gui/burstlevelanalysisdialog.cpp`.

---

## Phase 1 — Canonical per-frame capture and shared display decimation

Goal: sinks produce full-resolution per-frame results; display bucketing moves
into one shared, tested utility.

### Task 1.1 — Define canonical per-frame result semantics

- Update `FrameDropoutStats`, `FrameSNRStats`, `FrameBurstLevelStats`
  (`orc/sdk/include/orc/stage/common_types.h:224-283`) so each record
  represents exactly one analysed frame: integer dropout counts/lengths,
  per-frame SNR/burst values, `frame_number` = the analysed frame (1-based).
  Remove doc-comment references to "bucket".
- Since these are public SDK headers, follow `AGENTS.md` §9: bump
  `kStagePluginApiVersion`/`kStagePluginHostAbiVersion` as required, update
  `orc/sdk/abi_history.yaml`, and regenerate docs with
  `tools/gen_abi_history_docs.sh`.

**Acceptance criteria**

- Struct fields and comments describe single-frame measurements only.
- `ctest --test-dir build -L sdk` passes (ABI/doc sync gates).

### Task 1.2 — Replace implicit bucketing with an explicit sampling parameter

- Remove up-front bucket sampling from
  `snr_analysis_sink_deps.cpp:66-150` and
  `burst_level_analysis_sink_deps.cpp:63-71`; iterate frames directly.
- Add a `frame_interval` parameter (INT, default 1 = every frame) to the SNR
  and burst sinks controlling analysis density; analysed frames are
  `first, first + N, first + 2N, …` and each record carries its true frame
  number. Document the wall-clock trade-off in each stage's `instructions.md`.
- The dropout sink already analyses every frame
  (`dropout_analysis_sink_deps.cpp:61-146`); delete its post-analysis binning
  loop (`:151-186`) so `frame_stats` is the per-frame vector.

**Acceptance criteria**

- `compute_and_analyze` in all three deps returns one record per analysed
  frame; no `TARGET_DATA_POINTS`/`kDefaultBuckets` constants remain in sink
  code.
- Cancellation and progress reporting behave as before (progress is reported
  against analysed-frame count).
- Unit tests in `orc-tests/core/unit/stages/<stage_id>/` (labels `unit`,
  `sinks`) cover: interval = 1, interval > 1, interval > total frames, and
  per-frame value correctness with mocked representations per `TESTING.md`.

### Task 1.3 — Shared display decimation utility

- Add a decimation helper in `orc/core/analysis/` (e.g.
  `analysis_series_decimator.{h,cpp}`) that reduces a per-frame series to at
  most N display points. Each output bucket carries: `frame_start`,
  `frame_end`, and per-metric aggregates (sum for counts/lengths; mean, min,
  max for level metrics), plus the count of contributing records.
- The utility is pure (no I/O, no clock) and shared by all three graph paths.

**Acceptance criteria**

- Unit tests (label `unit`) cover: series shorter than N (pass-through),
  exact multiples, remainder buckets, empty series, and aggregate correctness.
- No sink or GUI code implements its own bucketing after this task.

### Task 1.4 — Route full-resolution results through the result interfaces

- `IDropoutAnalysisResults` / `ISNRAnalysisResults` /
  `IBurstLevelAnalysisResults`
  (`orc/sdk/include/orc/stage/analysis_sink_results.h`) now expose the
  canonical per-frame `frame_stats()`; consumers needing display buckets apply
  the Task 1.3 utility.
- Update `RenderPresenter` (`orc/presenters/src/render_presenter.cpp:300-400,
  900-1000`) and coordinator callbacks
  (`orc/gui/mainwindow_coordinator_callbacks.cpp:475-510`) to decimate before
  handing points to the dialogs, preserving current graph point counts
  (≤1000).

**Acceptance criteria**

- GUI graphs render with the same visual density as before for long
  recordings; short recordings (< 1000 analysed frames) plot every point.
- `ctest --test-dir build -R MVPArchitectureCheck` passes.
- GUI tier tests updated where coordinator behaviour changed
  (`orc-tests/gui/unit/`, labels per `AGENTS.md` §4.5).

---

## Phase 2 — Clear, structured CSV output (issue #213)

Goal: CSVs are per-frame, self-describing, and documented. Depends on Phase 1.

### Task 2.1 — Redefine CSV schemas

- Write CSVs from the canonical per-frame dataset in each deps `write_csv`:
  - Dropout: `frame_number,dropout_count,dropout_length_samples` — one row per
    analysed frame **including zero-dropout frames** (a zero row is data; a
    missing row means "not analysed").
  - SNR: `frame_number,white_snr_db,black_psnr_db` — one row per analysed
    frame; absent metric = empty field (never the string `nan`).
  - Burst: `frame_number,median_burst_10bit`.
- Units belong in the header names (`_samples`, `_db`, `_10bit`), values are
  plain numbers.

**Acceptance criteria**

- Row count equals analysed-frame count; `frame_number` values are real
  analysed frames (no bucket centres/ends).
- Missing values serialise as empty fields; no `nan` text appears.
- CSV-writer unit tests via the `set_deps_override()` seam assert header,
  row-per-frame behaviour, zero rows, and empty-field handling (labels
  `unit`, `sinks`; no filesystem — writers accept an `std::ostream&` or are
  tested through a seam consistent with `TESTING.md`).

### Task 2.2 — Refactor CSV writers to be stream-based

- Change `write_csv(path, stats)` in the three deps to a pure
  `write_csv(std::ostream&, stats)` plus a thin path-opening wrapper called
  from the stage trigger (e.g.
  `dropout_analysis_sink_stage.cpp:199-207`), enabling filesystem-free unit
  tests of the formatting logic.

**Acceptance criteria**

- Formatting logic is unit-testable without temp files; existing
  `output_path`/`write_csv` parameters behave unchanged from the user's
  perspective.

### Task 2.3 — Document the schemas

- Update `instructions.md` for all three sink stages with: exact column list,
  units, one-row-per-analysed-frame semantics, the `frame_interval` parameter,
  and a short example.
- Update `docs/gui-user-guide/stages/sink-analysis-stages.md` to match.

**Acceptance criteria**

- Each column in every analysis CSV is documented with its unit and meaning;
  docs ship in the same PR as the code change (`AGENTS.md` §9.1).

---

## Phase 3 — Dropout analysis reflects Dropout Map edits (issue #216)

Goal: dropout analysis is guaranteed to report the dropout state at the sink's
input, including `dropout_map` additions/removals, with fresh results after an
edit.

### Task 3.1 — Contract test: map edits are visible to the sink

- Add unit tests (mocked representations) proving
  `DropoutAnalysisSinkStageDeps::compute_and_analyze` reports hints as
  returned by its input representation, and an integration-style contract test
  wiring `DropoutMapStage` → `dropout_analysis_sink` where the analysis
  reflects map **additions** and **removals**
  (`orc/plugins/stages/dropout_map/dropout_map_stage.cpp:49-113` is the merge
  under test; existing coverage in
  `orc-tests/core/unit/stages/dropout_map/dropout_map_stage_test.cpp`).

**Acceptance criteria**

- Test demonstrates a run added via the map appears in sink stats and a
  removed sidecar run does not (labels `unit`, `sinks` where mockable;
  `functional` only if a full pipeline is genuinely required).

### Task 3.2 — Verify hint propagation through intervening wrappers

- Audit `get_dropout_hints()` forwarding in representations that may sit
  between `dropout_map` and a sink:
  `orc/plugins/stages/frame_map/frame_map_stage.cpp:207-216`,
  `orc/plugins/stages/source_align/source_align_stage.cpp:121-126,285-288`,
  stacker recomputation (`orc/plugins/stages/stacker/stacker_stage.cpp:372,
  665,786`). Fix any wrapper that falls back to source hints instead of
  forwarding its input's hints.

**Acceptance criteria**

- Every frame-representation wrapper forwards or correctly recomputes dropout
  hints from its *input*, with a unit test per fixed wrapper.

### Task 3.3 — Result freshness after a map edit

- Verify that editing the `dropout_map` parameter (via the GUI dropout editor
  or directly) invalidates the sink's cached results so the next trigger
  re-analyses; the graph and CSV must never show pre-edit data after a
  re-trigger. Fix invalidation if stale results survive a parameter change.

**Acceptance criteria**

- Sequence "trigger → edit map → trigger" yields updated stats in
  `frame_stats()`; covered by a unit test at the stage level.

### Task 3.4 — Document the topology requirement

- State in the dropout sink's `instructions.md` and
  `docs/gui-user-guide/stages/sink-analysis-stages.md` that the analysis
  reports the dropout state **at the sink's input**: to include `dropout_map`
  edits, the sink must be connected downstream of the `dropout_map` stage.

**Acceptance criteria**

- Docs updated in the same PR; wording reviewed against the actual DAG
  behaviour validated in Tasks 3.1–3.3.

---

## Phase 4 — Per-dropout detail report (issue #214)

Goal: a report listing every dropout with its location within the frame.
Depends on Phase 1 (per-frame capture) and Phase 3 (map-aware hints).

### Task 4.1 — Per-dropout record capture

- During dropout analysis, optionally collect per-run detail alongside the
  per-frame stats: `frame_number`, `line_number`, `sample_start`,
  `sample_end`, `length_samples`, derived from `DropoutRun`
  (`orc/sdk/include/orc/stage/dropout/dropout_run.h:27`) using the same
  frame-flat → line/sample conversion already used for visible-area filtering
  (`dropout_analysis_sink_deps.cpp:88-113`, nominal samples-per-line from
  video parameters).
- Collection is gated by the report parameter (Task 4.2) to avoid memory cost
  when unused.

**Acceptance criteria**

- Unit tests verify line/sample derivation for PAL (1135 spl) and NTSC
  (910 spl) nominal geometries and for runs spanning a line boundary.

### Task 4.2 — Report output formats and parameters

- Add parameters to `dropout_analysis_sink`: `write_report` (BOOL),
  `report_path` (FILE_PATH), `report_format` (ENUM: `csv`, `text`).
  - `csv`: one row per dropout run —
    `frame_number,line_number,sample_start,sample_end,length_samples`.
  - `text`: human-readable, grouped by frame — frame heading with dropout
    count and total length, then one line per run showing line/sample extent
    (the "map of where they are in the frame" from #214).
- Report generation is full-resolution and independent of graph decimation;
  it respects the existing `mode` (full/visible-area) parameter.

**Acceptance criteria**

- Both formats produced correctly from the same detail records
  (stream-based writers, unit-tested without filesystem access).
- Frames without dropouts do not appear in the report (contrast with the
  Phase 2 per-frame CSV, which lists all analysed frames).

### Task 4.3 — Documentation and GUI exposure

- Update `instructions.md` (parameters + example report excerpts) and
  `docs/gui-user-guide/stages/sink-analysis-stages.md`.
- Verify the new parameters appear and round-trip in the GUI parameter dialog
  (Tier 3 coverage per `AGENTS.md` §4.5 if a dialog change is needed).

**Acceptance criteria**

- Report can be produced end-to-end from the GUI trigger; docs ship in the
  same PR.

---

## Phase 5 — Presentation-path cleanup

Goal: general improvements uncovered by the earlier phases; independent of
Phases 2–4 apart from Phase 1's data model.

### Task 5.1 — Replace the `void*` analysis-data handoff

- `RenderPresenter::getDropoutAnalysisData` and siblings pass
  `frame_stats` as a `void*` through the presenter boundary
  (`orc/presenters/src/render_presenter.cpp:920-935`, noted in-code as a
  hack). Replace with typed view-model structs in `orc/view-types/` carrying
  the decimated display series (point list + bucket ranges + axis metadata),
  keeping `orc/gui` free of core types per MVP rules.

**Acceptance criteria**

- No `void*` (or equivalent unsafe cast) remains in the analysis data path;
  `MVPArchitectureCheck` and `-L gui` suites pass.

### Task 5.2 — Graph presentation of bucketed data

- Where a graph shows decimated data, the dialog states it (e.g. axis
  subtitle "each bar covers frames N–M") using bucket ranges from the
  Task 1.3 utility, so the interactive view is no longer mistakable for
  per-frame data. Point/bar hover or readout shows the bucket's frame range
  and aggregate values (`orc/gui/dropoutanalysisdialog.cpp`,
  `snranalysisdialog.cpp`, `burstlevelanalysisdialog.cpp`).

**Acceptance criteria**

- With > 1000 analysed frames the bucket range is visible in the dialog; with
  ≤ 1000 the display reads as per-frame. Tier 3 offscreen smoke tests updated.

### Task 5.3 — Consistent progress and cancellation UX

- Align progress reporting across the three sinks (per-analysed-frame with a
  uniform message format) and verify cancellation leaves no partial CSV or
  report files (write to a temporary name, rename on success).

**Acceptance criteria**

- Cancelling mid-analysis never leaves a truncated output file; unit tests
  cover the cancel path in each deps class.

---

## Validation

Per `AGENTS.md` §4.6, each phase must pass before merge:

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
ctest --test-dir build -R MVPArchitectureCheck --output-on-failure
ctest --test-dir build -L sdk --output-on-failure          # Phases touching SDK headers
QT_QPA_PLATFORM=offscreen ctest --test-dir build -L gui --output-on-failure  # Phases 1, 4, 5
```
