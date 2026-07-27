/*
 * File:        command_filter.cpp
 * Module:      orc-cli
 * Purpose:     Build and process a DAG from an input/filters/output triad.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "command_filter.h"

#include <cctype>
#include <map>
#include <string>
#include <vector>

#include "filtergraph_import.h"
#include "filtergraph_parser.h"
#include "logging.h"
#include "project_presenter.h"
#include "project_presenter_types.h"
#include "stage_category.h"

namespace orc {
namespace cli {

namespace {

using orc::presenters::ProjectPresenter;
using orc::presenters::StageInfo;
using orc::presenters::VideoFormat;

/**
 * Parse a video format name for --video-format (export-only override).
 * Accepts "NTSC", "PAL", or "PAL-M" (case-insensitive; "PAL_M"/"PALM" also
 * accepted for convenience). Returns false, leaving `out` untouched, for
 * anything else.
 */
bool parse_video_format_name(const std::string& text, VideoFormat& out) {
  std::string upper;
  upper.reserve(text.size());
  for (char c : text) {
    upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  if (upper == "NTSC") {
    out = VideoFormat::NTSC;
    return true;
  }
  if (upper == "PAL") {
    out = VideoFormat::PAL;
    return true;
  }
  if (upper == "PAL-M" || upper == "PAL_M" || upper == "PALM") {
    out = VideoFormat::PAL_M;
    return true;
  }
  return false;
}

/**
 * Validate that every stage named in `segment` belongs to `category`
 * (input segments must be sources, filters segments must be neither
 * source nor sink, output segments must be sinks). A stage's category is
 * never a guess: it comes straight from the same StageInfo the GUI uses, so
 * this works identically for core stages and for any third-party plugin
 * stage. Returns true (and leaves `error` untouched) when `segment` is empty
 * or every stage matches; otherwise returns false with `error` set.
 */
bool validate_triad_segment(const std::string& segment, StageCategory category,
                            const std::map<std::string, StageInfo>& stage_index,
                            std::string& error) {
  if (segment.empty()) {
    return true;
  }
  FilterGraphParseResult parsed = parse_filtergraph(segment);
  if (!parsed.ok) {
    error = std::string(category_flag(category)) + ": " + parsed.error;
    return false;
  }
  for (const auto& stage : parsed.graph.stages) {
    auto it = stage_index.find(stage.stage_name);
    if (it == stage_index.end()) {
      error = std::string(category_flag(category)) + ": unknown stage '" +
              stage.stage_name +
              "'. Run 'orc-cli plugins stages' to see available stages.";
      return false;
    }
    if (!stage_matches_category(it->second, category)) {
      error = std::string(category_flag(category)) + ": stage '" +
              stage.stage_name + "' (" + it->second.display_name + ") is " +
              actual_role_description(it->second) + " — it belongs under " +
              suggested_flag_for(it->second) + ", not " +
              category_flag(category) + ".";
      return false;
    }
  }
  return true;
}

/// Log runtime plugin diagnostics so stage-loading problems are visible, in the
/// same spirit as the process command.
void log_plugin_diagnostics(const ProjectPresenter& presenter) {
  const auto loaded = presenter.listLoadedPlugins();
  if (!loaded.empty()) {
    ORC_LOG_DEBUG("Loaded {} runtime stage plugin(s)", loaded.size());
  }
  for (const auto& diagnostic : presenter.listPluginDiagnostics()) {
    const std::string message =
        diagnostic.path.empty()
            ? diagnostic.message
            : diagnostic.message + " [" + diagnostic.path + "]";
    switch (diagnostic.severity) {
      case orc::presenters::PluginDiagnosticSeverity::Info:
        ORC_LOG_DEBUG("Plugin runtime: {}", message);
        break;
      case orc::presenters::PluginDiagnosticSeverity::Warning:
        ORC_LOG_WARN("Plugin runtime: {}", message);
        break;
      case orc::presenters::PluginDiagnosticSeverity::Error:
        ORC_LOG_ERROR("Plugin runtime: {}", message);
        break;
    }
  }
}

}  // namespace

int filter_command(const FilterOptions& options) {
  const auto stage_index = build_stage_index();

  std::string error;
  if (!validate_triad_segment(options.input_stages, StageCategory::kInput,
                              stage_index, error) ||
      !validate_triad_segment(options.filters_stages, StageCategory::kFilters,
                              stage_index, error) ||
      !validate_triad_segment(options.output_stages, StageCategory::kOutput,
                              stage_index, error)) {
    ORC_LOG_ERROR("{}", error);
    return 1;
  }

  std::vector<std::string> segments;
  for (const auto& segment :
       {options.input_stages, options.filters_stages, options.output_stages}) {
    if (!segment.empty()) {
      segments.push_back(segment);
    }
  }
  std::string combined_graph;
  for (size_t i = 0; i < segments.size(); ++i) {
    if (i > 0) {
      combined_graph += ",";
    }
    combined_graph += segments[i];
  }

  ProjectPresenter presenter;
  presenter.setProjectName("filtergraph");

  // Parsing, stage/parameter validation, video-format/source-type
  // auto-detection, and node/edge construction are all shared with the GUI
  // (any "paste a CLI command" feature would call the very same function).
  const orc::presenters::FiltergraphImportResult import_result =
      orc::presenters::import_filtergraph_into_project(presenter,
                                                       combined_graph);

  for (const auto& warning : import_result.warnings) {
    ORC_LOG_WARN("{}", warning);
  }
  if (!import_result.ok) {
    for (const auto& error : import_result.errors) {
      ORC_LOG_ERROR("{}", error);
    }
    return 1;
  }

  ORC_LOG_INFO("Parsed filtergraph successfully");
  log_plugin_diagnostics(presenter);

  // --export-project: save the assembled project instead of running it.
  // This calls the presenter's existing saveProject() (the same YAML writer
  // used elsewhere), so it's a different entry point into the existing save
  // path, not a new one.
  if (!options.export_project_path.empty()) {
    // Running in memory tolerates an undetermined video format (the core's
    // own compatibility checks are simply skipped), but a saved .orcprj file
    // is not: the loader requires an explicit NTSC/PAL/PAL-M value and
    // rejects a project without one. If none of the stages used gave any
    // format hint (e.g. tbc_source, which reads its own format from its
    // metadata sidecar rather than the project), --video-format lets the
    // export succeed anyway.
    if (presenter.getVideoFormat() == orc::presenters::VideoFormat::Unknown) {
      if (options.export_video_format.empty()) {
        ORC_LOG_ERROR(
            "Cannot export: none of the stages used imply a video format "
            "(NTSC/PAL/PAL-M), so the saved project would fail to reload. "
            "Pass --video-format NTSC|PAL|PAL-M to set it explicitly for "
            "the export, or run the pipeline directly instead of "
            "exporting.");
        return 1;
      }
      orc::presenters::VideoFormat forced =
          orc::presenters::VideoFormat::Unknown;
      if (!parse_video_format_name(options.export_video_format, forced)) {
        ORC_LOG_ERROR(
            "Unknown --video-format value '{}': expected NTSC, PAL, or "
            "PAL-M",
            options.export_video_format);
        return 1;
      }
      presenter.setVideoFormat(forced);
    }
    if (!presenter.saveProject(options.export_project_path)) {
      ORC_LOG_ERROR("Failed to save project to '{}'",
                    options.export_project_path);
      return 1;
    }
    ORC_LOG_INFO("Project saved to '{}'", options.export_project_path);
    return 0;
  }

  // Validate the assembled project before running.
  if (!presenter.validateProject()) {
    for (const auto& error : presenter.getValidationErrors()) {
      ORC_LOG_ERROR("Validation: {}", error);
    }
    ORC_LOG_ERROR("Filtergraph did not produce a valid project");
    return 1;
  }

  // Trigger all sinks with console progress reporting.
  size_t last_percent = 0;
  auto progress_callback = [&last_percent](size_t current, size_t total,
                                           const std::string& message) {
    if (total > 0) {
      size_t percent = (current * 100) / total;
      if (percent >= last_percent + 5 || current == total) {
        ORC_LOG_INFO("[Progress: {}%] {}", percent, message);
        last_percent = percent;
      }
    }
  };

  const bool all_success = presenter.triggerAllSinks(progress_callback);
  return all_success ? 0 : 1;
}

}  // namespace cli
}  // namespace orc
