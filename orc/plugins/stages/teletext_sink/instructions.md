# Teletext Sink

Extracts World System Teletext (WST) data lines from the VBI of PAL video and writes the recovered 42-byte packets as a `.t42` packet stream. **PAL WST only** (ETSI EN 300 706 System B, 625-line): French System A, North American NABTS (System C) and Japanese System D transmissions are not supported, and NTSC line-21 captions are covered by the Closed Caption Sink instead.

## When to use

Add this sink when a PAL LaserDisc, CVBS capture, or tape source carries teletext you want to preserve or inspect. The `.t42` output can be browsed and decoded with external tools such as vhs-teletext (`teletext filter` / `teletext interactive`) and wxTED — see the [T42 packet stream description](https://teletext.wiki.zxnet.co.uk/wiki/T42_packet_stream) on the zxnet teletext wiki.

Expected recovery quality depends on the source's luma bandwidth at the 6.9375 Mbit/s teletext bit rate:

* **LaserDisc and broadcast-quality CVBS captures** carry the full teletext spectrum; recovery is expected to perform like a hardware decoder.
* **Consumer VHS** truncates the upper half of the teletext spectrum, causing heavy intersymbol interference. Expect usable results from strong recordings (S-VHS, good decks and tapes) and degraded-to-poor results from typical consumer VHS. Stacking multiple captures and dropout correction upstream both improve the odds.

## What it does

For each frame in the input range, the stage probes the candidate VBI lines of both fields for a teletext data line: clock run-in acquisition, framing-code lock, then extraction of the 42 payload bytes (magazine/row address plus 40 data bytes). Recovered packets are written to the output file in strictly temporal order — frame, then field 1 and field 2, then ascending line — exactly as a receiver would see them broadcast.

The output is a flat, headerless sequence of 42-byte packets in **transmission coding**: Hamming 8/4 on addressing bytes and odd parity on display bytes are preserved, with no error correction applied. Consumers decode the stream exactly as they would a live transmission.

## Parameters

### output_path (file path)
Path to the output T42 packet stream. Required. A `.t42` extension is appended if absent.

### first_vbi_line (integer)
First candidate field line probed for teletext, 1-based and applied to both fields. Default: `6`. The default window 6–22 covers broadcast lines 6–22 (field 1) and 318–335 (field 2) permitted to carry teletext by ETSI EN 300 706.

### last_vbi_line (integer)
Last candidate field line probed for teletext, 1-based and applied to both fields. Default: `22`.

### keep_empty_packets (boolean)
When enabled, every candidate line with no recoverable teletext emits 42 zero bytes instead of being skipped, so each packet position in the file maps 1:1 back to a specific (frame, field, line) — the vhs-decode convention, useful for packet-for-packet comparison against other decoders. Default: `false`.

### tolerant_framing (boolean)
Accept framing codes with one bit error. Recovers more packets from noisy sources at the cost of a higher false-positive rate. Default: `false`.

### require_valid_mrag (boolean)
Drop packets whose magazine/row address bytes fail Hamming 8/4 correction. This suppresses false framing-code locks on noise while still passing single-bit-damaged packets through to downstream tools. Default: `true`.

## Notes

* PAL sources only; the stage reports an error for any other video system.
* Empty VBI lines are cheap to probe: a line only yields a packet when the clock run-in and framing code are both found.
* Packets are written uncorrected. Page decoding, error correction, and subtitle extraction are the consumer's job (vhs-teletext, wxTED, and similar tools).

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
