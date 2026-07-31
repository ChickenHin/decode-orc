/*
 * File:        stage_category.cpp
 * Module:      orc-cli
 * Purpose:     Implementation of shared stage-category classification.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "stage_category.h"

#include <cctype>

#include "project_presenter.h"

namespace orc {
namespace cli {

using orc::presenters::ProjectPresenter;
using orc::presenters::StageInfo;
using orc::presenters::VideoFormat;

const char* category_flag(StageCategory category) {
  switch (category) {
    case StageCategory::kInput:
      return "--source";
    case StageCategory::kFilters:
      return "--filters";
    case StageCategory::kOutput:
      return "--sink";
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
  return category_flag(category_of(info));
}

bool stage_matches_category(const StageInfo& info, StageCategory category) {
  return category_of(info) == category;
}

std::map<std::string, StageInfo> build_stage_index() {
  std::map<std::string, StageInfo> index;
  for (auto& info : ProjectPresenter::getAllStages()) {
    index.emplace(info.name, info);
  }
  return index;
}

bool parse_video_format_name(const std::string& text, VideoFormat* out) {
  std::string upper;
  upper.reserve(text.size());
  for (char c : text) {
    upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  if (upper == "NTSC") {
    *out = VideoFormat::NTSC;
    return true;
  }
  if (upper == "PAL") {
    *out = VideoFormat::PAL;
    return true;
  }
  if (upper == "PAL-M" || upper == "PAL_M" || upper == "PALM") {
    *out = VideoFormat::PAL_M;
    return true;
  }
  return false;
}

}  // namespace cli
}  // namespace orc
