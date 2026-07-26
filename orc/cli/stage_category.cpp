/*
 * File:        stage_category.cpp
 * Module:      orc-cli
 * Purpose:     Implementation of shared stage-category classification.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "stage_category.h"

#include "project_presenter.h"

namespace orc {
namespace cli {

using orc::presenters::ProjectPresenter;
using orc::presenters::StageInfo;

const char* category_flag(StageCategory category) {
  switch (category) {
    case StageCategory::kInput:
      return "--input";
    case StageCategory::kFilters:
      return "--filters";
    case StageCategory::kOutput:
      return "--output";
  }
  return "?";
}

const char* category_label(StageCategory category) {
  switch (category) {
    case StageCategory::kInput:
      return "input (source)";
    case StageCategory::kFilters:
      return "processing";
    case StageCategory::kOutput:
      return "output (sink)";
  }
  return "?";
}

StageCategory category_of(const StageInfo& info) {
  if (info.is_source) return StageCategory::kInput;
  if (info.is_sink) return StageCategory::kOutput;
  return StageCategory::kFilters;
}

const char* actual_role_description(const StageInfo& info) {
  if (info.is_source) return "an input (source) stage";
  if (info.is_sink) return "an output (sink) stage";
  return "a processing stage";
}

const char* suggested_flag_for(const StageInfo& info) {
  if (info.is_source) return "--input";
  if (info.is_sink) return "--output";
  return "--filters";
}

bool stage_matches_category(const StageInfo& info, StageCategory category) {
  switch (category) {
    case StageCategory::kInput:
      return info.is_source;
    case StageCategory::kFilters:
      return !info.is_source && !info.is_sink;
    case StageCategory::kOutput:
      return info.is_sink;
  }
  return false;
}

std::map<std::string, StageInfo> build_stage_index() {
  std::map<std::string, StageInfo> index;
  for (auto& info : ProjectPresenter::getAllStages()) {
    index.emplace(info.name, info);
  }
  return index;
}

}  // namespace cli
}  // namespace orc
