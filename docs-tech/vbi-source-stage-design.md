# decode-orc — VBI capture source stage

Design document for a generic source stage that ingests third-party raw VBI teletext
captures and presents them to decode-orc's existing teletext decoders as CVBS.

Target output: the [CVBS File Format Specification](https://simoninns.github.io/cvbs-file-format-specification)
(`PAL` / `NTSC` video standard presets, `CVBS_U10_4FSC` sample encoding).

Items marked **[verify]** need checking against real sample files.

---

## 1. Purpose and scope

Several long-standing teletext archives exist as raw VBI sample dumps produced by consumer
capture cards or cropped out of `.tbc` files. They are headerless, self-describing in no
way whatsoever, and each carries a different sample rate, sample width, line length and
line count. decode-orc should be able to consume them without a bespoke decoder per format.

**The stage's job is a geometry and level transform, nothing more.** Every input format in
scope is already time-base corrected — either by the capture card's line-locked sampling
clock, or by ld-decode/vhs-decode upstream. There is no sync to recover, no line-length
variation to correct, no jitter to remove. What the stage does is:

1. Parse a flat sequence of fixed-length line records into (frame, field, line) tuples.
2. Work out **where each of those lines belongs in a CVBS frame** — vertically (which frame
   line) and horizontally (which sample offset within that line).
3. Resample from the source rate to 4×fsc and map sample values into the preset's 10-bit
   amplitude domain.
4. Synthesise the rest of the frame — sync, blanking, vertical interval, optionally burst —
   so the result is a structurally valid CVBS frame.

**Explicitly out of scope:** slicing, deconvolution, packet decoding, page assembly. Those
belong to the teletext decoder stages that already exist. The source stage's success
criterion is simply: *the existing teletext decoder, pointed at the output, produces the
same packets it would from a native decode.*

### 1.1 Design principles

- **The stage is a CVBS encoder, not a decoder.** It writes standards-correct signal from
  a known input, rather than inferring structure from an unknown one.
- **All input parameters are explicit.** Auto-detection is offered as a convenience that
  produces a *suggested configuration*, never as a silent default. Misdetection here yields
  plausible-looking output that is subtly wrong.
- **Everything synthesised is declared.** The output metadata must make clear which parts of
  the frame carry recovered signal and which are manufactured, so nothing downstream
  mistakes a synthesised burst for a measured one.
- **Provenance survives the transform.** A packet emerging from the teletext sink must be
  traceable back to a source line record. Since the mapping is deterministic, this is a pure
  index relation — no bookkeeping required, provided the stage never silently drops or
  reorders lines.

---

## 2. The output contract

### 2.1 What the CVBS spec requires

| Aspect | Requirement |
|---|---|
| File layout | Sequence of frames, each two sequential fields, no headers or markers |
| Extension | `.composite` (this stage produces composite only; there is no chroma to separate) |
| Vertical origin | Stored frame line 0 = broadcast frame line 1 = first line of field 1 |
| Horizontal origin | 0H-aligned: sample 0 of a stored line is the first sampling instant at or after that line's 0H. Sync and blanking occupy the **start** of the line |
| Sample word | `CVBS_U10_4FSC`: signed 16-bit LE holding the 10-bit value 0–1023 directly |
| PAL frame | 709,379 samples (1,418,758 bytes) |
| NTSC frame | 525 × 910 = 477,750 samples (955,500 bytes) |
| Metadata | Optional `.meta` SQLite sidecar; the data file is self-contained |

### 2.2 Amplitude domain

Both level tables are normative for `CVBS_U10_4FSC`:

| Level | PAL | NTSC |
|---|---|---|
| Protected min (must never appear) | 0–3 | 0–3 |
| Sync tip | 4 | 16 |
| Blanking (0 IRE) | 256 | 240 |
| Black | 256 (no pedestal) | 282 (+7.5 IRE setup) |
| White (100 IRE) | 844 | 800 |
| Peak with chroma | 1019 | 1019 |
| Protected max | 1020–1023 | 1020–1023 |

Derived teletext levels — the two numbers the level mapper actually needs:

| | PAL (WST) | NTSC (NABTS) |
|---|---|---|
| Logic 0 | black = **256** | blanking = **240** |
| Logic 1 | 66 % of white = 256 + 0.66 × 588 = **644** | 70 IRE = 240 + 70 × 5.6 = **632** |
| Data amplitude | 388 counts | 392 counts |

(WST defines logic levels on a scale where black is 0 % and white is 100 %, with logic 1 at
66 %. NABTS defines logic 0 at blanking level. In PAL black *is* blanking, so the two
conventions coincide there.)

The stage must clamp to [4, 1019] before writing. With the levels above there is no
realistic risk of collision with the protected ranges, but overshoot from the anti-alias
filter can push a sharp CRI edge outside the nominal band, so clamp anyway.

### 2.3 PAL is not orthogonal — this is the main trap

PAL 4fsc gives **1135.0064 samples per line**, not 1135. Over 625 lines that is exactly
709,379 samples, and the sampling lattice repeats at *frame* rate rather than line rate.

**Do not index frame line _k_ at _k_ × 1135.** Compute line starts from the 0H rule:

```
line_start(k) = ceil(k × 1135.0064)      # k = 0 … 624
line_length(k) = line_start(k+1) − line_start(k)
```

This yields 621 lines of 1135 samples and **4 lines of 1136 samples per frame**. With the
spec's stated origin (0H of line 0 immediately preceding sample 0) the long lines fall at
frame lines 0, 156, 312 and 468. A naive constant-1135 stride drifts by up to 4 samples
(≈225 ns, ≈1.6 teletext bit periods) by the bottom of the frame, and produces a frame that
is 4 samples short — which then desynchronises every subsequent frame in the file.

There is a second, subtler consequence. Because sample 0 of each line is the first sampling
instant *at or after* 0H, each line has its own sub-sample phase offset:

```
phase(k) = line_start(k) − k × 1135.0064      # in [0, 1) samples
```

For the PAL teletext lines this works out at:

| Frame line (0-based) | Broadcast line | Line start sample | phase (samples) |
|---|---|---|---|
| 6 | 7 | 6 811 | 0.962 |
| 21 | 22 | 23 836 | 0.866 |
| 319 | 320 | 362 068 | 0.958 |
| 334 | 335 | 379 093 | 0.862 |

Teletext data should be placed at *time* 10.198 µs after 0H, which is
`180.860 − phase(k)` samples into the stored line. Ignoring `phase(k)` costs up to one
sample of placement error (56 ns, 0.39 of a bit) — harmless to a CRI-locking slicer, but
worth doing properly since it's a single subtraction in the resampler's fractional delay.

NTSC and PAL-M are orthogonal (910 and 909 samples/line exactly), so none of this applies —
`line_start(k) = k × 910`.

### 2.4 Presets to declare

| Field | Value | Rationale |
|---|---|---|
| `preset` | `PAL` / `NTSC` | From configuration; not inferable from the data alone |
| `sample_encoding_preset` | `CVBS_U10_4FSC` | The cvbs-encode encoding; natural target for a synthesiser |
| `signal_state_preset` | `STANDARD_TBC_UNLOCKED` (default) | Output is at exactly 4×fsc with fixed line lengths, so TBC is genuinely applied — but see below |
| `signal_type` | `composite` | |
| `decoder` | `other` | The spec's enumerated range is `ld-decode`, `vhs-decode`, `cvbs-encode`, `cvbs-decode`, `other`. Worth raising upstream whether decode-orc should be added |
| `number_of_sequential_frames` | known at write time | |
| `black_level` | `NULL` | There is no active picture; the preset default applies |
| `has_nonstandard_values` | `FALSE` | Everything written is inside the legal domain |
| `capture_notes` | synthesis description | See §7.3 |

**On `LOCKED` vs `UNLOCKED`:** claiming `STANDARD_TBC_LOCKED` asserts that subcarrier phase
is stable and known *and* — per the spec's frame boundary integrity clause — that frame
boundaries and the colour-frame progression are preserved with no skipped, duplicated or
shifted frames. If the stage synthesises a coherent burst sequence and the source has no
detected frame drops, both are true by construction and `LOCKED` is defensible. If burst is
omitted, or if the bt8x8 frame counter shows gaps (§6.3), the stage **must** fall back to
`UNLOCKED`. Make this an automatic consequence of the run, not a user-set flag.

---

## 3. Input model

### 3.1 The generic container

Every format in scope reduces to the same abstraction: a flat sequence of fixed-length line
records, grouped into fields, with some lines useful and some not.

```
sample_rate        Hz (exact rational preferred)
line_length        samples per stored line record
sample_format      u8 | u16le | s16le
field_lines        stored line records per field (the stride)
field_range        which of those records carry teletext, e.g. 0..15
capture_offset     time of sample 0 of each record, relative to 0H
first_field        which TV field the first stored field is
tv_system          PAL | NTSC | PAL_M
tt_system          WST | NABTS
```

`field_lines` and `field_range` differ because some sources store more lines than are
useful — cx88 stores 18 records per field with teletext in 1–16; a full `.tbc` stores an
entire 313-line field with teletext in 6–21.

`capture_offset` is the parameter that no format records and every conversion needs. It is
the single most important configuration value in the stage. See §6.2.

### 3.2 Known formats

| Config name | Sample rate (Hz) | Line length | Format | Field lines | Useful (0-based) | capture_offset | Bytes/frame |
|---|---|---|---|---|---|---|---|
| `bt8x8-pal` | 35 468 950 (8·fsc PAL) | 2048 | u8 | 16 | 0–15 | ~244 samples ≈ 6.879 µs | 65 536 |
| `bt8x8-ntsc` | 28 636 363 (8·fsc NTSC) | 2048 | u8 | 16 | 0–15 | ~244 samples ≈ 8.521 µs | 65 536 |
| `cx88-pal` | 35 468 950 | 2048 | u8 | 18 | 1–16 | ~214 samples ≈ 6.0 µs **[verify]** | 73 728 |
| `saa7131-pal` | 27 000 000 | 1440 | u8 | 16 | 0–15 | ~265 samples ≈ 9.8 µs **[verify]** | 46 080 |
| `tbc-pal` | 17 734 475 (4·fsc PAL) | 1135 | u16le | 313 | 6–21 | 0 | 1 421 020 |
| `tbc-vbi-pal` | 17 734 475 | 1135 | u16le | 16 | 0–15 | 0 | 72 640 |
| `tbc-vbi-ntsc` | 14 318 182 (4·fsc NTSC) | 910 | u16le | 16 | 0–15 | 0 | 58 240 |

Sources: `teletext/vbi/config.py` in
[ali1234/vhs-teletext](https://github.com/ali1234/vhs-teletext) for the card table; the
Linux `bt8xx` driver for the bt8x8 values; ld-decode `SysParams` for the 4fsc rates.
`tbc-vbi-ntsc` is not in vhs-teletext's table but is the format of circulating NTSC samples
(the ffplay recipe for those files uses `gray16`, `910x16`, `weave`).

Three corrections to assumptions that circulate in the community:

- **2048 samples/line is not universal** — it applies only to the bt8x8/cx88 family.
- **The sample rate does vary between NTSC and PAL.** The bttv driver selects it from the
  TV standard: 35 468 950 Hz for 625-line, 28 636 363 Hz for 525-line. What is invariant is
  the *samples-per-line count*, because 2048 is a buffer-size constant, not a timing one.
  This is almost certainly the origin of the confusion, and it matters — assuming the PAL
  rate on an NTSC capture gives a 24 % bit-width error.
- **vhs-teletext's `tbc` sample rate of 17 730 000 Hz is approximate.** The correct value
  is 4 × fsc = 17 734 475 Hz; ali compensates with `--sample-rate-adjust`. Use the exact
  value.

### 3.3 Per-format quirks the reader must handle

**bt8x8 — partial lines.** The 2048 figure is retained "for compatibility with earlier
driver versions". The hardware writes `1024 + vbipack × 4` samples per line:

- PAL: `vbipack = 255` → **2044 real samples** (57.63 µs), 4 bytes padding.
- NTSC: `vbipack = 144` → **1600 real samples** (55.87 µs), **448 bytes padding**.

For NTSC that is nearly a quarter of each record. It must not be resampled, must not
contribute to level statistics, and must not confuse an auto-detector. Model it as a
`valid_samples` parameter alongside `line_length`.

**bt8x8 — frame counter.** The last four bytes of each 65 536-byte frame are a `u32` frame
sequence number written by `read()`, in machine endianness (little on every platform these
captures come from **[verify]**). It is not sample data. On PAL it occupies exactly the
4 padding bytes of the last record. This is a gift: it gives the stage free frame-drop
detection, which is otherwise undetectable and silently corrupts frame numbering, field
parity and the PAL colour sequence for the remainder of the file. See §6.3.

**cx88 — 18 records per field.** Records 0 and 17 are skipped. Everything else matches
bt8x8 PAL.

**FLAC wrapping.** FLAC is used here as a generic lossless byte compressor, not as audio.
Decoded output is byte-identical to the raw file, so this is purely a transport concern —
decode to a byte stream and hand off to the same parser. Points to note:

- **The declared FLAC sample rate is meaningless** (48000 is the conventional placeholder).
  Never derive VBI timing from the FLAC header.
- The community encoder invocation is
  `flac --best --sample-rate=48000 --sign=unsigned --channels=1 --endian=little --bps=8 --blocksize=65535 --lax`.
  `--lax` is required because the blocksize/rate combination falls outside the FLAC Subset.
- `--bps` is 8 or 16 matching the source; this is the one FLAC header field that *is*
  trustworthy and can seed sample-format detection.
- Compression is roughly 25 GB → 10 GB with fast encodes — far better than gzip/xz/FFV1 on
  this data, because VBI samples have no vertical or temporal correlation for a video codec
  to exploit.
- Seeking is slow. If the stage needs random access (preview scrubbing, `--start`/`--stop`),
  build a seek-point index on first open rather than repeatedly seeking the FLAC stream.

Streaming rather than materialising is strongly preferred: a four-hour bt8x8 capture is
23 GB raw.

---

## 4. Configuration

The stage needs a source descriptor. Following decode-orc's canonical YAML project format:

```yaml
- id: vbi_src
  stage: vbi-source
  input: "{root}/captures/bbc1-1982-12-19.vbi.flac"
  format: bt8x8-pal              # named preset from §3.2, or 'custom'
  output:
    preset: PAL                  # CVBS video standard preset
    sample_encoding: CVBS_U10_4FSC
    geometry: full-frame         # full-frame | vbi-only
    synthesise_burst: true
  teletext:
    system: WST                  # WST | NABTS
    lines: [7, 22, 320, 335]     # broadcast frame line numbers, inclusive ranges
  calibration:
    capture_offset: auto         # auto | <samples> | <microseconds>us
    levels: per-line             # per-line | rolling | fixed
  ordering:
    first_field: 1               # 1 | 2 | auto
```

With `format: custom`, the full parameter set from §3.1 is spelled out instead:

```yaml
  format: custom
  container:
    sample_rate: 35468950
    line_length: 2048
    valid_samples: 2044
    sample_format: u8
    field_lines: 16
    field_range: [0, 15]
    frame_trailer_bytes: 4       # bt8x8 frame counter; 0 for other formats
```

Every named preset in §3.2 expands to exactly this structure, so there is one code path and
the presets are pure data. That also makes it trivial to add a format later without
touching the stage.

**Interaction with the pluggable slicer work.** `teletext.system` must reach the decoder
stages, not just the source stage. vhs-teletext hardcodes `teletext_bitrate = 6937500.0`
and therefore cannot decode NABTS at all; if decode-orc's slicer architecture is going to
be honest about NTSC sources, bit rate, packet length and framing code need to be slicer
parameters. It is entirely reasonable to ship only a WST slicer initially — but the source
stage should still carry the system through so an NABTS source fails with a clear error
rather than producing garbage packets.

---

## 5. The transform

```
bytes → [container reader]   → line records + (frame, field, record) index
      → [line mapper]        → CVBS frame line numbers
      → [level mapper]       → 10-bit domain floats
      → [resampler]          → 4×fsc grid, fractional delay applied
      → [frame synthesiser]  → complete CVBS frames
      → .composite + .meta
```

### 5.1 Vertical placement — which frame line

| System | Frame lines (broadcast, 1-based) | Stored frame lines (0-based) |
|---|---|---|
| PAL / WST | 7–22 and 320–335 | 6–21 and 319–334 |
| NTSC / NABTS | 10–21 and 273–284 | 9–20 and 272–283 |

The mapping from source record to frame line is:

```
frame_line = tt_lines[field][record_index − field_range.start]
```

Sources store their useful records contiguously, so this is a simple table lookup. It must
be a table rather than an offset+stride calculation, because the two fields' line ranges
are not related by a constant offset in either system, and because some sources may carry
fewer lines than the standard allows.

If the source's `field_range` is longer than the standard's teletext line list, the excess
records have nowhere to go. Fail with a clear error rather than truncating — it means the
configuration is wrong.

### 5.2 Horizontal placement — where in the line

The data region for a stored line _k_ begins at:

```
data_start_samples(k) = t_offset × fs_out − phase(k)
```

where `t_offset` is the standards-defined time from 0H to the leading edge of the first CRI
one bit, and `phase(k)` is the PAL sub-sample offset from §2.3 (zero for NTSC/PAL-M).

Every VBI data service defines this offset, because a decoder needs to know roughly where
to start looking. libzvbi's service table carries it for all of them (its own comment:
*"Leading edge hsync to leading edge first CRI one bit, half amplitude points, in
nanoseconds"*), which makes it the most convenient single reference:

| Service | Lines (F1 / F2) | `t_offset` | Bit rate | CRI + FRC (tx order, hex) | Payload |
|---|---|---|---|---|---|
| Teletext System A, 625 | 6–22 / 318–335 | 10 500 ns | 6 203 125 (397 × fH) | `AAAAE7` | 37 bytes |
| **Teletext System B, 625 (WST)** | **6–22 / 318–335** | **10 300 ns** | **6 937 500 (444 × fH)** | **`AAAAE4`** | **42 bytes** |
| Teletext System C, 625 | 6–22 / 318–335 | 10 480 ns | 5 734 375 (367 × fH) | `AAAAE7` | 33 bytes |
| Teletext System D, 625 | 6–22 / 318–335 | 10 500 ns | 5 642 787 (14/11 × fsc) | `AAAAE5` | 34 bytes |
| Teletext System B, 525 | 10–21 / 272–284 | 10 500 ns | 5 727 272 (364 × fH) | `AAAAE4` | 34 bytes |
| **Teletext System C, 525 (NABTS)** | **10–21 / 272–284** | **10 480 ns** | **5 727 272 (364 × fH)** | **`AAAAE7`** | **33 bytes** |
| Teletext System D, 525 | 10–21 / 272–284 | 9 780 ns | 5 727 272 (364 × fH) | `AAAAE5` | 34 bytes |
| VPS (625, line 16) | 16 / — | 12 500 ns | 2 500 000 biphase | `AAAA8A99` | 13 bytes |
| WSS 625 (line 23) | 23 / — | 11 000 ns | 833 333 biphase | `8E3C783E` | 14 bits |
| Closed Caption 625 | 22 / 335 | 10 500 ns | 500 000 | `5551` | 2 bytes |
| Closed Caption 525 | 21 / 284 | 10 500 ns | 503 488 | `5551` | 2 bytes |

The two systems in scope for decode-orc are in bold. Note that WST and NABTS share the
same 364 × fH bit rate on 525 lines but differ in framing code and payload length, so the
framing code is the *only* thing distinguishing them on an NTSC capture.

**Derived positions.** For WST at 4·fsc PAL, `10.3 µs → 182.67 samples`. Packet length is
360 bits = 51.892 µs = 920.3 samples, so data ends around sample 1103 — comfortably inside
the 1135-sample line. For NABTS at 4·fsc NTSC, `10.48 µs → 150.05 samples`, and 264 bits =
660 samples exactly (NABTS is 2.5 samples/bit at 4·fsc, which is pleasantly clean).

**A ~100 ns discrepancy worth knowing about.** EN 300 706 states the timing reference
differently: 12.0 µs nominal from the half-amplitude point of the leading edge of line sync
to the *mid-point of the penultimate '1' of the clock run-in*. The CRI is 16 bits
(`0x55 0x55`, LSB first → `1010…`), so the ones are at bit positions 1, 3, … 15 and the
penultimate '1' is bit 13, whose mid-point is 12.5 bit periods (1.802 µs) after the start
of bit 1. That gives:

```
t_offset = 12.0 − 1.802 = 10.198 µs   →  180.86 samples at 4·fsc PAL
```

which is **1.8 samples earlier** than libzvbi's 10.3 µs. The difference is smaller than a
bit period (2.56 samples) and both values are widely used, so it is almost certainly a
rounding or convention difference rather than either being wrong. It does not matter to a
slicer, but it does mean neither figure should be treated as exact. The practical answer is
to configure the nominal as 10.3 µs for compatibility with the rest of the ecosystem, and
let the calibrator (§5.3) settle the actual value from the data.

The spec also warns that real transmissions depart from nominal: *"it may be necessary to
depart from this to allow for the re-timing of the synchronising pulses on some networks,
particularly as a result of sync reprocessing"*. So a measured offset that differs from
nominal by a few hundred nanoseconds is a property of the source, not necessarily an error.

### 5.3 Locking to the line

**Yes — there is a purpose-built timing signal, and it is the first thing in every data
line.** The clock run-in exists precisely so a decoder can recover bit timing with no
external reference, and the framing code exists to convert that into an unambiguous
absolute position. Together they are exactly the lock signal this stage needs.

#### 5.3.1 What the two parts do

**Clock run-in.** 16 bits of `1010…` at the service's bit rate. It establishes the bit
clock's *frequency* and *phase*, and its energy is concentrated at a single frequency
(6.9375 MHz for WST, 5.7273 MHz for NABTS) which makes it easy to detect even in noise.

But on its own it is **ambiguous modulo two bit periods**, because a shifted alternating
pattern still correlates perfectly with itself. At 4·fsc PAL that is 5.11 samples (288 ns)
of positional uncertainty — more than enough to place the data in the wrong byte phase.
EN 300 706 also warns that *"the two leading data 'ones' may be absent or reduced in
amplitude"*, so the CRI's leading edge is not reliable as a hard marker either.

**Framing code.** 8 bits, transmitted `11100100` (byte value 0x27) for WST, `11100111`
(0xE7) for NABTS/System C, 0xE5 for System D. It deliberately breaks the alternation, and
its cross-correlation with the CRI pattern is poor — which is the whole point. **This is
what resolves the ambiguity and gives absolute position.**

#### 5.3.2 The template

Both mature implementations use the same approach: correlate against a **combined CRI +
framing code template**, not the CRI alone.

- libzvbi: `cri_frc = 0xAAAAE4` for System B — 24 bits, with `cri_bits = 18` and
  `frc_bits = 6` describing how the pattern splits across the (potentially different) CRI
  and data clock rates.
- vhs-teletext: `crifc` in `Config`, a 24-element ±1 array — 16 alternating bits followed
  by `1,1,1,-1,-1,1,-1,-1` (the framing code).

24 bits at 2.56 samples/bit is a ~61-sample template at 4·fsc PAL. That is plenty of
correlation gain for a clean source, and its autocorrelation sidelobes are low enough that
the 2-bit ambiguity disappears.

**For degraded sources, use a measured template rather than an ideal one.** vhs-teletext
also carries `observed_crifc`, a real captured CRI+FRC waveform sampled at 8 samples/bit,
plus its gradient — because on VHS the signal is heavily low-pass filtered and an idealised
square template mismatches badly. For LaserDisc and broadcast sources an ideal filtered-NRZ
template is fine; if decode-orc ever ingests VHS-sourced VBI captures, a measured template
becomes worth the trouble.

#### 5.3.3 Where locking is and isn't needed

| Source family | Horizontal origin | Locking required |
|---|---|---|
| TBC-derived (`tbc`, `tbc-vbi`) | sample 0 **is** 0H, by construction of the TBC | **No** — `capture_offset = 0` |
| Card captures (`bt8x8`, `cx88`, `saa7131`) | arbitrary, undocumented, hardware-dependent | **Yes** |

This is worth stating plainly because it halves the problem. For family B the upstream
decoder already solved it; the stage should assert `capture_offset = 0` and refuse a
configuration that says otherwise, rather than "calibrating" a value that is known exactly.

For family A the driver constant is explicitly unreliable. The bt8x8 source comment sets
expectations:

> "According to the datasheet, VBI capture starts VBI_HDELAY fCLKx1 pixels from the tailing
> edge of /HRESET … But it's not! The datasheet is Just Plain Wrong. The real value appears
> to be different for different revisions of the bt8x8 chips, and to be affected by the
> horizontal scaling factor. Experimentally, the value is measured to be about 244."

The undocumented offsets in §3.2 were inferred by inverting the relation against
vhs-teletext's empirically tuned CRI search windows, which are expressed in each card's own
sample coordinates. That they all agree with the standards-derived figure is good evidence
the anchor is right:

| Card | 10.3 µs in card samples | minus capture_offset | vhs-teletext `line_start_range` | Agrees |
|---|---|---|---|---|
| `tbc` PAL (4fsc) | 182.7 | (0) → 182.7 | (160, 190) | ✔ |
| `bt8x8` PAL (8fsc) | 365.3 | (244) → 121.3 | (60, 130) | ✔ |
| `cx88` (8fsc) | 365.3 | (~214) → ~151 | (90, 150) | ✔ (marginal) |
| `saa7131` (27 MHz) | 278.1 | (~265) → ~13 | (0, 20) | ✔ |

The cx88 case sitting right at the edge of its window suggests its true offset is a little
larger than the ~214 samples inferred here — another argument for calibrating rather than
configuring. **[verify]**

**Measured, `bt8x8` PAL.** Calibration against the reference sample puts the run-in at
**103.2** samples rather than the 121.3 the folkloric 244 predicts, so the offset for that
capture is **262.1** samples. That is still inside vhs-teletext's (60, 130) window, which is
the point: the folklore is a search hint and nothing more, and the driver's own comment
about the value differing between chip revisions is borne out. See §9.

#### 5.3.4 The calibration procedure

`capture_offset: auto` should:

1. Take a sample of records — a few hundred lines is ample, spread across the file rather
   than taken from the head, so a bad opening segment doesn't dominate.
2. Normalise each record (§5.4) and correlate against the CRI+FRC template at the
   configured bit rate, in source sample coordinates.
3. Reject records whose peak correlation is below threshold — these are lines with no
   teletext, and there will be many. The acceptance rate is itself a useful diagnostic.
4. Refine each accepted peak by parabolic interpolation of the correlation function to get
   sub-sample resolution. Per-line precision of ~0.1 sample is achievable on a clean source.
5. Take the **mode or median** of the accepted positions, not the mean — outliers from
   partial matches will skew a mean.
6. `capture_offset = t_offset − cri_position / sample_rate`.

Report the fitted value *and its spread*. This is the single best health check the stage
has:

| Spread of CRI positions | Interpretation |
|---|---|
| ≲ 0.5 sample | Source is genuinely TBC'd; the assumption holds |
| 1–3 samples | Mild residual jitter, or slight sample-rate error; usable but worth flagging |
| ≫ 3 samples, or drifting monotonically | Sample rate is wrong, or the source is not time-base corrected. **Stop and tell the user** |

A monotonic drift across the file is diagnostic of a sample-rate error specifically, and
the slope gives the correction directly: a drift of _d_ samples over _N_ lines implies the
true rate is `configured × (1 + d / (N × line_length))`. That is a genuinely useful
auto-fix — it is exactly what vhs-teletext's `--sample-rate-adjust` exists to do manually,
and it is what would catch the 17 730 000 vs 17 734 475 Hz discrepancy automatically.

**Apply the offset globally, not per line.** Per-line correction would erase real timing
information and would shift lines that were correctly placed. The input is time-base
corrected; a single global offset is the correct model, and any per-line residual is either
genuine source jitter (which should be preserved) or noise in the estimator (which should
not be propagated).

Records with no teletext get the same global offset and are written as ordinary blanking.

#### 5.3.5 Independent cross-checks

Three other references can corroborate the fitted offset without relying on teletext being
present — useful for validating a configuration on a capture where teletext is sparse:

- **Colour burst remnant.** For bt8x8 PAL the capture window opens at ~6.879 µs, which is
  *inside* the burst window (5.6–7.85 µs), so ~34 samples of the burst tail appear at the
  start of every record — including records with no teletext. It is a known-frequency
  sinusoid at a known nominal position, so its phase can be estimated very precisely. The
  subcarrier period (225 ns) creates its own ambiguity, but the burst envelope's trailing
  edge resolves it coarsely, and the teletext lock resolves it exactly. Together they
  should agree; if they don't, something is wrong with the sample rate.
- **Other VBI services in the captured range.** A bt8x8 PAL capture covers lines 7–22, which
  includes line 16 (VPS, offset 12 500 ns) and line 22 (Closed Caption 625, offset
  10 500 ns). Both have their own CRI/FRC patterns in the table above and give completely
  independent offset estimates on lines that carry no teletext.
- **Data end position.** WST packets are exactly 360 bits. Measuring where modulation stops
  and comparing against `t_offset + 51.892 µs` validates the bit rate independently of the
  start position — a bit-rate error shows up as a start/end disagreement even when the
  start alone looks fine.

None of these is necessary for a working conversion. All three are cheap, and any of them
firing a warning means the configuration deserves a second look before committing to a
multi-hour decode.

#### 5.3.6 VHS-sourced captures — can the clock still be recovered?

Yes, but the CRI is a much weaker reference than it is on broadcast or LaserDisc material,
and the consequences reach further than just "it's noisier".

**Why.** An alternating `1010…` pattern at 6.9375 Mbit/s is a tone at **3.469 MHz** — the
highest-frequency content in the whole teletext line. VHS luma bandwidth is roughly 3 MHz
(PAL; NTSC is a little lower), so the CRI fundamental sits at or just above the rolloff.
S-VHS at ~5 MHz is far better placed. The data itself fares better than the CRI, because
any run of two or more identical bits has energy at lower frequencies that VHS passes.

**How much weaker.** vhs-teletext's `observed_crifc` is a real measured CRI+FRC waveform
from VHS, sampled at 8 samples/bit, and the numbers are stark:

| Region of the measured waveform | Peak-to-peak |
|---|---|
| CRI steady state (bits 2–13) | **7 counts** |
| Framing code region (the `111` run down to `00`) | **127 counts** |
| CRI mean level | 119.1 |
| Framing code midpoint | 120.5 |

The CRI survives at **5.5 % of the full data amplitude** — an 18× reduction — and its mean
has collapsed to within 1.4 counts of the data midpoint. It has been very nearly filtered
out of existence. That corresponds to a Gaussian channel with σ ≈ 0.8 bit periods (≈115 ns):

| Gaussian σ | CRI amplitude as fraction of full |
|---|---|
| 0.5 bit (72 ns) | 0.371 |
| **0.8 bit (115 ns)** | **0.054** ← matches the measurement |
| 1.0 bit (144 ns) | 0.009 |
| 1.2 bit (173 ns) | 0.001 |

The response falls off a cliff, so a modestly worse tape puts the CRI below the noise
entirely. This is exactly why vhs-teletext exists and why it deconvolves rather than slices.

**Consequences for this stage — three of them, in increasing order of importance:**

**1. Position lock still works, but the energy is in the framing code, not the CRI.**
Seven counts of coherent tone integrated over 16 bit periods is still detectable, and the
framing code — with its 3-bit and 2-bit runs — comes through at full amplitude and carries
most of the usable timing information. This is why the combined CRI+FRC template (§5.3.2)
matters more here than on a clean source, and why vhs-teletext computes
`observed_crifc_gradient` over the *second half* of the template: the FRC edges are where
the signal actually is. For a VHS source, use the measured template, not the ideal one.

**2. The calibration health check needs different thresholds.** The spread table in §5.3.4
assumes a clean source. A VHS capture will legitimately show several samples of scatter —
the capture card's horizontal PLL line-locks the sampling clock, so per-line timing is
broadly corrected, but residual jitter, velocity error and the settling period after head
switching all remain. Head switching happens ~6.5 lines before field sync, so the teletext
lines are inside the recovery window and the first line or two of each field are typically
the worst. Thresholds should therefore be a property of the configured source format, not a
global constant, or the stage will cry wolf on every VHS capture.

Averaging still saves the global estimate: per-line precision may be a sample or worse, but
the offset is fitted over thousands of lines, so the *mean* remains tight. This is the key
asymmetry — **the source stage needs a global offset, and the downstream slicer does the
per-line lock.** VHS degradation hurts the slicer far more than it hurts this stage.

**3. Never use the CRI to set signal levels on a VHS source.** This one is a firm
requirement rather than a caveat. Taking the '1' level from CRI peaks would under-scale by
roughly 18× on the measured waveform above, producing output that is technically legal but
carries a data amplitude of ~20 counts instead of 388. Use the framing code's `111` run
instead — it is the only guaranteed multi-bit run of ones in every teletext line, and on a
Gaussian channel with σ ≤ 1 bit it reaches essentially full amplitude. See §5.4.

**The overriding design rule for VHS sources: do not clean anything up.** The source stage
must be a faithful analogue-domain transform. The deconvolving slicer downstream recovers
data by matching the *blurred* waveform against trained convolution tables; if the source
stage hard-slices, sharpens, re-quantises aggressively or clips, it destroys precisely the
information the deconvolver depends on. Resample transparently, map levels linearly, clamp
only at the protected boundaries, and pass the blur through untouched.

### 5.4 Level mapping

**Family A (u8 card captures).** Levels are relative and AGC-affected; per-line estimation
is the only robust approach.

- **Logic 0 / black reference:** the quiet region between the start of the record and the
  CRI. For bt8x8 PAL that is samples 0–117, about 3.3 µs of back porch — plenty.
- **Logic 1 reference:** depends on the source's bandwidth.
  - *Clean sources (LaserDisc, broadcast, off-air):* the peaks of the CRI. Its alternating
    pattern is known, so its extremes are more reliable than arbitrary data.
  - *Band-limited sources (VHS):* the **framing code's `111` run**. The CRI is attenuated to
    a few percent of full amplitude (§5.3.6) and using it under-scales catastrophically.
    A run of three identical bits is the shortest feature that reaches full amplitude
    through a Gaussian channel of σ ≤ 1 bit, and the framing code guarantees one in every
    teletext line.
  - A robust default is to measure both and take the larger, flagging a large disagreement
    as a bandwidth indicator — the CRI-to-FRC amplitude ratio is a direct, per-line
    estimate of how blurred the source is, and would make a genuinely useful quality metric
    to record in the provenance sidecar.
- Map: `black → 256` (PAL) or `blanking → 240` (NTSC); `one → 644` (PAL) or `632` (NTSC).

Two caveats:

- Per-line normalisation propagates per-line gain noise into the output. The `rolling` mode
  should take a median over N lines and apply per-line correction only on significant
  deviation.
- On VHS-sourced material the under-scale is not subtle — the measured example in §5.3.6
  is 18×. Under-scaled data will still slice (the deconvolver normalises per line anyway),
  but the output is then not a faithful reconstruction, and anything else inspecting the
  file sees a signal at 5 % of the correct amplitude.

**Family B (u16 TBC captures).** Already on an IRE-referred scale. ld-decode's mapping is:

| | PAL | NTSC |
|---|---|---|
| Sync tip | 256 | 1024 |
| Blanking (0 IRE) | 16 384 | 15 360 |
| White (100 IRE) | 54 016 | 51 200 |
| Counts per IRE | 376.32 | 358.4 |

These are exposed per-file as `white16bIre` / `black16bIre` / `blanking16bIre` in the
`.tbc.json` (or `white_16b_ire` etc. in the newer SQLite metadata). **Read them rather than
hardcoding** — vhs-decode and non-standard sources differ. If the sidecar is absent (which
it will be for a cropped VBI-only file), fall back to the table above and say so in the log.

Conversion to the 10-bit CVBS domain is then a two-point affine map through the shared
physical levels (blanking and white), not a naive `>> 6`.

### 5.5 Resampling

| Source | Target | Ratio |
|---|---|---|
| bt8x8 PAL, cx88 (8·fsc) | 4·fsc PAL | exactly 2:1 — decimate |
| bt8x8 NTSC (8·fsc) | 4·fsc NTSC | exactly 2:1 — decimate |
| saa7131 (27 MHz) | 4·fsc PAL | 17 734 475 : 27 000 000 — polyphase |
| tbc, tbc-vbi | 4·fsc | 1:1 — passthrough |

The 2:1 cases dominate. They still need a proper half-band low-pass before decimation, not
sample-dropping: WST at 6.9375 MHz sits close to the 8.87 MHz Nyquist of the 4·fsc PAL
grid, and aliasing there lands directly on the data.

The fractional delay from §2.3/§5.2 should be folded into the resampler's phase rather than
applied as a separate interpolation — one filter, one pass, no cumulative error.

The 1:1 case is worth special-casing to a straight copy, both for speed and to guarantee
bit-exactness when a TBC-derived source is round-tripped.

### 5.6 Frame synthesis

Everything outside the teletext data region is manufactured. For each line:

| Region | PAL | NTSC | Level |
|---|---|---|---|
| Line sync | 0 → 4.7 µs (83.4 samples) | 0 → 4.7 µs (67.3 samples) | sync tip (4 / 16) |
| Colour burst | 5.6 → 7.85 µs | 5.3 → 7.8 µs | optional, see below |
| Back porch | → 10.198 µs | → 10.5 µs | blanking |
| Data | 10.198 → 62.09 µs | 10.5 → 60.8 µs | resampled source, or blanking |
| Front porch | → end of line | → end of line | blanking |

Sync edges need a realistic rise/fall (~250 ns, raised-cosine) rather than a step; a step
will ring through any downstream filter and can look like signal.

**Vertical interval.** The source contains none, so it is synthesised entirely from the
standard. ld-decode's `SysParams` is a convenient citable source for the pulse geometry:

| Parameter | PAL | NTSC |
|---|---|---|
| Line sync width | 4.7 µs | 4.7 µs |
| Equalising pulse width | 2.35 µs | 2.3 µs |
| Broad (field sync) pulse width | 27.3 µs | 27.1 µs |
| Pulses per group | 5 | 6 |
| First-field half-line pattern | (1, 0.5) | (0.5, 1) |

**Colour burst — three options:**

1. **Omit.** Simplest; the output is effectively monochrome CVBS. A pure teletext path does
   not care. But `ld-analyse` will show no burst, PAL field-phase detection fails, and the
   stage must declare `STANDARD_TBC_UNLOCKED`.
2. **Synthesise with correct phase progression.** PAL: ±135° swinging burst with the 4-frame
   Sc/H progression; NTSC: 180° reference phase with the 2-frame A/B sequence. Makes the
   output a well-formed colour CVBS frame, permits `STANDARD_TBC_LOCKED`, and is also the
   more faithful reconstruction — real broadcast teletext lines do carry burst.
3. **Preserve.** Only possible where the capture window includes the burst. For full-field
   `tbc` sources it does. For bt8x8 PAL the window opens at 6.879 µs, which is *inside* the
   burst window (5.6–7.85 µs), so a partial burst is present in samples 0–34 of every
   record — interesting, but too little to be useful.

**Recommendation:** default `synthesise_burst: true` for both systems, note it in
`capture_notes`, and let option 3 remain unimplemented until someone has a use for it.

**Active picture.** Fill all non-teletext lines with a standards-correct blank line — sync,
burst, and blanking held at black level. The result is a legal black raster that any CVBS
consumer can open, rather than a frame full of structurally odd lines.

### 5.7 Geometry mode and the size problem

Full-frame synthesis is expensive. A PAL bt8x8 capture goes from 65 536 bytes/frame to
1 418 758 bytes/frame — a **21.6× expansion**, almost all of it synthesised blanking. A
four-hour capture goes from 23 GB to roughly 500 GB.

Hence the `geometry` option:

- **`vbi-only`** — emit only the lines carrying teletext, keeping the same horizontal
  geometry, sample encoding and levels. Expansion is ~1.4× and the file is directly usable
  by the teletext decoders. This should be the default for the teletext workflow. It is a
  decode-orc internal transport, not a conformant CVBS file, and must be labelled as such
  so it never escapes into an archive claiming to be one.
- **`full-frame`** — a conformant `.composite` file. Use when the result needs to be viewed
  in `ld-analyse`, archived, or fed to a stage that expects a complete frame.

Both modes should share all code except the frame assembler. If `full-frame` output is
being written to disk at scale, FLAC-compressing it is worth offering — the blanking
regions are constant and compress to almost nothing.

---

## 6. Ordering, parity and drops

### 6.1 Field order within a frame

bt8x8 gives 16 records of "field 1" then 16 of "field 2", but *which TV field is first* is
a driver convention, not recorded information. The CVBS spec is explicit that neither frame
ordering nor intra-frame field order is guaranteed by the format, and that validation is
the consumer's responsibility — so the stage is free to make an assumption, provided it
declares it.

Default to field-1-first (matching bttv's `start[]` ordering, which lists the lower frame
line first). Expose it as `ordering.first_field`. Record the choice in `capture_notes`.

Getting it backwards swaps the teletext line numbering between the two fields. Teletext
packets carry no line-number field, so nothing will visibly break — the packets decode
identically. What breaks is line-level provenance, which matters for the archive workflow
where a recovered page must be traceable to specific source lines.

**Possible check [verify]:** in WST, packet 0 (page headers) is not evenly distributed
between fields in most services, and magazine cycling has field-level structure. A
statistical check over a few thousand frames might identify field order without user input.
Worth investigating, but not worth blocking on.

### 6.2 Frame order

Frames are emitted in file order. The source is a linear capture, so this is correct by
construction — the only thing that can break it is a dropped frame.

### 6.3 Dropped frames

For bt8x8 sources, the per-frame counter (§3.3) makes drops detectable. The stage should:

1. Track the counter across frames and record every discontinuity.
2. **Not** silently paper over gaps. Two policies, both defensible:
   - `drops: preserve` — emit only the frames present. Frame indices in the output no longer
     match the source's wall-clock frame numbers, so the counter values must be recorded in
     the provenance sidecar for the mapping to be recoverable.
   - `drops: pad` — insert synthesised blank frames to keep output frame *n* aligned with
     source frame *n*. Preserves the timeline and the PAL colour sequence at the cost of
     inventing frames.
3. Downgrade `signal_state_preset` to `STANDARD_TBC_UNLOCKED` if any gap is found and
   `preserve` was used, because the frame-boundary integrity requirement is no longer met.

For non-bt8x8 sources there is no drop signal at all. Say so in the log rather than
implying the absence of reported drops means the absence of drops.

---

## 7. Metadata and provenance

### 7.1 Core `.meta`

Written as specified in §2.4. Nothing unusual.

### 7.2 Provenance sidecar

The core schema deliberately excludes per-frame and per-line annotation, directing
producers to a separate extension format. A `.vbisource.meta` sidecar following the same
pattern as the dropout extension would carry:

```sql
PRAGMA user_version = 1;

CREATE TABLE vbi_source (
    cvbs_file_id            INTEGER PRIMARY KEY,
    source_file             TEXT NOT NULL,
    source_format           TEXT NOT NULL,   -- 'bt8x8-pal', 'custom', …
    source_sample_rate      REAL NOT NULL,
    source_line_length      INTEGER NOT NULL,
    source_sample_format    TEXT NOT NULL,
    capture_offset_samples  REAL NOT NULL,
    capture_offset_method   TEXT NOT NULL,   -- 'configured' | 'cri-calibrated'
    capture_offset_stddev   REAL,            -- NULL if configured
    teletext_system         TEXT NOT NULL,   -- 'WST' | 'NABTS'
    field_order_method      TEXT NOT NULL,   -- 'assumed' | 'configured' | 'detected'
    burst_synthesised       BOOLEAN NOT NULL,
    geometry_mode           TEXT NOT NULL    -- 'full-frame' | 'vbi-only'
);

-- one row per emitted frame; the line mapping is deterministic from the config
CREATE TABLE vbi_source_frame (
    cvbs_file_id            INTEGER NOT NULL,
    frame_id                INTEGER NOT NULL CHECK (frame_id >= 0),
    source_frame_index      INTEGER NOT NULL,
    source_frame_counter    INTEGER,         -- bt8x8 only; NULL otherwise
    is_padding              BOOLEAN NOT NULL,
    PRIMARY KEY (cvbs_file_id, frame_id)
);
```

The per-frame table is what closes the provenance loop. Because the line mapping (§5.1) is
a pure function of the configuration, a packet emerging from the teletext sink at
(frame, field, line) resolves to a source byte range with no further bookkeeping — provided
the frame table records which source frame each output frame came from.

This dovetails with the per-packet frame provenance design in the teletext sink work: run
the deconvolver in keep-empty mode so the packet stream has exactly one record per input
VBI line, and provenance becomes a pure index relation end to end.

### 7.3 `capture_notes`

A human-readable summary belongs here, because it is the field that survives when sidecars
get separated from data files. Something like:

> Synthesised from raw VBI capture `bbc1-1982-12-19.vbi.flac` (bt8x8 PAL, 8fsc, u8,
> 16 lines/field). Teletext data on frame lines 7–22 / 320–335 is recovered signal;
> all sync, blanking, vertical interval and colour burst are synthesised. Capture offset
> 241.3 samples, CRI-calibrated (σ 0.4). Field order assumed field-1-first.
> 3 source frame drops detected and preserved (frames not padded).

---

## 8. Validation and diagnostics

The stage should be able to tell the user whether its configuration is right *before* they
spend hours decoding. Cheap checks over the first few hundred frames:

| Check | What a failure means |
|---|---|
| File size is an exact multiple of the configured frame size | Wrong format, or trailing garbage |
| CRI correlation peak strength | Wrong sample rate, or not a teletext capture at all |
| CRI position distribution width | Source is not actually time-base corrected, or wrong line length |
| Fraction of records containing a detectable CRI | Wrong `field_range`, or a mostly-empty capture |
| Level estimate stability | AGC hunting, or a level-mapping mode that is too aggressive |
| bt8x8 counter continuity | Dropped frames |
| Output frame size matches the preset's normative count | An indexing bug — for PAL this catches the constant-1135 mistake immediately |

A `--dry-run` that prints these and stops would be a good investment; a `--preview` that
renders a few frames' VBI region as an image is better still, and reuses whatever the live
preview dialogue for the teletext sink already does.

### 8.1 Format auto-detection

Offered as a configuration *suggestion*, ranked with confidence, never applied silently:

1. **Magic.** `fLaC` at offset 0 → decode and recurse; take `bps` from the header.
2. **File size factorisation.** Test the frame sizes in §3.2. Usually narrows to one or two.
3. **u8 vs u16.** On u16 LE data in the ld-decode range, high bytes cluster in a narrow band
   while low bytes are near-uniform. A histogram of even vs odd byte positions separates
   these cleanly.
4. **Line-length autocorrelation.** Autocorrelate a few hundred KB and look for the line
   stride. Robust and format-agnostic.
5. **CRI frequency.** With a line length hypothesised, FFT a few hundred lines and look for
   energy at 6.9375 MHz (WST) or 5.7273 MHz (NABTS) under each candidate sample rate. This
   is the only test that discriminates PAL-rate from NTSC-rate bt8x8 captures, which are
   otherwise byte-for-byte indistinguishable.
6. **Filename hints.** `_u16`, `_vbi_only`, `NTSC`, `PAL`. Lowest confidence, but free —
   the informal naming convention in circulation
   (`TBS_1988-01-03_Electra_teletext_NTSC_SP_vbi_only_u16.flac`) is often the only surviving
   metadata.

---

## 9. Open questions

- **[measured]** `capture_offset` for `bt8x8` PAL. Calibration against the reference sample
  (`0002.vbi.flac`, 512 records sampled across the whole four hours, 80 % of them locking)
  puts the clock run-in at **103.2 samples** into each record, giving a capture offset of
  **262.1 samples** — 18 samples (0.5 µs) later than the driver's folkloric 244, and well
  inside vhs-teletext's (60, 130) search window for this card. The lock is at the correct
  bit alignment, not two bits away: slicing the framing code at the fitted position recovers
  `0xE4` on about half the locked records and at no neighbouring bit alignment. The §3.2
  preset keeps 244 as the search hint, which is its only remaining role.
- **[measured]** Run-in position spread for `bt8x8` PAL: **4.2 samples**, not the ≲ 0.5 of a
  cleanly time-base corrected source. The global fit is nevertheless stable to under a
  sample across different samples of the file, exactly the asymmetry §5.3.6 describes. The
  preset's thresholds are set accordingly (8 samples), since a bt8x8 card is how tape and
  off-air material is captured in the first place.
- **[verify]** The burst remnant disagrees with the teletext lock on the reference sample by
  **11.3 samples (0.32 µs)**: the burst tail ends at record sample 28.1 where the fitted
  offset predicts 16.8. Both are reported and the teletext lock governs placement. This is
  the magnitude EN 300 706 warns about for sync-reprocessed transmissions (§5.2), and it is
  the same order as the 10.198 vs 10.3 µs question below, so the two should be settled
  together against a known-good broadcast source.
- **[verify]** `capture_offset` for cx88 and saa7131 — the values in §3.2 are inferred from
  vhs-teletext's CRI search windows, not measured.
- **[verify]** The NABTS nominal data amplitude against EIA-516 and a real capture. The
  70 IRE figure is conventional, not verified here. (The timing reference is now taken from
  libzvbi's service table at 10 480 ns, which is at least a second independent source.)
- **[verify]** Which of 10.198 µs (EN 300 706, derived) and 10.3 µs (libzvbi) is the better
  WST nominal — or whether the 1.8-sample difference is simply below anyone's threshold of
  caring. Measuring a known-good broadcast source would settle it.
- **[verify]** Endianness of the bt8x8 frame counter in practice. The kernel writes machine
  endianness; vhs-teletext assumes little.
- **[verify]** Whether any u8 captures exist at 16-bit, or any u16 captures from family A.
  So far u16 has only been seen on TBC-derived files.
- **[verify]** Whether the "recorded using a CX capture card" `.tbc` samples in circulation
  are CXADC composite decodes (family B, full field) or cx88 VBI dumps (family A). These
  are entirely different files despite both being "CX".
- Whether field order can be detected statistically from WST packet structure (§6.1).
- Whether the CVBS spec should gain a `decode-orc` value for the `decoder` field, and
  whether a `vbi-only` geometry deserves any formal recognition or should remain strictly
  an internal transport.
- Whether the full-frame synthesiser should be shared with a future `cvbs-encode`-style
  stage — the sync, vertical interval and burst generation are identical problems, and
  duplicating them is how the two drift apart.

---

## 10. References

**Specifications**

- [CVBS File Format Specification](https://simoninns.github.io/cvbs-file-format-specification) —
  video standard presets (frame geometry, level tables, 0H alignment, PAL non-orthogonality),
  sample encoding presets, signal state presets, metadata schema, dropout extension
- ETSI EN 300 706 — Enhanced Teletext specification (System B): §5 signalling levels and
  bit rate, §6 packet identification and timing reference
- EIA-516 / ITU-R BT.653 System C — NABTS
- EBU Tech. 3280-E, SMPTE 244M-2003, SMPTE 170M-2004 — via the CVBS spec's preset definitions

**Code**

- [zapping-vbi/zvbi](https://github.com/zapping-vbi/zvbi) (libzvbi)
  - `src/raw_decoder.c` — `_vbi_service_table[]`: per-service line ranges, 0H-to-CRI
    offsets, bit rates, CRI+FRC patterns and payload lengths for every VBI data service.
    The most complete single reference for the timing anchors in §5.2
  - `src/raw_decoder.h` — `struct _vbi_service_par`, which documents what each field means
  - `src/bit_slicer.c` — a reference CRI/FRC correlating slicer
- [ali1234/vhs-teletext](https://github.com/ali1234/vhs-teletext)
  - `teletext/vbi/config.py` — the `cards` table; the de facto format registry
  - `teletext/file.py` — `FileChunker`; the field/line stride logic this design mirrors
  - `teletext/vbi/line.py` — normalisation and resampling to 8 samples/bit
  - `teletext/vbi/config.py` — `crifc` (the ideal 24-bit CRI+FRC template) and
    `observed_crifc` (a measured one, for blurred sources)
  - `teletext/cli/clihelpers.py` — the parameter override flags worth matching
- [happycube/ld-decode](https://github.com/happycube/ld-decode)
  - `lddecode/core.py` — `SysParams_PAL` / `SysParams_NTSC` (pulse geometry, IRE mapping,
    `out_scale`), `hz_to_output`
- Linux kernel `drivers/media/pci/bt8xx/` — `bttv-vbi.c` (`VBI_OFFSET`, `try_fmt`),
  `bttvp.h` (`VBI_BPL`, `VBI_DEFLINES`, the `vbipack` note), `bttv-driver.c`
  (`bttv_tvnorms[]`: `Fsc`, `vbipack`, `vbistart`)
- V4L2 `struct v4l2_vbi_format` and the kernel's "Differences between V4L and V4L2"
  historical bttv parameter table

**Community**

- [vhs-decode wiki: Teletext](https://github.com/oyvindln/vhs-decode/wiki/Teletext) —
  archive proposal, storage and compression benchmarks, the `--keep-empty` provenance note
