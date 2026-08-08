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
[teletext_slicer.cpp](../orc/plugins/stages/common/vbi-services/teletext_slicer.cpp) that names NABTS as "the obvious candidate"
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
> those were in the SDK (`orc/support/teletext_slicer.h`), shared and staying shared. It is the thin
> frame-and-block loop above them that is duplicated, and duplicating it bought each copy its own
> shape — see the table in §2.1.
>
> **Amended 2026-08-08 (same day).** The bit detectors are no longer in the SDK: Phase 1 of
> [the externalisation plan](teletext-nabts-externalisation-plan.md) moved them, the page decoder,
> the row squasher, the recovery statistics and the NAPLPS display-list model into
> `orc/plugins/stages/common/vbi-services` (`orc-vbi-services`). Format-specific decoding for two
> stages has no third-party value, and the catalogue contracts sitting in the ABI-frozen stage tier
> meant a teletext change bumped the host ABI for every plugin in the ecosystem — abi 12 and 13 were
> both exactly that.
>
> This does not reinstate the tie the paragraph above was written to avoid, because the two sinks
> leave the tree *together*: `vbi-services` moves with them into their repository (Phase 4), where
> it stays a shared library between two plugins that were always going to ship as a pair. What the
> earlier decision correctly refused was a shared library binding an external plugin back to the
> *host* tree, and nothing here does that — `orc-vbi-services` compiles against the public SDK
> alone. The `Nabts`-prefixed copies of the frame pass stay copies; only the layers named above are
> shared.

The alternative — a `service` parameter on `teletext_sink` selecting WST or NABTS — was assessed
and rejected. The evidence:

1. **Nothing above the packet is shared, and that is most of a sink stage.** The WST sink's page
   catalogue (`teletext_page_catalogue`), row squasher (`vbi-services/teletext_row_squasher.h`),
   page decoder (`vbi-services/teletext_page_decoder.h`), squash statistics, subtitle export and page
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
   pass and the recovery statistics — already lives in `vbi-services/teletext_slicer.h`,
   `vbi-services/teletext_recovery_stats.h` and the plugin-side `teletext_frame_slicer` /
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
| Bit recovery | `vbi-services/teletext_slicer.h` | — | extended with System C (Phase 1) |
| Frame pass | `teletext_frame_slicer`, `teletext_scan_state`, `teletext_block_scanner` | `nabts_frame_slicer`, `nabts_scan_state`, `nabts_block_scanner` (Phase 2) | — |
| Diagnostics | `vbi-services/teletext_recovery_stats.h` | — | service-aware parity profile (Phase 1) |
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
[teletext_slicer.h](../orc/plugins/stages/common/vbi-services/teletext_slicer.h); extend `teletext_bit_rate()`
and `teletext_packet_bytes()`. Cite CEA-516 §1.3, §3.1 at each constant.

*Acceptance:* `teletext_packet_bytes(kNabts525) == 33`, `teletext_bit_rate(kNabts525) ==
kTeletext525BitRate`; the existing `static_assert` block in
[teletext_slicer.cpp](../orc/plugins/stages/common/vbi-services/teletext_slicer.cpp) is extended to cover the new row and
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
[teletext_line_synthesizer.h](../orc-tests/core/unit/common/vbi-services/teletext_line_synthesizer.h) (framing
byte 0xE7 on the wire, 33 payload bytes, Hamming-coded prefix), and extend
`orc-tests/core/unit/common/vbi-services/teletext_slicer_test.cpp`.

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

Turns the packet stream into the addressable objects a viewer lists. **Done 2026-08-08.**

Everything below the results contract is stage-local, in `orc/plugins/stages/nabts_sink/`: the SDK
carries the types that cross the stage-host boundary and nothing else, which is the boundary §2
settled on.

### Task 4.1 — `nabts_packet.h`: prefix and suffix decoding

Decodes P1-P3, CI, PS (§3.2) and the suffix (§3.4): suffix length from PS b8/b6, longitudinal
odd-parity check over data block plus suffix, and single-bit correction using the product code the
per-byte parity and the longitudinal byte form together. Reports per packet: channel, continuity
index, synchronizing flag, data-block extent, and whether the block was unchecked, clean, corrected
or uncorrectable.

The 28-byte suffix (PS b8/b6 = 1,1) is **not** decoded. §3.4 leaves its bundle error-protection
method "reserved for future standardization", so such a packet carries a zero-length data block and
is counted and skipped — while its continuity index is still consumed, which §3.4 requires.

One thing the standard forced a decision on: the product code needs the odd byte parity §3.3
requires of a *type-zero* group's data blocks, and a packet on its own cannot prove its group's type
— the group header is in the synchronizing packet, which the packet in hand may not be. The
correction runs anyway, bounded to one bit and only after the longitudinal check has already failed,
so a clean packet of any group type is never touched. Recorded at the head of
`nabts_decode_packet()`.

*Acceptance, as met:* 14 tests. The single-bit correction is swept exhaustively — every bit of every
byte of the prefix, and every bit of every byte of the data block and suffix — rather than sampled.
Two-bit errors are detected and reported uncorrectable in both their shapes (two bytes, and one byte
twice). A 1,1 packet contributes no data-block bytes and keeps its place in the continuity chain.

### Task 4.2 — `nabts_data_group.h`: group reassembly

Per data channel, accumulates packets from a synchronizing packet (§4.1) using S1/S2 and F1/F2 from
the eight Hamming-coded header bytes (§4.2), with the continuity index detecting loss (§3.2.4).

Three details of §4.2.6 and §8.4.2.6 that a plain concatenation gets wrong, all covered:
F1,F2 greater than a block reads as full; F1,F2 of zero discards the final non-zero block entirely;
and the final non-zero block need not be the last packet, because zero-length blocks with 28-byte
suffixes may follow it.

Bounded twice over: a group claiming more than the 67 further blocks of §8.4.2.5 is refused before
it can reserve what it asked for, and at most `kNabtsMaxOpenGroups` (32) groups are held open, which
is what stops a misread prefix inventing channels from opening a buffer for each of §3.2.3's 4096.

*Acceptance, as met:* 22 tests, covering reassembly across every suffix length, interleaved
channels, the continuity-index gap, both size bounds, and the wrap limit — a loss of exactly 16
packets is invisible, which is a limit of the standard and is pinned as such.

### Task 4.3 — `nabts_record.h`: record headers and linking

Parses the record header (§5.2): RT, RD, A1-A3, the optional A4-A9 address extension, the L1/L2
link, the classification sequence's pointer and flag bytes (§5.2.7), and header extension fields
(§5.2.8). Joins a linked series into a message (§5.2.6) keyed on {channel, record address, version}.
Application records (type 2) are split into their function descriptors (§7.2.2); presentation records
(types 0, 1, 3) hand their data on to Phase 5.

`HammingCursor` is why this is readable: §5.2.9 says the header's end "is a consequence of the
Record Header format itself and is not necessarily indicated by any one byte", so decoding is a walk
that can fail two ways at every step — out of bytes, or a byte that will not correct. Both are
terminal and both are folded into the cursor, so the walk states the format and nothing else.

*Acceptance, as met:* 31 tests. Short and long addresses and the equivalence §5.2.5 requires of
them; an unlinked record; a three-record series arriving out of order; a classification sequence with
two pointer bytes; chained header extensions; the reserved addresses of §7.1.5, including a long
address that reduces to one.

**A bug this found.** §5.2.8.2 puts the header-extension meaning in EI b6/b4/b2 with b8 as the
continuation flag — the low three bits of the information nibble. The first implementation shifted
right by one, reading b8 as part of the meaning. No real capture in the reference set carries a
header extension, so only the test caught it.

### Task 4.4 — Catalogue and results contract

`NabtsCataloguedRecord`, `NabtsRecoverySummary`, `NabtsAnalysisDataset` and `INabtsAnalysisResults`
in `orc/stage/analysis_sink_results.h` alongside the teletext ones, plus the stage-local
`nabts_record_catalogue.{h,cpp}` that fills them. The stage declares
`decode-orc.stage-tools.nabts-pages.v1` as a batch-analysis tool, which is what Phase 6's dialog
hangs off.

A catalogue entry is a *message* rather than a record, because §5.2.6 makes that the unit a receiver
presents. Which copy is kept is decided by quality before recency: a complete, undamaged copy is
never replaced by a damaged one, so a recording that degrades keeps the copy that arrived cleanly
rather than the last one before the tape ran out.

The SDK header manifest needed no change — `analysis_sink_results.h` is already on the allowlist —
so no regeneration was required.

*Acceptance, as met:* 17 catalogue tests; `ctest -L sdk` green. On the real captures:

| | records | groups complete | messages complete / partial | blocks corrected / damaged |
|---|---|---|---|---|
| CBS ExtraVision | 51 | 53 | 51 / 0 | 8 / 3 |
| NBC Teletext | 13 | 17 | 13 / 0 | 0 / 0 |
| TBS Electra (WST read as NABTS) | **0** | **0** | 0 / 0 | 0 / 0 |

The plan's stated acceptance — the master index at channel 000 address 000, and a cyclic marker —
was wrong about what this window contains: 300 frames is about one carousel cycle of the ExtraVision
magazine and it includes neither. What the recording does carry is a better test, and is what the
functional test now asserts: **channel A00, record address 000, record type 1, caption flag set.**
§7.1.5 reserves that channel and address for the start of captioning, §5.2.2.3 makes captioning a
type-1 record, and §5.2.7.3 has a caption record carry the caption flag in Y13 — three facts from
three sections of the standard, decoded by three separate parts of this (the address bytes, the
record type byte, and the classification pointer-and-flag walk), agreeing. A parse with any of those
bit positions wrong could not produce that agreement by chance.

### Task 4.5 — Optional record export

`export_records` writes each catalogued record beside the packet stream as
`<stream>.<channel>-<address>-v<version>.rec` — the identity §5.2.1 gives the record, so a directory
of them sorts into the order the records dialog lists them. Off by default; refused without an
output path, as the report is.

*Acceptance, as met:* the functional test reads every exported file back and compares it byte for
byte against the catalogued data. Which is how it was found that `write_records()` had been written
and never called.

### Two findings from the real captures

**1. The record layer rejects everything the bit detectors leak.** §2's claim was that the framing
code separates System B from System C; Phase 3 measured that as 126 false packets from a WST
recording against 2460 from a real NABTS one, all from the MLSE detector. Those 126 now produce
**zero** data groups and **zero** records: noise that passes a framing code and five Hamming bytes
does not also carry a group header that decodes. The 20x separation at the packet layer is total at
the record layer.

The same holds within a NABTS recording. Read on the full BT.653 window the ExtraVision capture
yields 2460 packets; read on the four lines the service actually uses it yields 2288. The 172
difference is noise on empty lines, and the report accounts for it exactly — 172 refused group
headers. Both runs catalogue the same 51 records.

**2. Spurious locks on empty lines cost the data-phase pin.** `NabtsPhaseTracker` pools its locks
over every line of the window, so those 172 spurious locks widen the distribution past
`kMaxRadiusSamples` and the hint is withheld: narrowed to lines 15-18 the same recording pins to
sample 146,1 +/- 3,0, and on the full window it does not pin at all. Pinning cannot lose a packet —
a hinted attempt that fails is retried over the full window — but the two runs acquire differently,
and on a marginal line the MLSE fit can settle on a different bit, which is why one of the 51 records
differs by a few bytes between them.

So narrowing the candidate window to the lines a service actually uses is worth doing twice over: a
cleaner packet stream, and the phase pin engages. Worth saying in the stage instructions (Phase 7).

Both are pinned by `TheRecordsDoNotDependOnTheCandidateLineWindow` and the record-layer half of
`AWstCaptureYieldsAlmostNothing`.

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

Mirrors the teletext viewer path, so the two behave the same way for a user.

> **Built 2026-08-08.** All five tasks are done. Two things were found on the way and are recorded
> in place: the character repertoires had to go into the SDK rather than the presenter (Task 6.0
> below, which the plan did not anticipate), and the interpreter was advancing the cursor over a
> non-spacing mark, which put a gap in front of every accented letter (Task 6.2).

### Task 6.0 — Character repertoires in the SDK (added)

Not in the original plan. Task 6.2 called for the presenter to map code points to Unicode, and Task
6.5 needs the same mapping to write caption text out of the *stage* — where a presenter does not
reach. Two implementations that must agree is the outcome to avoid, so the mapping went into the
support tier instead, beside the display list it reads:
[nabts_page.h](../orc/plugins/stages/common/vbi-services/nabts_page.h) gains `nabts_primary_to_unicode()`,
`nabts_supplementary_to_unicode()`, `nabts_supplementary_is_nonspacing()`, `nabts_is_mosaic_code()`,
`nabts_mosaic_sixels()`, `nabts_character_to_utf8()` and `nabts_page_text()`.

**The repertoires came from X3.110 itself rather than from T.101 Appendix I** (which Task 6.2
suggested). The primary set is ASCII across 2/1 to 7/14 — §7.2 bases the repertoire on
ANSI X3.4-1977, and Table 25 puts the number sign at 2/3, the dollar at 2/4, the grave at 6/0, the
circumflex at 5/14 and the tilde at 7/14, each where ASCII has it. The supplementary set was
transcribed position by position from the Coded Representation column of Tables 18 to 27, which
names the code position of every character the set carries; the layout that falls out is ISO
6937-1982's, which §7.2 names as its source, **plus X3.110's own additions at 5/6 to 5/11 and 6/5**
— the full horizontal and vertical lines, the four diagonals and the cross of Table 25 notes 1 to 7,
where ISO 6937 has `¬`, `¦` and three vacancies. A mapping taken from ISO 6937 alone would have got
those seven wrong.

The mosaic set was read off **Figure 62** as an image (`pages/page-108/img-72.jpeg` and
`img-73.jpeg`): the 2×3 cell is b1 top-left, b2 top-right, b3 middle-left, b4 middle-right, b5
bottom-left, **b7** bottom-right, with b6 = 1 marking the position as a mosaic. That is columns 2,
3, 6 and 7 — 64 positions — plus the second copy of the solid mosaic §5.4 places at 5/15, giving the
65 the text claims. **It is the same allocation World System Teletext uses for its G1 sixels**, so
the packing is literally the teletext presenter's `(code & 0x1F) | ((code >> 1) & 0x20)`, and 5/15
falls out of it as all-on without a special case.

*Acceptance:* met — `nabts_repertoire_test.cpp` checks 35 supplementary positions against the
tables that name them, the seven X3.110 additions, that column 4 and only column 4 is non-spacing,
and that the mosaic set has exactly 65 positions with Figure 62's bit order.

### Task 6.1 — View types

Built as planned: [orc_nabts.h](../orc/view-types/orc_nabts.h) with `NabtsColourView`,
`NabtsPrimitiveView`, `NabtsDrcsGlyphView`, `NabtsTextureMaskView`, `NabtsPageRecoveryView`,
`NabtsPageView`, `NabtsRecordFunctionView`, `NabtsCatalogueRecordView`, `NabtsCaptionCueView`,
`NabtsRecoverySummaryView` and `NabtsAnalysisView`. Plain value types, no Qt.

One departure from the plan's shape: `NabtsPrimitiveKind::kCharacter` splits into **three** view
kinds — `kText`, `kMosaic` and `kDrcs` — because a renderer does something different with each and
nothing useful with a union of the three. `kText` is the "text run" the plan asked for: consecutive
character primitives sharing their attributes and lying a character field apart along one axis are
coalesced into one entry with a UTF-8 string, a character count and the measured `advance`.

*Acceptance:* met — `ctest -R MVPArchitectureCheck` green.

### Task 6.2 — Presenter

[nabts_analysis_presenter.{h,cpp}](../orc/presenters/src/nabts_analysis_presenter.cpp): dataset →
view. Three things resolve here — colour (three bits per gun to 8-bit channels, and the incremental
colour runs of §5.3.3.6.3 through the colour map or as Figure 12 values depending on the colour
mode), characters (repertoire lookup and run coalescing), and the caption track.

**Finding: the four format effectors were all doing the same thing.** §6.1.2 defines APB, APF, APD
and APU as four *different* movements relative to the character path — back along it, forward along
it, and −90 and +90 degrees across it by the *interrow* spacing of §5.3.2.3.5 rather than the
inter-character spacing. Phase 5 collapsed all four onto "advance along the path", so APD (the line
feed) moved right instead of down and APR then put the cursor back at the left of the row it was
already on. Every multi-line record drew its rows on top of each other, one character out of step —
visible on the ExtraVision news pages as two headlines interleaved character by character.

Fixed in [naplps_interpreter.cpp](../orc/plugins/stages/nabts_sink/naplps_interpreter.cpp) with
`move_cursor_by(CursorMove)`, which rotates the path vector so all four follow the path wherever
TEXT put it, and picks the inter-character or interrow distance by whether the movement runs along
the path or across it. Six tests pin the four effectors, the APR-then-APD row break, and the
rotation property on a non-default path.

**Finding: the interpreter advanced the cursor over a non-spacing mark.** §7.2 transmits a composite
character as a mark from the supplementary set *followed by* the letter it applies to, and §7.1
makes the pair one character of the repertoire — so the pair occupies one character field. The
interpreter was calling `advance_cursor()` after every graphic including the mark, which put a blank
field in front of every accented letter. Fixed in
[naplps_interpreter.cpp](../orc/plugins/stages/nabts_sink/naplps_interpreter.cpp); the presenter then
composes the mark onto the letter behind it, which is Unicode's order and the reverse of the
transmission's. Nothing in the reference captures is accented, so only the unit test found it.

*Acceptance:* met — `nabts_analysis_presenter_test.cpp` converts synthetic datasets; an unwritten
colour map resolves to the 16 entries of T.101 Table II-3 (grey ramp low, hues high, entry 8 pure
blue) and a map the record wrote overrides them.

### Task 6.3 — Coordinator request

`GetNabtsAnalysisDataRequest`, `RenderCoordinator::requestNabtsAnalysisData()`,
`nabtsAnalysisDataReady` / `nabtsAnalysisProgress`, and
`RenderPresenter::getNabtsAnalysisData()` — fetch-or-trigger, exactly as the teletext path does it.

*Acceptance:* met — five `RenderCoordinator` tests: cached-catalogue service, trigger-then-retry,
failure after trigger, distinct ids for stale-response suppression, and clean shutdown with
requests in flight.

### Task 6.4 — Dialog and canvas

[NabtsDialog](../orc/gui/nabtsdialog.cpp) and [NabtsCanvasWidget](../orc/gui/nabtscanvaswidget.cpp),
wired into `MainWindow` on the teletext tool's pattern (tool-id dispatch, per-node dialog and
progress-dialog maps keyed on `NodeID` with `destroyed` erasing both, the close-project cleanup
loops, and the erase-then-delete ordering in `onNabtsAnalysisDataReady`).

The dialogue is a record table beside a stack of three panes, because a NABTS service holds three
genuinely different things: a **presentation record** is drawn on the canvas with its text beside
it, an **application record** is listed as its function descriptors (§7.2.2), and the **caption
track** replaces both on request.

**"Linked-record navigation" is stepping the catalogue.** §5.2.6's linked series is joined into one
message by the record assembler, so there is nothing left to navigate there. What a receiver
actually steps is §7.3's Next record, which "is either defined in Header Extension Field or is
obtained by incrementing the Record Address of the present Record" — and the catalogue is already in
ascending {channel, address, version} order, so stepping the list *is* stepping the service. The
header-extension redefinition is deliberately not followed: the extension-data table of §5.2.8.4 is
visibly garbled in the copy in hand (row 12 merges two meanings, and rows 6 and 9 label a 6- and a
9-byte field as a "Short Record Address"), no reference capture carries a header extension, and
guessing at a corrupt table would be worse than the documented default.

**Finding: an ARC was drawn as a quadratic Bezier, so circles vanished.** §5.3.3.3.1 codes a circle
as "an arc whose end points coincide and whose intermediate point (with the end points) defines the
diameter". Fitting a quadratic through those three points collapses it to a degenerate there-and-back
curve enclosing no area, so a filled circle painted **nothing** — which is why the CBS eye was missing
from the left of the ExtraVision index page's logotype while everything else on it drew. The canvas
now builds the real circle: the circumcircle of the three points, swept the way round that passes
through the intermediate one, with §5.3.3.3.1's two degenerate readings handled explicitly — start
coincident with end is a circle about the midpoint of start and intermediate, and three colinear
points are "a line drawn from the start point to the end point".

A latent second bug came out of the same clause and is fixed with it: "If the end point is omitted,
it is taken to be coincident with the start point and a circle is drawn." The interpreter was
counting a two-point ARC as a truncated PDI and dropping it, when it is the compact encoding of a
circle. The ExtraVision pages do not use it — their `truncated_pdis` is zero — so this was found by
reading rather than by seeing it fail.

**Finding: the cursor started at the bottom of the screen, so text-and-line-feed
records piled onto one row.** T.101 Table II-3 lists data syntax III's
current-text-position as "lower left corner", which Phase 5 read as the bottom
left of the screen. X3.110 says otherwise three times — §5.3.2.9.3 sends a reset
cursor to "its home position (top left character position in the display area)",
§6.1.2.6 and §6.1.2.8 home CS and APH to the upper left, and §6.1.6.5(6) numbers
NSR's rows from "the upper leftmost character position" — and Table II-3 itself
gives the other two data syntaxes an "upper left corner". The ExtraVision service
settles it: several of its records are plain text with CR and LF and nothing
else, so starting at the bottom clamps every line feed and puts the whole record
on the bottom row. "Lower left corner" is the corner of the *character field*
(§5.3.2.3.2), not of the screen. The text cursor now starts at home; the drawing
point still starts at the geometric origin, which Table II-3 lists separately for
the syntaxes that have both.

**Finding: NSR was not consuming its cursor address.** §6.1.6.5(6) makes NSR
"an alternative means to position the cursor": when the two bytes after it are
both from columns 4 to 7 they are a row and a column and are consumed. Phase 5
declined to consume them, so every ExtraVision record — which opens
`SO CAN NSR @ @`, i.e. home to row 0 column 0 — drew two stray `@` glyphs and
never moved. The full clause is now implemented, including "if the two bytes are
from columns 2 and 3 … they are ignored" and a following C0 terminating the
sequence to be executed in its own right.

**Finding: a SET COLOR shorter than the declared operand length was dropped.**
§5.3.2.5.1 lets a colour operand be shorter than the map can hold — "trailing
zero bits are supplied by the receiving presentation process" — so the DOMAIN
multi-value length is a maximum, not a requirement. Phase 5 required a whole
word and silently discarded anything shorter. ExtraVision sets white with a
single byte where DOMAIN declared three, so the CBS eye was drawn in whatever
colour came before it: the page background. Its arcs are white on the outside and
background-blue inside, and both were blue.

The same clause fixes how a short word scales. Its last sentence — "For each
primary, the maximum color fraction attainable, given the number of bits
specified in the color value operand, shall be interpreted as full intensity and
intermediate values shall be equally distributed between zero and full
intensity" — contradicts the zero-fill sentence for operands narrower than three
bits, and it is the one to follow: zero-filling makes the brightest colour a
one-byte operand can express 6 of 7 rather than white, which would leave a
service unable to send white at all in the encoding ExtraVision actually uses.
Two bits now read 0, 2, 5, 7 rather than 0, 2, 4, 6.

**Geometry.** The display area's nominal resolution is 256 × 200 (T.101 Table II-3) inside a unit
rect 1 × 0.78125, so its pixels are square — 1/256 across and 0.78125/200 = 1/256 down. The canvas
therefore keeps a 1 : 0.78125 aspect and uses **one** scale factor for both axes, which is what stops
a circle coming out an ellipse. Coordinates stay `double` from the interpreter to the QPainter call,
so the 12 bits X3.110 Appendix B asks for survive.

These four were found by decoding the reference ExtraVision capture and dumping
the display list rather than by reading the code — the capture is in-tree at
`test-data/teletext/NTSC NABTS Teletext samples/`, and two `vbi_source`
functional tests were silently skipping over it because they named the directory
`NTSC Teletext samples`. That path is corrected, so those two now run.

*Acceptance:* met — fifteen offscreen tests open the dialogue, load a synthetic view, select records,
step the series (wrapping at both ends), list an application record's descriptors, list the caption
cues, and paint a display list of text, line, rectangle and mosaic primitives. Three of them work on
the rendered pixels rather than on the primitive count, because a display list can be walked with
nothing reaching the screen — which is exactly how the vanishing circle hid. The map cleanup is the
teletext path's, unchanged.

### Task 6.5 — Captioning

The caption service is surfaced twice from one implementation: `nabts_caption_cues()` in the SDK
([nabts_captions.cpp](../orc/plugins/stages/common/vbi-services/nabts_captions.cpp)) reads a catalogue into cues, and both the
viewer's caption track and the stage's new `export_captions` parameter go through it — so the file
and the screen cannot disagree about what the service said.

Three readings the standard settles and the code follows:

1. **The Caption Flag, not the channel.** §7.3.10 makes A00/000 the entry point a receiver *acquires*
   captioning through; the captions themselves are whatever records carry the Caption Flag of
   §5.2.7.3. Filtering on the channel would have missed a service whose captions sit elsewhere.
2. **A cue runs to the next cue.** §7.3.10.1 has the receiver replace the caption on screen rather
   than being told when to take it down, so the next caption *is* the end of this one. The last cue
   runs to the last frame its own record was seen at, which is all the recording says about it.
3. **A caption record that drew nothing is an erase**, not an empty caption — §7.3.10.1's "Captions
   may be erased by the use of PLPS code that erases either the entire display". Such a record ends
   the cue before it and yields none of its own.

Cue timing is frames at 30000/1001 per second, which is the 59.94 fields/s of SMPTE 170M. The
document is written beside the packet stream as `<output>.t33.srt`, on the same rule the report and
the record files follow, and the parameter is refused without an output path for the same reason.

*Acceptance:* met for what can be tested here — eight SDK tests cover cue ordering, extents,
the erase case, the flag-not-channel rule and monotonic timing when two captions share a frame; two
stage tests cover the parameter and its refusal. The reference captures available in-tree carry no
caption records, so the "NBC capture" half of the original acceptance is untested against real data
and is left for Phase 7's sweep to revisit if a captioned capture appears.

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

**The viewer did not go in `dialogues/`.** That directory holds the dialogues that attach to a
previewed stage — Preview, Frame-timing, Line-scope and the main window — and each of its pages says
so of itself. A stage tool is a different thing, and the precedent is already set: the Teletext Pages
viewer is documented under **Stage tools** within its stage's section of `sink-core-stages.md`, not
as a dialogue. NABTS Records follows it. `plugin_ux_capabilities.yaml` needed no entry either, for
the reason the task anticipated: it records the plugin- and stage-*management* capabilities that
`orc-gui` and `orc-cli` must keep in parity, and a per-stage viewer is not one of them.

`sink-analysis-stages.md` also carried a sentence explaining why the Teletext Sink lives under sink
stages despite offering a batch-analysis dialog; that now covers both stages.

### Task 7.3 — Validation sweep

Full gate set from AGENTS.md §4.6: build, `ctest --output-on-failure`, `MVPArchitectureCheck`,
`-L sdk`, `-L gui` offscreen, and the functional lane against both NABTS captures and both WST
reference captures (the WST goldens must be unchanged by Phases 1-2).

*Acceptance:* all green; the WST golden stream hashes in `teletext_sink_pipeline_test.cpp` are the
values recorded before this work began.

Result: 3028 tests pass, 0 fail. `MVPArchitectureCheck` 1/1, `-L sdk` 9/9, `-L gui` 469/469 offscreen,
`mkdocs build` clean. All 11 NABTS functional tests and all 8 WST ones ran — none skipped — and
`teletext_sink_pipeline_test.cpp` has not been touched by any commit in this series, so the four WST
golden stream hashes are literally the pre-work values and the shared-slicer changes of Phases 1-2
did not move the recovered stream.

### What a marginal recording looks like, measured

Recorded here because it is the question a user asks first, and the answer is not in the code.

The NBC Teletext capture is VHS at **EP**, and it browses as nonsense: 239 records catalogued, 212 of
them drawing something, almost none legible. That is the recording, not the decode. Both captures,
decoded end to end at identical settings:

| | ExtraVision (VHS SP) | NBC Teletext (VHS EP) |
|-|-|-|
| MLSE decision confidence | 0.55 | 0.22 |
| record data bytes failing odd parity (§3.3) | 0.024 % (65/270425) | 7.10 % (9053/127554) |
| packets orphaned rather than placed in a group | 0.09 % (277/293575) | 62 % (111351/178730) |
| record headers refused | 1 of 7736 groups | 866 of 2824 |
| linked series joined | 1246 | 0 |
| blocks mended by the §3.4 product code | 768 | 0 (the service sends no suffix) |

The first row is the one to read. `AWstCaptureYieldsAlmostNothing` already records that a World System
Teletext recording read as NABTS — pure noise fitted by MLSE — arrives at a mean decision confidence
of 0.21. The EP tape is at 0.22: its bit decisions are at the noise floor.

Why 7 % of bytes destroys a page rather than blemishing it is the difference between the two services.
A WST page is a grid of independent parity-coded bytes, so damage stays where it lands. A NABTS record
is a stateful byte stream — opcodes, operand counts and coordinates — so one wrong byte changes how
the several after it are read. Record 000/001 has a row reading `EECFMAC 15, 1983` against a broadcast
`DECEMBER 15, 1983` (D→E, E→F, B→A, E→C, one or two bit flips each), and its whole content collapses
into the top eight rows because the positioning codes are hit too.

**The one thing that would help and is not done.** NBC runs a carousel: each record arrives 8 to 19
times. `NabtsRecordCatalogue::copy_is_better` keeps exactly one copy — the intact one if there is one,
otherwise the longest. No NBC record ever arrives intact, so it is always "longest wins", and length
is uncorrelated with correctness. Combining the copies would be a large win, and parity makes it cheap:
odd parity flags roughly seven of every eight corrupted bytes, so per byte offset one could take the
first copy that passes, or majority-vote among those that do. The obstacles are real though — copies
differ in length, alignment is not guaranteed where a group boundary was misdetected, and versions must
stay separate (they already are, being distinct catalogue keys). It is the same idea as
`squash_repeated_rows` in the teletext sink, one layer up.
