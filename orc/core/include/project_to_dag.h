/*
 * File:        project_to_dag.h
 * Module:      orc-core
 * Purpose:     Project to DAG conversion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

// =============================================================================
// MVP Architecture Enforcement
// =============================================================================
// This header is part of the CORE internal implementation.
// GUI code must NOT include this header directly.
// DAG operations are exposed through ProjectPresenter and RenderPresenter.
// =============================================================================
#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/project_to_dag.h. Use ProjectPresenter/RenderPresenter instead."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/project_to_dag.h. DAG is managed by presenters."
#endif

#include <map>
#include <memory>
#include <stdexcept>

#include "dag_executor.h"
#include "project.h"

namespace orc {

/**
 * @brief Exception thrown during Project-to-DAG conversion
 */
class ProjectConversionError : public std::runtime_error {
 public:
  explicit ProjectConversionError(const std::string& msg)
      : std::runtime_error(msg) {}
};

/**
 * @brief Convert a Project to an executable DAG
 *
 * This function bridges the gap between serializable Projects (strings, data)
 * and executable DAGs (C++ objects, stages).
 *
 * Conversion process:
 * 1. Create DAG nodes by instantiating stages from the stage registry
 * 2. SOURCE nodes use the configured source stage which loads source files from
 * parameters
 * 3. Set up edges and dependencies
 * 4. Validate the resulting DAG
 *
 * @param project The project to convert
 * @return Executable DAG ready for rendering or execution
 * @throws ProjectConversionError if conversion fails
 *
 * Example:
 * ```cpp
 * Project project = load_project("example.orc-project");
 *
 * // Convert to executable DAG
 * auto dag = project_to_dag(project);
 *
 * // Now can render frames
 * DAGFrameRenderer renderer(dag);
 * auto result = renderer.render_frame_at_node("transform_1", FrameID(42));
 * ```
 */
std::shared_ptr<DAG> project_to_dag(const Project& project);

/**
 * @brief Resolve the file-path parameters of a node against the project root
 *
 * Project files store paths as the user gave them: absolute, relative to the
 * project directory, or written with ${PROJECT_ROOT}. A stage only ever sees
 * resolved paths, so every consumer of a node's stored parameters has to run
 * them through this before handing them to a stage — otherwise a stage that
 * checks its input file (every source stage does, for its status dot) is told
 * a path that does not resolve from the process working directory and reports
 * itself unconfigured.
 *
 * Parameters whose name does not look like a path are left untouched.
 *
 * @param parameters  Parameter map, modified in place
 * @param project_root Directory the project file lives in; no-op when empty
 */
void resolve_path_parameters(std::map<std::string, ParameterValue>& parameters,
                             const std::string& project_root);

/**
 * @brief Validate that all source nodes in a DAG can be accessed
 *
 * This function attempts to execute each source node in the DAG to verify
 * that they can produce output. This is useful for validating that source
 * files exist and can be loaded.
 *
 * @param dag The DAG to validate
 * @throws ProjectConversionError if any source node fails validation
 */
void validate_source_nodes(const std::shared_ptr<DAG>& dag);

}  // namespace orc
