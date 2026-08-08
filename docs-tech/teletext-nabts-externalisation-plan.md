# Teletext and NABTS: removing them from the SDK and the host

The `teletext_sink` and `nabts_sink` stages are to become external plugins, distributed through the
plugin registry rather than bundled. Two things stand in the way, and they are not the same thing:

1. **~4 500 lines of format-specific decoding sit in the public plugin SDK**, and part of it sits in
   the ABI-frozen tier where a field addition breaks every plugin in the ecosystem.
2. **The host GUI has a hardcoded viewer for each of them.** `StageToolProvider` gets a plugin's
   tool *listed*; it gives the host no way to *draw* the result. Every non-trivial viewer needs a
   host-side special case, so an external plugin cannot bring its own presentation.

(1) is a straightforward relocation with no ABI break and no behaviour change. (2) needs a new SDK
contract, and the shape of that contract is the interesting part of this document.

---

## 1. What is in the SDK today

### 1.1 Support tier — `orc/sdk/include/orc/support/` and `orc/sdk/src/`

| Header | Header + impl lines | What it is |
|---|---|---|
| `teletext_slicer.h` | 646 + 1813 | WST data-line slicer → 42-byte (625) / 34-byte (525) / 33-byte (NABTS) packets |
| `teletext_page_decoder.h` | 529 + 1057 | Level 1 page decoder → `TeletextPageSnapshot`, subtitle cues, Hamming 8/4 and parity helpers |
| `teletext_recovery_stats.h` | 252 + 508 | Recovery-outcome accumulator for the diagnostic profile |
| `teletext_row_squasher.h` | 235 + 253 | Combines repeated copies of a page row into one best estimate |
| `nabts_page.h` | 440 + 390 + 76 | `NabtsPageSnapshot` NAPLPS display list, plus `nabts_captions.cpp` |

All five compile into the `orc-sdk-support` static library (`orc/sdk/CMakeLists.txt:63-79`) and are
installed as public SDK headers. They hold five entries in `orc/sdk/sdk_headers.yaml`, five lines in
`cmake/sdk_header_allowlist.txt` (82, 85-88), and rows in the `docs/technical/plugin-sdk.md` header
tables — all three kept in sync by the `SdkHeaderManifestSync` CTest.

### 1.2 Stage tier — `orc/stage/analysis_sink_results.h`

This is the part that matters most. The file is **stage-tier**, which `sdk_headers.yaml:10-12`
documents as *"stage contract types crossing the plugin boundary… Layout changes bump the ABI."*
Lines 15-16 pull `nabts_page.h` and `teletext_page_decoder.h` into that ABI-frozen include graph, and
lines 54-339 define:

* `TeletextCataloguedSubPage`, `TeletextCataloguedPage`, `TeletextRecoverySummary`,
  `TeletextAnalysisDataset`, `ITeletextAnalysisResults`
* `NabtsRecordFunction`, `NabtsCataloguedRecord`, `NabtsRecoverySummary`, `NabtsAnalysisDataset`,
  `NabtsCaptionCue`, `INabtsAnalysisResults`
* the free function `nabts_caption_cues()`

alongside three unrelated interfaces — `IDropoutAnalysisResults`, `ISNRAnalysisResults`,
`IBurstLevelAnalysisResults` — which need nothing but `common_types.h`.

### 1.3 Host side

Not SDK, but it exists only to serve the SDK types:

| Component | Lines |
|---|---|
| `orc/view-types/orc_teletext.h` | 234 |
| `orc/view-types/orc_nabts.h` | 448 |
| `orc/presenters/{include,src}/teletext_analysis_presenter.*` | 219 |
| `orc/presenters/{include,src}/nabts_analysis_presenter.*` | 627 |
| `orc/gui/teletextdialog.{h,cpp}` | 934 |
| `orc/gui/teletextpagewidget.{h,cpp}` | 373 |
| `orc/gui/nabtsdialog.{h,cpp}` | 895 |
| `orc/gui/nabtscanvaswidget.{h,cpp}` | 720 |

plus dedicated request types, signals and handlers in `orc/gui/render_coordinator.h:74-75, 213-231,
551-552, 789-807, 1079-1098, 1296-1298`.

---

## 2. Why it is there — three reasons, one load-bearing

**(a) The host needs the types to read the result.** `render_presenter.cpp:2748` and `:2771`
`dynamic_cast` the DAG node's stage pointer to `ITeletextAnalysisResults` / `INabtsAnalysisResults`.
Compiling that cast needs the complete definitions of `TeletextPageSnapshot` and `NabtsPageSnapshot`,
so the decoder headers were dragged into the stage-tier header. This is the real coupling, and it is
what §4 onwards is about.

**(b) Sharing between two bundled plugins.** NABTS rides the same NRZ line-slicing machinery as WST
— `0xE7` framing instead of `0xE4`, otherwise the same clock run-in, bit rate, lines, levels and bit
order — so `nabts_sink` includes `<orc/support/teletext_slicer.h>` at `nabts_frame_slicer.h:16`,
`nabts_packet.h:14`, `nabts_scan_state.h:14` and `nabts_record.cpp:13`, and the page decoder's
Hamming 8/4 helper at `nabts_packet.cpp:12`. Genuine reuse, but it is plugin↔plugin reuse, which does
not need a public host SDK — `orc/plugins/stages/common/` already exists for exactly this.

**(c) Tests.** `orc-tests/core/unit/support/` tests the decoders directly (`CMakeLists.txt:458-463`),
and the `vbi_source` functional tests use the slicer to prove the synthesised VBI lines round-trip.
Note that `vbi_source` itself does **not** use the SDK decoders — `vbi_teletext_service.h` is
self-contained. It is a test-only dependency.

---

## 3. Assessment: is it required?

**No.** Nothing here is generic plugin infrastructure. It is format-specific decoding for two
specific stages, consumed by exactly those two plugins, the host presenters, and the tests. Zero
third-party value.

The genuinely harmful part is `analysis_sink_results.h` being stage-tier. Adding a field to
`TeletextCataloguedPage` — or to `TeletextPageSnapshot` two headers down — is an ABI break for every
plugin in the ecosystem, including ones that have never heard of teletext. `abi_history.yaml` already
records two such bumps (abi 12 and 13) driven entirely by teletext and NABTS catalogue changes. The
current host ABI is **13**.

A second, smaller point: the header's comment at lines 24-28 justifies the cross-DSO `dynamic_cast`
by saying these interfaces are *"defined in orc-core, which is always a single shared symbol source"*.
That has been stale since the ABI-tiering work — they are header-only abstract classes in the SDK
now, so the cast under `RTLD_NOW | RTLD_LOCAL` (`stage_plugin_loader.cpp:224`) relies on the
runtime's typeinfo-name fallback rather than on a single definition. It works on libstdc++; it is the
exact fragility the comment was written to avoid, and the comment should be corrected or the claim
made true again.

---

## 4. What actually blocks externalisation

`orc/gui/mainwindow.cpp:3920-3927` hardcodes the contract IDs:

```cpp
const bool is_teletext_analysis_tool =
    tool_info.contract_id == "decode-orc.stage-tools.teletext-analysis.v1" ||
    tool_info.id == "teletext_analysis";
const bool is_nabts_analysis_tool =
    tool_info.contract_id == "decode-orc.stage-tools.nabts-pages.v1" ||
    tool_info.id == "nabts_analysis";
```

and branches at `:4457` and `:4531` into the bespoke dialogs. The generic path
(`generic_analysis_dialog.cpp`) handles chart-shaped results only, and it too has an id ladder at
`:145-151`.

So the host must already know how to draw a plugin's output before the plugin can ship. That is the
constraint to remove.

---

## 5. The seam that is already half-built

`orc/view-types/orc_nabts.h` is already a **resolved, renderer-agnostic display list in unit space**
— colours as RGB, code positions as Unicode or sixels, geometry in the unit Cartesian space of
X3.110 §5.3.1, no interpreter state needed. `NabtsPageSnapshot` in the SDK is very nearly the same
structure. `nabts_analysis_presenter.cpp` is 554 lines converting one into the other. The teletext
pair is the same story at smaller scale: `TeletextPageSnapshot` → `TeletextPageCellView` is a cell
grid on both sides.

There are, in other words, two near-duplicate presentation models straddling a presenter boundary
that exists mainly because one side is labelled "SDK" and the other "host". Collapse the duplication
and what is left is a *generic* seam:

* a **character-cell grid** (teletext, and any Level 1 service)
* a **2D display list** (NAPLPS, and any vector presentation)

Neither is a teletext or NABTS concept. A WSS listing, VITC, VPS or a Datacast viewer would reuse
one or the other, which is what makes this worth building rather than special-casing twice more.

---

## 6. Options considered and rejected

**Plugin rasterises into `PreviewImage` via `IStageCustomPreviewRenderer`.** Superficially the
cheapest: the plugin hands over RGB888 and the host shows it in a generic viewer. Rejected because
`nabtscanvaswidget.cpp:527-571` leans on `QFontDatabase` fixed fonts and `QPainterPath` arc and
spline construction (`:159`, `:220`, `:424-430`). Reimplementing that in the plugin means shipping a
font and a path rasteriser, and the result would look worse than what is there now.

**Plugin ships its own Qt UI.** Rejected outright. The SDK deliberately keeps Qt out of the plugin
contract, and cross-DSO Qt version locking would make every plugin a build-matrix problem.

**Leave the viewers in the host and externalise only the decoders.** Viable as an interim state, and
Phase 1 delivers exactly that. Rejected as a destination because the host would keep shipping
bespoke teletext and NABTS UI for plugins it no longer ships, and the `contract_id` ladder would keep
growing with every new service.

---

## Phase 1 — Take the format decoders out of the SDK

No ABI break, no behaviour change, entirely in-tree. Worth doing on its own merits even if the rest
is deferred: it removes the ABI-bump landmine described in §3.

### Task 1.1 — Create `orc/plugins/stages/common/vbi-services/`

Follow the `audio-resample` and `efm-decode` precedent — `orc/plugins/stages/common/CMakeLists.txt`
already states these are *"NOT part of the SDK contract: they compile against the public SDK headers
only and are linked privately into the plugins that need them."*

Move, unchanged:

```
orc/sdk/include/orc/support/teletext_slicer.h          → common/vbi-services/teletext_slicer.h
orc/sdk/include/orc/support/teletext_page_decoder.h    → common/vbi-services/teletext_page_decoder.h
orc/sdk/include/orc/support/teletext_recovery_stats.h  → common/vbi-services/teletext_recovery_stats.h
orc/sdk/include/orc/support/teletext_row_squasher.h    → common/vbi-services/teletext_row_squasher.h
orc/sdk/include/orc/support/nabts_page.h               → common/vbi-services/nabts_page.h
orc/sdk/src/teletext_slicer.cpp                        → common/vbi-services/teletext_slicer.cpp
orc/sdk/src/teletext_page_decoder.cpp                  → common/vbi-services/teletext_page_decoder.cpp
orc/sdk/src/teletext_recovery_stats.cpp                → common/vbi-services/teletext_recovery_stats.cpp
orc/sdk/src/teletext_row_squasher.cpp                  → common/vbi-services/teletext_row_squasher.cpp
orc/sdk/src/nabts_page.cpp                             → common/vbi-services/nabts_page.cpp
orc/sdk/src/nabts_captions.cpp                         → common/vbi-services/nabts_captions.cpp
```

New target `orc-vbi-services`, `PUBLIC` include directory `${CMAKE_CURRENT_SOURCE_DIR}/..` so
consumers write `#include "vbi-services/teletext_slicer.h"` and the provenance is visible at every
include site, matching the `audio-resample` comment. Link `orc-plugin-sdk` `PUBLIC`.

Delete the six source entries from `orc/sdk/CMakeLists.txt:63-79`.

### Task 1.2 — Split `analysis_sink_results.h`

`orc/stage/analysis_sink_results.h` keeps only `IDropoutAnalysisResults`, `ISNRAnalysisResults` and
`IBurstLevelAnalysisResults`, and drops the `nabts_page.h` / `teletext_page_decoder.h` includes.
Everything else moves to `orc/plugins/stages/common/vbi-services/vbi_analysis_results.h`, which the
two plugins and (for now) the host presenters include directly.

Both interfaces stay abstract classes cast to via `dynamic_cast` — Phase 1 changes only *where the
header lives*, not how the host reaches the data. The host's include path for a non-SDK plugin
header is the same arrangement `orc-tests/core/unit/CMakeLists.txt:25-26` already uses.

Correct the stale cross-DSO comment at lines 24-28 while the file is open.

### Task 1.3 — Update the manifest, allowlist and docs

* Delete the five `orc/support/{teletext_*,nabts_page}.h` entries from `orc/sdk/sdk_headers.yaml`.
* Regenerate `cmake/sdk_header_allowlist.txt` with `tools/gen_sdk_header_allowlist.sh`.
* Update the `docs/technical/plugin-sdk.md` header tables.
* `SdkHeaderManifestSync` (`CMakeLists.txt:116`) must pass; it is the gate that proves all three
  stayed in step.

### Task 1.4 — Rewire the consumers

Plugins (mechanical include-path change):

* `teletext_sink`: `teletext_squash_stats.{h,cpp}`, `teletext_frame_slicer.h`, `teletext_scan_state.h`,
  `teletext_sink_deps.{h,cpp}`, `teletext_sink_deps_interface.h`, `teletext_page_catalogue.h`,
  `teletext_sink_stage.cpp`
* `nabts_sink`: `nabts_sink_deps_interface.h`, `nabts_sink_deps.cpp`, `nabts_packet.{h,cpp}`,
  `nabts_record.cpp`, `nabts_frame_slicer.h`, `nabts_scan_state.h`, `naplps_pdi.h`,
  `naplps_interpreter.h`, `naplps_state.h`

Both plugin `CMakeLists.txt` gain `orc-vbi-services` as a private link.

Host: `orc/presenters/include/{teletext,nabts}_analysis_presenter.h`, `render_presenter.{h,cpp}`,
`orc/gui/teletextdialog.cpp`. These now include a plugin-side header, which is deliberately ugly —
it is the dependency Phase 2 exists to break, and making it visible is useful.

### Task 1.5 — Rewire the tests

* Move `orc-tests/core/unit/support/{teletext_slicer,teletext_page_decoder,teletext_recovery_stats,
  teletext_row_squasher,nabts_page,nabts_repertoire}_test.cpp` and `teletext_line_synthesizer.h` from
  the `support/` group to a `common/vbi-services/` group; link `orc-vbi-services`
  (`orc-tests/core/unit/CMakeLists.txt:457-463`).
* `orc-tests/core/functional/CMakeLists.txt:141-149` already compiles teletext sink sources directly
  into the vbi_source functional test; add the `orc-vbi-services` link there and to the nabts_sink
  and teletext_sink functional targets.
* `orc-tests/gui/unit/{teletext,nabts}_analysis_presenter_test.cpp` follow the presenters.

### Task 1.6 — Verify the SDK is genuinely smaller

Build with `ORC_SDK_DEPRECATED_INCLUDE_SHIMS=OFF` (the `build-noshims` preset) and confirm the
`BundledTieredIncludes` and `check_plugin_private_includes.sh` gates still pass. Confirm the
standalone SDK tarball CI job no longer ships the five headers.

**Exit criteria:** ~4 500 lines and 5 entries gone from the public SDK surface;
`analysis_sink_results.h` no longer reaches any format decoder; ABI still 13; full suite green.

---

## Phase 2 — A generic catalogue-results contract — **done**

One new stage-tier header replacing two format-specific ones, plus one generic dialog replacing two
bespoke ones. This is the phase that unblocks any future service viewer, not just these two.

**As built, Phase 2 also does what Task 3.1 described** — both sinks implement the new contract, so
the generic path is live end to end and Phase 2's own exit criteria are met. Phase 3 is now deletion
and the caption-cue seam only.

### Task 2.1 — `orc/stage/tooling/catalogue_results.h`

The contract has three parts.

**The catalogue** — what fills the item list:

```cpp
struct CatalogueColumn { std::string id, title; bool numeric; };
struct CatalogueItem {
  std::string id;                    // opaque, unique within one dataset
  std::string parent_id;             // empty = top level; else a variant of that item
  std::vector<std::string> values;   // one per schema column
  std::vector<std::string> badges;   // "subs" — appended to the first column
  bool selectable;
  std::string tooltip;               // why it is unselectable, what a badge means
  std::string find_key;              // what the reader types to reach it
  std::string variant_label;         // "0002", for the variant stepper
};
struct CatalogueSchema {             // columns + the chrome they imply
  std::vector<CatalogueColumn> columns;
  std::string item_noun, variant_noun;          // "Page", "Sub-page"
  std::string find_label, find_placeholder;     // empty hides the find box
  std::string highlight_label;                  // empty hides the damage toggle
  std::string empty_message;
};
struct CatalogueSummary { std::string headline; std::vector<std::string> notices; };
```

`parent_id` rather than the `parent_path` vector the first draft proposed: one level of nesting is
what a carousel of variants needs, and it keeps the host at a table plus a stepper rather than a
tree. `TeletextCataloguedPage`/`TeletextCataloguedSubPage` map onto parent and variant directly;
`NabtsCataloguedRecord` maps onto a flat item list.

**The payload** — what fills the viewer pane, four renderable forms:

* `CatalogueCellGrid` — the character grid, promoted from `orc_teletext.h`'s `TeletextPageCellView`
  (mosaic bits, double height, flash, conceal, boxed, damaged). Nothing in it is teletext-specific:
  the display palette and the nominal character-rectangle aspect travel *with the payload*, so a
  service with different colours or a different cell shape needs no host change.
* `CatalogueDisplayList` — promoted from `orc_nabts.h`: operations in unit Cartesian space, resolved
  RGB with a transparent flag, Unicode text runs with an explicit advance, sub-element mosaics,
  downloadable glyph bitmaps and programmable fill masks.
* `CatalogueTextDocument` — plain or monospaced text, for a function listing.
* `CatalogueTable` — columns and rows, for a cue track.

Plus the readouts that go around it: `companion_text` (the text form beside a visual payload),
`headline` and `condition`.

**The interface** — `ICatalogueResults::catalogue()` returns the whole `CatalogueDataset` at once,
mirroring the `dataset()` shape the analysis sinks already use, rather than the per-item `payload()`
call the first draft proposed. Both sinks build it lazily under a mutex on first ask and cache it:
resolving every sub-page into a cell grid, or every record into a display list, is real work that a
run which only exports a packet stream never needs.

`StageToolKind::CatalogueBrowser` is appended to the enum (so released values do not move) and
`kCatalogueBrowserContractId` is `decode-orc.stage-tools.catalogue.v1`.

**Not built: `exports()`.** The first draft assumed the dialogs had `.t42` and `.srt` export
buttons to generalise. They do not — export is a stage *parameter* (`export_subtitles`,
`subtitle_page`, `subtitle_format`), written during the trigger. Adding an export surface to the
contract would have been inventing UI, so it is left for whenever a viewer actually needs one.

**ABI stays 13.** The branch is unreleased, so the additions and the `analysis_sink_results.h`
removals are folded into the existing abi 13 entry rather than given one of their own.

### Task 2.2 — `CatalogueDialog` and two renderers

One host dialog (`orc/gui/cataloguedialog.{h,cpp}`): find box, item table with schema-driven columns,
item stepper, standing notices, payload stack, variant stepper, damage toggle, summary and a status
bar carrying the headline and condition. Every piece of chrome is shown only when the schema asks
for it.

Two payload renderers behind the stack:

* `CatalogueCellGridWidget` — the drawing half of `teletextpagewidget.cpp`, unchanged in substance
  including the two-pass background/foreground order and the glyph squeeze, with the fixed teletext
  palette replaced by the payload's own.
* `CatalogueDisplayListWidget` — the drawing half of `nabtscanvaswidget.cpp`, unchanged in substance
  including `arc_through()` and `spline_through()`.

Both keep their Qt font and `QPainterPath` use, which is the whole reason for choosing a display-list
contract over plugin-side rasterisation (§6).

### Task 2.3 — Coordinator plumbing

`GetTeletextAnalysisData` and `GetNabtsAnalysisData` collapse into one `GetCatalogueData` request,
one `catalogueDataReady` signal and one `catalogueProgress` signal. The fetch-or-trigger behaviour
carries over unchanged, as do the per-node dialog and progress-dialog maps, which become one each.
`IRenderPresenter` loses its two format-specific getters for one `getCatalogueData()`.

`mainwindow.cpp` routes on `orc::kCatalogueBrowserContractId` (or the `catalogue_browser` kind
string) and takes the window title from the tool's own display name, so "Teletext Pages" and "NABTS
Records" still title their windows without the host knowing what either is.

### Task 2.4 — Tests

`orc-tests/gui/unit/catalogue_dialog_test.cpp` — 26 tests over the dialog and both renderers, driven
by hand-built datasets in the two shapes the sinks produce. `render_coordinator_test.cpp`'s two
parallel blocks collapse into one catalogue block. A contract test in
`stage_registry_contract_test.cpp` asserts that every stage advertising
`StageToolKind::CatalogueBrowser` carries the right contract id and is castable to
`ICatalogueResults` — the two things the host's routing depends on and neither compiler-checked.

**Exit criteria (met):** the two sinks open their viewers through the generic path; no teletext or
NABTS contract string appears in `mainwindow.cpp`.

### Task 2.5 — The per-page lost-packet count (follow-up)

`TeletextPageRecoveryView::lost_packets` had been declared and never populated, which left the red
row-gap banding in the teletext overlay unreachable and the "packets lost" clause of the recovery
readout dead. Phase 2 preserved that; this task wires a real count.

**What does not work.** The obvious derivation — the page's transmission extent against the run's
usual packets-per-field, the same calibration
`TeletextRecoverySummary::lost_packets_estimate` uses — is unsound. A service inserting on several
VBI lines interleaves its magazines, so the packets in one field belong to several different pages
and a field that came back short is short for whichever page that packet belonged to, which is not
knowable. Charging the shortfall to every page whose extent covers the field double-counts badly:
measured on the 625-line reference capture it accused **94 of 107 sub-pages** and attributed 1008
lost packets against a run-wide estimate of 413.

**What does work.** The row copies. A row the carousel brought round `times_seen` times should have
arrived `times_seen` times, and `TeletextPageSnapshot::row_copies` says how many copies were
actually combined; the shortfall is what the recording lost *of that page*, with no overlap to
double-count. Rows that never arrived at all are excluded — a row with no copies is either one the
service never sent (which most pages do, to space themselves out) or one lost every single time, and
nothing can tell those apart. On the same capture this reports **3 of 107 sub-pages** and 10 lost
packets.

`teletext_subpage_lost_packets()` lives beside the catalogue; the recovery pass applies it only when
the row squasher was attached, because without repeats to compare there is one copy per received row
whatever the carousel did and every page seen twice would look half lost.

**The limitation, stated plainly.** The figure is a floor, and it is silent exactly where a gap is
most likely to be a loss: a page seen once has nothing to compare against, so it reports zero and
the banding stays off. It speaks on a recording long enough for the carousel to go round, which is
the case the viewer is for. Under-reporting is the right way to be wrong here — a figure that
accused pages which had arrived whole would train the reader to ignore the marks, which is worse
than not marking at all.

A functional assertion on both reference captures guards the property that matters: fewer than half
the catalogue accused. On the 625-line capture it reports 3 of 107 sub-pages and 10 lost packets;
on the noisier 525-line one, 11 of 57 and 534.

The run-wide estimate is printed beside it for context but deliberately not compared against — the
two count different things and neither bounds the other. The run-wide figure is the shortfall of
fields that carried *something*, and leaves out fields that yielded nothing at all (a service
inserting into one field of each frame would otherwise show a loss in every other field). The rows
those empty fields would have carried are missing from their pages all the same, which is why the
per-page total runs above the run-wide one on the 525-line capture.

## Phase 3 — Port the plugins and delete the host format code

### Task 3.1 — Implement `ICatalogueResults` in both sinks

Each stage builds the catalogue and payloads from its own dataset. The conversion logic is the two
presenters, moved across the boundary: `teletext_analysis_presenter.cpp` (170 lines) and
`nabts_analysis_presenter.cpp` (554 lines) become plugin-side code with their host include
dependency gone.

`ITeletextAnalysisResults` and `INabtsAnalysisResults` are dropped from the stages; the datasets
become plugin-private types with no cross-DSO exposure at all, which is what removes the typeinfo
fragility of §3 for these two stages.

### Task 3.2 — Resolve `nabts_caption_cues()`

Currently shared so the plugin's SubRip export and the host's caption track cannot disagree
(`analysis_sink_results.h:320-332`, used at `nabts_sink_deps.cpp:270` and
`nabts_analysis_presenter.cpp:474`). Once the plugin is external, sharing a *function* is no longer
available; the plugin must emit the cues as data.

The plugin already computes them for its own SRT export. Have it publish them through the existing
caption-cue path so the host consumes cues rather than deriving them — the same arrangement the
EIA-608 closed-caption route uses, where the host reads a typed observation and never touches the
decoder. Verify the SRT the plugin writes and the caption track the host builds remain byte-identical
across the change; that equivalence is the reason the function was shared in the first place.

### Task 3.3 — Delete

| Deleted | Lines |
|---|---|
| `orc/view-types/orc_teletext.h`, `orc_nabts.h` | 682 |
| `orc/presenters/**/{teletext,nabts}_analysis_presenter.*` | 846 |
| `orc/gui/teletextdialog.*`, `teletextpagewidget.*` | 1307 |
| `orc/gui/nabtsdialog.*`, `nabtscanvaswidget.*` | 1615 |
| `render_presenter.cpp:2733-2779`, coordinator handlers, `mainwindow.cpp` branches | ~250 |

net of the ~900 lines of drawing code that moved into the two generic renderers in Task 2.2.

**Exit criteria:** `grep -ril "teletext\|nabts" orc/gui orc/presenters orc/view-types` returns
nothing.

---

## Phase 4 — Move the plugins out of tree

### Task 4.1 — New repository

`teletext_sink`, `nabts_sink` and `orc-vbi-services` move to their own repository, building against
the installed `decode-orc-plugin-sdk` package with `find_package` + `orc_add_stage_plugin`, and
shipping `orc-plugin-manifest.yaml` with the ABI and toolchain tag. Their unit and functional tests
move with them.

### Task 4.2 — Host-side removal

* `orc/plugins/stages/CMakeLists.txt:28-29`
* `orc-tests/core/unit/include/public_stage_inventory.h:144-151`
* the `teletext_sink` / `nabts_sink` groups in the three test `CMakeLists.txt` files
* `orc/plugins/stages/common/vbi-services/` (moved, not deleted)

**Keep** `orc/core/project.cpp:621-654`, the legacy `teletext_analysis_sink` → `teletext_sink`
project migration. Existing projects still name the stage, and a project referencing an uninstalled
external plugin must fail with "plugin not installed", not with a mangled stage name.

### Task 4.3 — Registry entries

Add both to `orc-plugin-registry/index.yaml`, subject to the `PluginIndexValidation` gate. Users
install them from the GUI Browse dialog or `orc plugins install`.

---

## 7. Risks and open questions

**The migration path for existing projects.** A project containing a `teletext_sink` node opened on a
host without the plugin installed. The registry Browse dialog exists, but "this project needs a
plugin you don't have, install it?" is not currently a flow. Worth designing before Phase 4 ships,
not after.

**Payload size across the boundary.** `payload()` returns by value and a NAPLPS display list for a
dense page is not small. Measure before committing to the signature; if it matters, return a
`shared_ptr<const CataloguePayload>` and note the lifetime rule — the "never retain a pointer past
the call" trap that bit `transform3d` applies here too.

**Two payload forms may not be enough.** The contract should be additively extensible: a new payload
kind must not renumber the existing ones. Reserve the tag space accordingly.

**Phase 2 is the only ABI bump.** Phases 1, 3 and 4 are ABI-neutral. If Phase 2 needs to coincide
with other ABI work, it should — a single bump to 14 carrying several changes is cheaper for plugin
authors than three.

**Phase 1 stands alone.** If the rest is deferred indefinitely, Phase 1 is still the right change: it
removes format-specific decoding from a public SDK and stops teletext catalogue changes from breaking
unrelated plugins. Phases 2-4 should not be a precondition for it.
