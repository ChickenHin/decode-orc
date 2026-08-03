# Teletext Sink

Extracts World System Teletext (WST) data lines from the VBI of PAL video and writes the recovered 42-byte packets as a `.t42` packet stream. **PAL WST only** (ETSI EN 300 706 System B, 625-line): French System A, North American NABTS (System C) and Japanese System D transmissions are not supported, and NTSC line-21 captions are covered by the Closed Caption Sink instead.

## When to use

Add this sink when a PAL LaserDisc, CVBS capture, or tape source carries teletext you want to preserve or inspect. The `.t42` output can be browsed and decoded with external tools such as vhs-teletext (`teletext filter` / `teletext interactive`) and wxTED — see the [T42 packet stream description](https://teletext.wiki.zxnet.co.uk/wiki/T42_packet_stream) on the zxnet teletext wiki.

Expected recovery quality depends on the source's luma bandwidth at the 6.9375 Mbit/s teletext bit rate:

* **LaserDisc and broadcast-quality CVBS captures** carry the full teletext spectrum; recovery is expected to perform like a hardware decoder.
* **Consumer VHS** truncates the upper half of the teletext spectrum, causing heavy intersymbol interference. Threshold slicing cannot recover anything at all from such a recording — the clock run-in is the first thing the tape loses — so the stage carries a second bit detector for this case; see `detector` below. With it, PAL SP and LP recordings measured here yield readable pages. Stacking multiple captures and dropout correction upstream both improve the odds further.

## What it does

For each frame in the input range, the stage probes the candidate VBI lines of both fields for a teletext data line and extracts the 42 payload bytes (magazine/row address plus 40 data bytes). Recovered packets are written to the output file in strictly temporal order — frame, then field 1 and field 2, then ascending line — exactly as a receiver would see them broadcast.

The output is a flat, headerless sequence of 42-byte packets in **transmission coding**: Hamming 8/4 on addressing bytes and odd parity on display bytes are preserved, so consumers decode the stream exactly as they would a live transmission. What the coding does not promise is that every byte is the one this copy of the line carried. By default the stage mends display bytes that fail their parity check and combines the repeated transmissions of each page row (`repair_damaged_bytes` and `squash_repeated_rows` below), which is most of what makes a tape readable; turn both off and the packets are written exactly as recovered.

Optionally, the stage can additionally decode the subtitle page (pages flagged C6 in the header control bits, conventionally page 888 in the UK) and write the recovered subtitles as a SubRip (`.srt`) file next to the packet stream. Cue timing derives from the field number at 50 fields/s; colour, positioning, and other Level 1 presentation attributes are dropped — SRT carries the plain text.

## Parameters

### output_path (file path)
Path to the output T42 packet stream. Required. A `.t42` extension is appended if absent.

### first_vbi_line (integer)
First candidate field line probed for teletext, 1-based and applied to both fields. Default: `6`. The default window 6–22 covers broadcast lines 6–22 (field 1) and 318–335 (field 2) permitted to carry teletext by ETSI EN 300 706.

### last_vbi_line (integer)
Last candidate field line probed for teletext, 1-based and applied to both fields. Default: `22`.

### keep_empty_packets (boolean)
When enabled, every candidate line with no recoverable teletext emits 42 zero bytes instead of being skipped, so each packet position in the file maps 1:1 back to a specific (frame, field, line) — the vhs-decode convention, useful for packet-for-packet comparison against other decoders. Default: `false`.

### detector (string)
How data bits are recovered from each line. Default: `Automatic`.

* `Threshold` — locks to the clock run-in and slices at bit centres. Exact on a source that passes the whole data band, which is what a disc or a direct capture gives you, and the cheapest option.
* `MLSE` — fits the recording's frequency response to the 24 known bits every teletext line starts with, then picks the payload bit sequence that response most likely produced. This is what recovers teletext from tape, where the limited bandwidth smears each bit into its neighbours. Nothing is trained or configured beforehand: each line carries the reference used to fit its own channel. It then refits that response against the whole packet it has just read and reads the packet again, which is where most of its accuracy on a tape comes from: 24 known bits pin a frequency response far less well than 360 do. Costs roughly ten times the work per line and, because it fits rather than matching an exact framing code, it locks onto a noise-only line a few times in a hundred (5 of 64 synthesized noise lines here, and none at all on the tens of thousands of real non-teletext data lines measured) — `require_valid_mrag` and an internal parity plausibility check are what hold that down.
* `Automatic` — try `Threshold` first and fall back to `MLSE` only on lines it could not lock. A disc source therefore behaves and costs exactly as it did before, and a tape source gets the fallback without being told to. This also matches the configuration the preview dialog's page decoding uses.

### tolerant_framing (boolean)
Accept framing codes with one bit error. Recovers more packets from noisy sources at the cost of a higher false-positive rate. Default: `false`.

### require_valid_mrag (boolean)
Drop packets whose magazine/row address bytes fail Hamming 8/4 correction. This suppresses false framing-code locks on noise while still passing single-bit-damaged packets through to downstream tools. Default: `true`.

### repair_damaged_bytes (boolean)
Restore odd parity on damaged display bytes by flipping the bit the MLSE detector was least sure of. Default: `true`.

Every display byte carries a parity bit (ETSI EN 300 706 §8.1), so a byte that fails its parity check is *known* to be damaged — but parity says only that, not which of the eight bits is wrong. The MLSE detector does know: it chose the packet's bits by finding the most likely sequence, and it can say, for each bit, how much more likely that choice was than the opposite one. Flipping the bit it came closest to reading the other way is the best available repair of a single-bit error, and it restores parity, so the emitted packet is still valid transmission coding.

What it costs is the distinction between a byte that arrived intact and a byte that has been guessed. Downstream — the page view's damaged-byte readout, and the parity-first rule when repeated copies of a row are combined — a wrong repair looks exactly like clean data. On a recording where most bytes come back damaged that trade is worth making; on one where few do, it is not. A repaired byte does carry the low confidence of the bit that was flipped, so combining repeated rows still prefers a copy that arrived intact.

Applies only to the 40 data bytes of parity-coded rows (0–25), and only under the MLSE detector — a disc or a direct capture, which the threshold detector reads exactly, is untouched whatever this is set to. The magazine/row address, and the header's page number and control bytes, are Hamming 8/4 coded and carry their own correction, which page decoding already applies; rows above 25 are not byte-wise parity coded at all.

The default matches the host observer, which also repairs, and that is what lets a default run read the observations it has already cached. Turning it off makes the stage slice the lines itself, because those cached packets are repaired.

Measured on the reference VHS captures, packets whose 40 data bytes all satisfy parity rise from 70.4 % to 87.9 % (LP) and from 78.7 % to 91.6 % (SP) — though on a real recording that figure is partly manufactured by the repair itself. Against synthesized lines with known payloads, where the answer can be checked, 714 repairs across 2438 recovered packets corrected 598 bytes and damaged none.

### squash_repeated_rows (boolean)
Combine repeated transmissions of each page row and write the combined form. Default: `true`.

Teletext is a carousel, so any recording longer than one cycle holds several copies of every row, damaged in different places. Comparing them byte by byte recovers a row cleaner than any single copy of it. The vote goes first to values that pass their parity check — a byte known to be corrupt never wins over one that is not, however often it was seen — and then by how sure the detector was of each byte, so a copy read cleanly outweighs the same number of copies of one it nearly misread. Packet order, count and timing are unchanged; only damaged display bytes move. Page headers are left alone: their display bytes carry a clock that legitimately differs between transmissions.

Copies are only combined within one *run* of a page. A header with the erase bit set (C4, ETSI EN 300 706 §9.3.1.3 Table 2) says the page's content is being replaced, so what follows it is a different page that happens to share a number, and combining across it would blend the two. A service that sets C4 on every transmission therefore gives every transmission a run of its own and nothing can be combined — the report below says so directly, as a run count equal to the transmission count and a copies-per-row distribution that is entirely single-copy.

Costs a second pass over the recovered packets, which are held in memory (roughly 50 bytes each).

### write_report (boolean)
Write the run's diagnostic report next to the packet stream, named after it with a `.txt` extension — `mydata.t42` gives `mydata.t42.txt`. Default: `false`.

The same report is always written to the log at debug level; this only keeps a copy somewhere a reader can go back to.

It opens with the result in one line — how much of what came out is damaged — and the same figure appears in the stage's status when the run finishes:

```
Teletext export report
  Data loss 1.14% — 30 of 2,640 recovered characters are damaged
  Combining repeated rows mended 470 of the 500 characters that arrived
  damaged (94.0%); without it the loss would be 18.94%
```

Damage is counted by the odd parity the standard already puts on every display byte (§8.1), over the display rows of the stream as written. Read it as *of the characters this export produced, this share are known wrong*. Two caveats, both in the conservative direction: it is a floor, because a byte damaged in two bits passes parity and is counted as good; and it says nothing about rows that never arrived, which are absent from both sides of the ratio.

Below the headline the report gives what was exported (output path, frames, VBI window, detector, packet and field counts), how recovery went (the profile described under Notes below), and the detail behind the headline — the share of rows the vote changed, and how many copies each row was combined from:

```
Teletext squashing: 264 row packets over 1 page run; 246 rewritten (93.2%),
  660 of 10,560 display bytes replaced (6.25%)
  Odd-parity failures: 647 before (6.13%), 0 after (0.00%)
  Copies per row packet: 8+ copies 264 (100.0%)
```

The copies-per-row line is what separates a run that could not correct anything from one that had nothing to correct: a row transmitted once cannot be improved however good the vote is.

### export_subtitles (boolean)
Decode the subtitle page alongside the T42 export and write timed subtitle cues to a `.srt` file next to the output (same name, `.srt` extension). A subtitle is displayed when the page arrives, replaced when its text changes, and cleared when the page is erased or loses its subtitle flag. Default: `false`.

### subtitle_page (string)
The teletext page carrying the subtitles: a magazine digit (1–8) followed by two hexadecimal page digits, e.g. `888` (the UK convention). Only used when `export_subtitles` is enabled. Default: `888`.

### subtitle_format (string)
Subtitle output format. Currently only `SRT` (SubRip) is offered — the least lossy portable target for teletext subtitle text; colour and positioning are dropped at this level. Default: `SRT`.

## Notes

* PAL sources only; the stage reports an error for any other video system.
* Empty VBI lines are cheap to probe under every detector: a line that never rises meaningfully above black is rejected before any detection runs.
* Beyond the two options above, nothing is corrected: the addressing bytes are written as recovered and left to their own Hamming coding, and page decoding, further correction, and subtitle extraction are the consumer's job (vhs-teletext, wxTED, and similar tools).
* Combining repeated rows ("squashing") is an idea taken from [vhs-teletext](https://github.com/ali1234/vhs-teletext) by Alistair Buxton, with thanks. A row transmitted only once cannot be corrected, so the benefit grows with how long the recording runs and how often each page comes round.
* Every run logs a recovery profile at debug level, as part of the report described under `write_report`. How much it can say depends on where the packets came from. When the stage does its own slicing — that is, whenever `detector` is not `Automatic`, or `tolerant_framing` / `require_valid_mrag` / `repair_damaged_bytes` differ from their defaults — the profile covers how many candidate lines carried a data burst, how many packets came out of each detector, which gate discarded the rest, how the odd-parity failures of the recovered packets are spread across the 40 data-byte positions, and, for the MLSE detector, how its reconstruction error is spread along the packet, how sure it was of the bytes it emitted, and how many of them parity repair mended. That last profile is the timing reading: an error that grows from the first byte to the last means the bit clock is running at the wrong rate across the packet, while a level profile means noise and intersymbol interference. Under `Automatic` with default gating the run reads the host's cached teletext observations instead of slicing, so the burst, detector and rejection figures belong to a slice that happened elsewhere and are omitted rather than reported as zero; the parity profile, the per-line yield and the confidence mean are still built from the stored packets, and the observer that produced them logs its own full per-field profile at debug level. This is a diagnostic for judging a difficult tape; the `.t42` output is unaffected.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
