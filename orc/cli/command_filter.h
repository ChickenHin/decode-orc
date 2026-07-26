/*
 * File:        command_filter.h
 * Module:      orc-cli
 * Purpose:     Build and process a DAG from an ffmpeg-style filtergraph
 *              string (or an input/filters/output triad) instead of a
 *              .orcprj project file.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <string>

namespace orc {
namespace cli {

/**
 * @brief Options for the filter command.
 *
 * Exactly one of the two modes below is used, chosen by which fields are
 * non-empty:
 *
 * - Graph-string mode: `filtergraph` holds a full ffmpeg-style filtergraph.
 * - Triad mode: `input_stages` / `filters_stages` / `output_stages` each hold
 *   a (possibly comma-chained) fragment restricted to one stage category —
 *   source stages, processing stages, and sink stages respectively. This is
 *   friendlier for the common linear pipeline and is validated per category
 *   (see filter_command()).
 *
 * There is no video/source-format option: video format and source type are
 * inherent to the stage modules used (e.g. an NTSC-only source, or the
 * y_path/c_path/input_path parameters actually supplied) and are detected
 * automatically.
 */
struct FilterOptions {
  std::string filtergraph;     ///< Graph-string mode: full filtergraph.
  std::string input_stages;    ///< Triad mode: input (source) stage(s).
  std::string filters_stages;  ///< Triad mode: processing stage(s).
  std::string output_stages;   ///< Triad mode: output (sink) stage(s).
};

/**
 * @brief Execute a filtergraph decode run.
 *
 * Parses the filtergraph (or composes and validates the input/filters/output
 * triad), auto-detects the project's video format and source type from the
 * stages actually used, builds an in-memory project via the project
 * presenter, then triggers all sink nodes. No .orcprj file is read or
 * written.
 *
 * @param options Filtergraph or triad options.
 * @return Exit code (0 = success, non-zero = error).
 */
int filter_command(const FilterOptions& options);

}  // namespace cli
}  // namespace orc
