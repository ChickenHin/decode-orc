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
| ▶ / ⏸ | Play back at the project frame rate (PAL: 25 fps; NTSC: ~30 fps), or at the audio rate when a channel pair is selected. Press again to pause. Stops automatically at the last frame. |
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
| Audio | Choose an audio channel pair to play with the preview, or **No audio**. Greyed out when the stage's output carries no audio. |
| Volume | Playback volume for the selected pair. Takes effect immediately, including mid-playback. |
| 🔊 / 🔇 | Mute the audio without pausing. The preview keeps playing at the same rate. |

## Audio Playback

Select a channel pair in the **Audio** control and press play: the pair is
played at normal speed and the preview follows it.

Audio is the clock and the video chases it. Preview rendering is not
guaranteed to reach real time — a 3D chroma decoder or a neural stage can take
seconds per frame — so instead of slowing the audio to match, the preview shows
whichever frame the audio has reached and skips the ones the renderer could not
deliver in time. Sound is always continuous; on a heavy pipeline the picture
simply updates less often.

A few things worth knowing:

- **The first play may pause to prepare the audio.** Some sources decode their
  whole audio stream on first access — an EFM disc decode can take minutes.
  A progress window appears if the wait is long enough to notice, and the
  prepared audio is kept, so pausing and playing again is instant. A CVBS
  container reads audio per frame and needs no preparation at all.
- **Preparing audio can use a lot of memory.** A TBC or imported-WAV source
  holds the decoded audio in RAM for the whole disc; this can be hundreds of
  megabytes on a feature-length title.
- **The selection resets when you change stage.** Channel-pair numbering is a
  property of the stage's output, so the selector re-reads it and returns to
  **No audio** whenever you view a different stage.
- **Editing the pipeline stops playback.** Changing a parameter or the graph
  invalidates the prepared audio; press play again to restart.
- **Scrubbing restarts the audio** at the new position rather than trying to
  catch up.
- Most projects carry no audio at all. The **Audio** control is then disabled
  and play behaves exactly as it always has.

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
| Closed Captions | Ctrl+Shift+C | Decode the EIA-608 line 21 caption service into a running transcript. |

NTSC Observer and Closed Captions are available only on NTSC projects; on a
project of another standard the entry is greyed out.

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
including closed captions, VITC timecode, and LaserDisc programme metadata such
as chapter and frame numbers. Teletext itself is decoded by the Teletext
Analysis Sink stage rather than here; what this dialog shows is the LaserDisc
programme-status flag saying whether the disc carries a teletext service.

### Quality Metrics

Per-frame signal quality data:

| Metric | Description |
|--------|-------------|
| SNR | Signal-to-noise ratio of the luma channel. |
| Burst Level | Amplitude of the colour burst reference signal. |
| Dropout Count | Number of dropout samples detected in this frame. |

### NTSC Observer

Shows NTSC-specific frame metadata including the FM code status and white flag bit.

### Video Parameter Hints

Shows the video parameters currently in effect for the selected stage: colour
system (PAL/NTSC), nominal line count, black level, white level, first and last
active frame lines, and whether each value originates from the source signal or
a user override applied by the Video Parameters stage.
