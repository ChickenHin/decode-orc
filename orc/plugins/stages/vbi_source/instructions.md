# VBI Capture Source

Reads a raw VBI capture — a file holding nothing but the vertical-blanking line records a capture card or a cropped `.tbc` wrote — and synthesises the complete CVBS frames those lines were cut out of. The result is an ordinary CVBS_U10_4FSC VideoFrameRepresentation, so the existing teletext decoders and every other downstream stage see exactly what they see from a native decode.

## When to use

Add VBI Capture Source as the first stage of a pipeline when your material is a third-party VBI dump rather than a decoded capture: a bt8x8 card dump (`.vbi`, often FLAC-compressed as `.vbi.flac`), or another raw teletext capture in the same family. If you have a `.tbc` or a `.composite` file, use the TBC Source or CVBS Source stage instead — those carry a whole picture and need no synthesis.

Nothing is written to disk by this stage. To export the synthesised frames as a `.composite` + `.meta` pair, connect a CVBS Sink.

**What is implemented so far:** the `bt8x8-pal` preset — a 625-line, 8×fsc, 8-bit card capture carrying World System Teletext — raw or FLAC-wrapped, end to end. Every other format in the design's table (`bt8x8-ntsc`, `cx88-pal`, `saa7131-pal`, `tbc-pal`, `tbc-vbi-pal`, `tbc-vbi-ntsc`) and the NABTS data service are refused at configuration with a message saying so, rather than producing plausible but wrong output.

## Parameters

| Parameter | Meaning |
|-----------|---------|
| VBI Capture Path (`input_path`) | Path to the raw capture. FLAC-wrapped captures are unwrapped transparently; the wrapper's declared sample rate is a conventional placeholder and is never used for timing. |
| Capture Format (`format`) | Named container preset. `bt8x8-pal` is the implemented preset; `custom` spells the container out field by field instead. Default `bt8x8-pal`. |
| Teletext System (`teletext_system`) | Data service the captured lines carry: `WST` (System B, 625 lines) or `NABTS` (System C, 525 lines). Default `WST`; `NABTS` is refused with a clear error and the configured system is carried on the stage's output so a downstream decoder can see it. |
| Synthesise Colour Burst (`synthesise_burst`) | Write a coherent colour burst on every synthesised line. Default on. |
| Capture Offset (`capture_offset_mode`) | `auto` (default) fits the time from 0H to sample 0 of each record from the clock run-in of the captured lines; `manual` applies a configured figure unchanged. |
| Capture Offset (samples) (`capture_offset_samples`) | The figure `manual` applies, in source samples. Default 0. |
| Level Mapping (`levels`) | `per-line` (default) estimates the logic levels from each record's own structure; `rolling` holds a line at the frame's median except where it deviates significantly; `fixed` applies configured levels and measures nothing. |
| Fixed Logic 0 / 1 Level (`fixed_logic0`, `fixed_logic1`) | Source-domain levels used by `fixed` mode. Defaults 0 and 255. |
| First Stored Field (`first_field`) | Television field the first stored field of each frame carries, 1 or 2. Default 1. |
| Dropped Frames (`drops`) | `preserve` (default) emits only the frames present; `pad` synthesises blank frames so output frame *n* stays aligned with source frame *n*. |

With `format` set to `custom`, these container fields appear and must be filled in; they are ignored for every named preset, which supplies its own.

| Parameter | Meaning |
|-----------|---------|
| Sample Rate (Hz) (`container_sample_rate_hz`) | Exact sampling rate of the capture. |
| Record Stride (samples) (`container_line_length`) | Stored samples per line record, hardware padding included. |
| Valid Samples per Record (`container_valid_samples`) | Real samples at the start of each record; anything beyond is padding and is never resampled or measured. |
| Sample Format (`container_sample_format`) | `u8`, `u16le`, or `s16le`. Only `u8` has a decode path so far. |
| Records per Field (`container_field_lines`) | Stored line records per field: the field stride. |
| First / Last Data Record (`container_first_record`, `container_last_record`) | Which of those records carry the data service, 0-based and inclusive. |
| Frame Trailer Bytes (`container_frame_trailer_bytes`) | Trailing bytes of each stored frame that are not sample data. Four for the bt8x8 frame counter; zero for formats without one. |
| Television System (`container_tv_system`) | Television system the capture was made from. Only `PAL` can currently be synthesised. |

The frame lines the data is placed on are not configured directly: they follow from the television and teletext systems (WST occupies broadcast frame lines 7–22 and 320–335) and from which stored records the container declares as carrying data. A source holding fewer records than the standard defines maps contiguously from the first data record; one holding more means the container configuration is wrong, and is reported as an error rather than truncated.

## What it does

The capture is read as fixed-stride line records — the container descriptor gives the stride, the padding, which records carry data, and where the frame trailer is — and every record keeps its `(frame, field, record)` position exactly as stored. Records are never dropped or reordered, so a packet recovered downstream resolves back to the source bytes it came from as a pure index relation.

Each record is then:

1. **Level-mapped.** A card capture's levels are relative and move with the card's gain control, so logic 0 is read from the quiet region ahead of the clock run-in and logic 1 from the larger of the run-in's peaks and the framing code's leading run of ones. Taking the larger is what stops a band-limited source (a tape, where the run-in has been very nearly filtered away) from being scaled to a fraction of the correct amplitude. The mapping is linear and nothing else is done to the samples: a deconvolving slicer downstream recovers data by matching the blurred waveform it is given, so any sharpening or slicing here would destroy what it depends on.
2. **Resampled** onto the 4×fsc output lattice with a band-limited filter — not by dropping samples, which would fold the 6,9375 MHz teletext carrier back onto itself.
3. **Placed** at the data service's nominal time from 0H, corrected by the calibrated capture offset and by the frame line's own sub-sample lattice phase. All three fold into the resampler's filter phase as one pass, so nothing is interpolated twice and no error accumulates.

Everything around the data is manufactured to the standard: line sync with shaped edges, the front and back porches, the equalising and broad pulses of the vertical interval with the correct half-line pattern for each field, and — unless the burst is switched off — a colour burst following the PAL four-frame swinging-burst progression, coherent across frame boundaries. Every sample is clamped into the legal 10-bit range as the last step.

A PAL frame is asserted to be exactly 709,379 samples before it is handed on. That is a runtime check, not merely a test: PAL at 4×fsc is not orthogonal (1135,0064 samples per line), and a frame assembled at a constant 1135 is four samples short and displaces every frame after it.

### Capture offset calibration

The single most important value in the stage is the time from 0H to sample 0 of each stored record, and no capture format records it. For the bt8x8 family the documented figure is explicitly unreliable — the driver's own comment calls the datasheet value wrong and says the real one differs between chip revisions — so with **Capture Offset** at `auto` the stage measures it instead: records sampled from across the whole capture (not from its opening minutes, which may be a tape settling or a mistracked lead-in) are correlated against a generated clock-run-in and framing-code template, accepted peaks are refined to a fraction of a sample, and the median becomes a single global offset. It is applied globally and never per line: a per-line correction would erase real timing information and shift lines that were already right.

The fit is reported with its spread, its acceptance fraction and any drift. A monotonic drift across the capture is diagnostic of a wrong sampling rate specifically, and the slope gives the corrected rate directly, so it is reported rather than absorbed. If the fit fails its health checks the run stops with a diagnostic: a wrong global offset silently mis-places every line of a capture that may run for hours.

Independent cross-checks corroborate the fitted timing where the capture allows — the burst remnant a bt8x8 record catches at its start, other VBI services in the captured line range, and where modulation ends against what the configured bit rate predicts. These only ever warn, naming both figures when they disagree: the teletext lock is the primary measurement and these are checks on it.

Captures already derived from a time-base corrector have sample 0 of every record at 0H by construction. Their offset is exactly zero, is never calibrated, and a non-zero configured value is rejected.

### Dropped frames and signal state

The last four bytes of every bt8x8 frame are the driver's own frame sequence number. Because that counter advances once per captured frame whether or not the frame reached the file, comparing it at the ends of the capture says exactly how many frames were dropped across the whole of it — without reading the capture through. Frames are synthesised one at a time as they are asked for, so this is the difference between opening a four-hour capture instantly and decoding 10 GB before the first frame appears.

- `preserve` emits only the frames present. Output frame numbering no longer matches the capture's own, so frame-boundary integrity is lost.
- `pad` synthesises blank frames in the gaps, so output frame *n* stays aligned with source frame *n* and the PAL colour sequence stays coherent across the gap. Padded frames carry no source data and are flagged as such.

The output's signal state follows from what the run found, and is never a user setting. `STANDARD_TBC_LOCKED` is claimed only when a coherent burst was synthesised **and** the timeline survived; a run with the burst switched off, or one whose counter gaps were not padded, reports `STANDARD_TBC_UNLOCKED`. A format with no frame counter cannot report drops at all, and the stage says so in as many words rather than implying continuity.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured: the capture is accessible and the container configuration is internally consistent. The capture itself is checked against that configuration when the stage runs. |
| Red | Not configured, or unusable: no path is set, the path does not point to an accessible file, the format preset or data service is not one the stage implements, or the container fields contradict each other. |

Parameters can be set via **Edit Parameters...** in the node context menu.
