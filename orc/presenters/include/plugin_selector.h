/*
 * File:        plugin_selector.h
 * Module:      orc-presenters
 * Purpose:     One canonical handle per plugin registry entry, and its lookup
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <string>
#include <vector>

#include "project_presenter_types.h"

namespace orc::presenters {

/// Prefix marking a selector whose payload is a filesystem path.
inline constexpr const char* kPluginSelectorPathPrefix = "path:";

/// Prefix marking a selector whose payload is a release asset URL.
inline constexpr const char* kPluginSelectorUrlPrefix = "url:";

/// Prefix marking a selector that addresses an entry by its position in the
/// registry. Last resort for an entry that carries no usable identity at all.
inline constexpr const char* kPluginSelectorIndexPrefix = "index:";

/**
 * @brief Build the canonical handle for a registry entry.
 *
 * The plugin id when there is one, else a `path:` selector, else a `url:`
 * selector. Returns an empty string only when the entry carries no identity of
 * any kind; use the ordinal overload to get a never-empty selector.
 */
std::string makePluginSelector(const std::string& plugin_id,
                               const std::string& path,
                               const std::string& release_asset_url);

/**
 * @brief Build the canonical handle for a registry entry, never empty.
 *
 * @param entry Entry to address.
 * @param ordinal Position of @p entry in the registry, used only when the
 *                entry has no id, path or asset URL to name it by.
 */
std::string makePluginSelector(const PluginRegistryEntryInfo& entry,
                               size_t ordinal);

/**
 * @brief Resolve a user-supplied selector against a list of registry entries.
 *
 * Matching is tiered so that ids stay authoritative: an id (or an entry's own
 * canonical selector) is tried first, then paths, then release asset URLs. A
 * prefixed selector (`path:`, `url:`, `index:`) is matched against that field
 * only. Bare paths and asset URLs resolve too, so a line copied out of a
 * listing works without the user stripping the prefix.
 *
 * Matching more than one entry within a tier is reported as Ambiguous with the
 * candidates' unambiguous selectors, never resolved by guessing.
 */
PluginSelectorResolution resolvePluginSelector(
    const std::vector<PluginRegistryEntryInfo>& entries,
    const std::string& selector);

/**
 * @brief Resolve a selector, also honouring loaded plugins' runtime ids.
 *
 * Identical to the registry-only overload, with one extra last tier: a
 * selector equal to a loaded plugin's runtime id resolves onto the id-less
 * registry row recording that plugin's path. That happens when a plugin was
 * added by file before its id was known — the listing prints the runtime id,
 * so every command must accept it. Every resolution path (info, dry-run and
 * all mutations) goes through this overload so they can never disagree about
 * which entry a selector means; matches found this way set
 * @ref PluginSelectorResolution::resolved_via_runtime_id.
 */
PluginSelectorResolution resolvePluginSelector(
    const std::vector<PluginRegistryEntryInfo>& entries,
    const std::vector<LoadedPluginInfo>& loaded_plugins,
    const std::string& selector);

/// Human-readable "matches more than one plugin" message listing candidates.
std::string describeAmbiguousPluginSelector(
    const std::string& selector, const std::vector<std::string>& candidates);

}  // namespace orc::presenters
