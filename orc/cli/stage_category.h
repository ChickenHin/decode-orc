/*
 * File:        stage_category.h
 * Module:      orc-cli
 * Purpose:     Shared classification of stages into the three categories
 *              the CLI presents them under (source/filters/sink), used by
 *              the filtergraph triad validator to enforce that each flag
 *              only accepts stages of the matching role.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <map>
#include <string>

#include "project_presenter_types.h"

namespace orc {
namespace cli {

/// The three stage categories the CLI classifies every stage into. This
/// mirrors the --source/--filters/--sink triad.
enum class StageCategory { kInput, kFilters, kOutput };

/// The CLI flag associated with a category (e.g. "--source"/"-i").
const char* category_flag(StageCategory category);

/// A stage's category is never guessed from its name: it comes straight
/// from the same is_source/is_sink metadata the GUI uses, so this works
/// identically for core stages and for any third-party plugin stage.
StageCategory category_of(const orc::presenters::StageInfo& info);

/// Human-readable description of the category a stage actually belongs to,
/// for error messages ("X is <this>, not <expected>").
const char* actual_role_description(const orc::presenters::StageInfo& info);

/// Which CLI flag a stage's actual role belongs under (e.g. "--sink"/"-o").
const char* suggested_flag_for(const orc::presenters::StageInfo& info);

/// Whether `info` belongs to `category`.
bool stage_matches_category(const orc::presenters::StageInfo& info,
                            StageCategory category);

/// Stage name -> StageInfo, built once per run (core stages + loaded
/// plugins).
std::map<std::string, orc::presenters::StageInfo> build_stage_index();

/// Parse a video format name, accepting "NTSC", "PAL" or "PAL-M"
/// (case-insensitive; "PAL_M"/"PALM" too). Shared so every flag that takes a
/// format — --video-format, and the stage listing's --format — accepts exactly
/// the same spellings. Returns false, leaving @p out untouched, for anything
/// else.
bool parse_video_format_name(const std::string& text,
                             orc::presenters::VideoFormat* out);

}  // namespace cli
}  // namespace orc
