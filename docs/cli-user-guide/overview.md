# CLI User Guide

## Overview

`orc-cli` is the command-line interface for Decode Orc, a cross-platform orchestration and processing framework for LaserDisc and tape decoding workflows. It provides a text-based interface for batch processing video projects defined in `.orcprj` files.

The CLI uses the same core processing library as the GUI (`orc-gui`), ensuring that project files created in the graphical interface can be executed unchanged via the command line, and vice versa.

## Key Features

- **Batch Processing**: Process complete DAG pipelines without user interaction
- **Automation**: Integrate into scripts and automated workflows
- **Reproducibility**: Execute the same project file consistently across runs
- **Progress Tracking**: Real-time progress updates during processing
- **Flexible Logging**: Configurable logging levels and output destinations
- **Crash Reporting**: Automatic diagnostic bundle creation for troubleshooting

## Basic Usage

### Command Syntax

```bash
orc-cli <project-file> [options]
```

### Required Arguments

- `<project-file>`: Path to an Orc project file (`.orcprj`)

### Options

| Option | Description | Default |
|--------|-------------|---------|
| `--process` | Process the complete DAG pipeline (trigger all sink nodes) | Required |
| `--filter GRAPH` | Build and process a DAG from an ffmpeg-style filtergraph string (no `.orcprj` file) | - |
| `--filter-file FILE` | Read the filtergraph from `FILE` (`-` for stdin) | - |
| `--input GRAPH` | Input (source) stage(s), for the input/filters/output triad | - |
| `--filters GRAPH` | Processing stage(s), for the triad | - |
| `--output GRAPH` | Output (sink) stage(s), for the triad | - |
| `--log-level LEVEL` | Set logging verbosity level | `info` |
| `--log-file FILE` | Write logs to specified file | None (console only) |
| `--help`, `-h` | Display help message and exit | - |

### Log Levels

Available log levels (from most to least verbose):

- `trace`: Extremely detailed debugging information
- `debug`: Detailed debugging information
- `info`: General informational messages
- `warn`: Warning messages
- `error`: Error messages
- `critical`: Critical errors only
- `off`: Disable logging

## Examples

### Basic Processing

Process a project file with default settings:

```bash
orc-cli my-project.orcprj --process
```

### Detailed Logging

Enable debug logging for troubleshooting:

```bash
orc-cli my-project.orcprj --process --log-level debug
```

### Log to File

Save all log output to a file:

```bash
orc-cli my-project.orcprj --process --log-file processing.log
```

### Combined Options

Process with debug logging saved to file:

```bash
orc-cli my-project.orcprj --process --log-level debug --log-file debug.log
```

## Filtergraph Mode

As an alternative to authoring a `.orcprj` file, a linear decode pipeline can
be described directly on the command line. This builds the same in-memory DAG
a `.orcprj` file would and triggers all sink nodes, so results are identical.
The `.orcprj` workflow itself is unchanged.

Video format (NTSC/PAL/PAL-M) and source signal type (composite/Y-C) are
**never specified separately** — they are detected automatically from the
stage modules used (a stage that is exclusively NTSC-compatible implies NTSC;
supplying `y_path`/`c_path` implies Y/C; supplying `input_path` implies
composite). If two stages imply conflicting formats, that is reported as an
error before anything runs.

### Style 1: a single filtergraph string (`--filter`)

```
stage_name=key=value:key=value, stage_name=key=value
```

- **Stages** are separated by `,` and are auto-connected in order.
- **Values** may be wrapped in single quotes (`'...'`) **or** double quotes
  (`"..."`) — whichever is more convenient for your shell — to include `:`
  `,` `;` and spaces literally; the two quote styles are interchangeable, and
  a value quoted with one may freely contain the other. A value may also be
  escaped with a backslash (`\`) instead of quoting, and may itself contain
  `=`.

```bash
orc-cli --filter "tbc_source=input_path=capture.tbc, hvd_chroma_decoder, video_sink=output_path=capture.mp4"
```

Reading the graph from a file or standard input:

```bash
orc-cli --filter-file graph.txt
cat graph.txt | orc-cli --filter-file -
```

### Style 2: the input/filters/output triad

For the common case — one input, a chain of processing stages, one output —
`--input`, `--filters`, and `--output` avoid needing to know the full
filtergraph grammar, and they enforce that stages go where they belong:
**every stage named in `--input` must be a source, every stage in `--output`
must be a sink, and every stage in `--filters` must be neither** (a transform
or similar processing stage). Putting a sink in `--input`, for example, is
rejected with a clear error naming the correct flag — this is checked against
each stage's real role (the same metadata the GUI uses), not guessed from its
name, so it works for any third-party plugin stage too.

```bash
orc-cli \
  --input "tbc_source=input_path=capture.tbc" \
  --filters "hvd_chroma_decoder" \
  --output "video_sink=output_path=capture.mp4"
```

Any of the three may be omitted, and each may itself contain a
comma-separated chain of stages. `--input`/`--filters`/`--output` cannot be
combined with `--filter`/`--filter-file` in the same run — pick one style.

### Windows paths

Unquoted Windows paths (drive letters and backslashes) are parsed correctly
in both styles above:

```bash
orc-cli --input "tbc_source=input_path=C:\Users\me\capture.tbc" --output video_sink
```

If a value needs to rule out any ambiguity — for instance it contains a
literal comma or semicolon — wrap it in single **or** double quotes,
whichever your shell passes through more conveniently.

### Non-linear graphs

Anything with real shape — fan-in (multiple sources into one stage), fan-out
(one stage into several sinks), or branching — is not expressible this way by
design, and remains a `.orcprj` project file.

## Processing Workflow

When you run `orc-cli --process`, the following occurs:

1. **Project Loading**: The `.orcprj` file is loaded and validated
2. **DAG Construction**: The processing pipeline is built from the project definition
3. **Validation**: Input files and parameters are verified
4. **Sink Triggering**: All sink nodes in the DAG are triggered sequentially
5. **Progress Reporting**: Real-time progress updates are displayed (every 5%)
6. **Completion**: Exit code indicates success (0) or failure (non-zero)

### Progress Output

During processing, you'll see progress updates like:

```
[2026-02-08 10:15:23.456] [cli] [info] Loading project: my-project.orcprj
[2026-02-08 10:15:23.789] [cli] [info] Project loaded: My Video Project
[2026-02-08 10:15:24.012] [cli] [info] [Progress: 0%] Starting decoding...
[2026-02-08 10:15:45.234] [cli] [info] [Progress: 5%] Processing frames...
[2026-02-08 10:16:12.567] [cli] [info] [Progress: 10%] Processing frames...
...
[2026-02-08 10:25:34.890] [cli] [info] [Progress: 100%] Decode complete
```

## Exit Codes

- `0`: Success - all operations completed successfully
- `1`: Error - processing failed or invalid arguments

Always check the exit code in scripts:

```bash
if orc-cli project.orcprj --process; then
    echo "Processing successful!"
else
    echo "Processing failed!" >&2
    exit 1
fi
```

## Project Files

### Project File Format

Project files (`.orcprj`) are YAML-based files that define:

- Source configurations (input TBC files)
- DAG structure (processing nodes and connections)
- Node parameters (decoder settings, output formats, etc.)
- Project metadata (name, description, video format)
- Optional `required_plugins` metadata for third-party plugin-backed stages

When a project contains stages supplied by third-party plugins, Decode-Orc now
saves a root-level `required_plugins` block in the `.orcprj` file. Each entry
records the plugin identity, repository or release URL metadata, ABI
expectation, and the stage names from that plugin that are still used by the
project.

This block is refreshed every time the project is saved:

- Plugin entries are kept only if at least one of their stage names is still present in the current DAG
- Plugin metadata is refreshed from the current local plugin registry when available
- Stale entries are removed automatically if the corresponding plugin-backed stages were deleted while editing

If a project is opened on a machine where one of those stages is unavailable,
Decode-Orc uses the saved `required_plugins` metadata to give a more specific
missing-stage error that can point the user at the expected plugin and its
repository URL.

### Creating Projects

Project files are typically created using `orc-gui`, but they can also be:

- Hand-edited (with care - see technical documentation)
- Generated programmatically
- Version controlled (recommended for reproducibility)

### Project Compatibility

Projects created in the GUI can be executed in the CLI without modification. This ensures:

- Consistent results across interfaces
- Batch processing of GUI-created projects
- Easy integration into automated workflows

## Common Use Cases

### Batch Processing Multiple Files

Process multiple projects in a loop:

```bash
for project in *.orcprj; do
    echo "Processing $project..."
    orc-cli "$project" --process --log-level info
done
```

### Automated Workflow

Integrate into a processing pipeline:

```bash
#!/bin/bash
set -e

# Process video
orc-cli capture1.orcprj --process --log-file capture1.log

# Check for errors
if [ $? -ne 0 ]; then
    echo "Processing failed, check capture1.log"
    exit 1
fi

# Continue with next step...
```

### Monitoring Progress

Capture and monitor progress in real-time:

```bash
orc-cli project.orcprj --process 2>&1 | tee -a processing.log
```

## Error Handling

### Common Errors

**Project file not found:**
```
Error: No project file specified
```
→ Ensure the `.orcprj` file path is correct

**Missing command:**
```
Error: No command specified. You must use --process
```
→ Add the `--process` flag

**Processing failure:**
```
Failed to load project: <reason>
```
→ Check project file syntax and input file paths

**Filtergraph parse error:**
```
Failed to parse filtergraph: <reason> (at offset N)
```
→ Check the filtergraph syntax near character `N`; quote values containing
`:` `,` `;` or spaces (single or double quotes both work)

**Unknown stage:**
```
Unknown stage '<name>'.
```
→ Use a stage name that exists in your build

**Stage used in the wrong triad category:**
```
--input: stage 'video_sink' (Video Sink) is an output (sink) stage — it
belongs under --output, not --input.
```
→ Move the stage to the flag matching its actual role

**Missing required parameter:**
```
Stage '<name>': missing required parameter '<param>'.
```
→ Supply the parameter

### Crash Diagnostics

If `orc-cli` crashes unexpectedly, it automatically creates a diagnostic bundle containing:

- System information (OS, CPU, memory)
- Stack backtrace showing crash location
- Application logs
- Core dump file (when available)

The crash bundle is saved as a ZIP file in the current working directory:

```
crash-bundle-orc-cli-YYYY-MM-DD-HHMMSS.zip
```

When reporting issues, attach this bundle to your bug report on [GitHub Issues](https://github.com/simoninns/decode-orc/issues).
