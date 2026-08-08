# NABTS Sink

Recovers North American Basic Teletext (NABTS) data lines from the VBI of a 525-line source and writes the recovered packets as a flat stream.

NABTS is **ITU-R BT.653 System C**, specified by [CEA-516](https://www.cta.tech/) (formerly EIA-516). It is the service the US networks carried in the 1980s — CBS ExtraVision and NBC Teletext among them — at 5.727272 Mbit/s in 33-byte data packets, written here as `.t33`.

It is **not** World System Teletext. On a 525-line capture the two services share the clock run-in, the bit rate, the VBI lines and the data levels, and are told apart by the framing code alone: `0xE7` for NABTS against `0xE4` for WST. If a recording carries WST rather than NABTS — the TBS Electra service and its contemporaries did — use the **Teletext Sink** instead, which writes 34-byte `.t34` packets. A stage pointed at the wrong service recovers nothing rather than recovering nonsense.

## When to use

Add this sink when an NTSC or PAL-M source carries NABTS you want to preserve. The recovered packet stream is the product: it holds the transmitted packets exactly as broadcast, which is what a data-group and record decoder needs.

Expected recovery quality depends on the source's luma bandwidth at the data rate:

* **Broadcast-quality captures and direct CVBS** carry the full spectrum; recovery is expected to perform like a hardware decoder.
* **Consumer VHS** truncates the upper half of it, causing heavy intersymbol interference. Threshold slicing cannot recover anything at all from such a recording — the clock run-in is the first thing the tape loses — so the stage carries a second bit detector for this case; see `detector` below.

## What it does

Trigger the node and it makes one linear pass over the whole frame range. For each frame it probes the candidate VBI lines of both fields for a data line and extracts the 33 packet bytes. Recovered packets are written to the output file in strictly temporal order — frame, then field 1 and field 2, then ascending line — exactly as a receiver would have seen them broadcast, which is the order a data group has to be reassembled in.

The output is a flat, headerless sequence of whole 33-byte packets in **transmission coding**: the Hamming 8/4 protection on the five prefix bytes and the odd parity on the data block are preserved, so a consumer decodes the stream exactly as it would a live transmission. Nothing is corrected.

The pass also learns about the recording as it goes, and spends less on later frames than on the first ones: where in the line this recording's data bursts start, and which of the candidate lines carry them at all (`pin_data_phase` and `learn_active_lines` below).

Recovering a line reads that line's samples and nothing else, so frames are decoded on several threads at once while the packets are written from one, in the order they were transmitted (`decode_threads`). The recovered stream does not depend on how many threads were used.

## Parameters

### output_path (file path)
Path to the output packet stream. The `.t33` extension is appended if absent.

Optional. Leave it empty and the run decodes exactly as it would but writes no file, which is what to do when the recovery report is what you are after. `write_report` writes beside the packet stream, so it needs a path and the run is refused if it is enabled without one.

### first_vbi_line (integer)
First candidate field line probed, 1-based, applied to both fields. Default 10.

### last_vbi_line (integer)
Last candidate field line probed, 1-based, applied to both fields. Default 21.

Together these give the window CEA-516 §1.1.1 and ITU-R BT.653 §2 define: broadcast lines 10 to 21 of field 1 and 273 to 284 of field 2. CEA-516 §1.2 also permits full-field transmission on lines 10 to 262, which this stage does not cover.

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

## Notes

* NTSC and PAL-M sources are accepted. A 625-line source is reported as an error: CEA-516 §1.1.1 specifies NABTS on the 525-line NTSC signal, and no 625-line service exists to recover.
* Empty VBI lines are cheap to probe under every detector: a line that never rises meaningfully above blanking is rejected before any detection runs.
* Nothing is corrected in the packet stream. The prefix bytes are written as recovered and left to their own Hamming coding, and the data block to its own byte parity and longitudinal check byte.
* Byte parity is **not** used to repair damaged bytes, as it is for World System Teletext. CEA-516 §3.3 gives the data block odd parity only when its data group is of type 0, and a single packet does not say which type its group is — so a byte that fails parity is not known to be damaged.
* Every run logs a recovery profile at debug level, as part of the report described under `write_report`. It covers how many candidate lines carried a data burst, how many packets came out of each detector, and which gate discarded the rest.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — here, no output file is set, so the run reports how the recovery went and writes no packet stream. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu.
