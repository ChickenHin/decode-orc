/*
 * File:        command_filter.h
 * Module:      orc-cli
 * Purpose:     Build and process a DAG from a source/filters/sink triad
 *              instead of a .orcprj project file.
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
 * `input_stages` / `filters_stages` / `output_stages` correspond to the CLI's
 * --source/--filters/--sink flags respectively; each holds a (possibly
 * comma/semicolon-chained) fragment restricted to one stage category —
 * source stages, processing stages, and sink stages — and is validated per
 * category (see filter_command()). Any of the three may be omitted, but at
 * least one must be non-empty.
 *
 * There is no video/source-format option for running: video format and
 * source type are inherent to the stage modules used (e.g. an NTSC-only
 * source, or the y_path/c_path/input_path parameters actually supplied) and
 * are detected automatically. The two export_* overrides below exist only
 * because a saved .orcprj file requires an explicit value even for stages
 * that are legitimately format-agnostic when run directly.
 */
struct FilterOptions {
  std::string input_stages;    ///< --source: input (source) stage(s).
  std::string filters_stages;  ///< --filters: processing stage(s).
  std::string output_stages;   ///< --sink: output (sink) stage(s).

  /// When non-empty, save the assembled project to this .orcprj path
  /// instead of running it (e.g. for later editing in the GUI, or reuse
  /// with --process). No sinks are triggered when this is set. This calls
  /// the existing ProjectPresenter::saveProject() — the same YAML writer
  /// used elsewhere — so it is not a new save format, just a different
  /// entry point into the existing one.
  std::string export_project_path;

  /// Optional override for the project's video format ("NTSC", "PAL", or
  /// "PAL-M"), applied whenever none of the stages used imply a format on
  /// their own — whether running directly or exporting. Running in memory
  /// doesn't *need* this (the core's own compatibility checks tolerate an
  /// undetermined format), but supplying it anyway means the same graph
  /// behaves identically run directly or exported then reprocessed with
  /// --process, since format-specific parameter defaults are selected from
  /// the same video_format either way (see project_to_dag.cpp). Exporting
  /// specifically *requires* a concrete value one way or another — the
  /// .orcprj file format itself always requires an explicit format — so a
  /// stage that is legitimately format-agnostic (e.g. tbc_source, which
  /// reads its own format from its metadata sidecar file rather than from
  /// the project) needs this to export at all.
  std::string video_format_override;

  /// Optional override for the project's source signal type ("composite"
  /// or "yc"), applied under the same conditions and for the same reasons
  /// as video_format_override above (e.g. tbc_source with only pcm_path set
  /// reveals neither on its own).
  std::string source_type_override;
};

/**
 * @brief Execute a filtergraph decode run, or save it instead of running.
 *
 * Parses and composes the source/filters/sink triad (validating that each
 * segment only contains stages of the matching category), auto-detects the
 * project's video format and source type from the stages actually used, and
 * builds an in-memory project via the project presenter.
 *
 * Normally, triggers all sink nodes and returns their combined result. If
 * `export_project_path` is set, saves the assembled project as a .orcprj
 * file instead (for opening in the GUI, or later use with `--process`) and
 * does not run anything.
 *
 * @param options Triad options.
 * @return Exit code (0 = success, non-zero = error).
 */
int filter_command(const FilterOptions& options);

}  // namespace cli
}  // namespace orc
