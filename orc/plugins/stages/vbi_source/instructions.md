# VBI Capture Source

Reads a raw VBI capture — a file holding nothing but the vertical-blanking line records a capture card or a cropped `.tbc` wrote — and lays those lines onto CVBS frames at the timing point and the amplitude the standard puts them at. The result is an ordinary CVBS_U10_4FSC VideoFrameRepresentation, so the teletext decoders see the data exactly as they see it on a native decode.

The rest of each frame is blanking. A VBI capture carries no sync, no vertical interval and no colour burst, and nothing that reads this stage's output looks for them: the teletext slicer locks to the clock run-in inside the line it is handed. Manufacturing a whole television signal around the data would cost far more than placing the data does, and would be spent entirely on samples nobody reads.

## When to use

Add VBI Capture Source as the first stage of a pipeline when your material is a third-party VBI dump rather than a decoded capture: a capture-card dump (`.vbi`, often FLAC-compressed as `.vbi.flac`), or the VBI lines cropped off a decoded `.tbc`. If you have a whole `.tbc` or a `.composite` file, use the TBC Source or CVBS Source stage instead — those carry a picture.

Nothing is written to disk by this stage. To export the frames as a `.composite` + `.meta` pair, connect a CVBS Sink. Bear in mind that what is exported is a legal CVBS file carrying teletext on an otherwise blank raster, not a reconstruction of the broadcast the capture was cut from.

## Parameters

There are three. Everything else about a capture — its geometry, its sampling rate, the data service it carries, where 0H is, what its logic levels mean, which field it starts on — is a property of the format rather than something you could be expected to know, so it all follows from the **Capture Format** you pick.

| Parameter | Meaning |
|-----------|---------|
| VBI Capture Path (`input_path`) | Path to the capture. FLAC-wrapped files are unwrapped transparently; the wrapper's declared sample rate is a conventional placeholder and is never used for timing. |
| Capture Format (`format`) | What the capture is. Only the formats belonging to the project's television system are offered, so a PAL project has one choice and an NTSC project two. |
| Dropped Frames (`drops`) | `preserve` (default) emits only the frames present; `pad` emits a blank frame in each gap so output frame *n* stays aligned with source frame *n*. A format carrying no frame counter cannot report drops at all, so neither policy has anything to act on. |

## The capture formats

### `bt8x8 card dump, 8-bit (WST)` — PAL projects

A 625-line capture-card dump. 2048 samples per record of which 2044 are real, unsigned 8-bit, at 8×fsc (35 468 950 Hz); 16 records per field carrying field lines 7–22, which is the whole of the WST line list.

The card's own time from 0H is documented but unreliable — the driver's source calls its own datasheet figure wrong — so it is measured from the clock run-in when the capture is opened, and the run stops with a diagnostic if that measurement cannot be trusted. Logic levels are estimated per line, because a card's levels move with its gain control. The last four bytes of every frame are the driver's frame sequence number, which is what makes dropped frames detectable at all.

### `.tbc VBI crop, 16-bit (WST)` and `(NABTS)` — NTSC projects

The first 16 line records of each field of a decoded 525-line luma `.tbc`, which is the shape of every circulating NTSC teletext capture. 910 samples per record with no padding, unsigned 16-bit, at 4×fsc (14 318 182 Hz).

Records 1–12 carry field lines 10–21, the twelve lines the 525-line standard defines. Record 0 is the last post-equalising line and records 13–15 are the start of the picture; none of them carries data. That numbering was measured on the captures rather than assumed: record 0 carries a 2,3 µs equalising pulse where every other record carries a 4,7 µs line sync, and on a captioned recording record 12 carries the line 21 caption run-in.

Sample 0 of every record is already 0H and the levels are already the decoder's own fixed domain, so neither is measured — the record is copied onto the output line index for index and its samples are scaled by the one fixed factor between the two domains.

**Which of the two do I pick?** The one matching the service the broadcast carried. Nothing in the file records it: the two share the same lines and the same 5,7273 Mbit/s bit rate and differ only in framing code (`0xE4` against `0xE7`) and packet length (34 bytes against 33). Of the material in circulation, TBS Electra is **WST** and CBS ExtraVision and NBC Teletext are **NABTS** — US network teletext was NABTS almost throughout, and Electra is the exception. The filename is usually the best evidence you have.

Picking the wrong one is not a disaster here. Because the record is copied index for index, the data is not moved and not rescaled; all that changes is how much of the line is copied, by about fourteen samples at the front and six at the back. The choice is carried through to the stage's output either way, so a downstream decoder is told which service it has been handed rather than left to assume — and whether it can decode what was placed is the slicer's business, not this stage's.

### Captures ending on an odd field

These crops end on an odd field about as often as not. That is not a misconfiguration: the trailing field is one short of a frame, so it is not emitted, and the stage says so in the log rather than passing over it in silence.

### Adding a format

There is deliberately no "custom" entry. Every fact in the table above had to be measured off real captures — none of it is recoverable from the file, and guessing at it produces output that looks right and is not. A format that *has* been measured is a single data entry in the stage's preset table.

## What it does

The capture is read as fixed-stride line records — the container descriptor gives the stride, the padding, which records carry data, and where the frame trailer is — and every record keeps its `(frame, field, record)` position exactly as stored. Records are never dropped or reordered, so a packet recovered downstream resolves back to the source bytes it came from as a pure index relation.

Each record is then:

1. **Level-mapped.** A card capture's levels are relative and move with the card's gain control, so logic 0 is read from the quiet region ahead of the clock run-in and logic 1 from the larger of the run-in's peaks and the framing code's leading run of ones. Taking the larger is what stops a band-limited source (a tape, where the run-in has been very nearly filtered away) from being scaled to a fraction of the correct amplitude. A capture cropped from a decoded `.tbc` is different: the decoder has already normalised blanking and white to fixed values, so its levels are absolute and are mapped straight onto the output's rather than estimated again. Estimating them would also be wrong on such a capture, whose records start at 0H and so open with the line's sync pulse and colour burst, neither of which is the blanking a quiet-region estimate assumes it is reading. Taking the larger is what stops a band-limited source (a tape, where the run-in has been very nearly filtered away) from being scaled to a fraction of the correct amplitude. The mapping is linear and nothing else is done to the samples: a deconvolving slicer downstream recovers data by matching the blurred waveform it is given, so any sharpening or slicing here would destroy what it depends on.
2. **Resampled** onto the 4×fsc output lattice with a band-limited filter — not by dropping samples, which would fold the 6,9375 MHz teletext carrier back onto itself. A capture already sampled at 4×fsc with its records starting at 0H is on the output's own lattice, so it is copied rather than filtered: there is nothing to interpolate and nothing that could alias.
3. **Placed** at the data service's nominal time from 0H, corrected by the calibrated capture offset. Both fold into the resampler's filter phase as one pass, so nothing is interpolated twice and no error accumulates.

Only the data region of the line is written — the region the standard gives the packet, plus one bit period of guard at each end so the leading edge of the first run-in bit and the trailing edge of the last payload bit survive. A record covers more of the line than that at both ends (a bt8x8 record opens inside the colour burst window and runs on past the end of the packet), and the rest of it is discarded rather than laid over the blanking. Every sample is clamped into the legal 10-bit range as the last step.

Because the placement is the same on every data line of every frame, all of it — the filter taps included — is resolved once when the capture is opened. Building a frame is then a fill and one filtering pass per data line: thirty-two lines of the six hundred and twenty-five on PAL, twenty-four of the five hundred and twenty-five on NTSC, and nothing at all written to the rest.

Frames follow the host's flat-frame convention: 1135 samples per PAL line, with the two extra samples of each field taken up on its last line, and 910 on every NTSC line, whose lattice is orthogonal and needs no such correction. Every teletext line therefore starts within a sixth of a sample of its true 0H, which is also how a real time-base corrected capture is stored — so a line read out of this stage sits where a line read out of any other source does.

### Capture offset calibration

The single most important value in the stage is the time from 0H to sample 0 of each stored record, and no capture format records it. For the card-capture families the documented figure is explicitly unreliable — the driver's own comment calls the datasheet value wrong and says the real one differs between chip revisions — so for those formats the stage measures it instead: records sampled from across the whole capture (not from its opening minutes, which may be a tape settling or a mistracked lead-in) are correlated against a generated clock-run-in and framing-code template, accepted peaks are refined to a fraction of a sample, and the median becomes a single global offset. It is applied globally and never per line: a per-line correction would erase real timing information and shift lines that were already right.

The fit is reported with its spread, its acceptance fraction and any drift. A monotonic drift across the capture is diagnostic of a wrong sampling rate specifically, and the slope gives the corrected rate directly, so it is reported rather than absorbed. If the fit fails its health checks the run stops with a diagnostic: a wrong global offset silently mis-places every line of a capture that may run for hours.

Captures cropped from a time-base corrected decode have sample 0 of every record at 0H by construction. Their offset is exactly zero and is never fitted — there is nothing to measure, and the measurement would be taken against a record whose head is the line's sync pulse rather than the back porch a card capture opens in.

### Dropped frames

The last four bytes of every bt8x8 frame are the driver's own frame sequence number, and a container can declare such a trailer whatever wrote it. Because that counter advances once per captured frame whether or not the frame reached the file, comparing it at the ends of the capture says exactly how many frames were dropped across the whole of it — without reading the capture through. Frames are built one at a time as they are asked for, so this is the difference between opening a four-hour capture instantly and decoding 10 GB before the first frame appears.

- `preserve` emits only the frames present. Output frame numbering no longer matches the capture's own, so frame-boundary integrity is lost.
- `pad` emits a blank frame in each gap, so output frame *n* stays aligned with source frame *n*. Padded frames carry no source data and are flagged as such.

A format with no frame counter cannot report drops at all, and the stage says so in as many words rather than implying continuity.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured: the capture is accessible and a format is chosen. The capture itself is checked against that format — its length, its word size — when the stage runs. |
| Red | Not configured, or unusable: no path is set, or the path does not point to an accessible file. |

Parameters can be set via **Edit Parameters...** in the node context menu.
