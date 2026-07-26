/*
 * File:        stage_category.h
 * Module:      orc-cli
 * Purpose:     Shared classification of stages into the three categories
 *              the CLI presents them under (input/filters/output), used by
 *              both the filtergraph triad validator and the plugin/stage
 *              listing commands.
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
/// mirrors the --input/--filters/--output triad, and is also used to group
/// `orc-cli plugins stages`/`inputs`/`outputs`/`filters` output.
enum class StageCategory { kInput, kFilters, kOutput };

/// The CLI flag associated with a category (e.g. "--input").
const char* category_flag(StageCategory category);

/// Human-readable label for a category (e.g. "input (source)"), used as a
/// role tag in listings and error messages.
const char* category_label(StageCategory category);

/// A stage's category is never guessed from its name: it comes straight
/// from the same is_source/is_sink metadata the GUI uses, so this works
/// identically for core stages and for any third-party plugin stage.
StageCategory category_of(const orc::presenters::StageInfo& info);

/// Human-readable description of the category a stage actually belongs to,
/// for error messages ("X is <this>, not <expected>").
const char* actual_role_description(const orc::presenters::StageInfo& info);

/// Which CLI flag a stage's actual role belongs under (e.g. "--output").
const char* suggested_flag_for(const orc::presenters::StageInfo& info);

/// Whether `info` belongs to `category`.
bool stage_matches_category(const orc::presenters::StageInfo& info,
                            StageCategory category);

/// Stage name -> StageInfo, built once per run (core stages + loaded
/// plugins).
std::map<std::string, orc::presenters::StageInfo> build_stage_index();

}  // namespace cli
}  // namespace orc
