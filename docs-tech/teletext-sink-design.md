# Teletext Sink and Preview Design

Design for extracting World System Teletext (WST) data from the VBI of
decoded **PAL (625-line System B)** video, sinking it as a T42 packet
stream, and (later) previewing decoded pages in the GUI and exporting
teletext subtitles. PAL WST is the explicit and deliberate scope of this
design; other teletext systems are surveyed in §1.3 and excluded.

The design is **source-agnostic**: the observer and sink consume the
`VideoFrameRepresentation` arriving through the DAG, so they work
identically downstream of `tbc_source` (ld-decode *and* vhs-decode `.tbc`)
and `cvbs_source` — LaserDisc, tape, or direct CVBS captures. Teletext was
recorded by consumer VHS machines, so tape sources are a first-class input;
the signal-quality implications are covered in §2.4.

Addresses [issue #58](https://github.com/simoninns/decode-orc/issues/58)
("No Teletext observer or sink" — motivated by the British Garden Birds
LaserDisc, which carries teletext the project cannot currently extract).

---

## 1. Scope

### 1.1 Goals

All goals are scoped to **PAL WST** (ITU-R BT.653 System B, 625-line, as
specified by ETSI EN 300 706):

1. **Teletext observer** (`teletext`) — slices teletext data lines from the
   VBI of each field and records the recovered 42-byte packets as
   observations, exactly as `closed_caption` does for EIA-608.
2. **Teletext sink stage** (`teletext_sink`) — a standard sink plugin that
   runs the observer over the frame range and writes a `.t42` packet stream
   verifiable with external tools (vhs-teletext `vbiview`, wxTED, etc.).
3. **Teletext page preview dialog** (follow-on) — a modeless GUI dialog that
   renders basic (Level 1) pages and follows the frame previewer.
4. **Teletext subtitle export** (follow-on) — decode subtitle pages to timed
   text, mirroring the closed-caption path (`cc_sink` SCC/plain-text export,
   `video_sink` `mov_text` embedding).

### 1.2 Non-goals

- **Any teletext system other than PAL WST** — see §1.3. This includes
  every 525-line system (NABTS / System C, Japanese System D, 525-line
  System B) and the French System A. The sink is
  `VideoFormatCompatibility::PAL_ONLY`; NTSC data services on line 21 are
  already covered by the closed-caption path.
- Teletext Level 2.5/3.5 presentation (X/26, X/28, M/29 enhancements are
  *captured* in the T42 stream but not *rendered* by the preview).
- Deconvolution-grade signal recovery (vhs-teletext style). The initial
  slicer targets full-bandwidth sources (LaserDisc, broadcast-quality CVBS
  captures); severely band-limited VHS material may need a stronger
  recovery path later (§2.4). The slicer is a non-ABI support-tier
  component and the T42 output format is producer-independent, so better
  slicers can be substituted later without changing any contract.
- Full-field (non-VBI) teletext transmissions (ETSI EN 300 706 clause F.6).
  The candidate line window is parameterized, so this is a configuration
  change, not a design change, if ever needed.

### 1.3 Why PAL WST only (other teletext systems)

ITU-R BT.653-3 (vendored in the specifications submodule) defines four
teletext systems, most in both 625- and 525-line variants:

| System | Region | Data line | Bit rate | Protection |
|---|---|---|---|---|
| A | France (Antiope) | 320 bits | 6.203 Mbit/s (625) | Hamming |
| B (WST) | UK/Europe — **this design** | 360 bits / 45 bytes | 6.9375 Mbit/s (625) | Hamming 8/4 + odd parity |
| C (NABTS) | North America | 288 bits | 5.727272 Mbit/s (525) | Hamming + suffix FEC |
| D | Japan | 296 bits / 37 bytes | 5.727272 Mbit/s (525) | majority-logic (272,190) code |

This design targets System B on PAL exclusively, because:

- The motivating source ([issue #58](https://github.com/simoninns/decode-orc/issues/58))
  is a UK LaserDisc carrying WST, and the wider decode-orc input base for
  teletext — UK/European VHS recordings and CVBS captures — is WST
  territory as well.
- The external verification ecosystem is WST-only: neither vhs-decode nor
  tbc-tools decodes teletext at all (they delegate to ali1234's
  vhs-teletext, which handles only 625-line WST), and the `.t42`
  interchange format itself is WST-specific — 42-byte System B packets.
  A System A/C/D sink would have no established output format and no
  external tool to validate against.
- The content layers differ radically (System D: majority-logic FEC,
  two-byte JIS Kanji sets, bitmap pattern transmission), so nothing below
  the observation schema is reusable across systems anyway.

Consequences for naming and future extension, decided here:

- The observer id, stage name, and observation namespace stay the generic
  `teletext`, **documented as System B / PAL-restricted** (this section,
  the observation schema, and the stage's `instructions.md`). Support for
  another system would be additive — a sibling slicer class, an extended
  or sibling observer, and a separate sink stage with its own output
  format — without breaking any `teletext`-named contract.
- The parts of this design that would carry over to other systems: the
  per-line hex-packet observation schema (§3.2), the candidate-window
  parameterization (§3.1), and the correlation-based acquisition machinery
  of the slicer (§4.1). The parts that would not: the 42-byte extraction,
  the T42 container, the page decoder, and the preview renderer.

---

## 2. Background

### 2.1 Signal characteristics (authoritative spec references)

All references are vendored in
[docs-tech/analogue-video-specifications/docs/teletext/](analogue-video-specifications/docs/teletext/):
ETSI EN 300 706 V1.2.1 (primary), ITU-R BT.653-3, and the joint
BBC/IBA/BREMA Broadcast Teletext Specification (1976).

| Property | Value | Reference |
|---|---|---|
| Line allocation (625/50) | lines 6–22 (field 1) and 318–335 (field 2) may carry teletext | EN 300 706 §4.1 |
| Modulation | binary NRZ | EN 300 706 §5.1 |
| Data levels | 0 = black level ±2 %; 1 = 66 ±6 % of black-to-white | EN 300 706 §5.2 |
| Bit rate | 444 × fH = 6.9375 Mbit/s ±25 ppm | EN 300 706 §5.3 |
| Clock run-in | 16 bits `1010101010101010` (transmission order) | EN 300 706 §6.1 |
| Framing code | 8 bits `11100100` (transmission order) = 0xE4 | EN 300 706 §6.2 |
| Timing reference | data starts nominally 12.0 µs after sync leading edge | EN 300 706 §6.3 |
| Packet size | 360 bits = 45 bytes (run-in 2 + framing 1 + MRAG 2 + data 40), LSB first | EN 300 706 §7.1 |
| MRAG | 2 bytes, four Hamming 8/4 nibbles → 3-bit magazine + 5-bit packet number | EN 300 706 §7.1.2 |
| Practical caveat | lines 6, 318, 319 not decoded by some legacy receivers | EN 300 706 §F.4 |

At the decode-orc PAL sample rate (`kPalSampleRate` = 17,734,475 Hz,
[cvbs_signal_constants.h](../orc/sdk/include/orc/stage/cvbs_signal_constants.h)),
one teletext bit spans **≈ 2.556 samples** (17734475 / 6937500). This is the
central engineering constraint: every existing VBI slicer in the tree
(closed caption at 32 × fH ≈ 35.5 samples/bit, LaserDisc biphase at ~2 µs
cells) works on `vbi_utils::get_transition_map()`, whose 4-sample debounce
would destroy teletext transitions. **Teletext needs its own slicer**
(§4.1); `vbi_utilities.h` is not reusable here.

### 2.2 T42 packet stream format

Per the [zxnet teletext wiki](https://teletext.wiki.zxnet.co.uk/wiki/T42_packet_stream):

- A `.t42` file is a flat sequence of **42-byte packets**: the 45-byte
  transmission packet minus clock run-in and framing code (2-byte MRAG +
  40 data bytes).
- Bytes keep their **transmission coding** — Hamming 8/4 on addressing
  bytes, odd parity on display bytes. No decoding or correction is applied
  by the producer.
- There is no header, no timing, and no mandated structure; consumers
  decode it as a receiver decodes a live transmission.
- vhs-decode convention (adopted here as an option): with "keep empty"
  enabled, every candidate VBI line emits a packet — 42 zero bytes when no
  valid teletext was found — so packet position maps 1:1 back to
  (frame, field, line).

### 2.3 The existing closed-caption pattern (the template)

The closed-caption pipeline is the architectural template this design
copies:

- [closed_caption_observer.cpp](../orc/core/observers/closed_caption_observer.cpp)
  — host observer; slices line 21/22 per field, stores
  `closed_caption.{present,data0,data1,parity0_valid,parity1_valid}` keyed
  by `FieldID(frame_id*2 + field_idx)`.
- [cc_sink](../orc/plugins/stages/cc_sink/) — sink plugin
  (`DAGStage + ParameterizedStage + TriggerableStage`, deps-interface test
  seam); obtains the observer via
  `orc::plugin::get_observation_service()->create_observer("closed_caption")`,
  loops the frame range, skips frames already covered by the host's
  provenance store, reads observations, writes the output file.
- [video_sink](../orc/plugins/stages/sinks/common/video_sink_stage.cpp)
  (`embed_closed_captions`) — runs the same observer, feeds byte pairs to
  the SDK support-tier
  [EIA608Decoder](../orc/sdk/include/orc/support/eia608_decoder.h), muxes
  the resulting cues as `mov_text`.
- GUI observer dialogs — per-frame observation delivery through
  `RenderPresenter::requestObservations()` / `RenderCoordinator`, extracted
  into value-type view models by a presenter.

Teletext maps onto every one of these seams; the differences are the slicer
(§2.1) and the payload shape (up to 16 packets × 42 bytes per field instead
of 2 bytes).

### 2.4 Source considerations (LaserDisc, VHS, CVBS)

The observer/sink contract is identical for every source; what varies is
signal quality at 6.9375 Mbit/s:

- **LaserDisc and broadcast-quality CVBS captures** carry the full PAL
  luma bandwidth. The teletext data spectrum (skew-symmetrical about
  ~3.47 MHz, substantially zero by 5 MHz — EN 300 706 §5.4) survives
  intact, and the threshold slicer of §4.1 is expected to perform like a
  hardware decoder. This is the acceptance-tested path.
- **VHS** records luma with roughly 3 MHz of bandwidth, truncating the
  upper half of the teletext spectrum. The result is heavy intersymbol
  interference — the reason vhs-teletext exists and uses deconvolution
  rather than threshold slicing. Expectations for the initial slicer:
  usable on strong recordings (S-VHS, good decks/tapes), degraded to poor
  on typical consumer VHS. This is stated openly in the stage's
  `instructions.md` rather than hidden.
- **Tape-specific mitigations already in the DAG** apply upstream of the
  sink and benefit teletext for free: `stacker` (multi-capture stacking
  raises SNR before slicing) and `dropout_correct`. Additionally, the
  observer can cheaply consult `get_dropout_hints()` and reject candidate
  lines overlapped by dropouts instead of emitting corrupted packets
  (worth a parameter once real tape material is in hand). VHS
  head-switching noise sits at the bottom of the picture, not in the
  teletext line window, so it is not a factor.
- **Upgrade path for band-limited sources**: the slicer is a non-ABI
  support-tier component (§4.1), so a matched-filter/equalizing or
  soft-decision variant can replace the threshold slicer later without
  touching the observer schema, the sink, or the T42 contract. With
  `keep_empty_packets` on both sides, output remains packet-for-packet
  comparable against vhs-teletext deconvolution of the same `.tbc`, which
  is the natural benchmark for any such upgrade.

---

## 3. Data model

### 3.1 Line addressing

Frames reaching a sink are **full-field**: PAL `frame_height` = 625, all
VBI lines present and readable via `get_line()` / `get_line_samples()`
(this is how all four existing VBI observers work). Buffer layout is
frame-flat, field 1 first: frame-flat line = field-1 line (0-based), or
`field1_lines(system)` + field-2 line (`kPalField1Lines` = 313).

Default candidate window, expressed as **0-based field lines 5–21 in both
fields** (broadcast lines 6–22 / 318–335 per EN 300 706 §4.1):

| Field | 0-based field lines | Frame-flat lines |
|---|---|---|
| 1 | 5–21 | 5–21 |
| 2 | 5–21 | 318–334 |

Notes:

- The window is a parameter (§5.2); the exact default will be validated
  against real captures during implementation (off-by-one conventions
  between broadcast numbering, ld-decode TBC rows, and the field-2 line
  count differ by source — `mask_line`'s documented VBI spec is
  `6-22,319-335` 1-based frame-flat).
- Teletext lines never coincide with the PAL long lines (frame-flat 312,
  624), so a fixed 1135-sample line length is safe.
- Every candidate line is *probed*; a line only yields a packet when clock
  run-in and framing code are found. Empty lines are cheap to reject.
- YC sources: use `get_line_luma()` when `has_separate_channels()` is true,
  else `get_line()` — same idiom as
  [biphase_observer.cpp](../orc/core/observers/biphase_observer.cpp).
- Prefer `get_line_samples()` over `get_frame()` so per-line disk reads go
  through the source stage's field buffering.

### 3.2 Observation schema

Namespace `teletext`, keyed by `FieldID(frame_id*2 + field_idx)`, produced
by the new observer (§4.2):

| Key | Type | Meaning |
|---|---|---|
| `present` | BOOL | at least one valid packet recovered in this field |
| `line_count` | INT32 | number of candidate lines that yielded packets |
| `t42_<field_line>` | STRING | 84 hex characters — the 42 recovered bytes for 0-based field line `<field_line>` (key absent when the line had no data) |

Rationale for hex-in-string: `ObservationValue` is
`variant<int32_t,int64_t,double,string,bool>`; 42 raw bytes do not fit the
numeric arms, and hex keeps the provenance store and any YAML/SQLite
serialization printable. 84 chars × ≤16 lines per field is negligible
storage. Decoding back to bytes is a trivial shared helper.

The observer is **stateless** (`ObserverInfo.stateless = true`): each field
is sliced independently, so the background-observation store can cache and
spot-probe per frame. (Page *assembly* is stateful, but that lives in the
consumers — §6, §7.)

---

## 4. Core components

### 4.1 Teletext line slicer (SDK support tier)

New header + implementation:
`orc/sdk/include/orc/support/teletext_slicer.h`, `orc/sdk/src/teletext_slicer.cpp`.

Support tier so that the core observer, the sink plugin, and third-party
plugins can all compile it in; being non-ABI it can be tuned freely later.
Requires the SDK manifest dance: add to `orc/sdk/sdk_headers.yaml`,
regenerate the allowlist and the `plugin-sdk.md` header tables
(AGENTS.md §9).

Interface (sketch):

```cpp
struct TeletextLineResult {
  bool valid = false;               // run-in + framing code found
  std::array<uint8_t, 42> bytes{};  // MRAG + 40 data bytes, transmission coding
  int framing_bit_errors = 0;       // 0 or 1 (see framing tolerance below)
  double data_start_sample = 0.0;   // where the framing code ended (diagnostics)
};

class TeletextSlicer {
 public:
  // sample_rate in Hz; bit rate fixed at 444 x fH (EN 300 706 s5.3).
  TeletextSlicer(double sample_rate, double bit_rate = 6'937'500.0);
  TeletextLineResult slice(const int16_t* line, size_t sample_count,
                           int16_t black_level, int16_t white_level) const;
};
```

Algorithm (per candidate line):

1. **Coarse gate.** Compute the line's peak above black level; if below a
   fraction of the nominal data amplitude (66 % of black-to-white,
   EN 300 706 §5.2), reject immediately — most VBI lines are empty.
2. **Clock run-in acquisition.** Search a window starting after the colour
   burst (`colour_burst_range(system)`, ending ~sample 138 PAL) and
   spanning the §6.3 timing tolerance for the 16-bit `1010…` run-in:
   correlate the line against a ±alternating kernel at the known bit period
   (≈2.556 samples). The correlation peak yields **bit phase** and **data
   amplitude**; the slicing threshold is set midway between the recovered
   0/1 levels (adaptive, like the closed-caption observer's min/max
   midpoint, but local to the data burst).
3. **Framing code lock.** From the run-in phase, sample bit centres
   (linear interpolation between adjacent samples) and search ±2 bit
   positions for `11100100`. Accept an exact match by default; a
   1-bit-error match is accepted only in "tolerant" mode (some receivers
   do this; it raises the false-positive rate on noisy discs).
4. **Byte extraction.** Sample the following 336 bits at bit-centre
   positions, LSB-first per byte (EN 300 706 §7.1), into 42 bytes. No
   Hamming/parity correction is applied — the T42 contract is
   transmission coding (§2.2).
5. **Plausibility filter (optional, parameter-controlled).** Decode the
   MRAG's four Hamming 8/4 nibbles; reject the line if both MRAG bytes are
   uncorrectable. This suppresses false framing-code locks on noise while
   still passing single-bit-damaged packets through for downstream tools.

Unit-testable in isolation: tests synthesize NRZ teletext lines at
17,734,475 Hz from known packet bytes (with configurable amplitude, noise,
and timing offset) and assert byte-exact recovery — no filesystem, no real
media, per TESTING.md.

### 4.2 `TeletextObserver` (host)

New files `orc/core/observers/teletext_observer.{h,cpp}` plus a row in the
observer table in
[core_observation_service.cpp](../orc/core/core_observation_service.cpp)
(id `teletext`, stateless). Observers are a host extension point, not a
plugin one, so this is in-tree core code.

`process_frame()`:

1. `get_video_parameters()`; return early unless `system == PAL`.
2. For each field, for each candidate field line (§3.1): fetch the line
   (luma-aware), run `TeletextSlicer::slice()`, and `set()` the
   observations of §3.2.
3. Levels passed to the slicer come from `SourceParameters`
   (`black_level`, `white_level`) with the spec constants as fallback.

`get_provided_observations()` declares the schema. Because per-line keys
are dynamic, the declared set lists `present`, `line_count`, and one entry
per candidate line (the window is fixed at observer construction).

---

## 5. The `teletext_sink` stage

### 5.1 Shape

Standard sink plugin, modelled directly on `cc_sink` (explicit-list
CMake, no third-party libraries):

```
orc/plugins/stages/teletext_sink/
├── CMakeLists.txt                        # orc_add_stage_plugin(orc-stage-plugin-teletext-sink ...)
├── plugin.cpp                            # ORC_STAGE_PLUGIN_DESCRIPTOR("decode-orc.stage.teletext_sink", ...)
├── instructions.md                       # mandatory self-documentation (AGENTS.md s9.1)
├── teletext_sink_stage.{h,cpp}           # DAGStage + ParameterizedStage + TriggerableStage
├── teletext_sink_stage_deps_interface.h  # ITeletextSinkStageDeps + Options/Result structs
└── teletext_sink_stage_deps.{h,cpp}      # frame loop, observer session, T42 writer
```

`NodeTypeInfo{NodeType::SINK, "teletext_sink", "Teletext Sink",
"Extracts teletext from the VBI and exports a T42 packet stream",
1, 1, 0, 0, VideoFormatCompatibility::PAL_ONLY}`.

Configuration status: `Red` until `output_path` is non-empty, then `Green`
(the `cc_sink` idiom).

### 5.2 Parameters

| Name | Type | Default | Notes |
|---|---|---|---|
| `output_path` | FILE_PATH | — (required) | `.t42` appended if absent; `file_extension_hint = ".t42"` |
| `first_vbi_line` | INT32 | 6 | 1-based field line, both fields; UI 1-based per frame_numbering conventions |
| `last_vbi_line` | INT32 | 22 | 1-based field line, both fields |
| `keep_empty_packets` | BOOL | false | emit 42 zero bytes for every candidate line with no data (1:1 line mapping, vhs-decode convention) |
| `tolerant_framing` | BOOL | false | accept framing codes with one bit error |
| `require_valid_mrag` | BOOL | true | drop packets whose MRAG fails Hamming 8/4 correction (§4.1 step 5) |

### 5.3 Trigger behaviour

`trigger()` (all work here; `execute()` returns `{}`):

1. Downcast `inputs[0]` to `VideoFrameRepresentation`; verify PAL.
2. Create one `IObserverHandle` for `"teletext"` via
   `orc::plugin::get_observation_service()` and hold it for the whole run.
   Because the observer is stateless, per-frame coverage checks against the
   host's provenance store work frame-by-frame (simpler than `cc_sink`'s
   all-or-nothing stateful rule): skip `process_frame()` when
   `observation_context.has(field_id, "teletext", "present")`.
3. Open the output through
   `IStageServices::create_buffered_file_writer_uint8()` (the
   `daphne_vbi_sink` writer path — a T42 stream is exactly a flat uint8
   stream). The stage takes the `IStageServices*` constructor so services
   are auto-injected.
4. Loop `frame_range()`; per frame: cancel check
   (`std::atomic<bool> cancel_requested_`), progress callback (throttled),
   run/skip the observer, then for field 1 and field 2 in temporal order
   emit packets for candidate lines in ascending line order — decoded from
   the `t42_<line>` hex observations, or 42 zero bytes when
   `keep_empty_packets` and the key is absent. `clear_field()` afterwards
   (the `cc_sink` memory-hygiene idiom).
5. `get_trigger_status()` reports counts:
   `"Exported N teletext packets (M fields with data) to <path>"`.
   Exceptions are caught and reported as `false` + status, never thrown.

Packet order is therefore strictly temporal (frame → field → line), which
is what carousel-reassembling consumers expect.

### 5.4 Registration and documentation touch-points

Same checklist as any new core sink (all enforced by gates/contract tests):

- `orc/plugins/stages/CMakeLists.txt` — `add_subdirectory(teletext_sink)`.
- `orc-tests/core/unit/include/public_stage_inventory.h` —
  `PublicStageSpec` (`PublicStageFamily::Sink`).
- `orc-tests/core/unit/contracts/stage_registry_contract_test.cpp` — loader
  assertion list.
- Test suite `orc-tests/core/unit/stages/teletext_sink/` with labels
  `unit;sinks` (+ slicer tests, `unit`).
- `docs/gui-user-guide/stages/sink-core-stages.md` — stage table section.
- `instructions.md` in the plugin directory (What it does / When to use /
  Parameters), `ORC_STAGE_INSTRUCTIONS_MD` in the class body.
- SDK manifest updates for the new support headers (§4.1).
- `ctest -L sdk`, `ctest -R MVPArchitectureCheck` green.

### 5.5 Verification against external tools

The `.t42` output is validated end-to-end before any preview work begins:

- `teletext filter/interactive <file>.t42` (ali1234's vhs-teletext) — page
  browsing from a T42 stream.
- wxTED / other zxnet-wiki-listed T42 consumers.
- Cross-check against `teletext deconvolve` output from the same `.tbc`
  where available (packet-for-packet comparison is possible when
  `keep_empty_packets` is enabled on both sides) — for both LaserDisc and
  vhs-decode tape captures.

---

## 6. Teletext subtitle export (follow-on)

Teletext subtitles are ordinary pages flagged C5 (newsflash) / C6
(subtitle) in the page-header control bits, conventionally page 888 in the
UK. Two consumers, both mirroring the EIA-608 flow:

1. **`teletext_sink` subtitle mode.** New parameters
   (`export_subtitles` BOOL, `subtitle_page` STRING default `"888"`,
   `subtitle_format` `{"SRT"}`); the deps object feeds recovered packets
   into a `TeletextPageDecoder` (below) filtered to the subtitle page and
   emits SubRip cues timed from field number / field rate — the same
   timestamp derivation as
   [cc_sink_stage_deps.cpp](../orc/plugins/stages/cc_sink/cc_sink_stage_deps.cpp)
   `generate_timestamp()`. SRT rather than SCC because SCC is an EIA-608
   container; SRT is the least lossy portable target for teletext subtitle
   text (colour/positioning dropped at this level).
2. **`video_sink` embedding.** An `embed_teletext_subtitles` parameter plus
   a subtitle-page parameter; the collection pass mirrors the existing
   `embed_closed_captions` pass (create observer, loop range, feed
   decoder), and the resulting cues reuse the **existing** `mov_text`
   muxing path in
   [ffmpeg_output_backend.cpp](../orc/plugins/stages/sinks/common/ffmpeg_output_backend.cpp)
   unchanged — cues in, tx3g samples out. (Muxing raw DVB teletext streams
   is explicitly out of scope.)

New SDK support-tier component shared by both (and by the preview):

**`TeletextPageDecoder`** (`orc/support/teletext_page_decoder.{h,cpp}`):
consumes 42-byte packets in temporal order; performs Hamming 8/4 and odd
parity decoding; assembles magazine/page state (serial and parallel
magazine modes, EN 300 706 §7.2); exposes (a) completed page snapshots
(40×25 cells: character code, colours, double-height, mosaic/hold flags —
Level 1 attributes only) and (b) subtitle cue emission (page arrival =
display, header-with-C6-clear or page erase = clear). This is the stateful
component deliberately kept out of the observer.

---

## 7. Teletext page preview dialog (follow-on)

A modeless GUI dialog that renders the currently broadcast page(s) and
follows the frame previewer, plugged into the established observer-dialog
seams (the VBI dialog / NTSC observer dialog pattern):

- **Data path.** `TeletextObserver` observations →
  `IRenderPresenter`/`RenderPresenter::requestObservations()` (delivered
  from provenance store or background scheduler) → new
  `TeletextObservationPresenter::extractFieldObservations(FieldID, const void*)`
  → value-type `TeletextFieldPacketsView` in a new
  `orc/view-types/orc_teletext.h` (no Qt, no core types). The
  `RenderCoordinator` gains a `GetTeletextData` request/response pair
  (request id + stale-response suppression, mock extended in
  `orc-tests/gui/unit/mocks/mock_render_presenter.h`). The `const void*`
  observation context is only touched inside the callback.
- **Page assembly.** The dialog owns a `TeletextPageDecoder` (§6) and a
  small frame-window cache: on frame change it requests observations for a
  trailing window of frames ending at the current frame (window size a
  constant, tuned to typical carousel repetition on disc), feeds packets in
  temporal order, and renders the requested page. Random access is
  inherently approximate for a carousel medium; the dialog surfaces
  "page seen at frame N" rather than pretending continuous reception.
  Sequential playback in the previewer degrades gracefully to live
  reception.
- **Rendering.** A plain `QWidget` painting the 40×25 Level 1 grid:
  monospace font for alphanumerics, painted 2×3 block cells for mosaic
  graphics, Level 1 attributes (colours, double height, flash ignored or
  static). No `FrameViewportWidget` needed — teletext has its own fixed
  geometry.
- **Lifecycle.** An *observer* dialog, not a preview-view dialog: owned
  by `MainWindow` like `VBIDialog`, `NtscObserverDialog`, and
  `VideoParameterObserverDialog` (the View-menu dialogs — frame scope,
  waveform — are `PreviewDialog`-owned; the Observers-menu dialogs are
  not). Menu entry under the preview window's **Observers** menu,
  emitting a new `showTeletextDialogRequested` signal handled by a
  `MainWindow::onShowTeletextDialog()` slot — the exact
  `showVBIDialogRequested` wiring. Refresh via
  `MainWindow::updateTeletextDialog()` called from
  `updateAllPreviewComponents()` with the standard `isVisible()` guard;
  field IDs resolved via `getFrameFields()` (frame vs field preview
  modes). The only structural difference from its VBI/NTSC siblings is
  that this dialog is stateful (page decoder + trailing-frame-window
  cache) rather than a per-field stateless display.
- **Controls.** Page number entry, magazine/page spinner, subpage cycling,
  "hold", and a reveal toggle (conceals are a Level 1 attribute) can all be
  added incrementally; the initial dialog needs only page entry + render.
- **Testing.** Tier 1 for page-decoder-to-view-model helpers, Tier 2 for
  the coordinator request plumbing, Tier 3 offscreen smoke for the dialog
  (TESTING.md tiers; presenter-boundary only, no live core pipeline).

MVP boundary: core (observer, page decoder) → presenter (extraction) →
view-types (`TeletextPageView` / `TeletextFieldPacketsView`) → gui
(dialog). `ctest -R MVPArchitectureCheck` enforces.

---

## 8. Testing strategy

Per TESTING.md / AGENTS.md §4 (unit tests mocked, deterministic, no I/O):

| Component | Tests |
|---|---|
| `TeletextSlicer` | synthesized NRZ lines (known bytes → waveform at 17.73 MHz → slice → byte-exact compare); amplitude/noise/phase-offset sweeps; empty-line rejection; framing tolerance on/off; MRAG plausibility filter |
| `TeletextObserver` | mocked `VideoFrameRepresentation` serving synthesized lines on selected VBI rows; asserts schema keys, statelessness, PAL-only gating, luma-path selection |
| `teletext_sink` stage | `StrictMock<ITeletextSinkStageDeps>` for parameter parsing, config-status transitions, trigger dispatch (the `daphne_vbi_sink_stage_test` shape) |
| `TeletextSinkStageDeps` | mocked observation service/context + mocked `IFileWriterUint8`; asserts packet bytes, temporal ordering, keep-empty padding, coverage-skip, cancel, progress |
| `TeletextPageDecoder` | packet sequences (hand-built + Hamming/parity error cases) → page snapshots and subtitle cues |
| Contract coverage | registry/node-discovery/parameter-parity suites pick the stage up via `public_stage_inventory.h` |
| Functional (labelled `functional`) | end-to-end `.t42` from a short real capture with known content, compared against a golden file |

---

## 9. Implementation phasing (outline for the plan document)

1. **Slicer + observer**: `TeletextSlicer` support-tier component with
   synthesis-based unit tests; `TeletextObserver` + observation schema +
   service registration; SDK manifest/doc regeneration.
2. **Sink**: `teletext_sink` plugin (stage, deps, writer, parameters,
   instructions.md), registration touch-points, unit + contract tests;
   validate `.t42` output against external viewers on the British Garden
   Birds disc (issue #58) — this is the acceptance gate for the phase.
   Where a VHS teletext capture is available, characterise (not gate on)
   recovery quality against vhs-teletext deconvolution of the same `.tbc`
   (§2.4).
3. **Page decoder + subtitle export**: `TeletextPageDecoder`; SRT export in
   `teletext_sink`; `video_sink` `embed_teletext_subtitles` reusing the
   `mov_text` path.
4. **Preview dialog**: view types, presenter extraction, coordinator
   request plumbing, dialog + previewer integration, GUI test tiers.

Each phase is independently shippable; phases 3 and 4 are explicitly
deferred per the feature request (sink first, verified externally).
