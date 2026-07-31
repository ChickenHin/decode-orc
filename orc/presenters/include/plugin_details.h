/*
 * File:        plugin_details.h
 * Module:      orc-presenters
 * Purpose:     One ordered description of a plugin, shared by the GUI and CLI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <string>
#include <vector>

#include "project_presenter_types.h"

namespace orc::presenters {

/// One labelled line of a plugin description.
struct PluginDetailField {
  std::string label;  ///< Canonical field label from plugin_ux_strings.h.
  std::string value;  ///< Rendered value; never empty (empty fields are
                      ///< omitted from the list instead).
};

/**
 * @brief Describe a plugin as an ordered list of labelled fields.
 *
 * Both front ends render this list rather than assembling their own, so the
 * Plugin Manager's details pane, the browse dialog's details pane and
 * `orc-cli plugins info` name the same things in the same order. Fields with
 * nothing to say are omitted, so an index-only entry and an installed entry
 * both produce a coherent list from the one function.
 *
 * @param installed Registry entry for the installed copy, or nullptr when the
 *                  plugin is not in the local registry.
 * @param indexed Curated-index entry, or nullptr when the plugin is not
 *                offered by the index (a locally added or core plugin).
 * @param update Result of an update check for @p installed, or nullptr when no
 *               check has been made (the default: checking costs a network
 *               request).
 */
std::vector<PluginDetailField> makePluginDetails(
    const PluginRegistryEntryInfo* installed,
    const PluginIndexEntryInfo* indexed, const PluginUpdateStatusInfo* update);

/**
 * @brief Describe a bundled plugin that has no registry entry.
 *
 * Core plugins ship with the application and are discovered at runtime rather
 * than registered, so the registry knows nothing about them — yet stages name
 * them as their owning plugin, and that id has to resolve like any other
 * plugin selector. This turns what the runtime does know into the same entry
 * shape the registry produces, so one description path covers both.
 *
 * @param loaded Plugin as reported by the running stage registry.
 */
PluginRegistryEntryInfo makeEntryForLoadedPlugin(
    const LoadedPluginInfo& loaded);

/// The words the GUI's Update column and the CLI's `update:` field share.
std::string pluginUpdateStatusLabel(const PluginUpdateStatusInfo& status);

/// Stable lower-case identifier for the same outcome, for machine-readable
/// output. Unlike the label it carries no version number, so a script matches
/// on the outcome and reads the version from its own field.
const char* pluginUpdateStatusId(PluginUpdateStatus status);

/// Canonical severity word for a runtime plugin diagnostic.
const char* pluginDiagnosticSeverityLabel(PluginDiagnosticSeverity severity);

/// Stable lower-case identifier for machine-readable output.
const char* pluginDiagnosticSeverityId(PluginDiagnosticSeverity severity);

/// "Severity: message [path]" — the one rendering of a diagnostic, so the
/// Plugin Manager's Diagnostics section and `plugins doctor` read identically.
std::string formatPluginDiagnostic(const PluginDiagnosticInfo& diagnostic);

}  // namespace orc::presenters
