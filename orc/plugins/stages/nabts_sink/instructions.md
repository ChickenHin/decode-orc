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

Each record is presented the way a receiver would present the page. A record whose support-needed flag is set (§5.2.7.9) is drawn after its data channel's support record — address FFF with the support-record flag — so the macros, redefinable characters and colour maps a service defines once and shares across its pages (§8.7.1.4) reach the pages that invoke them. A record carrying the caption flag is drawn from the caption preset of §5.2.7.3: white text over black in colour mode 2, starting at the lower left.

A record may name a More Record — explicitly, in a header extension field (§5.2.8.4), or algorithmically, by its more flag and its long address plus one (§5.2.7.6, the last two digits counted in decimal per §7.3.4) — and that continuation is presented over the display its predecessor left (§5.2.7.8): a chain repaints only what changes between its screens. Each member of a chain is therefore drawn over the accumulated display of the members before it, and the viewer lists the chain as one page, marked with a sub-page count in the records list, whose members are stepped through as sub-pages the way the teletext viewer steps a multi-page set.

The pass also learns about the recording as it goes, and spends less on later frames than on the first ones: where in the line this recording's data bursts start, and which of the candidate lines carry them at all (`pin_data_phase` and `learn_active_lines` below).

Recovering a line reads that line's samples and nothing else, so frames are decoded on several threads at once while the packets are written from one, in the order they were transmitted (`decode_threads`). The recovered stream does not depend on how many threads were used.

## Tools

### NABTS Records

The record viewer for this node. Triggering the node opens it automatically, which is how the records are reached — leave `output_path` empty and triggering is a decode-and-browse that writes no file.

The catalogue stays with the stage after the run, so closing the viewer and picking **NABTS Records** from the **Stage Tools** menu re-opens it immediately, reading what the last trigger produced rather than decoding the source again. That menu entry only ever reads: on a node that has not been triggered it says there is nothing to show instead of starting the decode, because deciding when to spend that time is what **Trigger Stage** is for. Editing any stage's parameters rebuilds the graph and discards every stage's results, closing the open viewers with them — trigger again for a catalogue that matches the new settings.

The viewer lists every record the range carried: its channel and record address, its version, the record type, how often it was seen and over which frames, and the classification flags the service set on it. Because the catalogue comes from a pass over the whole source rather than a window around the preview position, the list is the service's full carousel rather than whatever happened to be on screen.

Selecting a record shows what it carries:

* A **presentation record** is drawn as its NAPLPS display list, beside the plain text of the page. Pages are drawn in a unit square whose lower 0.78125 is the display area every receiver is guaranteed to show (ANSI X3.110 Table D1); **Show display area** outlines it, which is how to tell a record drawn deliberately into one corner from one that was mis-scaled. The text pane is there because an index page is mostly words and picking them off a rasterised page is tedious — it is the same characters the drawing used, in reading order.
* An **application record** is shown as its function descriptors, each with its code in the code-table notation §7.2.2 uses and its arguments.

**Enable animations** runs the blink processes the record set up. A blink process (ANSI X3.110 §5.3.2.7.2) belongs to a **colour map entry**, not to a figure: it periodically overwrites the entry with a second colour, so what alternates is everything drawn in that colour, whenever it was drawn — and everything drawn in any other colour stands still, even if it was drawn after the BLINK command. Clear the box to hold the record still for reading or capture.

The second colour is the one the record names. §5.3.2.7.3 lets the BLINK command point at any map entry, so a blink is not necessarily an appearance and a disappearance: a figure alternating with another colour twinkles, and one alternating with the ground vanishes and returns. The C1 BLINK START of §6.2.8.1 is the second case written short — it blinks to nominal black, or to the background colour in colour mode 2. A blink-to entry rewritten later in the record changes what the figure alternates to, because §5.3.2.5 makes every map write retroactive.

What is not shown is the record's **timing**. §5.3.2.7.3 transmits an ON and an OFF interval in tenths of a second, and a start delay that phases one process against another; the viewer instead alternates every process together on the same 0.75 Hz cycle the page viewer flashes at. A record whose processes run at different rates, or deliberately out of phase with each other, is shown in step.

**Save PNG…** writes the display on screen to an image file. A presentation record is drawn on its own, 720 pixels across and at the aspect of its display area, with no border around it and the text pane left out — a display list carries no resolution of its own, so this is a size chosen to keep the fine strokes a service draws letterforms with well clear of a single pixel. Blink processes are saved in their lit phase whatever phase the display happens to be in, since a still of the other phase would lose whatever was blinking; **Show display area** is honoured. The name offered is the record's channel, address and version — `Record-000-1A4-v0.png`. The control appears for a drawn record only: an application record is a listing of function descriptors and there is nothing to rasterise.

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

### render_resolution (string)
The receiver resolution NAPLPS pages are drawn against in the **NABTS Records** tool, and the one the tool opens on. The same four choices sit on a **Receiver** dropdown at the top of that window, which is where you would normally change it — see *Choosing a receiver while reading* below.

ANSI X3.110 draws into an abstract unit screen, but it sizes several things in the physical pixels of the receiver displaying them: §5.3.2.2.6 defines the logical pel — what gives a line its width — as covering "all of those pixels that lie under any portion of the logical pel as it is mapped to the display screen", and guarantees it "will always map to at least one and possibly many display pixels". Line texture dot and dash lengths (§5.3.2.4.2), hatch spacing (§5.3.2.4.4) and the raster step of INCREMENTAL POINT are all multiples of that pel. A page therefore has no single correct appearance until a receiver is named; in particular a stroke the service left dimensionless is one *pixel* wide, and without a resolution there is no pixel to measure it in.

* **256 x 200 (reference receiver)** (default) is the grid Table D1 item 10 requires of a receiver — "resolution shall be on the order of 256 pixels horizontal by 200 pixels vertical" — and so what a set-top decoder of the period put on screen. Choose it to see the page as its author would have.
* **512 x 400** and **768 x 600** are that grid at twice and three times, for a sharper reading of the same page. Appendix D Scope note (2) contemplates exactly this: a receiver exceeding the reference model "may produce more pleasing images". Each keeps the grid pixel square in unit space, so nothing the service drew is distorted.
* **512 x 400 (vector)** draws the same geometry as resolution-independent shapes rather than pixels. Curves stay smooth at any zoom, which suits reading fine detail, but the pixel structure a receiver had is lost.

The choices are named after the grid rather than by the 240p/480p shorthand a television is described by, because that shorthand counts the lines a set *scanned* and these count the pixels a decoder *drew into* — the reference model's buffer is 200 rows whatever the set showing it scanned, so calling it 240p would invite reading 240 rows into it.

Only whole multiples of the reference grid are offered. A page is authored against that grid, so at twice or three times everything its author placed on a pixel boundary lands on one again; at some fraction in between it lands between pixels, which thickens strokes unevenly and — because a character pattern is a bitmap of a fixed cell — breaks the letterforms outright.

The three pixel modes deposit the page into the receiver's frame buffer exactly as the standard describes and show you those pixels. The pixels are drawn as scalable blocks, not as a fixed-size image, so they stay sharp at any window size and in an exported PNG.

Everything the standard sizes in logical pels — stroke width, the dot and dash lengths of §5.3.2.4.2, hatch width and spacing, a programmable fill mask's step and repeat — comes out at that size in every mode, so a figure keeps its weight and its texture whichever receiver is chosen and however large the window is. The pixel modes step a texture along a line one grid cell at a time and are exact everywhere; the vector mode does the same for straight lines, and along an arc, which turns as it goes, uses the pel measured along an axis for the whole curve.

The built-in character patterns follow the receiver like everything else. §5.1 leaves them to it — "the particular patterns (font) chosen for the characters are implementation-dependent and are constrained only by the specified character field at each size for a given display resolution" — and a set with more pixels had a character generator with finer patterns, not the same coarse ones magnified. **256 x 200** draws from a 6 by 10 face, which is both the cell Appendix B arrives at from first principles ("the most readable characters in this size range are 6 pixels by 10 pixels") and exactly the character field that grid gives the standard's default text size; **512 x 400** draws from a 10 by 20 and **768 x 600** from a 9 by 15 at double size, so text sharpens with the rest of the page rather than staying blocky in the middle of it. All three are members of the public-domain X11 "misc-fixed" family, so the letterforms are of a piece across receivers, and the face is chosen per character field rather than per page: a field too small for the finer patterns takes the 6 by 10, which is what a receiver with one generator and a field below it does. Downloadable characters have always followed the receiver, because the service sizes those itself (§6.2.3).

A stroke comes out the pel's weight wherever it falls on the grid and whichever way it runs: a one-pel line is one pixel across drawn horizontally, vertically or on any slope between. The standard sizes the pel without reference to either, so a line that grew a pixel where it happened to straddle a pixel boundary, or where it turned a corner, would be showing the grid rather than the page. A filled figure and the same figure outlined agree pixel for pixel, so a service that fills a shape and then draws its border — as a weather map does with each region — gets a border with no colour showing past it and no gap behind it.

**Save PNG…** writes the same size whichever is chosen — 1536 by 1152, a whole multiple of every grid across. Comparing one receiver with another is what the choice is for, and images of different sizes would make the comparison about the sizes instead.

Whichever is chosen, the page is displayed in a 4:3 area: §4.2.2 puts the guaranteed-visible unit screen — x 0 to 1 and y 0 to 0.78125 — in the display area of a television set, which makes the receiver's pixels slightly wider than they are tall.

#### Choosing a receiver while reading

Nothing in the recovery depends on this setting — the records are read off the recording, and the receiver decides only how they are drawn afterwards — so the **NABTS Records** window offers the same four choices on a **Receiver** dropdown beside its other display switches. Picking one redraws the page you are looking at and leaves you on it, without reading the recording again. That includes downloadable characters: §6.2.3 sizes a DRCS character's storage buffer from the physical resolution its character field covers, and the presentation code is run when the page is browsed rather than when the recording is read, so those buffers follow the choice too.

The two controls do different jobs, and where they disagree the dropdown wins:

* This parameter is the project's own setting. It is saved with the project, the command-line interface can set it, and it is what the window opens on.
* The dropdown overrides it for as long as the window is open, and is not saved. Close the window and reopen it and you are back to the parameter.

Prefer the dropdown for comparing receivers, which is the reason there is more than one: editing any stage parameter rebuilds the graph and discards every stage's results, closing this window with them, so changing the parameter to compare two receivers costs a full re-run of the recovery pass for a change that affects nothing it found.

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

The file holds the record's data as the record dialog reads it: NAPLPS presentation code, or application data for a record of type 2. Where an undamaged copy arrived that is that copy byte for byte; where none ever did, it is the combination of the damaged copies described below. Byte parity is left in place, as it is in the packet stream. This is what to use to take a record to an external NAPLPS tool.

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

Both figures above were measured before repeated copies of a record were combined. Combining them changes what a marginal tape yields, and changes what the report means. A later NBC EP transfer measuring 0.36 mean confidence — above the 0.22 of the table, below the 0.5 that reads cleanly — decoded end to end as:

| channel 000, the NBC service | one copy kept | copies combined |
|---|---|---|
| record bytes failing odd parity | 2.53 % | **0.67 %** |
| record length | short by a third | as the group header declares |
| positions no copy ever received | — | 14.5 %, left as NUL |

Two things are worth taking from that. Damage among the bytes that did arrive falls by about three quarters, because the copies disagree only where they were damaged. And the records come out at their true length: a copy that lost packets used to close up over the hole, which moved every byte after it and is what turned a page into overlaid nonsense. What replaces the shortfall is honest — positions nothing ever arrived for, left as a control with no presentation effect.

What it does not do is invent data. A third of that recording's record positions were never received on any pass, and no vote recovers those. Nor does it move the bottom of the range: at 0.2 the bit decisions are the detector fitting noise, the copies disagree everywhere rather than in a few places, and there is nothing for a vote to find.

## Notes

* NTSC and PAL-M sources are accepted. A 625-line source is reported as an error: CEA-516 §1.1.1 specifies NABTS on the 525-line NTSC signal, and no 625-line service exists to recover.
* Empty VBI lines are cheap to probe under every detector: a line that never rises meaningfully above blanking is rejected before any detection runs.
* Nothing is corrected in the packet stream. The prefix bytes are written as recovered and left to their own Hamming coding, and the data block to its own byte parity and longitudinal check byte.
* Byte parity is not used to repair damaged bytes **in the packet stream**, as it is for World System Teletext. CEA-516 §3.3 gives the data block odd parity only when its data group is of type 0, and a single packet does not say which type its group is — so at the packet layer a byte that fails parity is not known to be damaged. Where a packet carries the longitudinal check byte of §3.4, that byte and the per-byte parity do form a product code, and a single-bit error in the block is located and corrected. Whether a service sends it is the service's choice: ExtraVision does and 768 blocks were mended on the reference capture, NBC does not and none were.
* By the time a record is assembled the group type **is** known — §4.3 makes a type-zero group the only one carrying a teletext record, and everything else has already been set aside — so at that layer every byte is known to be parity-coded and one that fails the check is known to be corrupt. That is what gates the vote described next.
* Where no copy of a record ever arrives undamaged, the damaged copies are **combined** rather than one of them chosen. A carousel (§7.1.2) brings each record round for the length of the recording, and the copies differ only where they were damaged, so a vote across them recovers bytes that no single copy has right. The vote is per byte position: a parity-clean candidate beats a corrupt one however often the corrupt one arrived, and a position every copy damaged falls back to the best of a bad set. The record dialog says how many copies a record was combined from. The moment an undamaged copy arrives it is used as it stands and the copies held for the vote are dropped.
* Among candidates of equal standing each copy is weighted by how sure the detector was of its byte, rather than counting one vote apiece, so a value read cleanly outweighs more copies of one the MLSE detector nearly decided the other way. The threshold detector decides each bit on one sample and has no path metric to compare, so it measures nothing and its bytes weigh full — a detector with no way of saying it is unsure has not said so. A tie goes to the most recent copy.

  The weighting earns its keep where copies are few. With two copies disagreeing there is no majority to find at all, and the choice would otherwise fall to whichever arrived last; with eight or more the majority is usually decisive on its own. On the NBC EP transfer, which averages around eight copies per record, weighting changed no byte of the result — the parity gate and the count between them had already settled every position.
* Only copies that line up position for position take part. A group that lost packets (§3.2.4) is missing bytes from its middle, so everything after the hole has moved earlier; such a copy is counted as an appearance but kept out of the vote, since voting it in would corrupt every position past the hole. The same goes for an incomplete linked series (§5.2.6), which is missing a whole record from the middle of its concatenation. A copy damaged only by bit errors still lines up and still votes.
* Sixteen copies per record are retained for the vote — beyond a handful the result rarely changes — and it is the most recent sixteen that are kept.
* Only the combined result is run through the NAPLPS interpreter, once per record rather than once per copy.
* A position no copy ever received is left as NUL. X3.110 §6.1.4 makes that a transparent control with no presentation effect, so a hole costs the drawing nothing beyond the bytes that were actually lost — which is the point of holding the hole open rather than letting the record close up over it.

## Choosing the line range

`first_vbi_line` and `last_vbi_line` default to the whole of 10-21, which is where CEA-516 §1.2 permits NABTS. A recording rarely uses all of it, and the lines it does not use are not free: a line carrying another service, or nothing but noise, still produces the occasional 33 bytes whose Hamming 8/4 prefix decodes by chance, and those enter data group reassembly as packets of whatever channel they appeared to name. There they open groups that never complete and consume continuity indices that belonged to real ones.

The report's per-line table is what to read. A line carrying a service shows a large, steady packet count; a line being fitted to noise shows a high burst count and a small packet count. Narrowing the range to the lines that carry the service is worth doing — on the NBC EP transfer above, going from 10-21 to 15-16 took group headers refused from 19691 to 1675, recovered seven more records, and took damage among received record bytes from 0.67 % to 0.57 %.
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
