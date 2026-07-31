/*
 * File:        plugin_index.h
 * Module:      orc-core
 * Purpose:     Data model and YAML parser for the curated plugin index
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#if defined(ORC_GUI_BUILD) || defined(ORC_CLI_BUILD)
#error \
    "plugin_index.h is a core-only header. Access plugin discovery through ProjectPresenter."
#endif

#include <string>
#include <vector>

namespace orc {

/// Schema major version understood by this host. Additions within a major
/// version are non-breaking (unknown fields are ignored); a higher major is
/// parsed best-effort with a warning so older hosts tolerate newer indexes.
/// Schema 2 dropped pinned artifacts: an index entry names a plugin and its
/// source repository, and the host resolves the latest published release at
/// runtime.
inline constexpr int kPluginIndexSchemaVersion = 2;

/**
 * @brief One plugin as advertised by the curated index.
 *
 * The index curates *which* plugins are offered, not their versions: the
 * host resolves the current release from `source_repo_url` at browse and
 * install time.
 */
struct PluginIndexEntry {
  std::string id;            ///< Unique plugin identifier.
  std::string display_name;  ///< Human-readable name.
  std::string description;   ///< Short description.
  std::vector<std::string> tags;
  std::string maintainer;
  std::string license_spdx;
  std::string source_repo_url;  ///< GitHub repository the host installs from.
};

/**
 * @brief Parsed curated plugin index.
 */
struct PluginIndex {
  int schema_version = 0;
  std::vector<PluginIndexEntry> entries;
};

/**
 * @brief Outcome of parsing an index document.
 */
struct PluginIndexParseResult {
  bool success = false;
  PluginIndex index;
  std::vector<std::string> warnings;
  std::string error_message;
};

/**
 * @brief Parse a curated plugin index from YAML text.
 *
 * Forward-compatible: unknown top-level, per-entry, and per-artifact fields are
 * ignored, and a schema major greater than the host's known version is parsed
 * best-effort with a warning. Malformed YAML fails with an error_message.
 */
PluginIndexParseResult parse_plugin_index_yaml(const std::string& yaml_text);

}  // namespace orc
