/*
 * File:        plugin_load_state.h
 * Module:      orc-presenters
 * Purpose:     Derive whether a registered plugin will load, and why not
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <string>

#include "project_presenter_types.h"

namespace orc::presenters {

/**
 * @brief Compute the single load state for a registry entry.
 *
 * The states are ranked by what actually blocks loading, most fundamental
 * first: a core plugin always loads; an artifact that cannot run here (wrong
 * ABI, binary absent) is reported as such whatever the user's enable/trust
 * flags say, because changing those flags would not make it load; only then do
 * the user's own decisions (trust, then enabled) apply.
 *
 * @param entry Registry entry as read from the persistent registry.
 * @param detail Optional out-parameter receiving the lower-case detail clause
 *               for the returned state (empty when the state needs none).
 */
PluginLoadState computePluginLoadState(const PluginRegistryEntryInfo& entry,
                                       std::string* detail = nullptr);

/// Canonical short label for a load state (from plugin_ux_strings.h).
const char* pluginLoadStateLabel(PluginLoadState state);

/// Stable lower-case identifier for machine-readable output.
const char* pluginLoadStateId(PluginLoadState state);

/// "Label — detail" for display, or just the label when there is no detail.
std::string pluginLoadStateSummary(PluginLoadState state,
                                   const std::string& detail);

}  // namespace orc::presenters
