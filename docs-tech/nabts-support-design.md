# NABTS support: design and implementation plan

North American Basic Teletext Specification (NABTS) — ITU-R BT.653 System C, specified by
[CEA-516-S-2013](../docs-nodist/CEA-516_S-2013/CEA-516_S-2013.md) (formerly EIA-516) — end to end:
data-line recovery, packet-stream export, data-group and record assembly, presentation decoding,
and a page viewer equivalent to the one the WST teletext sink already provides.

## 1. What NABTS is, on the wire and above it

### 1.1 Data line (CEA-516 §1-§3)

| Property | NABTS (System C) | WST 525 (System B) | WST 625 (System B) |
|---|---|---|---|
| Bit rate | 5 727 272 bit/s ± 16 (§1.3) | 5 727 272 bit/s | 6 937 500 bit/s |
| Clock sync sequence | 16 bits, `1010…`, first bit '1' (§2.2.2) | identical | identical |
| Framing code | `11100111` = 0xE7 (§2.2.3) | `11100100` = 0xE4 | `11100100` = 0xE4 |
| Packet | 264 bits = 33 bytes (§3.1) | 34 bytes | 42 bytes |
| Data line | 288 bits total (§2.1) | 296 bits | 360 bits |
| Bit order | b1 first, b8 (MSB, parity) last (§3.1, §3.3) | identical | identical |
| Levels | '1' = 70 ± 2 IRE, '0' = blanking (§1.6) | identical (BT.653 Table 1b) | 66 % of black-to-white |
| First 0→1 CS transition | 10.48 ± 0.34 µs from sync leading edge (§1.3) | ≈ 9.3 µs (measured) | ≈ 10.2 µs |
| VBI lines | 10-21 / 273-284 (BT.653 §2) | identical | 6-22 / 318-335 |

The framing code is the **only** thing that separates System B and System C on a 525-line capture:
same clock run-in, same bit rate, same lines, same levels, same bit order. `0xE7` is symmetric under
bit reversal, so it reads the same whichever end of the byte the reader calls first.

The measured anchor already exists in-tree: `vbi_teletext_service.cpp` records a run-in leading edge
of 10 270-10 340 ns over some three hundred lines of the ExtraVision capture, against libzvbi's
tabulated 10 480 ns and the spec's nominal 10 480 ± 340 ns. All three agree to within two bit
periods, unlike the 525-line WST case where libzvbi is seven bit periods out.

**Full-field transmission is out of scope.** CEA-516 §1.2 also permits NABTS on lines 10 to 262 of
field 1 and the corresponding lines of field 2 — the whole active picture, not just the VBI. Both
reference captures are VBI crops, the `.tbc VBI crop` presets store twelve records per field
(`standard_teletext_lines_per_field()` returns 12 for a 525-line system), and there is no full-field
sample to validate against, so the line-window parameter is bounded at the VBI window. Widening it
is a later change if such a capture appears; nothing in the design forecloses it.

### 1.2 Packet (CEA-516 §3.2-§3.4)

```
P1 P2 P3 CI PS | data block (0, 26, 27 or 28 bytes) | suffix (0, 1, 2 or 28 bytes)
\----- prefix, all Hamming 8/4 -----/
```

* **P1-P3** — packet address = data channel number, three hex digits, 4096 channels (§3.2.3).
* **CI** — continuity index 0-15, increments per packet within a channel; detects packet loss (§3.2.4).
* **PS** — packet structure: b2 = synchronizing packet (starts a data group), b4 = not full,
  b8/b6 = suffix length (§3.2.5).
* **Suffix** — 1 byte = longitudinal odd parity over data block + suffix; combined with the per-byte
  parity it is a product code correcting all single-bit errors over the block (§3.4).

The Hamming 8/4 code of §3.2.2 (bits 7, 5, 3, 1 protect 8, 6, 4, 2, odd parity over the byte) is the
same code as ETSI EN 300 706 §8.2, so `teletext_hamming84_decode()` applies unchanged.

Five Hamming-coded prefix bytes is a far stronger false-lock gate than WST's two MRAG bytes: random
bytes clear it with probability (16/256)^5 ≈ 1×10⁻⁶.

### 1.3 Above the packet (CEA-516 §4-§7)

```
packets (same address, one flagged synchronizing)
  → data group      [GT GC GR S1 S2 F1 F2 GN, all Hamming]        §4.2
    → teletext record (GT = 0 carries exactly one)                 §5.1
      → record header [RT RD A1 A2 A3 (A4-A9) (L1 L2) (Y…) (HE…)] §5.2
      → record data   = NAPLPS presentation code, 7-bit only       §6.1
```

* Record types: 0 = cyclic presentation, 1 = non-cyclic presentation (captioning), 2 = application
  (date/time/control, not presentation), 3 = priority presentation (§5.2.2).
* A **page** is an unlinked presentation record, or a linked series of records sharing a record
  address; the page number is the record address (§7.1.1). Records are linked by L1/L2 (§5.2.6).
* Reserved addresses: channel 000 record 000 = master index, 000/FFE = service application record,
  A00/000 = captioning, B00/000 = Flash, any/FFF = support record (§7.1.5).
* Maximum data group is 1904 bytes (§8.4.2.5).

### 1.4 The consequence: presentation is NAPLPS, not a character grid

CEA-516 §6.1 and §8.6.1 defer presentation entirely to ANSI X3.110 / CSA T500, *Videotex/Teletext
Presentation Level Protocol Syntax* (NAPLPS) — a geometric, resolution-independent drawing protocol
with picture description instructions (point, line, arc, rectangle, polygon, incremental fill),
a programmable colour map, DRCS, macros and texture fills. Only the 7-bit code environment is used,
and only the teletext Service Reference Model subset may be transmitted.

**Nothing about the WST Level 1 page model survives this.** There is no 40×25 cell grid, no spacing
attributes, no magazine/row addressing, no page header, and no row-copy carousel to vote over.

### 1.5 What already exists in-tree

Not a greenfield start. The VBI source stage learned NABTS in the 20260804-vbi work and carries a
measured description of the service:

* `vbi_source_format.cpp` — the `.tbc VBI crop, 16-bit (NABTS)` container preset, `u16le`, 910-sample
  records, twelve data records per field.
* `vbi_teletext_service.cpp` — `kNABTSCRIFRCPattern = 0xAAAAE7`, `kNABTSPayloadBytes = 33`,
  `kNABTSOffsetNs = 10300` (measured), bit rate shared with the 525-line WST service.
* `vbi_output_frame.cpp` — `kNTSCNABTSLogic0 = kNtscBlanking` (240) and `kNTSCNABTSLogic1 = 632`
  counts, from 70 IRE at the 5,6 counts/IRE of the NTSC 10-bit scale.
* `vbi_source_validation.cpp` / `vbi_line_mapping.cpp` — NABTS accepted on NTSC and PAL-M, refused on
  PAL, on the same line list as the 525-line WST service.

So the generator side is done and measured; this plan is the reader side. Two in-code statements go
stale and must change with it: the `SystemGeometry` comment in
[teletext_slicer.cpp](../orc/sdk/src/teletext_slicer.cpp) that names NABTS as "the obvious candidate"
for a third row (Task 1.2 is exactly that row), and the class comment in
[teletext_frame_slicer.h](../orc/plugins/stages/teletext_sink/teletext_frame_slicer.h) saying NABTS
lines "are seen and rejected rather than decoded".

One negative finding worth recording so nobody goes looking: **the CLI has no teletext surface at
all** — no `orc/cli` source mentions it, and the WST page viewer is reachable only as a GUI stage
tool. The NABTS viewer follows suit, so Phase 6 is GUI-only and no CLI command is in scope.

## 2. Architecture decision: a separate `nabts_sink` stage

**Recommendation: implement NABTS as a new sink stage, `nabts_sink`, self-contained down to the SDK
boundary.**

> **Revised 2026-08-08.** Phase 2 originally put the frame pass in a shared plugin-side library,
> `orc/plugins/stages/common/teletext-recovery`, that both sinks linked. That library has since been
> removed and its three modules copied into each stage, `Nabts`-prefixed on the System C side. The
> reason is that both sinks are destined to leave the tree as external plugins, and an external
> plugin can depend on the SDK but not on `orc/plugins/stages/common/` — a shared in-tree library is
> a tie that has to be cut at exactly the moment the two stages are hardest to separate. Point 4
> below still holds for what it actually names: the expensive, subtle part is the bit detectors, and
> those are in the SDK (`orc/support/teletext_slicer.h`), shared and staying shared. It is the thin
> frame-and-block loop above them that is duplicated, and duplicating it bought each copy its own
> shape — see the table in §2.1.

The alternative — a `service` parameter on `teletext_sink` selecting WST or NABTS — was assessed
and rejected. The evidence:

1. **Nothing above the packet is shared, and that is most of a sink stage.** The WST sink's page
   catalogue (`teletext_page_catalogue`), row squasher (`orc/support/teletext_row_squasher.h`),
   page decoder (`orc/support/teletext_page_decoder.h`), squash statistics, subtitle export and page
   viewer all encode the Level 1 model. NABTS replaces every one of them with a different mechanism:
   data-group reassembly, longitudinal-parity correction, continuity-index loss detection, record
   linking, and a NAPLPS interpreter.

2. **The stage-tool registry is keyed on the stage *name* and queried on a default-constructed
   stage.** `AnalysisPresenter::…` builds the tool list from
   `StageRegistry::instance().create_stage(stage_name)` and calls `get_stage_tools()` on that fresh
   instance ([analysis_presenter.cpp:202-204](../orc/presenters/src/analysis_presenter.cpp#L202-L204)).
   A stage therefore *cannot* vary its tool list by parameter. One stage carrying both viewers would
   permanently advertise "Teletext Pages" and "NABTS Pages", one of which is always dead — and the
   GUI dispatches viewer dialogs on the tool id
   ([mainwindow.cpp:3901-3904](../orc/gui/mainwindow.cpp#L3901-L3904)), so both dialogs would be
   reachable on every node regardless of configuration.

3. **The parameter surface splits almost cleanly.** Of the sixteen parameters `teletext_sink`
   exposes, five are Level 1 concepts that NABTS has no analogue for (`squash_repeated_rows`,
   `repair_damaged_bytes`, `export_subtitles`, `subtitle_page`, `subtitle_format`) and NABTS needs
   several of its own (data-channel filter, suffix error correction, record export). Only the
   recovery parameters are common.

4. **What *is* shared is not stage code.** The expensive, subtle part — the threshold and MLSE bit
   detectors, the phase hint, the amplitude gates, the per-line scan state, the block-parallel frame
   pass and the recovery statistics — already lives in `orc/support/teletext_slicer.h`,
   `orc/support/teletext_recovery_stats.h` and the plugin-side `teletext_frame_slicer` /
   `teletext_scan_state`. Phase 2 promotes the plugin-side pair into
   `orc/plugins/stages/common/`, alongside `audio-resample` and `efm-decode`, so both sinks compile
   one copy. Commit 54f1658f removed teletext code duplication once already; re-introducing it is
   the outcome to avoid, and a shared library is how that is avoided regardless of which stage
   layout is chosen.

**The cost, stated plainly.** A user who does not know which service a 525-line capture carries must
try one node and then the other, rather than flipping a dropdown. Mitigations: both stages state the
service in their node description and reject the wrong television system with a specific message;
`teletext_sink`'s `instructions.md` and its "no packets recovered" status both point at
`nabts_sink`; and the reverse. This is judged the lesser cost against a stage whose parameter list,
tool menu and results interface are each half inapplicable at any moment.

### 2.1 Component map

| Layer | WST (exists) | NABTS (new) | Shared |
|---|---|---|---|
| Bit recovery | `orc/support/teletext_slicer.h` | — | extended with System C (Phase 1) |
| Frame pass | `teletext_frame_slicer`, `teletext_scan_state`, `teletext_block_scanner` | `nabts_frame_slicer`, `nabts_scan_state`, `nabts_block_scanner` (Phase 2) | — |
| Diagnostics | `orc/support/teletext_recovery_stats.h` | — | service-aware parity profile (Phase 1) |
| Stage | `teletext_sink` | `nabts_sink` (Phase 3) | — |
| Packet → structure | `teletext_page_decoder` | `nabts_record_assembler` (Phase 4) | — |
| Presentation | Level 1 cell grid | `naplps_interpreter` → display list (Phase 5) | — |
| Results contract | `ITeletextAnalysisResults` | `INabtsAnalysisResults` (Phase 4) | — |
| View types | `orc_teletext.h` | `orc_nabts.h` (Phase 6) | — |
| Presenter | `teletext_analysis_presenter` | `nabts_analysis_presenter` (Phase 6) | — |
| Viewer | `TeletextDialog` + `TeletextPageWidget` | `NabtsDialog` + `NabtsCanvasWidget` (Phase 6) | — |

### 2.2 Packet-stream file naming

`teletext_sink` names its headerless stream by packet length so a reader can identify it: `.t42` for
42-byte 625-line packets, `.t34` for 34-byte 525-line ones. NABTS follows the same rule: **`.t33`**.
(`.nab` was considered; it names the service rather than the packet length and breaks the rule the
other two follow.)

### 2.3 Presentation-layer reference documents

CEA-516 §6.1 and §8.6.1 defer presentation entirely to ANSI X3.110-1983 / CSA T500-1983 (NAPLPS).
The CCITT publishes the same protocol as **Data Syntax III** of ITU-T T.101, in **Annex D**.

**NAPLPS is data syntax III, not II.** T.101 numbers the syntaxes CAPTAIN, CEPT, NAPLPS; the
GKS-flavoured commands in the recommendation (POLYLINE, POLYMARKER, FILL AREA, GDP, workstation
window/viewport, segments, deferral mode) belong to the European syntax and must not be used to
model NABTS. Three independent checks in the copy in hand settle it: Table II-3 gives data syntax III
a screen height of **0.78125**, which is the NABTS display area of CEA-516 §7.2.1 exactly; it gives
macros and DRCS **3072 bytes shared**, which is the budget CEA-516 §8.6.1 states exactly; and Annex
A attributes the "PDI G Set" — the picture description instruction set, a NAPLPS-only construct — to
data syntax III (A.3.9.13.3).

#### The primary reference — present and complete

[FIPS PUB 121 / ANSI X3.110-1983 / CSA T500-1983](../docs-nodist/fipspub121/markdown.md), 176 pages,
is in the repository. It was checked clause by clause against what Phase 5 needs, including opening
the scanned figures, and **nothing is missing**:

| Phase 5 needs | Where it is | Form |
|---|---|---|
| C-/G-set designation escape sequences | §4.3 Table 1 (primary `ESC I 4/2`, supplementary `ESC I 7/12`, PDI `ESC I 5/7`, mosaic `ESC I 7/13`, macro `ESC I 7/10`, DRCS `ESC I 7/11`) | markdown table |
| Invocation, defaults, 7-/8-bit transform | §4.3.1.3, §4.3.2-3 Table 2, Appendix C. Defaults: G0 primary, G1 PDI, G2 supplementary, G3 mosaic | text |
| **PDI opcode code positions** | **Figure 13**, `pages/page-47/img-16.jpeg` — all 32 opcodes across columns 2 and 3, columns 4-7 numeric data | image, fully legible |
| Opcode vs operand discrimination | §5.3.1: b7 = 0 opcode, b7 = 1 numeric data | text |
| Single-value operand format | Figure 10 + §5.3.1 (1-4 bytes, b6→b1 concatenated, unsigned) | image + text |
| **Multi-value operand format** | **Figure 11**, `pages/page-43/img-13.jpeg` (2D) and `img-14.jpeg` (3D) — X in b6-b4, Y in b3-b1, sign in the first byte, LSB in the last; §5.3.1 adds "signed, two's complement … binary decimals where the MSB represents the digit just to the right of the decimal point" | image, legible + text |
| Colour value format | Figure 12 + §5.3.1: GRB three-tuples, two per byte, decreasing luminance | image + text |
| Operand type per opcode | Table 3 | markdown table |
| DOMAIN | §5.3.2.2 with Tables 4 and 5 (single-value 1-4 bytes, multi-value 1-8 bytes), dimensionality, logical pel | markdown tables + text |
| TEXT, TEXTURE, SET COLOR, SELECT COLOR, BLINK, WAIT, RESET | §5.3.2.3 to §5.3.2.9, including the three colour modes and the selective RESET bit map | text + figures |
| Geometric primitives | §5.3.3.1 to §5.3.3.6.5 | text + figures |
| C0 control set | §6.1, code positions stated inline in the prose (APB 0/8, APS 1/12, SO 0/14, SI 0/15, SS2 1/9, SS3 1/13, ESC 1/11, CAN 1/8, NSR 1/15 …) + Figure 64 | text |
| **C1 control set** | **Figure 65**, `pages/page-116/img-76.jpeg` — DEF MACRO / DEFP / DEFT / DEF DRCS / DEF TEXTURE / END / REPEAT / video / text-size / BLINK START-STOP in column A, protect, word wrap, scroll, underline, cursor and unprotect in column B, under the A = 4-or-8 / B = 5-or-9 convention stated beneath it | image, legible |
| Macros | §5.5 and §6.2.2 — 96 names, nesting, definition-terminating controls | text |
| DRCS | §5.6 and §6.2.3 — 96 characters, buffer aspect ratio, element on/off semantics | text |
| Mosaic set | §5.4 and Figure 63 — 65 block mosaics, contiguous vs separated via underline mode and logical pel | text + image |
| Graphic character repertoire | §7 with Table 25 | markdown tables |
| **Teletext Service Reference Model** | **Appendix D Table D1** — the conformance target CEA-516 §8.6.1 mandates | markdown tables |
| Coordinate precision guidance | Appendix B — 12 bits of internal precision at 256-pixel resolution, 1/40 as 102/4096 | text |

Table D1 is the one to design the viewer to, because CEA-516 §8.6.1 makes the teletext SRM binding
on the service as well as the receiver: **16 simultaneous colours out of 512** (three bits per gun),
character formats 40 × 24, 40 × 20, 40 × 10, 32 × 16 and 20 × 10, polygon and spline vertex limit
256, four line textures, four texture patterns plus four programmable 16 × 16 masks, 16 blink
processes, all DOMAIN operand lengths, both dimensionality modes with the third dimension ignored,
four character rotations and paths, four inter-character and four inter-row spacings, four cursor
styles. CEA-516 §8.6.3 recommends providers work to a 4096-colour palette chosen so that a
512-colour receiver still displays acceptably, so the SRM is the floor rather than the ceiling.

#### Corroboration from the T.101 copy

[ITU-T T.101 (1988-11)](../docs-nodist/ITU-T_T.101-1988-11/markdown.md) carries none of the three
data syntaxes — its contents list Annexes B, C and D, but a footnote on the same page says they
"will not be published in Fascicle VII.5 (T-Series Recommendations) but will be issued as a separate
publication", and the file holds one annex, `## ANNEX A`, across 80 pages. It stays useful as a
second opinion: **Table II-3** tabulates the data syntax III reset state in one place (screen
0.9999 × 0.78125, colour map limit 16, 40 × 20 characters, 256 × 200 nominal resolution, macro and
DRCS 3072 bytes shared, logical pel 0,0, texture mask 1/40 by 5/128, character size dx = 1/40
dy = 1/128) with a **16-entry default colour map at three bits per gun** — greys at addresses 0-7,
hues at 8-15 — which agrees with Table D1's "16 simultaneous colours out of 512" independently.
Appendix I adds the mosaic repertoires under the same 2×3 sub-cell numbering the teletext presenter
already maps sixels through.

[ITU-T T.101 (11/94)](https://www.itu.int/rec/T-REC-T.101-199411-I/en) publishes Annexes B, C and D
inline should a third reading ever be wanted; its download is a ZIP of PDFs
(`…id=T-REC-T.101-199411-I!!ZPF-E&type=items`, the `!!PDF-E` form returns HTTP 500).

**No phase of this plan is blocked on a document.**

## Phase 1 — System C in the slicer

Extends the SDK slicer so that a NABTS data line is recovered as a 33-byte packet. No stage changes.

### Task 1.1 — `TeletextSystem::kNabts525` and its public constants

Add `kNabtsPacketBytes = 33` and the `kNabts525` enumerator to
[teletext_slicer.h](../orc/sdk/include/orc/support/teletext_slicer.h); extend `teletext_bit_rate()`
and `teletext_packet_bytes()`. Cite CEA-516 §1.3, §3.1 at each constant.

*Acceptance:* `teletext_packet_bytes(kNabts525) == 33`, `teletext_bit_rate(kNabts525) ==
kTeletext525BitRate`; the existing `static_assert` block in
[teletext_slicer.cpp](../orc/sdk/src/teletext_slicer.cpp) is extended to cover the new row and
compiles.

### Task 1.2 — Per-system framing code and prefix length in `SystemGeometry`

`kFramingCodeBits` is currently a file-scope constant assumed by both detectors and by
`preamble_bit()`. Move it into the `SystemGeometry` table together with a new
`hamming_prefix_bytes` field (2 for either WST — the MRAG; 5 for NABTS — P1-P3, CI, PS, CEA-516
§3.2.1) and a `parity_coded_rows` flag (true for WST, false for NABTS: CEA-516 §3.3 gives the data
block odd parity only for data group type 0, which a single packet cannot establish). Add the System
C row: framing `{1,1,1,0,0,1,1,1}`, 33 bytes, `data_one_fraction` 0.70, run-in search window 8.5 to
12.2 µs (CEA-516 §1.3 nominal first-bit centre 10.57 µs, measured 10.39 µs, lower bound clearing the
colour burst at ≈ 7.8 µs, upper bound giving the +1.3 µs of network re-timing headroom the other
rows allow while leaving the 288-bit line inside the 910-sample NTSC line: 12.2 µs + 50.3 µs of
burst ends at 62.5 of 63.56 µs).

`data_one_fraction` is 0.70, the 525-line WST value, reused deliberately rather than recomputed. The
slicer measures the '1' level as a fraction above *black*, while NABTS transmits '0' at blanking:
against the in-tree levels (`kNtscBlack` 282, `kNtscWhite` 800, `kNTSCNABTSLogic1` 632) the true
fraction is (632 − 282) / (800 − 282) = 0.676, so 0.70 sets the nominal 3,5 % high and the
half-nominal amplitude gate correspondingly strict. The real burst still clears that gate by 350
counts against 181, a factor of 1,9 — the same margin the existing 525-line comment records.

*Acceptance:* the geometry table is the only place a per-system fact appears; `grep` finds no
service ternary in either detector. `system_geometry(kWst625).framing == {1,1,1,0,0,1,0,0}` and the
whole 625-line and 525-line unit suite is unchanged and green.

### Task 1.3 — Threshold detector: framing search and prefix gate

The framing-code search loop reads the geometry's framing bits; the `require_valid_mrag` candidate
filter checks `hamming_prefix_bytes` bytes rather than a hard-coded two. Update the option's
documentation comment to describe it as the packet's Hamming-coded prefix, naming the MRAG (EN 300
706 §7.1.2) and the packet prefix (CEA-516 §3.2) as its two instances. The parameter name stays
`require_valid_mrag` for project-file compatibility; the display name becomes service-dependent in
Phase 3.

*Acceptance:* a synthesized NABTS line with a deliberately damaged CI byte (two bit errors) is
rejected with `kInvalidMrag` when the option is set and accepted when it is clear; the WST 525
behaviour is byte-identical to before.

### Task 1.4 — MLSE detector: plausibility gate for System C

The MLSE path currently gates on the fraction of data bytes carrying odd parity, which is a Level 1
row property. For System C, replace it with an unconditional requirement that all five prefix bytes
Hamming-decode (rejecting as `kInvalidMrag`), and skip parity-guided repair entirely — CEA-516 §3.3
makes byte parity conditional on the data group type, which is not knowable from one packet.

*Acceptance:* MLSE recovers a band-limited (3 MHz cut-off) synthesized NABTS line; a line of uniform
noise at the same amplitude yields no packet across 1000 seeds; `repaired_bytes` is always zero for
System C.

### Task 1.5 — Observation-string lengths

Add 33 to `kPacketByteLengths` so `teletext_hex_to_observed_packet()` accepts the 66- and
99-character forms. Confirm they stay distinct from the existing 68/84/102/126.

*Acceptance:* a unit test round-trips a 33-byte packet with and without a confidence suffix, and
asserts the five accepted lengths are pairwise distinct.

### Task 1.6 — Recovery statistics: suppress the parity profile for System C

`TeletextRecoveryStats::add_line()` derives a Level 1 row from the first two bytes to decide whether
the per-position parity profile applies. Pass the service through (or the `parity_coded_rows` flag)
so a System C packet contributes to the yield, detector and confidence figures but not to the parity
profile.

*Acceptance:* a stats instance fed only NABTS packets reports `parity_checked_packets() == 0` and
non-zero `packets()`.

### Task 1.7 — Unit tests

Add `nabts_synth_options()` and a NABTS transmission-packet builder to
[teletext_line_synthesizer.h](../orc-tests/core/unit/support/teletext_line_synthesizer.h) (framing
byte 0xE7 on the wire, 33 payload bytes, Hamming-coded prefix), and extend
`orc-tests/core/unit/support/teletext_slicer_test.cpp`.

Coverage: clean round-trip at both detectors; band-limited round-trip at MLSE; noise rejection;
timing tolerance across the ± 0.34 µs of CEA-516 §1.3; and — the test that matters most — **cross
rejection**: a synthesized WST 525 line must yield nothing from a System C slicer and a NABTS line
nothing from a System B slicer, at both detectors, with and without tolerant framing.

*Acceptance:* `ctest -L unit -R Teletext --output-on-failure` green; the cross-rejection test fails
if the framing code is made service-independent again.

## Phase 2 — A frame-recovery pass per stage

Gives `nabts_sink` its own block-parallel frame pass. Behaviour-preserving for `teletext_sink`.

> **Revised 2026-08-08.** As first written this phase moved the pass into a shared plugin-side
> library, `orc-teletext-recovery`, that both sinks linked, and Tasks 2.1-2.3 below described that.
> They now describe what was built in its place: a copy per stage, with no shared plugin-side
> library at all. §2 states the reason — an external plugin cannot link
> `orc/plugins/stages/common/`. The shared library existed for one commit and is recorded here
> because the reasoning that killed it is worth keeping.

### Task 2.1 — Each stage owns its pass

`teletext_sink` keeps `teletext_frame_slicer.{h,cpp}`, `teletext_scan_state.{h,cpp}` and the new
`teletext_block_scanner.{h,cpp}`, all in the stage directory and included by plain name.
`nabts_sink` gets `nabts_frame_slicer.{h,cpp}`, `nabts_scan_state.{h,cpp}` and
`nabts_block_scanner.{h,cpp}`.

The System C copies are renamed rather than only moved — `NabtsFrameSlicer`, `NabtsScanState`,
`nabts_slice_block()` and so on. That is not cosmetic: the unit-test binary links both stage
libraries statically, so two different definitions under one name would not link. The runtime path
is safe either way (`stage_plugin_loader.cpp` uses `RTLD_LOCAL`), but the tests would have caught
it first.

Neither copy links the other, and neither links anything under `orc/plugins/stages/common/`. Both
compile against the SDK alone, which is the dependency an external plugin will be left with.

*Acceptance:* `cmake --build build -j` clean; `ctest -L sdk` green (both stages use public SDK
headers only); `ctest -R "StagePluginLoader"` green.

### Task 2.2 — What the System C copy dropped

Duplicating the pass is only worth what specialising it buys, so the NABTS copy is not a
search-and-replace of the WST one:

- **No 625-line row.** `kNabtsVideoSystems` is `{NTSC, PAL_M}`, two slicers rather than three, and
  `profile_for()` has no PAL branch to write and no `TeletextSystem` to select — every slicer it
  builds is `kNabts525`. CEA-516 §1.1.1 defines the service on the 525-line signal only.
- **No parity-repair option.** CEA-516 §3.3 makes byte parity conditional on the data group type,
  which a single packet cannot establish. The WST copy carries `parity_repair` through from the
  stage; the NABTS copy pins it false at the one place it reaches the SDK slicer and documents why
  in `NabtsFrameSlicerOptions`, so there is no parameter to offer and no path that could set it.
- **`require_valid_prefix`, not `require_valid_mrag`.** The System C prefix is five Hamming 8/4
  bytes — P1-P3, CI, PS (CEA-516 §3.2.1) — not a two-byte MRAG. The SDK option keeps its System B
  name; the stage-facing one does not.
- **One window pair, not two.** `kNabtsFirstFieldLine` / `kNabtsLastFieldLine` (9-20) rather than
  the 625- and 525-line pairs, because there is only one.

A `service` enum threaded through a shared pass could express none of this: every one of these is
the *absence* of a branch, and a shared copy has to keep them all.

*Acceptance:* `nabts_frame_slicer_test.cpp` covers `applies_to()` on all four video systems,
`profile_for()` on both 525-line systems and the window override; `SliceField_YieldsNothingOnA625LineSource`
pins the PAL case at the slicing path rather than only the profile.

### Task 2.3 — Tests follow their stage

`teletext_frame_slicer_test.cpp` and `teletext_scan_state_test.cpp` stay in
`orc-tests/core/unit/stages/teletext_sink/`; `nabts_frame_slicer_test.cpp` and
`nabts_scan_state_test.cpp` are their System C counterparts under
`orc-tests/core/unit/stages/nabts_sink/`.
[orc-tests/core/functional/CMakeLists.txt](../orc-tests/core/functional/CMakeLists.txt) compiles
`teletext_frame_slicer.cpp` and `teletext_scan_state.cpp` directly into the VBI source test, as it
did before the shared library existed, and neither functional sink target links a common library.

*Acceptance:* the 625-line and 525-line functional golden streams in
`teletext_sink_pipeline_test.cpp` are byte-identical to before the split; the four
`nabts_sink_pipeline_test.cpp` cases still pass.

## Phase 3 — `nabts_sink`: recovery and packet export

A complete, useful stage: recovers NABTS lines and exports the packet stream with a diagnostic
report. No structure decoding yet; the stage tool arrives in Phase 6.

### Task 3.1 — Stage skeleton and registration

`orc/plugins/stages/nabts_sink/` with `plugin.cpp` (`decode-orc.stage.nabts_sink`),
`nabts_sink_stage.{h,cpp}`, `CMakeLists.txt` linking `orc-teletext-recovery`, and `instructions.md`
exposed through `ORC_STAGE_INSTRUCTIONS_MD`. Add to
[orc/plugins/stages/CMakeLists.txt](../orc/plugins/stages/CMakeLists.txt). Node type SINK, one
input, no outputs.

The three behaviours `teletext_sink` gets from being a triggerable sink come with it: `execute()`
caches the input representation only, `get_preview_capability()` returns
`PreviewHelpers::make_signal_preview_capability()` over that cache so the node previews before it is
triggered, and `set_parameters()` reports Green with an output path and Yellow without — an empty
path is the browse-only run, not a missing requirement.

*Acceptance:* the stage appears in node discovery; `ctest -L contracts` and `-L sdk` green;
**Help…** renders the instructions; a node with no output path shows Yellow and still previews.

### Task 3.2 — Parameters

`output_path` (`.t33`), `first_vbi_line` / `last_vbi_line` (defaults 10 and 21, 1-based, from the
BT.653 §2 window), `keep_empty_packets`, `detector`, `tolerant_framing`,
`require_valid_prefix` (the five Hamming prefix bytes), `pin_data_phase`, `learn_active_lines`,
`decode_threads`, `write_report`. `get_parameter_descriptors()` returns an empty list with a
"525-line sources only" descriptor note when the project format is 625-line, matching how
`teletext_sink` filters its subtitle parameters by project format.

*Acceptance:* parameter/default parity contract tests pass; a PAL project offers no NABTS decode
parameters; `parse_config()` rejects an inverted line window with a specific message.

### Task 3.3 — Deps and the recovery pass

`nabts_sink_deps.{h,cpp}` behind `INabtsSinkStageDeps`, reusing the shared frame slicer, scan state
and recovery stats. One linear block-parallel pass, strict frame → field → ascending-line emission
order, cancellation and progress plumbing, `.t33` extension applied, report written beside the
stream when asked.

*Acceptance:* deps unit tests with a mock representation cover: extension applied, browse-only run
(empty path), `keep_empty_packets` emitting 33 zero bytes per empty slot, cancellation leaving a
prefix of the stream, and the decode being independent of `decode_threads`.

### Task 3.4 — Guards and reporting

Refuse a 625-line source with "NABTS (ITU-R BT.653 System C) is carried on 525-line systems only;
use the Teletext Sink for a PAL source". Trigger status reports packets, fields with data, and the
per-line yield.

*Acceptance:* stage unit tests assert both refusal messages and the success status string.

### Task 3.5 — Functional test against the reference captures

Extend the vbi_source → sink pipeline pattern of
[teletext_sink_pipeline_test.cpp](../orc-tests/core/functional/stages/teletext_sink/teletext_sink_pipeline_test.cpp)
with the two NABTS captures in `test-data/teletext/NTSC NABTS Teletext samples/` (NBC Teletext 1983,
CBS ExtraVision 1985), driven through the `.tbc VBI crop, 16-bit (NABTS)` preset. Skip when absent,
as the existing tests do. Record golden stream SHA-256, byte count and packet count for a fixed
window of each.

*Acceptance:* both captures decode; packet yield per field is consistent with a real service (not a
handful of false locks); the streams match their goldens across thread counts 1, 3 and 8.

## Phase 4 — Data groups and teletext records

Turns the packet stream into the addressable objects a viewer lists.

### Task 4.1 — `nabts_packet.h`: prefix and suffix decoding

Decode P1-P3, CI, PS (CEA-516 §3.2) and the suffix (§3.4): suffix length from PS b8/b6, longitudinal
odd-parity check over data block + suffix, and single-bit correction using the product code the
per-byte parity and the longitudinal byte form together. Report per-packet: channel, continuity
index, synchronizing flag, data-block extent, and whether the block was clean, corrected or failed.

The 28-byte suffix (PS b8/b6 = 1,1) is **not** decoded. §3.4 leaves its bundle error-protection
method "reserved for future standardization", so such a packet carries a zero-length data block and
is counted and skipped rather than guessed at — while its continuity index is still consumed, which
§3.4 requires.

*Acceptance:* unit tests over hand-built packets for each suffix length; a single-bit error anywhere
in the data block is corrected; a two-bit error is detected and reported uncorrectable; a 1,1 packet
contributes no data-block bytes and does not break the continuity chain.

### Task 4.2 — `nabts_data_group.h`: group reassembly

Per data channel, accumulate packets from a synchronizing packet (§4.1) using S1/S2 (further block
count) and F1/F2 (final non-zero block size) from the eight Hamming-coded header bytes (§4.2), with
the continuity index detecting loss (§3.2.4). Bound the working set; drop and report a group whose
header does not decode or which exceeds the 1904-byte maximum (§8.4.2.5).

*Acceptance:* unit tests reassemble a multi-packet group across all suffix lengths, detect a dropped
packet through the continuity index, and refuse an oversized group without unbounded allocation.

### Task 4.3 — `nabts_record_assembler.h`: record headers and linking

Parse the record header (§5.2): RT, RD, A1-A3, optional A4-A9 address extension, L1/L2 link,
classification sequence pointer/flag bytes (§5.2.7), and header extension fields (§5.2.8, skipped by
length). Join a linked series into a message (§5.2.6) keyed on {channel, record address, version}.
Application records (type 2, §7.2) are decoded to their function descriptors; presentation records
(types 0, 1, 3) hand their data on to Phase 5.

*Acceptance:* unit tests over hand-built records cover short and long addresses, an unlinked record,
a three-record linked series arriving out of order, and a classification sequence with two pointer
bytes; the reserved addresses of §7.1.5 are recognised and labelled.

### Task 4.4 — Catalogue and results contract

`NabtsAnalysisDataset` and `INabtsAnalysisResults` in the SDK stage tier, mirroring
`ITeletextAnalysisResults`: every record the range carried, its channel, address, type, version,
first/last seen frame, times seen, and recovery figures. Wire it into `nabts_sink`, and add the
`decode-orc.stage-tools.nabts-pages.v1` batch-analysis tool descriptor.

*Acceptance:* SDK header manifest (`orc/sdk/sdk_headers.yaml`) regenerated with
`tools/gen_sdk_header_allowlist.sh` and `tools/gen_sdk_header_docs.sh`; `ctest -L sdk` green; the
functional test asserts the ExtraVision capture catalogues its master index (channel 000, record
000) and the CBS service's cyclic marker.

### Task 4.5 — Optional record export

`export_records` parameter writing each assembled record as a separate file beside the packet
stream, named `<channel>-<address>-v<version>.rec`, so the presentation data can be examined with
external tools before Phase 5 lands.

*Acceptance:* records written round-trip byte-identically to the assembled data block bytes;
disabled by default; refused without an output path, as the report is.

## Phase 5 — NAPLPS presentation decoding

Produces a resolved display list rather than pixels, so the MVP boundary stays where the WST
viewer's does: decode in the SDK, render in the GUI. Spec references throughout are to ANSI
X3.110-1983 (§2.3), which is the form CEA-516 §6.1 requires; the teletext SRM of Appendix D Table D1
is the conformance target, so a capability the SRM does not require is a capability the service may
not transmit.

### Task 5.1 — Code environment

The 7-bit environment only (CEA-516 §6.1). C0 set (X3.110 §6.1) and C1 set (§6.2, Figure 65 under
its A = 4 / B = 5 convention for the 7-bit escape form); G0-G3 designation by the escape sequences
of Table 1 and invocation by SI, SO, SS2, SS3 and the locking shifts of Table 2, starting from the
defaults of §4.3.1.3 — G0 primary, G1 PDI, G2 supplementary, G3 mosaic. Codes with no presentation
effect (NUL, the transmission and device control characters of §6.1.4-6.1.6.1) pass through without
terminating a PDI sequence. Unsupported or null-set sequences are skipped with a diagnostic count
rather than aborting the record.

*Acceptance:* unit tests drive designation and invocation sequences and assert the active set after
each, including that a single shift reverts after one character; an unknown escape sequence advances
the parser by its full length; an embedded NUL or DC1 does not terminate an open PDI.

### Task 5.2 — Picture description instructions

The PDI set of X3.110 §5.3, opcodes taken from Figure 13 rather than from any secondary source:
RESET, DOMAIN, TEXT and TEXTURE at 2/0-2/3; POINT SET and POINT in absolute and relative forms at
2/4-2/7; LINE and SET & LINE at 2/8-2/11; ARC and SET & ARC, outlined and filled, at 2/12-2/15;
RECT and SET & RECT at 3/0-3/3; POLY and SET & POLY at 3/4-3/7; FIELD at 3/8; the three INCREMENTAL
forms at 3/9-3/11; SET COLOR, WAIT, SELECT COLOR and BLINK at 3/12-3/15; numeric data in columns 4-7,
discriminated by b7 (§5.3.1).

Operands per Table 3 and the formats of Figures 10-12: single-value 1-4 bytes and multi-value 1-8
bytes at the lengths DOMAIN sets (Tables 4 and 5), coordinates as two's-complement binary fractions
with X in b6-b4 and Y in b3-b1 of each byte, and the third dimension parsed then discarded as Table
D1 permits.

*Acceptance:* unit tests over hand-built PDI sequences produce the expected primitives at the
expected unit-space coordinates for every opcode of Figure 13; operand-length changes mid-record are
honoured; a coordinate that would leave the unit screen is handled by the documented error path
(§5.3.1) rather than wrapping.

### Task 5.3 — Presentation state

The colour map with its three modes and the GRB value format (§5.3.2.5 SET COLOR, §5.3.2.6 SELECT
COLOR), TEXTURE (line texture, highlight, texture pattern, mask size — §5.3.2.4), TEXT (rotation,
character path, inter-character and inter-row spacing, move attributes, cursor style, character field
dimensions — §5.3.2.3), DOMAIN (§5.3.2.2), BLINK, WAIT and the selective RESET of §5.3.2.9, plus
macros (§5.5, §6.2.2 — 96 names, nesting, the definition-terminating control list) and DRCS (§5.6,
§6.2.3 — 96 characters, buffer aspect ratio from the character field, elements set on unless written
in nominal black) within the 3072-byte shared budget of CEA-516 §8.6.1.

Reset values come from the per-command default statements in X3.110 and are cross-checked against
ITU-T T.101 Table II-3, which tabulates the same state in one place.

*Acceptance:* a freshly reset interpreter matches Table II-3 field for field, and its default colour
map matches Table II-3's 16 three-bit-per-gun entries and Table D1's "16 simultaneous colours out of
512"; unit tests assert colour-map writes are visible to subsequent primitives, that a macro defined
and invoked expands to the same display list as its inline form, that a DEF DRCS terminated by
another DEF DRCS advances through the G-set in the circular sequence §6.2.3 specifies, and that the
storage budget is enforced rather than exceeded.

### Task 5.4 — `NabtsPageSnapshot`

The decoded record as a display list in unit space (0 ≤ x < 1, 0 ≤ y < 1 with the display area
0 ≤ y < 0.78125 of §7.2.1), plus the recovery figures for the record. This is what the dataset
carries and the presenter converts.

*Acceptance:* the snapshot is a value type with no SDK decoder types in its interface; a golden test
decodes a captured ExtraVision record to a stable display list.

## Phase 6 — Viewer

Mirrors the teletext viewer path exactly, so the two behave the same way for a user.

### Task 6.1 — View types

`orc/view-types/orc_nabts.h`: `NabtsPrimitiveView` (kind, resolved RGB, unit-space geometry, text
run), `NabtsPageView` (display list, display-area extent, recovery), `NabtsCatalogueRecordView`,
`NabtsRecoverySummaryView`, `NabtsAnalysisView`. Plain value types, no Qt.

*Acceptance:* `ctest -R MVPArchitectureCheck` green.

### Task 6.2 — Presenter

`orc/presenters/{include,src}/nabts_analysis_presenter.{h,cpp}`: dataset → view, resolving the
colour map to RGB and the code points to Unicode so the GUI holds no NAPLPS knowledge. Character and
mosaic mapping follows the repertoires of ITU-T T.101 Appendix I — Repertoire 7 block mosaics use
the same 2×3 sub-cell numbering the teletext presenter already maps sixels through.

*Acceptance:* Tier 1 presenter tests convert a synthetic dataset; an unwritten colour map resolves
to the 16 entries of ITU-T T.101 Table II-3, and a map written by the record overrides them.

### Task 6.3 — Coordinator request

`GetNabtsAnalysisDataRequest` and `RenderCoordinator::requestNabtsAnalysisData()`, following
`requestTeletextAnalysisData()`
([render_coordinator.cpp:427-433](../orc/gui/render_coordinator.cpp#L427-L433)).

*Acceptance:* `RenderCoordinator` tests cover request ordering, response delivery, stale-response
suppression and clean shutdown, as AGENTS.md §4.5 requires.

### Task 6.4 — Dialog and canvas

`NabtsDialog` (record list: channel, address, type, times seen, first/last frame; recovery summary;
linked-record navigation) and `NabtsCanvasWidget` (QPainter over the display list, aspect-correct,
scaling with the dialog). Coordinates arrive as unit-space fractions, so the widget keeps at least
the 12 bits of internal precision X3.110 Appendix B calls for at a 256-pixel resolution.

Wire into `MainWindow` on the pattern the teletext tool already sets: tool-id dispatch beside
`is_teletext_analysis_tool`, per-node dialog and progress-dialog maps keyed on `NodeID` with the
`destroyed` signal erasing both entries, the close-project cleanup loops, and
`onNabtsAnalysisDataReady` / `onNabtsAnalysisProgress` mirroring their teletext counterparts —
including the erase-then-delete ordering that keeps a re-entrant progress callback safe.

*Acceptance:* Tier 3 offscreen smoke test opens the dialog, loads a synthetic view, selects records
and steps a linked series; closing the project destroys both dialog and progress dialog without a
dangling map entry; `QT_QPA_PLATFORM=offscreen ctest -L gui` green.

### Task 6.5 — Captioning

Record type 1 on channel A00 (§7.1.5, §7.3.10) is the NABTS caption service. Surface it in the
viewer as a caption track, and add an `export_captions` parameter writing SubRip cues, with timing
from the 59.94 fields/s of SMPTE 170M.

*Acceptance:* the NBC capture's caption records, if present, produce cues with monotonic timing;
the option is refused without an output path.

## Phase 7 — Documentation and closeout

### Task 7.1 — Stage documentation

`orc/plugins/stages/nabts_sink/instructions.md` covering what the stage does, when to use it, the
tool, every parameter, and the notes — at the depth `teletext_sink/instructions.md` sets. Update
`teletext_sink/instructions.md` to point at it (it currently says NABTS is not supported).

*Acceptance:* both files render in **Help…**; no parameter is undocumented.

### Task 7.2 — User guides

`docs/gui-user-guide/stages/sink-core-stages.md` gains the stage;
`docs/gui-user-guide/dialogues/` gains the viewer. Check
[orc/plugin_ux_capabilities.yaml](../orc/plugin_ux_capabilities.yaml) — the teletext viewer has no
entry, so none is expected, but the `CLI.PluginUxCapabilityParity` gate decides.

*Acceptance:* `ctest -L contracts` green; `mkdocs build` clean.

### Task 7.3 — Validation sweep

Full gate set from AGENTS.md §4.6: build, `ctest --output-on-failure`, `MVPArchitectureCheck`,
`-L sdk`, `-L gui` offscreen, and the functional lane against both NABTS captures and both WST
reference captures (the WST goldens must be unchanged by Phases 1-2).

*Acceptance:* all green; the WST golden stream hashes in `teletext_sink_pipeline_test.cpp` are the
values recorded before this work began.
