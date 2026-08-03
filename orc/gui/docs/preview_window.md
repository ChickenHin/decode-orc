# Preview Window

## Overview

The Preview Window displays the video output of the currently active pipeline
stage. It provides frame-by-frame navigation, channel selection, dropout
visualisation, and a suite of signal-analysis tools: Line Scope, Frame Timing,
Waveform Monitor, Vectorscope, and several metadata observers.

## Navigation Bar

| Control | Description |
|---------|-------------|
| << | Jump to the first frame or field. |
| < | Step back one frame or field. Hold to step continuously. |
| ▶ / ⏸ | Play back at the project frame rate (PAL: 25 fps; NTSC: ~30 fps). Press again to pause. Stops automatically at the last frame. |
| > | Step forward one frame or field. Hold to step continuously. |
| >> | Jump to the last frame or field. |
| Frame spinbox | Type a frame number to jump there directly (1-indexed display). |
| Slider | Drag to scrub through the entire range. Rendering is debounced during rapid movement. |

## Control Row

| Control | Description |
|---------|-------------|
| Preview Mode | Select the output type: **Frame**, **Top Field**, **Bottom Field**, or other modes offered by the selected stage. |
| Channel | For YC (separate luma/chroma) sources: **Y+C** (composite display), **Luma (Y)**, or **Chroma (C)**. Hidden for composite CVBS sources. |
| Aspect Ratio | Set the display aspect ratio: Square Pixels, 4:3, 16:9, and others. |
| Zoom 1:1 | Resize the preview window so the image is displayed at its native pixel resolution. |
| Dropouts: Off/On | Toggle the dropout overlay. When enabled, samples flagged as dropout are highlighted in red over the preview image. |

## Line Scope

Click anywhere in the preview image to open the **Line Scope**, which shows the
raw sample waveform for that horizontal line.

- Horizontal axis: sample position across the line.
- Vertical axis: signal amplitude in millivolts.
- For YC sources, separate **Y** (luma) and **C** (chroma) waveforms are shown.
- Black level and white level markers are drawn from active video parameter hints.
- Up/Down navigation controls step to adjacent lines without closing the scope.
- A cross-hair on the preview image tracks the sample marker position in the
  scope and vice versa.

## File Menu

| Action | Shortcut | Description |
|--------|----------|-------------|
| Export PNG... | Ctrl+Shift+E | Save the currently displayed preview image as a PNG file. |

## Observers Menu

Observers decode metadata from the video signal and display it in floating
dialogs that update as you navigate frames.

| Action | Shortcut | Description |
|--------|----------|-------------|
| VBI Decoder | Ctrl+Shift+V | Decode Vertical Blanking Interval data: closed captions, VITC timecode, teletext, and LaserDisc programme metadata. |
| Quality Metrics | Ctrl+Shift+M | Per-frame signal quality statistics: SNR, colour burst level, and dropout sample count. |
| NTSC Observer | Ctrl+Shift+N | NTSC-specific frame metadata: FM code status and white flag bit. |
| Teletext Pages | Ctrl+Shift+X | Render PAL World System Teletext pages recovered from the VBI of recent frames. |
| Closed Captions | Ctrl+Shift+C | Decode the EIA-608 line 21 caption service into a running transcript. |

NTSC Observer and Closed Captions are available only on NTSC projects, and
Teletext Pages only on PAL projects; the standard the entry does not apply to
leaves it greyed out. PAL-M is 525-line and carries neither, so all three are
unavailable there.

## Hints Menu

| Action | Shortcut | Description |
|--------|----------|-------------|
| Video Parameter Hints | Ctrl+Shift+H | Show the video parameters in effect for the selected stage: colour system (PAL/NTSC), line count, black level, white level, and active video geometry. User overrides are flagged separately from signal-derived values. |

## View Menu

Signal analysis tools displayed as waveform or vector graphs in floating dialogs.

| Action | Shortcut | Description |
|--------|----------|-------------|
| Frame Timing | Ctrl+Shift+T | Show the sync and timing waveform across all lines of the current frame. Useful for diagnosing sync pulse position and line structure. |
| Waveform Monitor | Ctrl+Shift+W | Sample histogram across multiple lines with adjustable gain and range. Available for supported stages only. |
| Vectorscope | Ctrl+Shift+S | U/V chroma vector plot with a standard vectorscope graticule. Available for stages with separate Y and C channels. |

### Waveform Monitor

Displays a stacked histogram of sample values across a range of lines.

| Control | Description |
|---------|-------------|
| Gain | Vertical amplification of the trace. |
| Range | The vertical display range in millivolts. |

Only available when the selected stage provides this view.

### Vectorscope

Plots the chroma U and V components as a scatter on a standard vectorscope graticule.

| Control | Description |
|---------|-------------|
| Field | Choose which field (top, bottom, or full frame) contributes to the plot. |
| Blend | Persistence factor — higher values retain more signal history on screen. |
| Defocus | Gaussian blur on the trace to aid reading at high dot density. |

Only available for stages that output separate Y and C channels.

### VBI Decoder

Decodes and displays Vertical Blanking Interval content for the current frame,
including closed captions, VITC timecode, teletext lines, and LaserDisc
programme metadata such as chapter and frame numbers.

### Quality Metrics

Per-frame signal quality data:

| Metric | Description |
|--------|-------------|
| SNR | Signal-to-noise ratio of the luma channel. |
| Burst Level | Amplitude of the colour burst reference signal. |
| Dropout Count | Number of dropout samples detected in this frame. |

### NTSC Observer

Shows NTSC-specific frame metadata including the FM code status and white flag bit.

### Teletext Pages

Renders the requested teletext page (PAL World System Teletext, Level 1) from
the packets recovered in a trailing window of frames ending at the current
frame. Enter the page number in the conventional magazine + two-hex-digit form
(for example 100 or 888). Teletext is a carousel medium, so random access is
approximate: the dialog reports the frame at which the page transmission was
actually seen, and pages not yet met are reported as not seen. Sequential
playback behaves like live reception, and everything decoded along the way is
kept, so the page list keeps growing until you jump somewhere unrelated.

A page is transmitted a packet at a time, and a source carrying only a couple
of teletext lines per field takes several frames to send one. Stepping through
those frames shows the page filling in, so the status bar says whether the
transmission has finished: **Complete** once the service has moved on to the
next page, **Partial - still arriving** while more rows are yet to come. Rows
that have not been sent yet look exactly like transmitted blank ones, so this
is the only way to tell "wait" from "damaged". The page list marks a page in
that state with an ellipsis after its frame number.

**Show data errors** outlines characters whose byte failed its parity check.
It bands whole rows only when the page's transmission lost packets — a page
leaving rows out is normal rather than a fault, because services omit the
blank lines that space a page out instead of transmitting a row of spaces.
Loss is detected from the VBI packet slots the transmission's own fields gave
up, so a recording that inserts on fewer lines is not accused of losing the
rest.

### Video Parameter Hints

Shows the video parameters currently in effect for the selected stage: colour
system (PAL/NTSC), nominal line count, black level, white level, first and last
active frame lines, and whether each value originates from the source signal or
a user override applied by the Video Parameters stage.
