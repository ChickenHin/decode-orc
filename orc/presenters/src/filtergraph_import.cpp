/*
 * File:        filtergraph_import.cpp
 * Module:      orc-presenters
 * Purpose:     Implementation of the shared filtergraph-to-project builder.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "filtergraph_import.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "filtergraph_parser.h"
#include "i_project_presenter.h"
#include "project_presenter_types.h"

namespace orc {
namespace presenters {

namespace {

using orc::cli::FilterStage;

/// Result of auto-detecting the project's video format from its stages.
struct VideoFormatInference {
  VideoFormat format = VideoFormat::Unknown;
  bool conflict = false;
  std::string conflict_detail;
};

/**
 * Auto-detect the project's video format from the stages actually used,
 * with no naming assumptions: a stage's format exclusivity comes from its
 * formally declared VideoFormatCompatibility (NTSC_ONLY / PAL_ONLY /
 * PAL_M_ONLY), the same metadata the GUI uses to filter its "Add Stage"
 * menu. Stages compatible with every format (the common case: transforms,
 * sinks, and dual-mode sources like tbc_source) contribute no signal.
 */
VideoFormatInference infer_video_format(
    IProjectPresenter& presenter, const std::vector<FilterStage>& stages) {
  VideoFormatInference result;

  std::set<std::string> ntsc_names, pal_names, pal_m_names;
  for (auto& s : presenter.listAvailableStagesForFormat(VideoFormat::NTSC)) {
    ntsc_names.insert(s.name);
  }
  for (auto& s : presenter.listAvailableStagesForFormat(VideoFormat::PAL)) {
    pal_names.insert(s.name);
  }
  for (auto& s : presenter.listAvailableStagesForFormat(VideoFormat::PAL_M)) {
    pal_m_names.insert(s.name);
  }

  std::string detected_from;
  for (const auto& stage : stages) {
    const bool in_ntsc = ntsc_names.count(stage.stage_name) != 0;
    const bool in_pal = pal_names.count(stage.stage_name) != 0;
    const bool in_pal_m = pal_m_names.count(stage.stage_name) != 0;
    const int membership =
        (in_ntsc ? 1 : 0) + (in_pal ? 1 : 0) + (in_pal_m ? 1 : 0);
    if (membership == 0 || membership == 3) {
      continue;  // Not a registered stage, or compatible with every format.
    }
    const VideoFormat this_format =
        in_ntsc ? VideoFormat::NTSC
                : (in_pal ? VideoFormat::PAL : VideoFormat::PAL_M);
    if (result.format == VideoFormat::Unknown) {
      result.format = this_format;
      detected_from = stage.stage_name;
    } else if (result.format != this_format) {
      result.conflict = true;
      result.conflict_detail = "stage '" + detected_from +
                               "' requires one video format but '" +
                               stage.stage_name + "' requires another";
      return result;
    }
  }
  return result;
}

/// Result of auto-detecting the project's source signal type from its stages.
struct SourceFormatInference {
  SourceType format = SourceType::Unknown;
  bool conflict = false;
  std::string conflict_detail;
};

/**
 * Auto-detect the project's source signal type (composite vs Y/C) from the
 * parameters actually supplied to source stages: y_path/c_path implies Y/C,
 * input_path implies composite — the same convention every dual-mode source
 * stage (tbc_source, the CVBS sources) already follows, so this works
 * without hardcoding any specific stage name. A stage name containing "YC"
 * is used as a fallback hint, mirroring the core's own heuristic.
 */
SourceFormatInference infer_source_format(
    const std::vector<FilterStage>& stages,
    const std::map<std::string, StageInfo>& stage_index) {
  SourceFormatInference result;

  std::string detected_from;
  for (const auto& stage : stages) {
    auto it = stage_index.find(stage.stage_name);
    if (it == stage_index.end() || !it->second.is_source) {
      continue;
    }

    SourceType this_format = SourceType::Unknown;
    const bool has_yc_path =
        stage.params.count("y_path") != 0 || stage.params.count("c_path") != 0;
    const bool has_input_path = stage.params.count("input_path") != 0;
    const bool name_implies_yc =
        stage.stage_name.find("YC") != std::string::npos && !has_input_path;
    if (has_yc_path || name_implies_yc) {
      this_format = SourceType::YC;
    } else if (has_input_path) {
      this_format = SourceType::Composite;
    }
    if (this_format == SourceType::Unknown) {
      continue;
    }

    if (result.format == SourceType::Unknown) {
      result.format = this_format;
      detected_from = stage.stage_name;
    } else if (result.format != this_format) {
      result.conflict = true;
      result.conflict_detail = "source '" + detected_from +
                               "' looks like one signal type but '" +
                               stage.stage_name + "' looks like another";
      return result;
    }
  }
  return result;
}

}  // namespace

FiltergraphImportResult import_filtergraph_into_project(
    IProjectPresenter& presenter, const std::string& filtergraph) {
  FiltergraphImportResult result;

  orc::cli::FilterGraphParseResult parsed =
      orc::cli::parse_filtergraph(filtergraph);
  if (!parsed.ok) {
    result.errors.push_back("Failed to parse filtergraph: " + parsed.error);
    return result;
  }

  // Validate stage names up front so callers get a clear message rather
  // than an opaque failure deep in DAG construction.
  for (const auto& stage : parsed.graph.stages) {
    if (!presenter.stageExists(stage.stage_name)) {
      result.errors.push_back("Unknown stage '" + stage.stage_name + "'.");
    }
  }
  if (!result.errors.empty()) {
    return result;
  }

  // Build a stage-name -> StageInfo index once, reused for source-format
  // inference and parameter validation below.
  std::map<std::string, StageInfo> stage_index;
  for (auto& info : presenter.listAllStages()) {
    stage_index.emplace(info.name, info);
  }

  // Auto-detect video format and source signal type from the stages
  // actually used — these are properties of the modules, not something the
  // caller should have to state separately.
  const VideoFormatInference video_inference =
      infer_video_format(presenter, parsed.graph.stages);
  if (video_inference.conflict) {
    result.errors.push_back("Conflicting video formats between stages: " +
                            video_inference.conflict_detail);
  }
  const SourceFormatInference source_inference =
      infer_source_format(parsed.graph.stages, stage_index);
  if (source_inference.conflict) {
    result.errors.push_back("Conflicting source signal types between stages: " +
                            source_inference.conflict_detail);
  }
  if (!result.errors.empty()) {
    return result;
  }

  // Validate parameters against each stage's descriptors. This uses only
  // the generic getStageParameters() API, so it works identically for core
  // stages and for third-party plugin stages: missing required parameters
  // are reported as errors (instead of an opaque failure later inside the
  // stage's trigger()), and unrecognised parameter names are reported as
  // warnings (likely typos) without blocking the build.
  for (const auto& stage : parsed.graph.stages) {
    const auto descriptors = presenter.getStageParameters(stage.stage_name);
    if (descriptors.empty()) {
      continue;
    }

    for (const auto& descriptor : descriptors) {
      const bool supplied =
          stage.params.find(descriptor.name) != stage.params.end();
      if (descriptor.constraints.required && !supplied) {
        result.errors.push_back("Stage '" + stage.stage_name +
                                "': missing required parameter '" +
                                descriptor.name + "'.");
      }
    }

    for (const auto& [key, value] : stage.params) {
      (void)value;
      const bool known =
          std::any_of(descriptors.begin(), descriptors.end(),
                      [&key](const auto& d) { return d.name == key; });
      if (!known) {
        result.warnings.push_back(
            "Stage '" + stage.stage_name + "': parameter '" + key +
            "' is not recognised by this stage (check for typos).");
      }
    }
  }
  if (!result.errors.empty()) {
    return result;
  }

  if (video_inference.format != VideoFormat::Unknown) {
    presenter.setVideoFormat(video_inference.format);
  }
  if (source_inference.format != SourceType::Unknown) {
    presenter.setSourceType(source_inference.format);
  }

  // Add nodes (auto-layout left to right) and their parameters, then wire up
  // edges. Core-side validation (e.g. source format compatibility) can throw
  // for any stage, including third-party plugin stages, so surface those as
  // a clean error rather than letting the exception propagate.
  try {
    std::vector<orc::NodeID> node_ids;
    node_ids.reserve(parsed.graph.stages.size());
    double x = 0.0;
    for (const auto& stage : parsed.graph.stages) {
      orc::NodeID id = presenter.addNode(stage.stage_name, x, 0.0);
      node_ids.push_back(id);
      if (!stage.params.empty()) {
        presenter.setNodeParameters(id, stage.params);
      }
      x += 200.0;
    }

    for (const auto& edge : parsed.graph.edges) {
      presenter.addEdge(node_ids[edge.first], node_ids[edge.second]);
    }
  } catch (const std::exception& e) {
    result.errors.push_back(std::string("Failed to build filtergraph: ") +
                            e.what());
    return result;
  }

  result.ok = true;
  return result;
}

}  // namespace presenters
}  // namespace orc
