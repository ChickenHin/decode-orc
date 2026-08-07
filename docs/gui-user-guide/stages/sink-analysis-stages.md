# Analysis Sink Stages

Analysis sink stages are **terminal stages** that generate diagnostics, metrics, and reports rather than producing media or hardware output. They consume processed data from upstream stages and emit **analysis results** intended for comparison, validation, or debugging.

Analysis sinks:

* Do not modify video, audio, or metadata
* Do not produce outputs that can be connected further downstream
* Display their results in a dialog of their own and write them to a file

They are typically used to:

* Compare capture quality across multiple sources
* Validate signal stability and decode quality
* Quantify the effects of transform stages such as stacking or dropout correction

Every analysis sink works the same way: trigger the stage to compute the dataset, after which its dialog opens automatically. The dataset is cached in the stage and the dialog can be re-opened at any time from the **Stage Tools** menu; opening it there on a node that has not been triggered runs the analysis first, with progress and cancel.

The burst level, dropout and SNR sinks measure the signal and present a chart with an optional CSV export. The teletext sink recovers a data service instead, so its dialog is a page viewer and its file output is the packet stream.

**CSV output format.** Each CSV is written from the full-resolution, canonical per-frame dataset — **one row per frame** (every analysis sink analyses every frame), with the frame's true (1-based) frame number in the first column. Units are carried in the header names (`_samples`, `_db`, `_10bit`) and values are plain numbers. A metric that was not measured for a frame is written as an **empty field** (never the string `nan`). The CSV is independent of the display decimation used to draw the chart, so it always contains every frame regardless of the on-screen point count.

---

## Burst Level Analysis Sink

| | |
|-|-|
| **Stage id** | `burst_level_analysis_sink` |
| **Stage name** | Burst Level Analysis Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Measure colour burst level stability across fields |

**Use this stage when:**

* Evaluating chroma signal stability
* Comparing multiple captures of the same source
* Diagnosing colour amplitude fluctuations or capture issues

**What it does**

This stage measures the amplitude of the colour burst for each field and generates statistics describing burst level variation over time (per-field measurements plus aggregate mean, variance, min/max). After triggering, the Burst Level Analysis chart is opened automatically.

**Parameters**

* `output_path` (file path)
    - Destination CSV file for burst metrics. Leave empty to skip file output.
* `write_csv` (bool)
    - Enable writing results to CSV at trigger time.

**CSV columns**

`frame_number, median_burst_10bit` — median colour-burst amplitude (10-bit sample units) per frame; the value column is empty when not measured.

**Stage tools**

* **Burst Level Analysis** — displays per-frame colour-burst amplitude measurements in a chart window. Invoked automatically after triggering; can be re-opened from the Stage Tools menu.

**Notes**

* Results are meaningful only if colour burst timing is correct upstream.
* Masking or altering the burst region before this stage will invalidate results.
* Connect one instance before and one after the Stacker stage to compare burst stability across captures.

---

## Dropout Analysis Sink

| | |
|-|-|
| **Stage id** | `dropout_analysis_sink` |
| **Stage name** | Dropout Analysis Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Produce statistics describing dropout frequency, size, and distribution |

**Use this stage when:**

* Comparing dropout levels between captures
* Evaluating the effectiveness of stacking or dropout correction
* Identifying problematic regions of a capture

**What it does**

This stage reads dropout hints present in the stream (originating from the source or modified by transform stages such as `dropout_map`) and generates statistical summaries: total dropout count, per-field counts, size distributions, and line/field density metrics. After triggering, the Dropout Analysis chart is opened automatically.

It does **not** perform dropout detection or correction itself.

**Parameters**

* `output_path` (file path)
    - Destination CSV file for dropout metrics. Leave empty to skip file output.
* `write_csv` (bool)
    - Enable writing results to CSV at trigger time.
* `mode` (choice: `full`, `visible`, default `full`)
    - `full` counts dropouts across the whole field; `visible` restricts the count to the active picture area.
* `write_report` (bool)
    - Enable writing a per-dropout detail report at trigger time (one entry per dropout run).
* `report_path` (file path)
    - Destination file for the detail report. Leave empty to skip report output.
* `report_format` (choice: `csv`, `text`, default `csv`)
    - `csv` writes one row per dropout run; `text` writes a human-readable report grouped by frame.

**CSV columns**

`frame_number, dropout_count, dropout_length_samples` — one row for **every** frame (the sink analyses every frame). A zero row means the frame was analysed and had no dropouts; an absent frame number means the frame was not analysed.

**Per-dropout detail report**

Separate from the per-frame CSV, the detail report records *where* each individual dropout sits within its frame. It is written only when `write_report` is enabled with a `report_path`, is always full-resolution (never decimated), and honours the same `mode`. Coordinates are frame-flat: `line_number` is a 0-based line within the frame and `sample_start` / `sample_end` are 0-based, inclusive sample indices within that line, derived from the nominal samples-per-line (PAL 1135, NTSC 910). Unlike the per-frame CSV, **frames with no dropouts do not appear**.

* `csv` — one row per run: `frame_number, line_number, sample_start, sample_end, length_samples`.
* `text` — grouped by frame, e.g. `Frame 1: 2 dropouts, 50 samples total` followed by one indented `line N, samples A-B (L samples)` line per run.

**Stage tools**

* **Dropout Analysis** — displays dropout frequency, size, and distribution charts. Invoked automatically after triggering; can be re-opened from the Stage Tools menu.

**Where the analysis reads its data**

The analysis reports the dropout state **at the sink's input** — the dropout hints visible on the representation connected to it. To include edits made in the **Dropout Map** stage (added or removed dropout regions), the sink must be connected **downstream of the `dropout_map` stage**. A sink placed upstream of, or on a branch that bypasses, `dropout_map` sees only the original sidecar hints. Editing the map and re-triggering re-analyses from scratch, so the chart and CSV never show pre-edit data after a re-trigger.

**Notes**

* Results depend on the quality of upstream dropout detection.
* Removing or adding dropouts upstream will directly affect analysis output.
* Connect one instance before the Stacker and one after to see the dropout reduction achieved by stacking.

---

## SNR Analysis Sink

| | |
|-|-|
| **Stage id** | `snr_analysis_sink` |
| **Stage name** | SNR Analysis Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Produce signal-to-noise metrics for capture quality comparison |

**Use this stage when:**

* Comparing multiple captures of the same material
* Quantifying improvements from stacking or filtering
* Evaluating capture hardware or settings

**What it does**

This stage estimates signal-to-noise ratio using spatial and temporal analysis of the incoming video stream, reporting **white SNR** and **black SNR** per field along with aggregate statistics. Results are consistent across comparable pipelines, allowing meaningful cross-capture comparison. After triggering, the SNR Analysis chart is opened automatically.

**Parameters**

* `output_path` (file path)
    - Destination CSV file for SNR metrics. Leave empty to skip file output.
* `write_csv` (bool)
    - Enable writing results to CSV at trigger time.
* `mode` (choice: `white`, `black`, `both`, default `both`)
    - Selects which SNR metrics to measure.

**CSV columns**

`frame_number, white_snr_db, black_psnr_db` — white SNR and black PSNR (dB) per frame. A column is empty when that metric was not measured (for example, `mode = white` leaves `black_psnr_db` empty).

**Stage tools**

* **SNR Analysis** — displays white SNR and black SNR metrics over time in a chart window. Invoked automatically after triggering; can be re-opened from the Stage Tools menu.

**Notes**

* Meaningful SNR comparison requires aligned sources.
* Use `source_align` and `stacker` appropriately upstream when comparing captures.
* Stacking improves SNR only where sources contain independent noise; identical sources will not show an SNR improvement on dropout-free areas.

---

## Teletext Analysis Sink

| | |
|-|-|
| **Stage id** | `teletext_analysis_sink` |
| **Stage name** | Teletext Analysis Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Recover World System Teletext from the VBI, export the packet stream, and browse the pages the recording carried |

**Use this stage when:**

* Preserving teletext carried by a LaserDisc, CVBS capture, or tape source
* Reading the pages a recording carried without leaving decode-orc
* Producing a packet stream for external teletext tools (vhs-teletext, wxTED)

**What it does**

Triggering the stage makes one linear pass over the whole frame range. It probes the candidate VBI lines of both fields of every frame for teletext data lines, recovers the packets, and writes them as a flat, headerless packet stream in strictly temporal order (frame → field → ascending line). Packets keep their transmission coding (Hamming 8/4 addressing, odd-parity display bytes), so consumers decode the stream exactly as a receiver decodes a live broadcast.

Both television systems ITU-R BT.653 defines System B on are covered, and the service decides the file the run writes:

* **625 lines** (PAL) — ETSI EN 300 706, 42-byte packets, written as `.t42`
* **525 lines** (NTSC, PAL-M) — BT.653 Table 1b, 34-byte packets, written as `.t34`

The same pass assembles the pages, so the run also produces a catalogue of every page the recording carried — where each was first and last seen, how often the carousel brought it round, and its best assembly from every copy recovered. That is what the **Teletext Pages** tool shows. Because it comes from a pass over the whole source rather than a window around the preview position, the list is the service's full carousel.

Recovery quality tracks the source's luma bandwidth: LaserDisc and broadcast-quality CVBS captures are read exactly by threshold slicing, while consumer VHS loses the clock run-in entirely and needs the MLSE detector, which recovers readable pages from PAL SP and LP recordings. The default `detector` setting picks between the two per line, so neither source needs configuring. By default the stage also mends display bytes that fail their parity check and combines the repeated transmissions of each page row before writing (`repair_damaged_bytes`, `squash_repeated_rows`); turn both off to write the packets exactly as recovered.

**Parameters**

* `output_path` (file path)
    - Path to the output packet stream. The service's extension is appended if absent — `.t42` on a 625-line source, `.t34` on a 525-line one.
    - Required.
* `first_vbi_line` (integer)
    - First candidate field line probed, 1-based, both fields.
    - Default: `6` on a 625-line project, `10` on a 525-line one.
* `last_vbi_line` (integer)
    - Last candidate field line probed, 1-based, both fields.
    - Default: `22` on a 625-line project, `21` on a 525-line one.
* `keep_empty_packets` (boolean)
    - Emit a whole zero packet for candidate lines with no data so packet position maps 1:1 to (frame, field, line) — the vhs-decode convention.
    - Default: `false`.
* `detector` (string)
    - How data bits are recovered: `Threshold` (slice at bit centres; exact on discs and direct captures), `MLSE` (fit the recording's frequency response to the known start of each line, detect against it, then refit that response to the whole packet just read and read it again; recovers teletext from tape, where limited bandwidth smears bits into their neighbours), or `Automatic` (threshold first, MLSE only where it fails — same behaviour and cost as threshold alone on a disc source).
    - Default: `Automatic`.
* `tolerant_framing` (boolean)
    - Accept framing codes with one bit error (more packets from noisy sources, higher false-positive rate).
    - Default: `false`.
* `require_valid_mrag` (boolean)
    - Drop packets whose magazine/row address fails Hamming 8/4 correction (suppresses false locks on noise).
    - Default: `true`.
* `repair_damaged_bytes` (boolean)
    - Every display byte carries a parity bit, so a byte that fails its parity check is known to be damaged. Restore it by flipping the bit the MLSE detector came closest to reading the other way. Recovers characters a difficult tape would otherwise lose; the cost is that a repaired byte can no longer be told from an undamaged one, so a repair that guessed wrong is no longer marked as damage. Applies to the MLSE detector only, so a disc or direct capture is unaffected.
    - Default: `true`.
* `squash_repeated_rows` (boolean)
    - Teletext pages are transmitted on a loop, so a recording holds several copies of every page row, damaged in different places. Combine them byte by byte — preferring values that pass their parity check, then weighting by how sure the detector was of each byte — and write the combined rows. Packet order, count and timing are unchanged; only damaged display bytes move. The pages shown in the viewer are built from the combined rows too. Needs a second pass over the recovered packets, held in memory (roughly 50 bytes each).
    - Copies are combined only within one run of a page: a header with the erase bit set (C4) says the content is being replaced, so what follows is a different page sharing a number. A service that erases on every transmission gives each one a run of its own, and nothing can be combined — the report says so, as a run count matching the transmission count.
    - Default: `true`.
* `write_report` (boolean)
    - Write the run's diagnostic report next to the packet stream under its full name plus `.txt` (`mydata.t42` gives `mydata.t42.txt`). It opens with the result in one line — `Data loss 1.14% — 30 of 2,640 recovered characters are damaged` — and the same figure appears in the stage's status when the run finishes. Below that it covers what was exported, how recovery went, how many pages were catalogued, and what combining repeated rows changed. The same report always goes to the log at debug level.
    - Damage is counted by the odd parity every display byte carries, over the display rows as written. It is a floor rather than an exact count — a byte damaged in two bits passes parity — and it says nothing about rows that never arrived.
    - Default: `false`.
* `export_subtitles` (boolean)
    - Decode the subtitle page alongside the packet export and write timed cues to a `.srt` file next to the output. Offered on 625-line projects only: the cue timing derives from 50 fields per second.
    - Default: `false`.
* `subtitle_page` (string)
    - Teletext page carrying the subtitles: magazine digit (1–8) plus two hexadecimal page digits, e.g. `888`.
    - Default: `888`.
* `subtitle_format` (string)
    - Subtitle output format; currently `SRT` (SubRip) only.
    - Default: `SRT`.

**Stage tools**

* **Teletext Pages** — the page viewer for this node. It lists every page the range carried, with how many times each was seen and the frames it was first and last seen at, and renders the selected page as a Level 1 display alongside the run's recovery summary. Opened automatically after triggering; opening it from the Stage Tools menu on a node that has not been triggered runs the decode first, with progress and cancel.

**Notes**

* PAL, NTSC and PAL-M sources are accepted; any other video system reports an error. NABTS (System C) shares the 525 lines but not the framing code, so its lines are seen and rejected rather than decoded, and NTSC line-21 captions are handled by the Closed Caption Sink instead.
* This stage writes no CSV — its file output is the packet stream, and optionally the subtitle document and the report.
* The `.t42` format is described on the zxnet teletext wiki (T42 packet stream); `.t34` is the same flat, headerless convention at the 525-line packet length.
* A 525-line service sends the last eight columns of its rows in separate row-extension packets, which the page viewer reassembles; the packet stream holds them as transmitted.
* Subtitle export drops Level 1 colour and positioning attributes; the `.srt` carries plain text timed from the field rate. With `squash_repeated_rows` enabled the cues are decoded from the combined rows, so they benefit from the same correction.
* Combining repeated rows ("squashing") is an idea taken from [vhs-teletext](https://github.com/ali1234/vhs-teletext) by Alistair Buxton. A row transmitted only once cannot be corrected, so the benefit grows with how long the recording runs and how often each page recurs.

---

## Notes on Analysis Sink Stages

* Analysis sink stages terminate pipeline branches.
* Multiple analysis sinks may consume the same upstream output.
* Analysis sinks are side-effect-free with respect to media data.
* Results are intended for diagnostics, comparison, and validation—not for further pipeline processing.
