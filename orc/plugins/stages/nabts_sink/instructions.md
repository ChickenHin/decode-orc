# NABTS Sink

Recovers North American Basic Teletext (NABTS) data lines from the VBI of a 525-line source, writes the recovered packets as a flat stream, and presents the records it found — decoded as pages — in a record viewer.

NABTS is **ITU-R BT.653 System C**, specified by [CEA-516](https://www.cta.tech/) (formerly EIA-516). It is the service the US networks carried in the 1980s — CBS ExtraVision and NBC Teletext among them — at 5.727272 Mbit/s in 33-byte data packets, written here as `.t33`. Its presentation layer is **NAPLPS** (ANSI X3.110-1983, also CSA T500-1983 and ITU-T T.101 Data Syntax III): a page is not a grid of characters but a drawing program, so a record can contain lines, arcs, polygons and redefinable characters as well as text.

It is **not** World System Teletext. On a 525-line capture the two services share the clock run-in, the bit rate, the VBI lines and the data levels, and are told apart by the framing code alone: `0xE7` for NABTS against `0xE4` for WST. If a recording carries WST rather than NABTS — the TBS Electra service and its contemporaries did — use the **Teletext Sink** instead, which writes 34-byte `.t34` packets. A stage pointed at the wrong service recovers nothing rather than recovering nonsense.

## When to use

Add this sink when an NTSC or PAL-M source carries NABTS you want to preserve or inspect. The recovered packet stream is the product: it holds the transmitted packets exactly as broadcast, which is what any other data-group and record decoder needs. The record catalogue rides along with it, which is why this is a sink rather than an analysis sink; leaving `output_path` empty makes the catalogue the only thing the run produces, and that is supported.

Expected recovery quality depends on the source's luma bandwidth at the data rate:

* **Broadcast-quality captures and direct CVBS** carry the full spectrum; recovery is expected to perform like a hardware decoder.
* **Consumer VHS** truncates the upper half of it, causing heavy intersymbol interference. Threshold slicing cannot recover anything at all from such a recording — the clock run-in is the first thing the tape loses — so the stage carries a second bit detector for this case; see `detector` below.

How well a *tape* does depends sharply on its recording speed, and NABTS is far less forgiving of a marginal recording than World System Teletext is. See **What a marginal tape looks like** below before concluding that a disappointing result is a fault.

## What it does

Trigger the node and it makes one linear pass over the whole frame range, carrying the recovery up through all four layers CEA-516 defines.

**Packets (§3).** For each frame it probes the candidate VBI lines of both fields for a data line and extracts the 33 packet bytes. Recovered packets are written to the output file in strictly temporal order — frame, then field 1 and field 2, then ascending line — exactly as a receiver would have seen them broadcast, which is the order a data group has to be reassembled in.

The output is a flat, headerless sequence of whole 33-byte packets in **transmission coding**: the Hamming 8/4 protection on the five prefix bytes and the odd parity on the data block are preserved, so a consumer decodes the stream exactly as it would a live transmission. Nothing is corrected in the file.

**Data groups (§4).** Packets are reassembled into data groups per data channel, since §3.2.3 makes the packet address the channel number and a service may interleave several. A group opens on a synchronizing packet and runs until its length is satisfied, with the continuity index catching a packet that went missing in between.

**Records (§5, §7).** A group of type zero carries a teletext record: a Hamming 8/4 header giving the record type, its address and its classification flags, then the record's data. A record can be transmitted as a linked series (§5.2.6), and the series is joined back into one message in link order — which may arrive out of order, because a recording that starts part way through a carousel sees the middle of a series before its beginning. §5.2.1 makes channel, record address and version the identity of a record, and that is what the catalogue is keyed on.

**Presentation (§6.1).** A record of type 0, 1 or 3 carries NAPLPS, which is interpreted into a display list of primitives — text, lines, arcs, rectangles, polygons, mosaics and redefinable characters, with their colours and their positions in the unit screen. A record of type 2 is an application record instead, and its data is decoded as the function descriptors of §7.2.2 rather than drawn.

The pass also learns about the recording as it goes, and spends less on later frames than on the first ones: where in the line this recording's data bursts start, and which of the candidate lines carry them at all (`pin_data_phase` and `learn_active_lines` below).

Recovering a line reads that line's samples and nothing else, so frames are decoded on several threads at once while the packets are written from one, in the order they were transmitted (`decode_threads`). The recovered stream does not depend on how many threads were used.

## Tools

### NABTS Records

The record viewer for this node. Triggering the node opens it automatically, which is how the records are reached — leave `output_path` empty and triggering is a decode-and-browse that writes no file.

The viewer lists every record the range carried: its channel and record address, its version, the record type, how often it was seen and over which frames, and the classification flags the service set on it. Because the catalogue comes from a pass over the whole source rather than a window around the preview position, the list is the service's full carousel rather than whatever happened to be on screen.

Selecting a record shows what it carries:

* A **presentation record** is drawn as its NAPLPS display list, beside the plain text of the page. Pages are drawn in a unit square whose lower 0.78125 is the display area every receiver is guaranteed to show (ANSI X3.110 Table D1); **Show display area** outlines it, which is how to tell a record drawn deliberately into one corner from one that was mis-scaled. The text pane is there because an index page is mostly words and picking them off a rasterised page is tedious — it is the same characters the drawing used, in reading order.
* An **application record** is shown as its function descriptors, each with its code in the code-table notation §7.2.2 uses and its arguments.

**Caption track** switches the right-hand pane to the recording's captioning: the records the service marked with the caption flag of §5.2.7.3, in transmission order, each cue running until the next replaces it. §7.3.10 carries captioning as a run of records that each replace the last, so the cues — not the individual records — are what the service actually says. The control is only enabled on a recording that carried captioning, and the line above the list says which records those were.

## Parameters

### output_path (file path)
Path to the output packet stream. The `.t33` extension is appended if absent.

Optional. Leave it empty and the run decodes exactly as it would but writes no file, which is what to do when the records themselves are what you are after — the **NABTS Records** tool is filled either way. `write_report`, `export_records` and `export_captions` all write beside the packet stream, so they need a path and the run is refused if any of them is enabled without one.

### first_vbi_line (integer)
First candidate field line probed, 1-based, applied to both fields. Default 10.

### last_vbi_line (integer)
Last candidate field line probed, 1-based, applied to both fields. Default 21.

Together these give the window CEA-516 §1.1.1 and ITU-R BT.653 §2 define: broadcast lines 10 to 21 of field 1 and 273 to 284 of field 2. CEA-516 §1.2 also permits full-field transmission on lines 10 to 262, which this stage does not cover.

Narrowing the window to the lines a service actually uses is worth doing when you know them. It keeps noise that happened to pass a framing code out of the packet stream, and — because the data-phase tracker pools its locks over every line of the window — it can be the difference between the phase pin engaging and not. On the CBS ExtraVision reference capture, reading the full window yields 2460 packets against 2288 on the four lines the service uses; the 172 extra are noise, every one of them refused by the group header check. The records a user browses come out the same either way.

### keep_empty_packets (boolean)
Emit a whole zero packet for every candidate line that yielded nothing, so packet position in the file maps 1:1 to (frame, field, line). Off by default, which writes only the packets that were recovered.

### detector (string)
How data bits are recovered from each line.

* **Threshold** slices at interpolated bit centres after locking to the clock run-in. Cheapest and exact on a clean signal; suits discs and direct captures.
* **MLSE** fits the recording's frequency response to the known 24-bit start of each line and finds the bit sequence that response would most likely have produced. This is what recovers data from tape, where the limited bandwidth smears each bit into its neighbours and no bit-centre threshold can work. Roughly an order of magnitude more work per line.
* **Automatic** (default) tries Threshold first and falls back to MLSE only on lines it could not lock, so a clean source pays nothing extra.

### tolerant_framing (boolean)
Accept a framing code with one bit error. Off by default, and worth leaving off: the framing code is the only thing that separates NABTS from the 525-line World System Teletext service, so tolerating an error in it both raises the false-positive rate on noise and weakens that separation.

### require_valid_prefix (boolean)
Drop packets whose five-byte prefix does not survive Hamming 8/4 correction. On by default.

The prefix is the three packet address bytes, the continuity index and the packet structure byte (CEA-516 §3.2.1), all error-protected under the same code. Requiring all five suppresses false locks on noise very effectively — random bytes clear it about once in a million — and a packet whose prefix is unrecoverable cannot be placed in a data group anyway.

### pin_data_phase (boolean)
Narrow the search for where the data burst starts to where this recording's lines have already been seen to start. On by default. A narrowed search that finds nothing is repeated over the full window, so this costs a few percent on empty lines and cannot lose a packet.

### learn_active_lines (boolean)
After the first frames, read only the candidate lines this recording has been seen to carry data on, rechecking the full window periodically. On by default.

### decode_threads (integer)
Threads to recover lines on; 0 (default) uses one per processor. The recovered stream is identical whatever this is set to, so lower it only to leave the machine free for other work.

### write_report (boolean)
Write the run's diagnostic report next to the packet stream, named after it with a `.txt` extension (`mydata.t33` gives `mydata.t33.txt`). Off by default; the same report is always written to the log at debug level. Needs an output file to sit beside.

Beyond the packet-level recovery profile, the report accounts for every layer above it — how many packets were orphaned rather than placed in a group, how many groups completed, how many record headers were refused, how many linked series were joined, and how many records were catalogued. Those counts are the quickest way to tell a recording that is merely lossy from one that is failing (see below).

### export_records (boolean)
Write each teletext record the recording carried as its own file beside the packet stream, named for the channel, record address and version that identify it — `mydata.t33.000-1A4-v2.rec`. Off by default; needs an output file to sit beside.

The file holds the record's data exactly as transmitted, byte for byte: NAPLPS presentation code, or application data for a record of type 2. Byte parity is left in place, as it is in the packet stream. This is what to use to take a record to an external NAPLPS tool.

### export_captions (boolean)
Write the recording's captioning as a SubRip subtitle file beside the packet stream (`mydata.t33` gives `mydata.t33.srt`). Off by default; needs an output file to sit beside.

The cues are the records the service marked with the caption flag of §5.2.7.3, in the order they were transmitted, each running until the next one replaces it; a caption record with no text is an erase and ends the cue before it. Cue timing comes from the 59.94 fields per second of SMPTE 170M. The caption flag is what selects them, not the data channel — §7.3.10 leaves a service free to carry captioning on whichever channel it likes. A recording that carried no captioning writes no file.

Colour, positioning and the rest of the NAPLPS presentation are dropped: SRT carries the plain text.

## What a marginal tape looks like

NABTS asks much more of a recording than World System Teletext does, and it is worth knowing what the failure looks like so a bad tape is not mistaken for a bad decode.

A WST page is a grid of characters, each byte independent and parity-coded, so damage stays where it lands — a corrupted byte is one wrong character. A NABTS page is a **stateful byte stream**: bytes are opcodes, operand counts and coordinates, so one wrong byte changes how the several after it are read. Damage propagates, and a page degrades into overlaid or displaced nonsense rather than into a page with typos.

The two reference recordings measured here bracket the range, decoded end to end at identical settings:

| | CBS ExtraVision (VHS **SP**) | NBC Teletext (VHS **EP**) |
|---|---|---|
| MLSE decision confidence | 0.55 | **0.22** |
| record bytes failing odd parity | 0.02 % | **7.10 %** |
| packets orphaned rather than placed in a group | 0.09 % | **62 %** |
| record headers refused | 1 in 7736 groups | **866 in 2824** |
| linked record series joined | 1246 | **0** |

The SP recording browses as a working service. The EP recording catalogues 239 records and draws 212 of them, and almost all of them are unreadable. The reason is in the first row: a World System Teletext recording read as NABTS — that is, pure noise fitted by the MLSE detector — comes through at a mean decision confidence of 0.21. The EP tape sits at 0.22. Its bit decisions are at the noise floor, and no amount of decoding recovers a page from that.

So: **mean decision confidence in the report is the number to read first.** Around 0.5 and above, expect readable pages. Approaching 0.2, the recording is at the limit of what can be detected at all, and a better transfer — or a copy recorded at a longer-playing speed's expense — is the only thing that will help.

## Notes

* NTSC and PAL-M sources are accepted. A 625-line source is reported as an error: CEA-516 §1.1.1 specifies NABTS on the 525-line NTSC signal, and no 625-line service exists to recover.
* Empty VBI lines are cheap to probe under every detector: a line that never rises meaningfully above blanking is rejected before any detection runs.
* Nothing is corrected in the packet stream. The prefix bytes are written as recovered and left to their own Hamming coding, and the data block to its own byte parity and longitudinal check byte.
* Byte parity is **not** used to repair damaged bytes, as it is for World System Teletext. CEA-516 §3.3 gives the data block odd parity only when its data group is of type 0, and a single packet does not say which type its group is — so at the packet layer a byte that fails parity is not known to be damaged. Where a packet carries the longitudinal check byte of §3.4, that byte and the per-byte parity do form a product code, and a single-bit error in the block is located and corrected. Whether a service sends it is the service's choice: ExtraVision does and 768 blocks were mended on the reference capture, NBC does not and none were.
* Only one copy of each record is kept — the intact copy if one ever arrives, otherwise the longest. A carousel transmits each record many times, and combining those copies the way the Teletext Sink combines repeated rows is not done here.
* Header extension fields (§5.2.8) are decoded and reported but the link redefinitions of §5.2.8.4 are not acted on. The clause's data table is unusable as published — row 12 merges two meanings, and rows 6 and 9 label six- and nine-byte fields as a "Short Record Address" — and no capture available here exercises it.
* Every run logs a recovery profile at debug level, as part of the report described under `write_report`. It covers how many candidate lines carried a data burst, how many packets came out of each detector, and which gate discarded the rest.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — here, no output file is set, so the run fills the **NABTS Records** tool and writes no packet stream. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above).
