# Source stages

Source stages are the **starting point of every decode-orc pipeline**. They load captured video (and any associated audio or disc data) from disk and make it available for processing.

You normally use **one source stage per capture**. If you have multiple captures of the same material, you add multiple source stages and combine them later using transform stages.

Source stages do not improve or modify the signal. Their purpose is to:

* Load captured files correctly
* Validate the video system (PAL, NTSC, or PAL-M)
* Keep video, audio, and disc data synchronised

> **Project format note:** Decode-Orc 2.0 requires that every project declares a `video_format` (PAL, NTSC, or PAL-M) and a `source_format` (Composite or YC) at creation time. These fields are read-only after the project is created. The stage picker shows only source stages that match the declared format.

---

## TBC Source

| | |
|-|-|
| **Stage id** | `tbc_source` |
| **Stage name** | *derived from metadata at load time* (e.g. `PAL TBC Composite`, `NTSC TBC YC`) |
| **Connections** | No inputs → 1 output |
| **Purpose** | Load TBC files produced by ld-decode or vhs-decode |

**Use this stage when:**

* Your capture comes from a LaserDisc or colour-under tape format
* You decoded the RF capture using ld-decode or vhs-decode into `.tbc` files

**What it does**

This stage reads one or more TBC files, detects the video system and signal type (composite or Y/C) from the `.tbc.db` metadata database, and assembles full-frame CVBS_U10_4FSC buffers for downstream processing. The stage display name is resolved at load time from the metadata (`PAL TBC Composite`, `NTSC TBC YC`, etc.).

All TBC level values are remapped from the ld-decode/vhs-decode internal 16-bit domain to the CVBS_U10_4FSC 10-bit domain. PAL frames have exactly 709,379 samples; NTSC frames have 477,750 samples; PAL-M frames have 477,225 samples.

Associated audio (analogue `.pcm`), EFM disc data (`.efm`), and AC3 RF symbols (`.ac3sym`) are attached if present alongside the `.tbc` file. When a `.pcm` sidecar is present it becomes **audio channel pair 0** (named from `pcm_name`, default `Analogue`). The `.pcm` sidecar — raw signed 16-bit little-endian stereo PCM, nominally 44100 Hz as written by ld-decode — is always converted on ingest to the pipeline's only audio form: 48 kHz synchronous (frame-locked) 24-bit stereo per SMPTE 272M (widened to 24-bit and resampled with SoXR HQ). The conversion is deferred until audio is first read, so video-only preview never pays for it.

**Composite variant user-facing inputs**

* **TBC file** (`.tbc`)
* Accompanying metadata database (`.tbc.db`)
* PCM audio file (`.pcm`, optional)
* EFM data file (`.efm`, optional)
* AC3 RF symbols file (`.ac3sym`, optional)

**Y/C variant user-facing inputs**

* **Luma (Y) file** (`.tbcy`)
* **Chroma (C) file** (`.tbcc`)
* Accompanying metadata database (auto-detected)
* PCM audio file (`.pcm`, optional)
* EFM data file (`.efm`, optional)
* AC3 RF symbols file (`.ac3sym`, optional)

**Parameters**

* `input_path` (file path) — Composite `.tbc` file. Composite captures only.
* `y_path` / `c_path` (file paths) — Luma `.tbcy` and chroma `.tbcc` files. Y/C captures only; set together.
* `pcm_path` (file path) — Analogue audio `.pcm` sidecar. Becomes channel pair 0, converted to 48 kHz frame-locked 24-bit stereo.
* `pcm_name` (string) — Name for the analogue audio channel pair (shown in the CVBS container and as the Video Sink stream title). Empty uses `Analogue`.
* `efm_path` (file path) — EFM t-value `.efm` sidecar.
* `ac3rf_path` (file path) — AC3 RF symbols `.ac3sym` sidecar.

**Notes**

* The stage validates that the Y/C colour-frame phase is aligned at open time. Misaligned Y/C files are rejected with a clear error.
* NTSC-J sources with a non-standard black level are detected automatically from metadata and exposed via a per-frame black level override.
* Legacy `.tbc.json` metadata produced by older ld-decode/vhs-decode versions is accepted with a warning; re-decoding with a current version (which produces `.tbc.db`) is recommended.

---

## CVBS Source

| | |
|-|-|
| **Stage id** | `PAL_CVBS_Source`, `NTSC_CVBS_Source`, or `PAL_M_CVBS_Source` (one variant per video system; the stage picker offers the one matching the project) |
| **Stage name** | CVBS Source |
| **Connections** | No inputs → 1 output |
| **Purpose** | Load CVBS captures stored in the CVBS file-format family |

**Use this stage when:**

* Your source is a CVBS file (`.composite`, or a `.y`/`.c` pair for Y/C projects) rather than a TBC capture

**What it does**

This stage reads CVBS payloads from `.composite` files (or `.y`/`.c` pairs) and normalises them to the CVBS_U10_4FSC 10-bit domain. By default the video system, sample encoding, and signal state are read from the `.meta` SQLite sidecar; because the CVBS file format declares metadata optional, the sample encoding can also be selected manually so that sources without a sidecar can be used.

Only the `STANDARD_TBC_LOCKED` signal-state preset is accepted. Files with any other signal state are rejected with a clear error before any frame data is returned. When a sample encoding is selected manually the sidecar is ignored: the signal is assumed to be TBC-locked and the frame count is measured from the file size.

The following sample encodings are normalised automatically:

| Encoding | Normalisation |
|----------|---------------|
| `CVBS_U10_4FSC` | Identity (already 10-bit) |
| `CVBS_U16_4FSC` | `value = uint16_value / 64` |
| `CVBS_TPG21_4FSC` | `value = int16_value / 64 + 508` |
| `CVBS_S16_4FSC` | `value = int16_value / 32 + blanking_10bit` |

Associated dropout, audio, EFM, and AC3 sidecars are loaded automatically if present.

**Parameters**

The file-path parameters offered match the project's source type: a composite project shows only the CVBS file path, while a Y/C project shows only the Y (luma) and C (chroma) paths.

* `input_path` (file path)
    - Path to the composite data file (`.composite`). Composite projects only.

* `y_path` / `c_path` (file paths)
    - Paths to the luma (`.y`) and chroma (`.c`) channel files. Y/C projects only; set together.

* `sample_encoding` (string)
    - `From metadata` (default) reads the encoding from the `.meta` sidecar.
    - Selecting `CVBS_U10_4FSC`, `CVBS_U16_4FSC`, `CVBS_TPG21_4FSC`, or `CVBS_S16_4FSC` manually makes the sidecar optional.

**Notes**

* Colour-frame index (PAL: 1–4, NTSC: 0–1, PAL-M: 1–4) is measured from the colour burst on each frame and stored in the frame descriptor. Frames where the burst is absent or unmeasurable carry `colour_frame_index = -1`.

---

## VBI Capture Source

| | |
|-|-|
| **Stage id** | `vbi_source` |
| **Stage name** | VBI Capture Source |
| **Connections** | No inputs → 1 output |
| **Purpose** | Ingest raw VBI teletext captures by synthesising the CVBS frames their lines were cut out of |

**Use this stage when:**

* Your material is a raw VBI dump rather than a decoded capture — a bt8x8 card dump (`.vbi`, commonly FLAC-compressed as `.vbi.flac`) or another raw teletext capture in the same family

**What it does**

A raw VBI capture holds nothing but the vertical-blanking line records: no sync, no burst, no picture, and no timing reference of any kind. This stage reads those records and manufactures the complete PAL CVBS frames around them — line sync with shaped edges, the porches, the vertical interval's equalising and broad pulses with the correct half-line pattern per field, and (by default) a coherent colour burst following the PAL four-frame progression. The result is an ordinary CVBS_U10_4FSC representation, so the existing teletext decoders see exactly what they see from a native decode.

Frames are synthesised lazily, one at a time, as they are asked for: the full-frame expansion of a raw capture is 21.6× its size, so a four-hour bt8x8 capture would be 522 GB if it were materialised.

Records are level-mapped from the card's relative levels into the CVBS amplitude domain (logic 0 from the quiet region ahead of the clock run-in, logic 1 from the larger of the run-in peaks and the framing code's leading ones), resampled onto the 4×fsc lattice with a band-limited filter, and placed at the data service's nominal time from 0H. The mapping is linear and nothing else is done to the samples — a deconvolving slicer downstream recovers data by matching the blurred waveform it is given.

Because no capture format records the time from 0H to sample 0 of a record, and the bt8x8 family's documented figure is unreliable, the stage measures it: records sampled from across the whole capture are correlated against a generated clock-run-in and framing-code template and the median becomes a single global offset. A fit that fails its health checks stops the run rather than decoding hours of material at a wrong offset.

The last four bytes of every bt8x8 frame are the driver's frame sequence number. Comparing it at the two ends of the capture says how many frames were dropped across the whole of it without reading the capture through, which is what the `drops` policy acts on.

**Currently implemented:** the `bt8x8-pal` preset (625-line, 8×fsc, 8-bit, World System Teletext), raw or FLAC-wrapped. The remaining formats in the design's table (`bt8x8-ntsc`, `cx88-pal`, `saa7131-pal`, `tbc-pal`, `tbc-vbi-pal`, `tbc-vbi-ntsc`) and the NABTS data service are refused at configuration with a clear error rather than producing plausible but wrong output.

**Parameters**

* `input_path` (file path) — Path to the raw capture. FLAC-wrapped captures are unwrapped transparently; the wrapper's declared sample rate is a placeholder and is never used for timing.
* `format` (string) — Container preset: `bt8x8-pal` (default) or `custom`.
* `teletext_system` (string) — `WST` (default) or `NABTS`. Only WST can currently be placed; the configured system is carried on the stage's output for downstream decoders.
* `synthesise_burst` (bool) — Write a coherent colour burst on every line. Default on.
* `capture_offset_mode` / `capture_offset_samples` — `auto` (default) fits the offset from the captured clock run-in; `manual` applies the configured figure unchanged.
* `levels` (string) — `per-line` (default), `rolling`, or `fixed`; `fixed_logic0` / `fixed_logic1` supply the levels for `fixed`.
* `first_field` (uint) — Television field the first stored field of each frame carries, 1 (default) or 2.
* `drops` (string) — `preserve` (default) emits only the frames present; `pad` synthesises blank frames so output frame *n* stays aligned with source frame *n*.
* `container_*` — Sample rate, record stride, valid samples, sample format, records per field, first/last data record, frame trailer bytes, and television system. Shown and used only when `format` is `custom`.

**Notes**

* Nothing is written to disk by this stage. To export the synthesised frames, connect a CVBS Sink.
* The output declares `STANDARD_TBC_LOCKED` only when a coherent burst was synthesised **and** the frame timeline survived; a run with the burst off, or one whose counter gaps were not padded, reports `STANDARD_TBC_UNLOCKED`. This is a consequence of the run, never a setting.
* Which frame lines carry data follows from the television and teletext systems (WST occupies broadcast frame lines 7–22 and 320–335) and from which stored records the container declares as data records; it is not configured directly. A capture holding more data records than the standard defines is reported as an error rather than truncated.
