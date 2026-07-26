/*
 * File:        filtergraph_import.h
 * Module:      orc-presenters
 * Purpose:     Parse an ffmpeg-style filtergraph string and build it into a
 *              project via the presenter — shared between orc-cli
 *              (--filter/--input/--filters/--output) and any GUI feature
 *              that lets a person paste a CLI command to build a project.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <string>
#include <vector>

namespace orc {
namespace presenters {

class IProjectPresenter;

/**
 * @brief Outcome of importing a filtergraph string into a project.
 *
 * `ok` is false if parsing failed, a stage name was unrecognised, a
 * required parameter was missing, or building the DAG raised a core-side
 * validation error (e.g. a source/format incompatibility) — in every case
 * `errors` holds one or more human-readable messages and nothing was added
 * to the project. `warnings` holds non-fatal issues (currently: parameter
 * names not recognised by a stage, which may be a typo) even when `ok` is
 * true — the project is still built in that case.
 */
struct FiltergraphImportResult {
  bool ok = false;
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
};

/**
 * @brief Parse `filtergraph` and build it into `presenter`.
 *
 * Performs, in order: parsing, stage-name validation, video-format/source-
 * type auto-detection from the stages used (see the CLI's Filtergraph Mode
 * documentation for the exact rules — the same logic applies here), stage
 * parameter validation (missing required / unrecognised names), and finally
 * adds every node, sets its parameters, and wires every edge.
 *
 * Does not call `validateProject()` or trigger anything — that decision is
 * left to the caller (the CLI runs the pipeline immediately; a GUI "paste
 * command" feature would more likely leave the person to review the
 * resulting graph first).
 *
 * On failure, `presenter` is left exactly as it was: nodes are only added
 * once the input passes every validation step, so a bad paste cannot leave
 * a partially-built graph behind.
 *
 * @param presenter Project to build into. Any existing nodes are left
 * alone; the new nodes are added alongside them.
 * @param filtergraph The filtergraph string (same grammar as orc-cli's
 * `--filter`).
 * @return Result; check `.ok` before assuming the project changed.
 */
FiltergraphImportResult import_filtergraph_into_project(
    IProjectPresenter& presenter, const std::string& filtergraph);

}  // namespace presenters
}  // namespace orc
