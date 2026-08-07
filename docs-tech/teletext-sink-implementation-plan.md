# Teletext sink — implementation plan

Refactors teletext recovery from the current split design — a background
`TeletextObserver` in `orc/core/observers/` plus a `teletext_sink` export stage —
into a single self-contained **analysis sink stage** that owns all decoding
logic and presents a teletext page viewer as its stage tool, following the same
pattern as `snr_analysis_sink`, `dropout_analysis_sink` and
`burst_level_analysis_sink`.

Normative signal references: [ETSI EN 300 706
(WST)](./analogue-video-specifications/docs/teletext/ETSI-EN-300-706-2003/ETSI-EN-300-706-2003.md)
and the VBI capture formats in
[vbi-source-stage-design.md](./vbi-source-stage-design.md).

## Motivation

- The observer runs unconditionally in the background sweep on every
  PAL/NTSC/PAL_M frame (`applies_to()` gates on video system only): 34 (625) or
  24 (525) `TeletextSlicer::slice()` calls per frame, each falling through to
  the expensive MLSE detector on every line of a source that carries no
  teletext at all. Observers take no configuration, so this cost cannot be
  turned off. As more teletext services are added (NABTS is already placeable
  by `vbi_source` but not decodable), the per-frame observer cost multiplies
  per format. Decoding must become on-demand.
- The GUI teletext dialogue is currently an *observer* dialogue: opened from
  the preview window, fed frame-by-frame from stored observations through the
  `GetTeletextData` coordinator pair and a 750-frame trailing-window assembler.
  It is replaced by the analysis-sink stage-tool flow: trigger the node, decode
  the full frame range once with progress and cancel, pop up the viewer.
- The stage will likely move to an external plugin, so its decoding logic must
  live only in the plugin directory and the public SDK tiers — never in
  `orc/core`.

## Scope

**In scope:** the new `teletext_analysis_sink` stage (reworked from
`teletext_sink`); deletion of `TeletextObserver`, the observer-dialogue GUI
machinery, and `video_sink` teletext subtitle embedding (removed for the time
being rather than migrated); the viewer stage-tool plumbing
(presenter → coordinator → per-node dialog).

**Out of scope:** NABTS decoding (System C framing is a future slicer variant —
the shared helper's system-profile seam is the extension point); any change to
the SDK slicer/page-decoder/row-squasher algorithms or to the `.t42` format
itself; CLI chart/viewer surfaces (the CLI runs the stage via `process` →
trigger-all-sinks and gets the file exports, as with the existing analysis
sinks).

## Binding architecture decisions

- **Stage identity.** `orc/plugins/stages/teletext_sink/` is renamed to
  `orc/plugins/stages/teletext_analysis_sink/`, stage id
  `teletext_analysis_sink`, plugin id
  `decode-orc.stage.teletext_analysis_sink`, `NodeType::ANALYSIS_SINK`.
  Existing project files referencing `teletext_sink` will no longer resolve;
  accepted (pre-release, no migration shim). All export behaviour
  (`.t42`, SRT, report) carries over unchanged.
- **Stage tool, not observer dialogue.** The stage implements
  `StageToolProvider` with a single `StageToolDescriptor` of kind
  `BatchAnalysis`, contract id `decode-orc.stage-tools.teletext-analysis.v1`.
  The host routes on the contract string exactly as for the three existing
  analysis sinks (auto-open after trigger; fetch-or-trigger when opened on a
  node that has not been triggered).
- **One full-range decode, two consumers.** `trigger()` performs one linear
  pass over the entire frame range producing a single decoded packet stream;
  the file exports and the viewer both consume it, exactly as the other
  analysis sinks offer a `.csv` export and a graph from the same trigger run.
  The viewer therefore always reflects the whole source, not a window around
  the preview position.
- **Results are a page catalogue, not raw packets.** For the viewer side of
  that pass, decoded output goes slice → `TeletextPageDecoder` →
  `TeletextRowSquasher`, merging snapshots into a bounded page catalogue (cap
  512 pages, matching the current assembler) plus aggregate recovery
  statistics. The host pulls this via a new orc-core-owned
  cross-DSO interface `ITeletextAnalysisResults` in
  `orc/sdk/include/orc/stage/analysis_sink_results.h` (the same
  macOS-safe downcast mechanism as `ISNRAnalysisResults`). Caching the
  catalogue instead of per-field packets bounds memory regardless of capture
  length and moves page assembly out of the GUI into the stage, per the goal
  that *all* decoding logic lives in the stage.
- **Frame-slicing logic lives in the plugin directory.** The observer's
  per-frame logic (system profile selection 625/525, candidate line windows
  5–21 / 9–20, black/white level derivation, luma-vs-composite line fetch, one
  pre-built slicer per television system) moves verbatim into a plugin-local
  `TeletextFrameSlicer` module. With `video_sink` teletext embedding removed
  (below), the stage is the sole consumer, so no new public SDK header is
  needed — maximally self-contained for the eventual external-plugin move.
  Promotion to the SDK support tier is the recorded option if a second
  consumer ever appears (e.g. re-added subtitle embedding, NABTS tooling).
- **`video_sink` teletext subtitle embedding is removed for now.** Its only
  data source was the observer; rather than migrate it, the embedding path,
  its `teletext_subtitle_feed`, and the associated `video_sink` parameters are
  deleted. Re-introduction later would consume the stage's decode logic, not
  a resurrected observer.
- **No observation store involvement.** The stage neither reads nor writes the
  `"teletext"` observation namespace. The dual observer-path/direct-path split
  in the current sink deps collapses to a single direct-slice path driven by
  the stage parameters. Stored `"teletext"` records in existing sidecars
  become orphaned; the generic schema tolerates this and no cleanup pass is
  required.
- **Format coverage.** The stage applies to PAL, NTSC and PAL_M (the viewer
  handles both 42-byte 625-line and 34-byte 525-line WST packets, as the
  dialogue already does). Packet export follows the service: 625-line sources
  write the existing headerless `.t42` stream; 525-line sources write the
  analogous `.t34` (flat 34-byte packets, same strict frame → field →
  ascending-line ordering) instead of dropping packets as the current sink
  does. Which extension applies is selected by the project format, via the
  project-format-filtered parameter descriptors; subtitle export remains
  625-only (SRT timing assumes 50 fields/s).

---

## Phase 1 — Self-contained decode core and stage rework

Goal: all teletext decoding reachable from the stage plugin through public SDK
tiers only; `TeletextObserver` deleted; `video_sink` migrated. Core and SDK
only — no GUI change; the existing dialogue keeps working against dead code
until Phase 2 removes it.

### Task 1.1 — Plugin-local `TeletextFrameSlicer` module

Move the per-frame slicing logic out of
`orc/core/observers/teletext_observer.cpp` into a new module in the stage's
plugin directory, class `TeletextFrameSlicer`: constructed once (three
per-system slicers), with a
`slice_field(const VideoFrameRepresentation&, FrameID, field_idx)` →
per-line `TeletextLineResult` surface. Slicer options (detector, parity
repair, tolerant framing, MRAG validation, line window override) are
constructor parameters so the stage's existing tuning parameters map directly.
Only existing public SDK headers (`teletext_slicer.h`,
`video_frame_representation.h`, signal constants) may be included.

**Acceptance criteria**
- Unit tests (labelled `unit` + `sinks`) cover system-profile selection, line
  windows, level fallback, and luma-vs-composite fetch using mocked
  `VideoFrameRepresentation` — behaviourally equivalent to the current
  observer tests, which they replace.
- `PluginPrivateIncludeScan` passes; no `orc/core` header is included.

### Task 1.2 — Stage rework to `teletext_analysis_sink`

Rename the plugin directory, targets, ids and `instructions.md`;
`NodeType::ANALYSIS_SINK`; add `StageToolProvider` returning the
`BatchAnalysis` descriptor with contract
`decode-orc.stage-tools.teletext-analysis.v1`. Rework the deps
implementation: delete the observer path (`create_observer("teletext")`,
coverage skip, `t42_keys` reads, `clear_field`) and drive
`TeletextFrameSlicer` directly for every frame; the parameter surface is
unchanged except that detector/repair parameters no longer need to match "the
host observer's configuration". Packet export follows the service per the
binding decision: `.t42` on 625-line sources, `.t34` (34-byte packets, same
ordering rules) on 525-line sources. `execute()` caches the input
representation as the other analysis sinks do; all work stays in `trigger()`
with the existing progress/cancel contract.

**Acceptance criteria**
- Existing stage unit tests pass, updated for the rename and the removed
  observer path; the mocked deps seam (`ITeletextSinkStageDeps` →
  `ITeletextAnalysisSinkStageDeps`) is retained.
- `.t42` output for the default parameter set is byte-identical to the
  current stage's direct-slice path on the same input (unit-level fixture
  comparison).
- `.t34` export on a 525-line fixture writes exactly the sliced 34-byte
  packets in frame → field → ascending-line order (unit test with mocked
  writer).
- `PluginPrivateIncludeScan` / `PluginPrivateLinkScan` pass; the contract test
  expectation (`StageToolProvider` present, `AnalysisToolProvider` absent)
  holds for the new stage.

### Task 1.3 — Page catalogue and `ITeletextAnalysisResults`

Add a plugin-local `teletext_page_catalogue` module (reusing SDK
`TeletextPageDecoder` + `TeletextRowSquasher`) that the trigger loop feeds in
ascending frame order: per-page snapshot merge, `times_seen`, first/last seen
frame, sticky subtitle (C6) flag, 512-page cap, plus aggregate recovery
statistics (fields with data, packets recovered/corrected, bytes repaired,
lost-packet estimate). Define the result structs and

```cpp
class ITeletextAnalysisResults {
  virtual bool has_results() const = 0;
  virtual const TeletextAnalysisDataset& dataset() const = 0;
};
```

in `orc/sdk/include/orc/stage/analysis_sink_results.h` (structs alongside the
existing analysis stats types); the stage implements it and caches the dataset
from the last trigger.

**Acceptance criteria**
- Unit tests cover snapshot merge, carousel repeat squash-correction, the
  page cap, and subtitle-flag stickiness with synthetic packet fixtures (the
  existing `teletext_packet_fixtures.h` material relocated to core tests).
- Trigger memory is bounded by the page cap, not the frame range.
- Doc-sync gates pass for the modified SDK header.

### Task 1.4 — Remove `video_sink` teletext subtitle embedding

Delete the teletext embedding path: the `create_observer("teletext")` +
per-frame observation flow in `video_sink_stage.cpp`,
`teletext_subtitle_feed.{h,cpp}` and its test, the teletext-specific parts of
`subtitle_embed_policy.h` (closed-caption embedding is untouched), and the
`video_sink` subtitle-embedding parameters that fed it. Update the
`video_sink` `instructions.md` in the same change (AGENTS.md §9.1).

**Acceptance criteria**
- `video_sink` no longer references teletext in code, parameters, or
  `instructions.md`; closed-caption embedding behaviour is unchanged.
- Remaining `video_sink` unit tests green.

### Task 1.5 — Delete `TeletextObserver`

Remove `orc/core/observers/teletext_observer.{h,cpp}`, the
`core_observation_service.cpp` registry entry (10 → 9 observers) and the
`orc/core/CMakeLists.txt` source line. Update the observer-facing tests:
delete `teletext_observer_test.cpp`, fix the identity-contract and
service-registry expectations, and re-key `observer_pass_test.cpp`'s spy
observer to a surviving id.

**Acceptance criteria**
- Full unit suite and `MVPArchitectureCheck` green.
- Grep proves no remaining reference to `TeletextObserver` or observer id
  `"teletext"` outside git history and docs scheduled for Phase 3.
- Background sweep on a PAL source no longer performs any teletext work
  (verified via the observation scheduler's observer list in a unit test).

---

## Phase 2 — Viewer as a stage tool

Goal: the GUI flow matches the other analysis sinks end-to-end; the
observer-dialogue machinery is removed.

### Task 2.1 — Presenter data path

Add `RenderPresenter::getTeletextAnalysisData(NodeID)` following
`getSNRAnalysisData`: locate the node, downcast to
`ITeletextAnalysisResults`, convert the dataset to view types. Extend
`orc/view-types/orc_teletext.h` with a catalogue view
(`TeletextAnalysisView`: page list entries + recovery summary), reusing
`TeletextPageView` and the existing `makePageView()` snapshot conversion
(which moves from `teletext_observation_presenter` into the surviving
presenter; `extractFieldObservations()` is deleted). No decimation stage —
the catalogue is already bounded.

**Acceptance criteria**
- Presenter unit tests cover dataset→view conversion including 525-line
  (34-byte) pages, mosaics, and the empty/no-results case.
- `teletext_observation_presenter.{h,cpp}` removed or reduced to the page-view
  conversion only; MVP check green.

### Task 2.2 — Coordinator request and fetch-or-trigger

Add `GetTeletextAnalysisData` to `RenderCoordinator` (request type, handler,
`teletextAnalysisDataReady` signal) modelled on `handleGetSNRData`: pull via
the presenter; if `std::nullopt`, `triggerStage(node_id, progress)` once and
retry. Remove the old `GetTeletextData` request type, handler and
`teletextDataReady` signal.

**Acceptance criteria**
- `render_coordinator_test.cpp` covers ready-data delivery, the
  trigger-then-retry path, and stale-request suppression for the new request;
  old `GetTeletextData` tests deleted.
- Cancel during the auto-trigger propagates through the existing
  `trigger_cancel_requested_` chain.

### Task 2.3 — Dialog rework and MainWindow routing

Rework `TeletextDialog` into a per-node catalogue viewer: page-number entry,
seen-pages table (page / times seen / first–last frame), recovery summary and
`TeletextPageWidget` rendering are retained; the current-frame coupling,
`setCurrentFrame`/`deliverFrameData` API and `teletext_page_assembler` are
deleted. MainWindow: per-node dialog cache
(`teletext_analysis_dialogs_`), routing branch on the new contract string in
`runAnalysisForNode`, auto-open from `onTriggerComplete` via the existing
queued `createAndShowAnalysisDialog` path, progress dialog + pending-request
map as for SNR.

**Acceptance criteria**
- Trigger of a `teletext_analysis_sink` node auto-opens the viewer; opening
  via the analysis routing on an untriggered node triggers first
  (fetch-or-trigger), with working progress and cancel.
- Tier 3 offscreen smoke test for the reworked dialog (extend
  `analysis_dialog_smoke_test.cpp` or equivalent) plus Tier 2 coverage of the
  MainWindow request/response bookkeeping at the presenter boundary.

### Task 2.4 — Remove the observer dialogue surface

Delete the preview-window "Teletext Pages" menu action, shortcut and
availability wiring (`previewdialog`, `mainwindow` connect/update/issue
methods, `pending_teletext_requests_`, cache-node tracking) and
`teletext_page_assembler.{h,cpp}` with its tests. Update
`mock_render_presenter.h` and any fixtures still referencing the removed
surface.

**Acceptance criteria**
- Full GUI test suite green offscreen; no reference to the removed request
  pair, assembler, or preview menu action remains.
- `plugin_ux_capabilities.yaml` reflects the capability change if the
  manifest models it (parity gate green either way).

---

## Phase 3 — Validation and documentation

### Task 3.1 — Functional end-to-end coverage

`functional`-labelled tests decoding the reference captures
(`test-data/teletext/`, skipped when absent, per the `vbi_source` pattern):
PAL WST via `tbc_source` or `vbi_source`, and NTSC WST 525 via
`vbi_source`, asserting page catalogue content and packet export against the
outputs of the pre-refactor pipeline captured as golden data (`.t42` for PAL;
for NTSC the `.t34` golden is generated once from the same capture at
refactor time, since the old sink had no 525 export).

**Acceptance criteria**
- `.t42` golden comparison passes for the PAL sample; `.t34` export of the
  NTSC sample matches its golden; catalogue contains the expected page
  numbers for both samples.
- Tests skip cleanly without the gitignored samples.

### Task 3.2 — Documentation sweep

Update: the stage's `instructions.md` (parameters unchanged, new tool
section); `docs/gui-user-guide/observers/overview.md` (remove the Teletext
Observer section); `orc/gui/docs/preview_window.md` (remove the teletext
dialogue); `docs/gui-user-guide/stages/` (move the stage from sink-core to
sink-analysis, describe the viewer, drop the `video_sink` teletext embedding
documentation).

**Acceptance criteria**
- All doc-sync gates (`SdkHeaderDocsSync`, capability parity) green.
- No documentation references the observer, the preview teletext dialogue, or
  the `teletext_sink` stage id.

### Task 3.3 — Final gate run

Full validation per AGENTS.md §4.6: clean build with
`BUILD_UNIT_TESTS=ON -DBUILD_GUI_TESTS=ON`, full `ctest` including `-L sdk`,
`-L gui` offscreen, and `MVPArchitectureCheck`.

**Acceptance criteria**
- All suites green; no new MVP or SDK-gate violations; stage loads and
  triggers in a live GUI sanity pass on a real capture.

---

## Phase 4 — Recategorisation and learned scan (2026-08-07)

A follow-on to the phases above, after the reworked stage had been used on real
captures. Two unrelated changes that touch the same files.

### Task 4.1 — Back to a sink, and named to match

Phase 1 made the stage an analysis sink because it grew a batch-analysis
dialog. That was the wrong reading of the category: a batch-analysis stage tool
is `StageToolProvider` surface and any node type may declare one, while
`NodeType` describes what the node is *for*. This one writes a packet stream —
that is its product, and the page catalogue is a by-product of the same pass.

`orc/plugins/stages/teletext_analysis_sink/` returns to
`orc/plugins/stages/teletext_sink/`; stage id `teletext_sink`, plugin id
`decode-orc.stage.teletext_sink`, target
`orc-stage-plugin-teletext-sink`, `NodeType::SINK`,
`TeletextAnalysisSinkStage` → `TeletextSinkStage` and its deps types with it.
`TeletextAnalysisDataset` and `ITeletextAnalysisResults` keep their names:
they describe analysis results, which is still what they are.

Two consequences had to be handled rather than accepted:

- **Saved projects.** Unlike the Phase 1 rename this one is not pre-release, so
  `Project::load` migrates `teletext_analysis_sink` nodes to `teletext_sink`,
  their display name with them, and rewrites the stored `node_type` from
  `ANALYSIS_SINK` to `SINK` — the stored type is what decides which nodes the
  DAG treats as outputs, so leaving it would quietly drop the migrated node
  from that set.
- **Browse-only triggering.** `can_trigger_node` refuses to trigger a
  `NodeType::SINK` node with no `output_path`, and a run with no output path is
  a documented, supported use of this stage. The gate now exempts any sink that
  declares a `BatchAnalysis` stage tool, which is exactly the statement that a
  run of it produces something besides a file.

The stage's user-guide page moves from `sink-analysis-stages.md` to
`sink-core-stages.md`.

### Task 4.2 — What a pass learns about its recording

Profiling a pass showed the cost is not where the design assumed. Nothing about
*which* teletext service to decode is determined at runtime — `VideoSystem`
selects it outright, and with it the bit rate, packet length, data-'1' level,
timing window and line window — so there is no format detection to make cheaper.
What is determined at runtime, on every candidate line of every field of every
frame, is where in the line the data burst starts and whether the line carries
one at all.

Two additions, both learning from the pass rather than from configuration, and
both held in a new plugin-local `TeletextScanState` passed into
`TeletextFrameSlicer::slice_field()` so the frame slicer and the SDK slicer stay
const and shareable:

- **`TeletextPhaseHint` (SDK) + `TeletextPhaseTracker` (plugin).** The slicer
  gains an optional hint narrowing the acquisition sweep, and reports the phase
  it locked at (`TeletextLineResult::lock_sample`) so a caller can accumulate
  them. The tracker offers a hint once 24 lines have yielded packets, centred on
  the trimmed middle of the last 64 locks and widened to cover their spread;
  locks scattered wider than 7 samples withhold it. **A hinted attempt that
  recovers nothing is repeated over the full window**, which is what makes the
  hint impossible to lose a packet to.
- **`TeletextLineTracker` (plugin).** Reads every line for the first 50 frames,
  then only lines that have produced a packet, re-reading the full window every
  50th frame. Per field. This one *can* lose a packet — a line carrying data
  exactly once, on neither a learning nor a recheck frame — measured at one
  packet in 3,964 on the 625-line reference.

Both are exposed as `pin_data_phase` and `learn_active_lines`, default true,
and the run's report states where the window was pinned and how many lines the
mask skipped.

**Measured on the reference captures** (625: 200 frames of the bt8x8 sample;
525: 300 frames of the TBS Electra tape), decode wall time and packets:

| | 625 time | 625 packets | 525 time | 525 packets |
|-|-|-|-|-|
| neither | 1977 ms | 3964 | 946 ms | 2652 |
| pin only | 1415 ms | 4124 | 754 ms | 2670 |
| mask only | 1913 ms | 3963 | 758 ms | 2652 |
| both (default) | 1378 ms | 4119 | 534 ms | 2670 |

Pinning *recovers* packets as well as time: an exhaustive sweep can settle on a
false correlation peak, and a window pinned to where the data actually is
cannot. The mask's value depends entirely on what the unused lines hold — blank
on the 625-line sample (rejected by the amplitude gate for almost nothing),
signal-bearing on the 525-line tape (reaching the MLSE fallback every frame),
which is the whole of the difference between its 3 % and its 20 %.

**Acceptance criteria**
- The functional goldens are unchanged with both options off, which is what
  establishes that the acquisition refactor is bit-exact; a second pair of
  goldens covers the default configuration, asserting alongside them that the
  learned scan recovers no fewer packets than the exhaustive decode.
- Slicer-level tests cover the hint contract: the reported lock is where the
  burst is, a correct hint decodes identically to the full sweep, a wrong hint
  falls back and still recovers the packet, a hint outside the timing window is
  ignored, and the MLSE acquisition is pinned too.
- Tracker unit tests cover the hint threshold, the spread widening, the
  disagreement cutoff, the running window, per-field masking, recheck
  readmission, and the untrackable-line guard.

---

## Phase 5 — Parallel decode, and one table per system (2026-08-07)

### Task 5.1 — Recover on several threads

Slicing is nearly all of what a pass costs and every line is recovered from its
own samples, so the frames are independent. Emission is not: a teletext stream
is strictly ordered, and the page catalogue, the row squasher and the recovery
statistics all depend on that order. So the pass splits — workers slice ahead,
this thread emits behind them.

What makes that harder than a parallel-for is Phase 4's learning. It is
feedback: what a frame costs depends on what earlier frames produced. Left as
it was, a frame's packets would have depended on how far ahead of it the other
threads had got, and no two runs would have agreed.

The fix is to make what a worker may know a **value**, frozen for a span of
frames:

- `TeletextScanSnapshot` — pinning on/off, the phase hint, the per-(field,line)
  mask and the mask's timing options, with `reads_full_window()` and
  `should_probe()` as pure functions of it and a frame index. This is all a
  worker sees.
- `TeletextScanState` — the live trackers, advanced only by the ordered
  emission pass, and frozen into a snapshot per block.
- `TeletextFrameSlicer::slice_field()` is now const with no mutable argument at
  all: snapshot and frame index in, `TeletextFieldScan` out.

Frames are taken in blocks, sliced against one snapshot, then emitted in order,
which advances the state for the next block. Blocks start at 16 frames and
double to 128: small early, when there is most to learn and the pinning has yet
to engage, larger later, where the size amortises thread creation and keeps the
workers busy to the block's tail. The ceiling is memory — a block holds every
line it sliced, ~1,6 kB each, so 128 frames of a 625-line window is ~7 MB.

The work unit is a **whole frame**, not a field. Both fields read the same
frame from the source, and splitting them across threads made each frame be
fetched twice: going to whole frames took the 625-line reference at 8 threads
from 660 ms to 295.

`decode_threads` (0 = one per processor) is exposed, mostly so a user can leave
the machine free for other work — and so the determinism claim can be tested.

**Measured** (625: 200 frames of the bt8x8 sample; 525: 300 of the TBS Electra
tape), decode wall time:

| | 625 | 525 |
|-|-|-|
| exhaustive, 1 thread (pre-Phase 4) | 1970 ms | 946 ms |
| learned scan, 1 thread | 1406 ms | 535 ms |
| learned scan, 2 threads | 760 ms | 300 ms |
| learned scan, 4 threads | 442 ms | 178 ms |
| learned scan, 8 threads | **295 ms** | **156 ms** |
| learned scan, 16 threads | 303 ms | 178 ms |

6,7× and 6,1× end to end. It stops scaling at the physical core count: past
that the pass is waiting on the source, which serves frames from one file
position under a lock (`VBIFrameRepresentation::io_mutex_`) and so hands them
out one at a time. Making that concurrent is a change to the source stages, not
to this one.

The learned-scan goldens moved again, because the learning is now quantised to
blocks rather than applied per line. On the 625-line capture it recovers 4097
packets against the previous 4119 and 105 pages against 103, with data loss
0,73 % → 0,67 %.

**Acceptance criteria**
- Functional tests decode both reference captures at one, three and eight
  threads and require the same SHA-256, byte count and packet count from each,
  compared against the golden as well as against one another — so a change that
  made every thread count agree on the wrong answer still fails.
- A worker that throws does not take the process with it: the exception is
  held, the queue drained, and it is rethrown once the threads are joined.
- A cancelled block is discarded whole, so the stream stays a prefix of the run
  rather than gaining a hole where the workers stopped taking jobs.

### Task 5.2 — One table per system, not four

Recovery decided things per television system in four places: the slicer's
constructor (four parallel ternaries on `TeletextSystem`), the frame slicer's
`applies_to()`, `profile_for()` and `slicer_for()`, its default black/white
levels, and the stage's own copies of the UI line windows.

Now:

- `system_geometry()` in `teletext_slicer.cpp` holds the bit rate, packet
  length, data-'1' fraction and acquisition window of each service. The header's
  `teletext_bit_rate()` and `teletext_packet_bytes()` stay — they must be usable
  in constant expressions by callers that never see the table — and a
  `static_assert` ties the two together.
- `TeletextFrameSlicer::SystemProfile` gains `carries_teletext`, the 4FSC sample
  rate, the default levels and a slicer index, so `profile_for()` is the only
  place a `VideoSystem` is turned into anything. `applies_to()` and the slicer
  lookup read it; `slicer_for()` is gone, and the slicers are an array built
  from `kTeletextVideoSystems`.
- The stage derives its UI defaults from `profile_for()` through
  `profile_for_project()` rather than restating the windows, so the lines the UI
  offers and the lines recovery probes cannot drift.

Adding a service — NABTS is the obvious candidate — is now a row in each of
those two tables plus an enumerator, rather than a hunt for the ternaries.
