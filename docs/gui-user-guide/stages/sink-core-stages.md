# Sink Stages

Sink stages are the **endpoints of a decode-orc pipeline**. They consume processed data from upstream stages and write results to disk. Unlike transform stages, sink stages do not produce outputs that can be connected further downstream.

A pipeline may contain **multiple sink stages** in parallel, allowing the same processed stream to be written in different formats or to different destinations.

Sink stages are used to:

* Write final video outputs (TBC + metadata, CVBS files, or encoded video)
* Export auxiliary data such as audio, EFM, AC3, or closed captions
* Export intermediate data for inspection or external tools

---

## AC3 RF Sink

| | |
|-|-|
| **Stage id** | `AC3RFSink` |
| **Stage name** | AC3 RF Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Decode AC3 RF (Dolby Digital) samples and write AC3 frames to file |

**Use this stage when:**

* Processing later North American NTSC LaserDiscs that carry AC3 RF 5.1 surround sound
* You want the extracted AC3 audio alongside the video output in a single pipeline trigger

**What it does**

This stage reads AC3 RF samples from the incoming stream, decodes the RF-modulated Dolby Digital bitstream frame by frame, and writes the resulting AC3 audio frames sequentially to the output file. The output is a raw AC3 elementary stream with no container wrapping; it can be played back directly or muxed into a video container.

**Parameters**

* `output_path` (string)
    - Path to the output AC3 file. The conventional extension is `.ac3`.
    - Required.

**Notes**

* The upstream source must supply AC3 RF data; the pipeline will abort at trigger time if none is present.
* This stage is specific to AC3 RF as found on LaserDiscs; it does not handle AC3 carried in other formats or containers.

---

## Audio Sink

| | |
|-|-|
| **Stage id** | `AudioSink` |
| **Stage name** | Audio Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Export any pipeline audio channel pair to a WAV file |

**Use this stage when:**

* Your source carries audio channel pairs
* You want to export audio independently of video output
* You want to inspect or process audio externally

**What it does**

This stage extracts one audio channel pair from the incoming stream and writes it to a standard WAV file. The pair can be any channel pair carried by the pipeline — analogue capture audio, decoded EFM digital audio, an imported WAV, or a channel pair derived by a transform. Audio remains synchronised to the processed video timeline, so any frame trimming or reordering performed upstream is reflected in the output.

The pipeline carries stereo audio channel pairs at exactly 48,000 Hz, frame-locked (synchronous) to the video for every system, following SMPTE 272M-1994. The WAV output is 24-bit signed little-endian PCM declaring 48,000 Hz; no resampling or bit-depth conversion is performed.

**Parameters**

* `output_path` (string)
    - Path to the output WAV file.
    - Required.

* `channel_pair` (integer)
    - Audio channel pair to write, 0-based (0–7), matching the CVBS container's `_audio_<p>.wav` numbering.
    - Default 0. Triggering fails if the selected channel pair does not exist.

**Notes**

* This stage writes whatever channel pair you select. Analogue capture audio arrives as channel pair 0 from the source; EFM digital audio (CD-quality stereo) becomes a channel pair when you add an **EFM Audio Decode** transform upstream, after which it can be written here like any other pair. For a bit-exact, un-resampled WAV of EFM audio use the EFM Decoder Sink instead; AC3 RF (Dolby Digital) is exported via the AC3 RF Sink.
* Audio stacking or selection must be performed upstream (e.g. via `stacker`).

---

## Closed Caption Sink

| | |
|-|-|
| **Stage id** | `CCSink` |
| **Stage name** | Closed Caption Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Extract and write NTSC Line 21 closed-caption (CC) data |

**Use this stage when:**

* Working with NTSC sources containing Line 21 closed captions
* You want to extract captions for archival or conversion
* You want to inspect CC data independently of video

**What it does**

For each field the stage reads the two caption bytes embedded in VBI Line 21, accumulates the byte pairs across the full field sequence, and writes them in the chosen format: Scenarist SCC V1.0 (industry-standard, with HH:MM:SS:FF timestamps and hex byte pairs) or plain text (printable ASCII only, control codes stripped).

**Parameters**

* `output_path` (string)
    - Path to the closed-caption output file. Use `.scc` for SCC format or `.txt` for plain text.
    - Required.

* `format` (string)
    - Export format.
    - Allowed values: `Scenarist SCC`, `Plain Text`.
    - Default: `Scenarist SCC`.

**Notes**

* Handles NTSC Line 21 only; PAL sources do not carry Line 21 CC data.
* CC data must be preserved upstream — masking Line 21 before this stage will destroy the caption payload.
* If the source contains no CC data the output file will be empty but the stage will not abort.

---

## CVBS Sink

| | |
|-|-|
| **Stage id** | `CVBSSink` |
| **Stage name** | CVBS Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Write CVBS frames to a CVBS file-format family output |

**Use this stage when:**

* You want to archive or exchange a processed CVBS signal in the standard CVBS file format.
* You want to produce a `.cvbs` or `.cvbsy`/`.cvbsc` output that can be re-opened by the CVBS Source stage.
* You need to write associated dropout, audio, EFM, or AC3 sidecars alongside the video.

**What it does**

This stage writes processed frame data using the selected sample encoding, and a `.meta` SQLite sidecar. The output signal type follows the project type automatically: a composite project is written as a single `.cvbs` file and a Y/C project as a `.cvbsy`/`.cvbsc` pair (per the CVBS file format naming convention) — Y/C cannot be derived from a composite signal, so this is not a choice. The `.meta` file records the signal type and the selected `sample_encoding_preset`, and always carries `signal_state_preset = 'STANDARD_TBC_LOCKED'`. The signal state is not user-configurable — it reflects the pipeline invariant that only locked, standard-state signals appear at this point.

Associated sidecars are written automatically when the upstream source provides them:

- `.dropouts.meta` — when dropout hints are present
- `_audio_0.wav` … `_audio_7.wav` — when audio is present (one 24-bit 48 kHz stereo WAV per channel pair)
- `.efm` + `.efm.meta` — when EFM data is present
- `.ac3` + `.ac3.meta` — when AC3 RF data is present

A CVBS file written by this stage can be round-tripped back through the CVBS Source stage.

**Parameters**

* `output_path` (string)
    - Base path for output files. A trailing `.cvbs`, `.cvbsy`, or `.cvbsc` extension is stripped when present.
    - Required.

* `sample_encoding` (string)
    - Sample encoding of the output data, recorded as `sample_encoding_preset` in the `.meta` file.
    - Allowed values: `CVBS_U10_4FSC`, `CVBS_U16_4FSC`, `CVBS_TPG21_4FSC`, `CVBS_S16_4FSC`.
    - Default: `CVBS_U10_4FSC` (lossless; preserves headroom). The other encodings clamp to their representable domain before scaling.

* `capture_notes` (string)
    - Optional free-text notes written to the `.meta` file.
    - Default: `""` (not written when empty).

**Notes**

* `signal_state_preset` in the output `.meta` is always `STANDARD_TBC_LOCKED` and cannot be overridden by the user.
* Absent upstream extensions (no audio, no EFM, etc.) produce no sidecar files — this is not an error.

---

## Daphne VBI Sink

| | |
|-|-|
| **Stage id** | `daphne_vbi_sink` |
| **Stage name** | Daphne VBI Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Write per-field VBI data in the format required by the Daphne arcade LaserDisc emulator |

**Use this stage when:**

* Archiving a LaserDisc title for use with the Daphne arcade LaserDisc emulator

**What it does**

Reads VBI data from each frame in the incoming stream and writes binary VBI records field by field to a `.vbi` file according to the Daphne VBIInfo specification. The `.vbi` file carries the per-field VBI metadata that Daphne requires to emulate the disc's interactivity correctly.

**Parameters**

* `output_path` (string)
    - Path to the output `.vbi` file.
    - Required.

**Notes**

* This sink produces a file specific to the Daphne emulation project and is not a general-purpose VBI archive format.
* The `.vbi` format is documented at the Daphne VBIInfo wiki page.
* Connect other sinks in parallel if you also need video output.

---

## EFM Decoder Sink

| | |
|-|-|
| **Stage id** | `EFMSink` |
| **Stage name** | EFM Decoder Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Decode EFM t-values to audio WAV or ECMA-130 binary sector data |

**Use this stage when:**

* Extracting digital audio from a LaserDisc source as a WAV file
* Extracting ECMA-130 data sectors from a LaserDisc source
* You want the fully decoded output of the EFM stream rather than the raw t-values

**What it does**

This stage accumulates EFM t-values from the incoming stream and runs the full EFM decode pipeline (demodulation, error detection, CIRC error correction, de-interleaving), producing either a standard PCM audio WAV file or ECMA-130 binary sector data depending on the chosen decode mode.

**Parameters**

* `output_path` (string)
    - Path to the decoded output file. Use `.wav` for audio mode or `.bin` for data mode.
    - Required.

* `decode_mode` (string)
    - Selects the decode target. `audio` (default) produces a WAV or raw PCM file; `data` produces ECMA-130 binary sector data.
    - Allowed values: `audio`, `data`.
    - Default: `audio`.

* `no_timecodes` (boolean)
    - Disable timecode verification (early discs did not include time-codes in the EFM and will fail to decode without this option).
    - Applies to both `audio` and `data` modes.
    - Default: `false`.

* `audacity_labels` (boolean)
    - Write an Audacity label file alongside the audio output indicating the position of chapters as well as any missing samples.
    - Applies only in `audio` mode.
    - Default: `false`.

* `no_audio_concealment` (boolean)
    - Disable interpolation-based audio error concealment. When disabled, affected samples are zeroed instead of interpolated.
    - Applies only in `audio` mode.
    - Default: `false`.

* `ignore_preemphasis` (boolean)
    - Ignore the 50/15 µs pre-emphasis CONTROL flag (IEC 60908 §17.5) and write the audio exactly as decoded. When unchecked (default), sections flagged as pre-emphasised are de-emphasised during decode with a 50/15 µs filter so the output plays back with a flat response; enable this only if you want the raw pre-emphasised samples. When `audacity_labels` is enabled, a pre-emphasised track's label reads `Preemphasis:50/15us(removed)` when de-emphasis was applied, or `Preemphasis:50/15us` when this flag is set.
    - Applies only in `audio` mode.
    - Default: `false`.

* `zero_pad` (boolean)
    - Zero-pad the start of audio output so the sample starts from 00:00:00.0 relative to the first valid time-code.
    - Applies only in `audio` mode.
    - Default: `false`.

* `no_wav_header` (boolean)
    - Output raw PCM samples without a WAV file header.
    - Applies only in `audio` mode.
    - Default: `false`.

* `output_metadata` (boolean)
    - Write a bad-sector map metadata file alongside the sector output.  This file contains the number of any missing or corrupt sectors.
    - Applies only in `data` mode.
    - Default: `false`.

* `report` (boolean)
    - Write a detailed decode statistics report file.
    - Default: `false`.

**Notes**

* The source stage must supply an EFM file; the pipeline will abort if no EFM data is present in the incoming stream.
* Audio and data decoding are mutually exclusive — select `decode_mode` before enabling mode-specific parameters. Parameters for the inactive mode are silently ignored.
* EFM stacking or correction should be performed upstream before this stage.

---


## ld-decode Sink

| | |
|-|-|
| **Stage id** | `ld_sink` |
| **Stage name** | ld-decode Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Write an ld-decode-compatible TBC and metadata output |

**Use this stage when:**

* Producing final archival-quality outputs
* Feeding results back into the ld-decode ecosystem (ld-chroma-decoder, ld-analyse, ld-process-vbi, …)
* Preserving full per-field metadata

**What it does**

This stage writes:

* A `.tbc` file containing processed video fields
* A `.tbc.db` metadata database compatible with ld-decode

The output can be used directly with existing ld-decode tools.

**Parameters**

* `output_path` (string)
    - Base path for the output files — the stage appends the `.tbc` and `.tbc.db` extensions automatically.
    - Required.

**Notes**

* This is the most common "final output" sink stage.
* All upstream corrections, stacking, and parameter overrides should be complete before this stage.
* The target directory must exist and be writable at trigger time.
* This stage writes video and metadata only — export analogue audio, EFM, or AC3 RF data with the Audio Sink, Raw EFM Data Sink / EFM Decoder Sink, or AC3 RF Sink stages connected in parallel.

---

## Raw EFM Sink

| | |
|-|-|
| **Stage id** | `RawEFMSink` |
| **Stage name** | Raw EFM Data Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Write raw EFM t-values to a binary file |

**Use this stage when:**

* Archiving LaserDisc EFM t-values for later processing
* Feeding raw EFM data into external decoding or analysis tools
* Verifying EFM integrity after stacking or correction

**What it does**

This stage extracts raw EFM (Eight-to-Fourteen Modulation) t-values from the incoming stream and writes them to a binary file. The output contains only 8-bit unsigned integers representing valid t-values in the range 3–11, stored field by field with no headers or additional formatting.

**Parameters**

* `output_path` (string)
    - Path to the output EFM file (raw t-values). Conventionally uses the `.efm` extension.
    - Required.

**Notes**

* The source stage must supply an EFM file; the pipeline will abort if no EFM data is present in the incoming stream.
* EFM stacking behaviour is controlled upstream (e.g. via `stacker`).
* This stage does not modify or decode EFM data. Use the EFM Decoder Sink stage to decode t-values to audio or sector data.

---

## Teletext Sink

| | |
|-|-|
| **Stage id** | `teletext_sink` |
| **Stage name** | Teletext Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Recover World System Teletext from the VBI, export the packet stream, and browse the pages the recording carried |

**Use this stage when:**

* Preserving teletext carried by a LaserDisc, CVBS capture, or tape source
* Reading the pages a recording carried without leaving decode-orc
* Producing a packet stream for external teletext tools (vhs-teletext, wxTED)

**What it does**

Triggering the stage makes one linear pass over the whole frame range. It probes the candidate VBI lines of both fields of every frame for teletext data lines, recovers the packets, and writes them as a flat, headerless packet stream in strictly temporal order (frame → field → ascending line). Frames are decoded on several threads while the stream is written from one, and the pass learns where in the line this particular recording puts its data and which lines carry it, so later frames cost far less than the first ones — together these take a pass over the reference captures from 1970 ms to 295 ms (625-line) and 946 ms to 156 ms (525-line). None of it changes what is recovered: see `decode_threads`, `pin_data_phase` and `learn_active_lines` below. Packets keep their transmission coding (Hamming 8/4 addressing, odd-parity display bytes), so consumers decode the stream exactly as a receiver decodes a live broadcast.

Both television systems ITU-R BT.653 defines System B on are covered, and the service decides the file the run writes:

* **625 lines** (PAL) — ETSI EN 300 706, 42-byte packets, written as `.t42`
* **525 lines** (NTSC, PAL-M) — BT.653 Table 1b, 34-byte packets, written as `.t34`

The same pass assembles the pages, so the run also produces a catalogue of every page the recording carried — where each was first and last seen, how often the carousel brought it round, and its best assembly from every copy recovered. That is what the **Teletext Pages** tool shows. Because it comes from a pass over the whole source rather than a window around the preview position, the list is the service's full carousel.

Recovery quality tracks the source's luma bandwidth: LaserDisc and broadcast-quality CVBS captures are read exactly by threshold slicing, while consumer VHS loses the clock run-in entirely and needs the MLSE detector, which recovers readable pages from PAL SP and LP recordings. The default `detector` setting picks between the two per line, so neither source needs configuring. By default the stage also mends display bytes that fail their parity check and combines the repeated transmissions of each page row before writing (`repair_damaged_bytes`, `squash_repeated_rows`); turn both off to write the packets exactly as recovered.

**Parameters**

* `output_path` (file path)
    - Path to the output packet stream. The service's extension is appended if absent — `.t42` on a 625-line source, `.t34` on a 525-line one.
    - Optional. Left empty, the run decodes exactly as it would but writes no file, which is what to do when the pages themselves are what you are after — the **Teletext Pages** tool is filled either way. `write_report` and `export_subtitles` are written beside the packet stream, so enabling either without a path fails the run.
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
* `pin_data_phase` (boolean)
    - Most of the work of reading a line is searching the whole of the standard's data-timing window for where the data burst starts. Every line of a time-base-corrected recording starts at very nearly the same place, so once enough lines have been read the search narrows to where they agreed. A narrowed search that finds nothing is repeated over the full window, so this cannot lose a packet; it costs a few percent on lines that carry no data.
    - Measured on the reference captures this is the larger of the two savings, and it also *recovers* packets an exhaustive search misses — narrowing the window rejects the false correlation peaks a whole-window search can settle on. The report says where the window was pinned, or why it was not.
    - Default: `true`.
* `learn_active_lines` (boolean)
    - A service uses a few of the lines its standard permits, but every line of the window is read on every frame, and on a line carrying picture content, VITS, VITC or captions that work is spent reaching a rejection already reached on the frame before. Read every line for the first 50 frames, then only the lines that have carried a packet, rechecking the full window every 50th frame so a service that starts part way into a recording is still picked up.
    - Unlike `pin_data_phase` this can lose a packet: a line that carries data exactly once, outside both the learning frames and a recheck frame, is not read. On the reference PAL capture that cost one packet in 3,964. Turn it off for an archival pass where every packet matters.
    - Default: `true`.
* `decode_threads` (integer)
    - Threads to recover lines on; `0`, the default, uses one per processor. Each line is recovered from its own samples, so frames are decoded several at a time while the packets are written from one thread in the order they were transmitted.
    - The recovered stream is identical whatever this is set to, so lower it only to leave the machine free for other work. Measured on the 625-line reference capture: 1406 ms on one thread, 442 on four, 295 on eight; past the processor's physical cores there is nothing more to win, because the run is by then waiting on the source stage to hand out frames.
    - Default: `0`.
* `squash_repeated_rows` (boolean)
    - Teletext pages are transmitted on a loop, so a recording holds several copies of every page row, damaged in different places. Combine them byte by byte — preferring values that pass their parity check, then weighting by how sure the detector was of each byte — and write the combined rows. Packet order, count and timing are unchanged; only damaged display bytes move. The pages shown in the viewer are built from the combined rows too. Needs a second pass over the recovered packets, held in memory (roughly 50 bytes each).
    - Copies are combined only within one run of a page: a header with the erase bit set (C4) says the content is being replaced, so what follows is a different page sharing a number. A service that erases on every transmission gives each one a run of its own, and nothing can be combined — the report says so, as a run count matching the transmission count.
    - Default: `true`.
* `write_report` (boolean)
    - Write the run's diagnostic report next to the packet stream under its full name plus `.txt` (`mydata.t42` gives `mydata.t42.txt`, `mydata.t34` gives `mydata.t34.txt`), so it needs `output_path` set. It opens with the result in one line — `Data loss 1.14% — 30 of 2,640 recovered characters are damaged` — and the same figure appears in the stage's status when the run finishes. Below that it covers what was exported, how recovery went, how many pages were catalogued, and what combining repeated rows changed. The same report always goes to the log at debug level.
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

* **Teletext Pages** — the page viewer for this node. It lists every page the range carried, with how many times each was seen and the frames it was first and last seen at, and renders the selected page as a Level 1 display alongside the run's recovery summary. It opens automatically when the node is triggered, which is how the pages are reached: leave `output_path` empty and triggering the node is a decode-and-browse with no file written.

**Notes**

* PAL, NTSC and PAL-M sources are accepted; any other video system reports an error. NABTS (System C) shares the 525 lines but not the framing code, so its lines are seen and rejected rather than decoded, and NTSC line-21 captions are handled by the Closed Caption Sink instead.
* This stage writes no CSV — its file output is the packet stream, and optionally the subtitle document and the report.
* The `.t42` format is described on the zxnet teletext wiki (T42 packet stream); `.t34` is the same flat, headerless convention at the 525-line packet length.
* A 525-line service sends the last eight columns of its rows in separate row-extension packets, which the page viewer reassembles; the packet stream holds them as transmitted.
* Subtitle export drops Level 1 colour and positioning attributes; the `.srt` carries plain text timed from the field rate. With `squash_repeated_rows` enabled the cues are decoded from the combined rows, so they benefit from the same correction.
* Combining repeated rows ("squashing") is an idea taken from [vhs-teletext](https://github.com/ali1234/vhs-teletext) by Alistair Buxton. A row transmitted only once cannot be corrected, so the benefit grows with how long the recording runs and how often each page recurs.

---

## Video Sink

| | |
|-|-|
| **Stage id** | `video_sink` |
| **Stage name** | Video Sink |
| **Connections** | 1 input → no outputs |
| **Purpose** | Chroma-decode the processed video and write it to a file, either FFmpeg-encoded (MP4/MKV/MOV/MXF) or uncompressed raw (RGB/YUV/Y4M) |

**Use this stage when:**

* You want a playable, distributable, or archival video file (FFmpeg mode)
* You want optional embedded audio, closed captions, or chapter metadata (FFmpeg mode)
* You need an uncompressed output for external tools such as FFmpeg, VirtualDub, or image-processing scripts (raw mode)

**What it does**

Applies the selected chroma decoder to convert the incoming TBC video stream to colour video, then writes the result according to the selected output mode. In FFmpeg mode the video is encoded into the chosen container and codec, optionally embedding pipeline audio (up to 8 channel pairs, one output stream per pair), closed captions (as mov_text subtitles, MP4/MOV only), and chapter markers derived from VBI data. In raw mode the decoded frames are written to a file without compression; the raw format determines the pixel layout and whether a Y4M header is prepended.

**Parameters**

* `output_path` (string)
    - Output file path. Match the extension to the selected mode and format: `.mp4`, `.mkv`, `.mov`, or `.mxf` for FFmpeg output; `.rgb`, `.yuv`, or `.y4m` for raw output.
    - Required.

* `decoder_type` (string)
    - Chroma decoder to apply. PAL: `pal2d`, `transform2d`, `transform3d`. NTSC: `ntsc1d`, `ntsc2d`, `ntsc3d`, `ntsc3dnoadapt`. Other: `mono`.

* `output_mode` (string)
    - Output path selection. Values: `ffmpeg` (encoded output via FFmpeg), `raw` (uncompressed file output). Default: `ffmpeg`.

* `raw_format` (string)
    - Raw output format (raw mode only). Values: `rgb` (RGB48, 16-bit per channel), `yuv` (YUV444P16, planar), `y4m` (YUV444P16 with Y4M header). Default: `rgb`.

* `ffmpeg_format` (string)
    - Container and codec (FFmpeg mode only). Values include `mp4-h264`, `mkv-ffv1`, `mov-prores`, `mov-v210`, `mov-v410`, `mxf-mpeg2video`, `mov-h264`, `mp4-hevc`, `mov-hevc`, and `mp4-av1`. Default: `mp4-h264`.

* `chroma_gain` (double) / `chroma_phase` (double)
    - Chroma gain multiplier (0.0–10.0, default 1.0) and phase rotation in degrees (-180 to 180, default 0).

* `luma_nr` (double) / `chroma_nr` (double)
    - Luma / chroma noise reduction levels. Higher values reduce noise at the cost of sharpness or chroma resolution.

* `ntsc_phase_comp` (bool)
    - Enable NTSC phase compensation. NTSC sources only.

* `simple_pal` (bool)
    - Enable simple PAL chroma decoding (1D UV filter for Transform PAL). `transform2d`/`transform3d` decoders only.

* `transform_threshold` (double)
    - Similarity threshold for the Transform PAL decoder. Higher = more transform filtering. Range: 0.0–1.0. Default: 0.4. `transform2d`/`transform3d` decoders only.

* `chroma_weight` (double)
    - Chroma weight for the NTSC 3D adaptive filter. Higher = prefer more 2D result. Range: 0.0–10.0. Default: 1.0. `ntsc3d`/`ntsc3dnoadapt` decoders only.

* `adapt_threshold` (double)
    - NTSC 3D adaptive filter threshold. Higher = prefer more 3D result. Range: 0.0–10.0. Default: 1.0. `ntsc3d` decoder only.

* `output_padding` (int)
    - Alignment padding added to each output frame. Default: 8.

* `encoder_preset` (string)
    - FFmpeg mode only. Encoder speed/quality trade-off. Values: `fast`, `medium`, `slow`, `veryslow`.

* `encoder_crf` (int)
    - FFmpeg mode only. Constant Rate Factor for quality-based encoding. Range: 0–51 (lower = higher quality). Default: 18. Used when `encoder_bitrate` is 0.

* `encoder_bitrate` (int)
    - FFmpeg mode only. Target bitrate in bits per second. When non-zero, overrides CRF mode. Default: 0 (use CRF).

* `hardware_encoder` (string)
    - FFmpeg mode only. Hardware-accelerated encoding backend. Values: `none`, `vaapi`, `nvenc`, `qsv`, `amf`, `videotoolbox`. Default: `none`.

* `prores_profile` (string)
    - FFmpeg mode only, `mov-prores` format. ProRes quality profile: `proxy`, `lt`, `standard`, `hq`, `4444`, `4444xq`. Default: `hq`.

* `use_lossless_mode` (bool)
    - FFmpeg mode only. Enable mathematically lossless encoding (H.264/H.265/AV1 only, overrides CRF). Default: `false`.

* `apply_deinterlace` (bool)
    - FFmpeg mode only. Apply bwdif deinterlacing for progressive web playback. One frame is produced per field, so the output frame rate doubles (50 fps PAL, 59.94 fps NTSC). Default: `false`.

* `display_aspect_ratio` (string)
    - FFmpeg mode only. Display aspect ratio signalled to players. Metadata only — the video is not rescaled. Values: `auto` (square pixels), `4:3`, `16:9`. Most SD material should be played back at `4:3`. Default: `auto`.

* `video_filter` (string)
    - FFmpeg mode only. Custom FFmpeg video filter chain applied before encoding, using the same syntax as ffmpeg's `-vf` option (e.g. `fieldmatch,decimate` for inverse telecine, `crop=692:554`). Filters may change output dimensions and frame rate; the encoder follows the filter output automatically. An invalid filter string fails the export with the FFmpeg error message. Default: empty (no filtering).

* `embed_audio` (bool)
    - FFmpeg mode only. Embed pipeline audio into the output file, one output audio stream per selected channel pair. Requires audio in the pipeline. Default: `false`.

* `audio_channel_pairs` (string)
    - FFmpeg mode only; available only when `embed_audio` is enabled. Which audio channel pairs to embed: `all` (default) or a comma-separated list of 0-based channel pair indices, e.g. `0,2`. Indices match the CVBS container's `_audio_<p>.wav` numbering. The export fails if a listed channel pair does not exist.

* `audio_gain_db` (double)
    - FFmpeg mode only; available only when `embed_audio` is enabled. Gain applied to the embedded audio in decibels. `0` = unchanged; positive boosts (6 dB roughly doubles the amplitude), negative attenuates. Samples are clipped at full scale. Range: -24 to 24. Default: `0`.

* `embed_closed_captions` (bool)
    - FFmpeg mode only. Embed closed captions as mov_text subtitles. MP4/MOV output only. Default: `false`.

* `embed_chapter_metadata` (bool)
    - FFmpeg mode only. Write chapter markers derived from VBI data into the output file. Default: `false`.

**Stage tools**

* **FFmpeg Preset Config** — a preset helper dialog that applies well-tested encoder combinations without setting each parameter manually. Applying a preset switches the stage to FFmpeg output mode.

**Notes**

* Raw mode does not support audio, closed caption, or chapter embedding; those options apply to FFmpeg output only.
* Raw output files can be very large; ensure sufficient disk space before triggering.
* The `y4m` raw format is directly readable by tools such as FFmpeg and rav1e without specifying the pixel format manually.
* CRF and bitrate modes are mutually exclusive; set `encoder_bitrate` to a non-zero value to switch from CRF mode.
* Video filtering (`apply_deinterlace` or `video_filter`) is not supported with hardware encoders that use GPU surfaces (`vaapi`, `qsv`, `videotoolbox`); the export automatically falls back to the software encoder in that case.
* When a video filter chain is active, interlaced coding flags are not forced on the encoder; the field structure of the filter output determines how frames are flagged.
* Projects created with the earlier separate `raw_video_sink` and `ffmpeg_video_sink` stages are migrated to this stage automatically when loaded.

---

## Notes on Sink Stages

* Sink stages terminate pipeline branches.
* Multiple sink stages may consume the same upstream output.
* Sink stages do not alter timing or metadata beyond their specific export role.

---

## Removed stages

### HackDAC Sink (removed in v2.0)

The `hackdac_sink` stage was removed in Decode-Orc 2.0. It is no longer available in the plugin registry. Projects that referenced this stage must be recreated without it.
