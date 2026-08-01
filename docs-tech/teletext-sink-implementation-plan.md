# Teletext Sink Implementation Plan

Phased implementation plan for the design in
[teletext-sink-design.md](teletext-sink-design.md) (addresses
[issue #58](https://github.com/simoninns/decode-orc/issues/58)). Scope is
**PAL WST only** (design §1.3); read the design document in full before
starting any phase.

## Authoritative references

Specifications vendored in the `analogue-video-specifications` submodule —
**use these, do not work from memory or external summaries**:

| Reference | Location | Use for |
|---|---|---|
| ETSI EN 300 706 V1.2.1 (primary) | [analogue-video-specifications/docs/teletext/ETSI-EN-300-706-2003/ETSI-EN-300-706-2003.md](analogue-video-specifications/docs/teletext/ETSI-EN-300-706-2003/ETSI-EN-300-706-2003.md) | line allocation (§4.1), data levels (§5.2), bit rate (§5.3), spectrum (§5.4), clock run-in (§6.1), framing code (§6.2), timing reference (§6.3), packet structure (§7.1), MRAG (§7.1.2), page/magazine model (§7.2, §7.3), odd parity (§8.1), Hamming 8/4 (§8.2), Hamming 24/18 (§8.3), packet coding (§9), legacy line caveats (§F.4) |
| ITU-R BT.653-3 | [analogue-video-specifications/docs/teletext/BT-653-3-1998/BT-653-3-1998.md](analogue-video-specifications/docs/teletext/BT-653-3-1998/BT-653-3-1998.md) | System B definition and cross-system context (design §1.3) |
| BBC/IBA/BREMA Broadcast Teletext Specification (1976) | [analogue-video-specifications/docs/teletext/BBC-Broadcast-Teletext-1976/BBC-Broadcast-Teletext-1976.md](analogue-video-specifications/docs/teletext/BBC-Broadcast-Teletext-1976/BBC-Broadcast-Teletext-1976.md) | historical context for UK broadcasts (the motivating disc) |

Other authoritative inputs:

- T42 packet stream format: [zxnet teletext wiki — T42 packet stream](https://teletext.wiki.zxnet.co.uk/wiki/T42_packet_stream) (design §2.2).
- External validation tools: [vhs-teletext](https://github.com/ali1234/vhs-teletext) (`teletext filter/interactive/deconvolve`), wxTED (design §5.5).
- PAL sample rate constant: `kPalSampleRate` in [../orc/sdk/include/orc/stage/cvbs_signal_constants.h](../orc/sdk/include/orc/stage/cvbs_signal_constants.h) — one teletext bit ≈ 2.556 samples; this is why `vbi_utilities.h` must **not** be reused (design §2.1).

In-tree templates to copy from:

- Observer pattern: [../orc/core/observers/closed_caption_observer.cpp](../orc/core/observers/closed_caption_observer.cpp), luma-path idiom in [../orc/core/observers/biphase_observer.cpp](../orc/core/observers/biphase_observer.cpp), registration table in [../orc/core/core_observation_service.cpp](../orc/core/core_observation_service.cpp).
- Sink plugin pattern: [../orc/plugins/stages/cc_sink/](../orc/plugins/stages/cc_sink/) (stage/deps/deps-interface split), uint8 writer path in [../orc/plugins/stages/daphne_vbi_sink/](../orc/plugins/stages/daphne_vbi_sink/).
- Subtitle embedding: `embed_closed_captions` in [../orc/plugins/stages/sinks/common/video_sink_stage.cpp](../orc/plugins/stages/sinks/common/video_sink_stage.cpp), timestamping in [../orc/plugins/stages/cc_sink/cc_sink_stage_deps.cpp](../orc/plugins/stages/cc_sink/cc_sink_stage_deps.cpp), mov_text muxing in [../orc/plugins/stages/sinks/common/ffmpeg_output_backend.cpp](../orc/plugins/stages/sinks/common/ffmpeg_output_backend.cpp).

Process rules that apply to every phase: [../AGENTS.md](../AGENTS.md) §4
(testing), §5 (coding standards, incl. §5.3.6 spec-reference comment
format), §9 (SDK/plugin rules, manifest regeneration, `instructions.md`),
and [../TESTING.md](../TESTING.md).

---

## Phase 1 — Teletext line slicer and host observer

Delivers the signal-recovery core and the observation schema. No plugin or
GUI work. Design sections: §2.1, §3, §4.

### Task 1.1 — `TeletextSlicer` support-tier component

Create `orc/sdk/include/orc/support/teletext_slicer.h` and
`orc/sdk/src/teletext_slicer.cpp` implementing the interface and algorithm
of design §4.1: coarse amplitude gate → clock run-in correlation
(16-bit `1010…`, [EN 300 706 §6.1](analogue-video-specifications/docs/teletext/ETSI-EN-300-706-2003/ETSI-EN-300-706-2003.md))
→ framing-code lock (`11100100`, §6.2; exact match default, optional
1-bit-tolerant mode) → 336-bit LSB-first byte extraction (§7.1) → optional
MRAG Hamming 8/4 plausibility filter (§7.1.2, §8.2). Bit rate fixed at
444 × fH (§5.3); data levels per §5.2; timing window per §6.3. No
Hamming/parity **correction** of the payload — output stays in
transmission coding (T42 contract, design §2.2). Constants must carry
spec-reference comments per AGENTS.md §5.3.6.

**Acceptance criteria**

- `TeletextLineResult`/`TeletextSlicer` match the design §4.1 sketch (naming may be refined, semantics may not).
- Every numeric constant cites its EN 300 706 section in a comment.
- Builds warning-clean; no dependency on `vbi_utilities.h` or any core-private header.

### Task 1.2 — Slicer unit tests (synthesis-based)

Add a `unit`-labelled suite that synthesizes NRZ teletext lines at
17,734,475 Hz from known packet bytes (configurable amplitude, noise,
sub-sample timing offset) and asserts byte-exact recovery. Cover: clean
recovery, amplitude/noise/phase sweeps, empty-line rejection, framing
tolerance on/off, MRAG filter accept/reject, run-in found but framing
absent. No filesystem, no real media (AGENTS.md §4.2).

**Acceptance criteria**

- Byte-exact round-trip for clean synthesized lines across a phase-offset sweep covering at least one full bit period.
- Empty and noise-only lines never produce `valid == true`.
- Suite registered with `gtest_discover_tests(... LABELS "unit")` and green via `ctest -L unit`.

### Task 1.3 — SDK manifest and generated docs

Register the new support header in
[../orc/sdk/sdk_headers.yaml](../orc/sdk/sdk_headers.yaml); regenerate the
allowlist (`tools/gen_sdk_header_allowlist.sh`) and the `plugin-sdk.md`
header tables (`tools/gen_sdk_header_docs.sh`) per AGENTS.md §9. Do not
hand-edit generated files.

**Acceptance criteria**

- `ctest -L sdk` green (`SdkHeaderManifestSync`, `SdkHeaderDocsSync`, include/link scans).

### Task 1.4 — `TeletextObserver`

Create `orc/core/observers/teletext_observer.{h,cpp}` and register id
`teletext` (stateless) in
[../orc/core/core_observation_service.cpp](../orc/core/core_observation_service.cpp).
Behaviour per design §4.2: PAL-only early return; per field, probe the
candidate window (0-based field lines 5–21 both fields, design §3.1 —
broadcast lines 6–22/318–335 per
[EN 300 706 §4.1](analogue-video-specifications/docs/teletext/ETSI-EN-300-706-2003/ETSI-EN-300-706-2003.md));
luma-aware line fetch via `get_line_luma()`/`get_line()` using
`get_line_samples()`; levels from `SourceParameters` with spec-constant
fallback. Emit the §3.2 schema: `present` (BOOL), `line_count` (INT32),
`t42_<field_line>` (STRING, 84 hex chars). Provide the shared hex
encode/decode helper the sink will reuse.

**Acceptance criteria**

- `get_provided_observations()` declares `present`, `line_count`, and one `t42_<n>` entry per candidate line.
- `ObserverInfo.stateless == true`; observations keyed by `FieldID(frame_id*2 + field_idx)`.
- Non-PAL frames produce no observations and no errors.

### Task 1.5 — Observer unit tests

Suite under `orc-tests/core/unit/` using a mocked
`VideoFrameRepresentation` that serves synthesized teletext lines on
selected VBI rows (reuse the Task 1.2 synthesizer). Assert: schema keys and
hex payload correctness, key absence for empty lines, PAL-only gating,
luma-path selection when `has_separate_channels()` is true, statelessness
(identical output for repeated `process_frame()` on the same frame).

**Acceptance criteria**

- All assertions above covered; suite labelled `unit`; deterministic, no I/O.
- Full existing test suite still green (`ctest --test-dir build --output-on-failure`).

---

## Phase 2 — `teletext_sink` stage plugin

Delivers the user-facing T42 export. Design sections: §2.2, §5. The phase
acceptance gate is external validation of real `.t42` output (design §5.5,
§9 item 2).

### Task 2.1 — Plugin skeleton

Create `orc/plugins/stages/teletext_sink/` with the design §5.1 layout:
`CMakeLists.txt` (`orc_add_stage_plugin`, explicit source list, no
third-party libs), `plugin.cpp`
(`ORC_STAGE_PLUGIN_DESCRIPTOR("decode-orc.stage.teletext_sink", ...)`),
`teletext_sink_stage.{h,cpp}` (`DAGStage + ParameterizedStage +
TriggerableStage`, `IStageServices*` constructor),
`teletext_sink_stage_deps_interface.h`, `teletext_sink_stage_deps.{h,cpp}`
stubs. `NodeTypeInfo` and config-status behaviour exactly per §5.1
(`VideoFormatCompatibility::PAL_ONLY`; `Red` until `output_path` set).
Parameters per §5.2: `output_path` (FILE_PATH, `.t42` hint),
`first_vbi_line`/`last_vbi_line` (INT32, 1-based UI per
`frame_numbering.h` conventions, defaults 6/22), `keep_empty_packets`
(BOOL, false), `tolerant_framing` (BOOL, false), `require_valid_mrag`
(BOOL, true).

**Acceptance criteria**

- Plugin builds and loads; stage appears with the §5.1 `NodeTypeInfo` strings.
- Parameter set matches the §5.2 table exactly (names, types, defaults).
- Config status transitions Red→Green on `output_path` assignment.

### Task 2.2 — Trigger implementation (deps object)

Implement `ITeletextSinkStageDeps` per design §5.3: PAL verification;
single `IObserverHandle` for `"teletext"` held across the run; per-frame
coverage skip via `observation_context.has(field_id, "teletext",
"present")`; output through
`IStageServices::create_buffered_file_writer_uint8()` (the
`daphne_vbi_sink` writer path); frame loop with cancel check
(`std::atomic<bool>`), throttled progress, `clear_field()` hygiene.
Packet emission strictly temporal — frame → field (1 then 2) → ascending
line — decoding `t42_<line>` hex, or 42 zero bytes when
`keep_empty_packets` and the key is absent (vhs-decode convention, design
§2.2). `get_trigger_status()` reports counts; exceptions caught and
reported, never thrown.

**Acceptance criteria**

- Output is a flat sequence of 42-byte packets with no header, in transmission coding, temporally ordered.
- `keep_empty_packets` yields exactly `(last_vbi_line − first_vbi_line + 1) × 2 × frames` packets.
- Cancel aborts promptly with a truthful status; partial file state reported.

### Task 2.3 — Registration touch-points

Complete the design §5.4 checklist:
`orc/plugins/stages/CMakeLists.txt` `add_subdirectory`;
`PublicStageSpec` in
[../orc-tests/core/unit/include/public_stage_inventory.h](../orc-tests/core/unit/include/public_stage_inventory.h)
(`PublicStageFamily::Sink`); loader assertion in
[../orc-tests/core/unit/contracts/stage_registry_contract_test.cpp](../orc-tests/core/unit/contracts/stage_registry_contract_test.cpp).

**Acceptance criteria**

- Registry, node-discovery, and parameter/default-parity contract suites pick the stage up and pass (`ctest -L contracts`).
- `ctest -R StagePluginLoader` and `ctest -L sdk` green.

### Task 2.4 — Stage and deps unit tests

Suite `orc-tests/core/unit/stages/teletext_sink/`, labels `unit;sinks`
(AGENTS.md §4.4). Stage tests with `StrictMock<ITeletextSinkStageDeps>`
(the `daphne_vbi_sink_stage_test` shape): parameter parsing incl. 1-based↔
0-based line conversion, config-status transitions, trigger dispatch,
`execute()` returning `{}`. Deps tests with mocked observation
service/context and mocked `IFileWriterUint8`: packet bytes, temporal
ordering, keep-empty padding, coverage skip, cancel, progress (design §8
table rows 3–4).

**Acceptance criteria**

- All §8 table behaviours for the stage and deps rows covered; suites deterministic, no filesystem.
- `ctest -L sinks` green.

### Task 2.5 — Stage documentation

Write `instructions.md` (What it does / When to use / Parameters) in the
plugin directory with `ORC_STAGE_INSTRUCTIONS_MD` in the class body
(AGENTS.md §9.1). Must state: PAL WST only (design §1.3), the T42 format
and the zxnet wiki reference, expected quality per source class —
LaserDisc/CVBS good, consumer VHS degraded (design §2.4) — and the
`keep_empty_packets` 1:1 line-mapping semantics. Add the stage to
`docs/gui-user-guide/stages/sink-core-stages.md`.

**Acceptance criteria**

- `instructions.md` present, rendered by the GUI help dialog, and covering all six parameters.
- User-guide stage table updated in the same change.

### Task 2.6 — External validation (phase acceptance gate)

Run the sink over the British Garden Birds LaserDisc capture (issue #58)
and validate the `.t42` with external consumers per design §5.5:
vhs-teletext `teletext filter`/`teletext interactive`, and wxTED or
another zxnet-listed consumer. Where a vhs-decode tape capture with
teletext is available, characterise (not gate on) recovery versus
`teletext deconvolve` of the same `.tbc` with `keep_empty_packets` on both
sides. Add a `functional`-labelled end-to-end test comparing a short real
capture against a golden `.t42` (design §8, last row) — functional label
only, never in the unit lane (AGENTS.md §4.1).

**Acceptance criteria**

- Pages from the issue #58 disc browse correctly in at least one external T42 consumer.
- Golden-file functional test added and passing locally; documented as excluded from the unit CI lane.
- Findings (including the validated candidate-window default — the §3.1 off-by-one caveat) fed back into `instructions.md` and, if the design changed, `teletext-sink-design.md`.

---

## Phase 3 — Page decoder and subtitle export

Delivers `TeletextPageDecoder` and the two subtitle consumers. Design
section: §6. Requires Phase 2 complete (validated packets to feed).

### Task 3.1 — `TeletextPageDecoder` support-tier component

Create `orc/sdk/include/orc/support/teletext_page_decoder.h` and
`orc/sdk/src/teletext_page_decoder.cpp` per design §6: consumes 42-byte
packets in temporal order; Hamming 8/4 and odd-parity decoding
([EN 300 706 §8.1–8.2](analogue-video-specifications/docs/teletext/ETSI-EN-300-706-2003/ETSI-EN-300-706-2003.md));
magazine/page assembly in serial and parallel magazine modes (§7.2, §7.3);
page-header control bits incl. C5/C6 subtitle flags (§9.3.1.3); exposes
completed 40×25 Level 1 page snapshots (character, colours, double height,
mosaic/hold) and subtitle cue emission (page arrival = display; header
with C6 clear or page erase = clear). Stateful by design — this is why it
lives outside the observer (design §3.2). Update the SDK manifest and
regenerate docs as in Task 1.3.

**Acceptance criteria**

- Serial and parallel magazine modes both assemble correct pages from interleaved packet streams.
- Hamming/parity errors degrade gracefully (flagged cells, no crashes, no page corruption beyond the damaged bytes).
- `ctest -L sdk` green after manifest regeneration.

### Task 3.2 — Page decoder unit tests

`unit`-labelled suite feeding hand-built packet sequences: page assembly
across header/body packets, subpage replacement, serial vs parallel mode,
Hamming single-bit correction and double-bit rejection, parity-error cell
flagging, subtitle cue lifecycle (C6 page arrival/clear/erase) using
synthetic page-888-style streams (design §8 table).

**Acceptance criteria**

- All listed behaviours asserted; deterministic, no I/O.

### Task 3.3 — `teletext_sink` subtitle export

Add parameters `export_subtitles` (BOOL), `subtitle_page` (STRING,
default `"888"`), `subtitle_format` (`{"SRT"}`) per design §6.1. The deps
object feeds recovered packets into a `TeletextPageDecoder` filtered to
the subtitle page and emits SRT cues timed from field number / field rate
— same derivation as `generate_timestamp()` in
[../orc/plugins/stages/cc_sink/cc_sink_stage_deps.cpp](../orc/plugins/stages/cc_sink/cc_sink_stage_deps.cpp).
Update `instructions.md` in the same change (AGENTS.md §9.1).

**Acceptance criteria**

- SRT output has monotonic, correctly formatted timestamps; cue text matches decoder output with Level 1 attributes dropped.
- New parameters covered by stage/deps unit tests; parameter-parity contract suite green.
- `instructions.md` updated.

### Task 3.4 — `video_sink` subtitle embedding

Add `embed_teletext_subtitles` plus a subtitle-page parameter to
[../orc/plugins/stages/sinks/common/video_sink_stage.cpp](../orc/plugins/stages/sinks/common/video_sink_stage.cpp),
mirroring the existing `embed_closed_captions` collection pass (create
observer, loop range, feed decoder) and reusing the existing `mov_text`
cue muxing in
[../orc/plugins/stages/sinks/common/ffmpeg_output_backend.cpp](../orc/plugins/stages/sinks/common/ffmpeg_output_backend.cpp)
**unchanged** — cues in, tx3g samples out. Raw DVB teletext stream muxing
is out of scope (design §6.2).

**Acceptance criteria**

- Embedded `mov_text` track plays with correct cue timing in a standard player for a known capture.
- No modification to the mov_text muxing path itself; unit tests cover the new collection pass at the deps/backend seam.
- `video_sink` `instructions.md` updated.

---

## Phase 4 — Teletext page preview dialog

Delivers the modeless GUI preview. Design section: §7. Requires Phase 3
(`TeletextPageDecoder`). MVP boundaries enforced by
`ctest -R MVPArchitectureCheck` throughout (AGENTS.md §2).

### Task 4.1 — View types and presenter extraction

New `orc/view-types/orc_teletext.h` with value types
`TeletextFieldPacketsView` and `TeletextPageView` (no Qt, no core types).
New `TeletextObservationPresenter::extractFieldObservations(FieldID,
const void*)` in `orc/presenters/` converting `teletext` observations
(hex `t42_<line>` strings) into the view types; the `const void*`
observation context is touched only inside the callback (design §7).

**Acceptance criteria**

- Extraction covered by Tier 1 `gui-logic`-style presenter unit tests (hex → packet bytes, absent keys, empty fields).
- `ctest -R MVPArchitectureCheck` green.

### Task 4.2 — Coordinator request plumbing

Add a `GetTeletextData` request/response pair to `RenderCoordinator` /
`RenderPresenter::requestObservations()` with request-id matching and
stale-response suppression, following the VBI-dialog pattern (design §7).
Extend
[../orc-tests/gui/unit/mocks/mock_render_presenter.h](../orc-tests/gui/unit/mocks/mock_render_presenter.h).

**Acceptance criteria**

- Tier 2 `gui-model` tests cover request ordering, response delivery, stale-response suppression, and clean shutdown (AGENTS.md §4.5).

### Task 4.3 — Page rendering widget and dialog

Plain `QWidget` painting the 40×25 Level 1 grid — monospace
alphanumerics, painted 2×3 mosaic cells, Level 1 attributes (colours,
double height; flash static or ignored) per
[EN 300 706 §9.3](analogue-video-specifications/docs/teletext/ETSI-EN-300-706-2003/ETSI-EN-300-706-2003.md)
Level 1 semantics. Dialog owns a `TeletextPageDecoder` and the
trailing-frame-window cache of design §7 (feed packets in temporal order,
surface "page seen at frame N"). Initial controls: page-number entry +
render only; hold/reveal/subpage cycling deferred.

**Acceptance criteria**

- Known synthetic pages (from Task 3.2 fixtures) render cell-accurately, verified via the page-view model rather than pixel comparison where practical.
- Tier 3 offscreen smoke test constructs, shows, and closes the dialog (`QT_QPA_PLATFORM=offscreen`).

### Task 4.4 — Previewer integration

Wire the dialog as an **observer dialog** (design §7), following the
`VBIDialog` / `NtscObserverDialog` pattern exactly: entry in the preview
window's **Observers** menu ([../orc/gui/previewdialog.cpp](../orc/gui/previewdialog.cpp),
alongside VBI Decoder / NTSC Observer / Video Parameters) emitting a new
`showTeletextDialogRequested` signal; dialog owned by `MainWindow` (not
`PreviewDialog`) with `onShowTeletextDialog()` and
`updateTeletextDialog()` slots; refresh from
`updateAllPreviewComponents()` guarded by `isVisible()`; field IDs via
`getFrameFields()`.

**Acceptance criteria**

- Menu entry appears under the Observers menu, not the View menu; dialog ownership and signal wiring match the `showVBIDialogRequested` pattern in `mainwindow.cpp`.
- Dialog follows frame changes in both frame and field preview modes; sequential playback degrades to live-reception behaviour as designed.
- Full GUI test suite green: `QT_QPA_PLATFORM=offscreen ctest -L gui`; `ctest -R MVPArchitectureCheck` green.
