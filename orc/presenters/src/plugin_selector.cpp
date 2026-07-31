/*
 * File:        plugin_selector.cpp
 * Module:      orc-presenters
 * Purpose:     One canonical handle per plugin registry entry, and its lookup
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "plugin_selector.h"

#include <algorithm>
#include <functional>

namespace orc::presenters {

namespace {

bool starts_with(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() &&
         text.compare(0, prefix.size(), prefix) == 0;
}

std::string trim(const std::string& text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return std::string();
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

/// The selector an entry advertises. Falls back to deriving it so callers can
/// resolve against entries they built themselves (tests, in-memory fixtures).
std::string selectorOf(const PluginRegistryEntryInfo& entry, size_t ordinal) {
  return entry.selector.empty() ? makePluginSelector(entry, ordinal)
                                : entry.selector;
}

/// Collect the indices of every entry satisfying @p matches.
std::vector<size_t> collectMatches(
    const std::vector<PluginRegistryEntryInfo>& entries,
    const std::function<bool(const PluginRegistryEntryInfo&)>& matches) {
  std::vector<size_t> hits;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (matches(entries[i])) {
      hits.push_back(i);
    }
  }
  return hits;
}

PluginSelectorResolution finish(
    const std::vector<PluginRegistryEntryInfo>& entries,
    const std::vector<size_t>& hits) {
  PluginSelectorResolution resolution;
  if (hits.size() == 1) {
    resolution.status = PluginSelectorStatus::Resolved;
    resolution.entry_index = hits.front();
    resolution.entry = entries[hits.front()];
    return resolution;
  }

  resolution.status = PluginSelectorStatus::Ambiguous;
  for (const size_t index : hits) {
    std::string candidate = selectorOf(entries[index], index);
    if (std::find(resolution.candidates.begin(), resolution.candidates.end(),
                  candidate) == resolution.candidates.end()) {
      resolution.candidates.push_back(std::move(candidate));
    } else {
      // Two entries that would print the same selector cannot be told apart by
      // it; offer the positional form so the user can still address each one.
      resolution.candidates.push_back(std::string(kPluginSelectorIndexPrefix) +
                                      std::to_string(index));
    }
  }
  return resolution;
}

}  // namespace

std::string makePluginSelector(const std::string& plugin_id,
                               const std::string& path,
                               const std::string& release_asset_url) {
  if (!plugin_id.empty()) {
    return plugin_id;
  }
  if (!path.empty()) {
    return std::string(kPluginSelectorPathPrefix) + path;
  }
  if (!release_asset_url.empty()) {
    return std::string(kPluginSelectorUrlPrefix) + release_asset_url;
  }
  return std::string();
}

std::string makePluginSelector(const PluginRegistryEntryInfo& entry,
                               size_t ordinal) {
  std::string selector = makePluginSelector(
      entry.plugin_id, entry.path.empty() ? entry.local_dev_path : entry.path,
      entry.release_asset_url);
  if (selector.empty()) {
    // A hand-edited registry can hold an entry with no identity at all. It
    // still has to be addressable, or the user cannot remove it.
    selector =
        std::string(kPluginSelectorIndexPrefix) + std::to_string(ordinal);
  }
  return selector;
}

PluginSelectorResolution resolvePluginSelector(
    const std::vector<PluginRegistryEntryInfo>& entries,
    const std::string& selector) {
  PluginSelectorResolution not_found;
  const std::string wanted = trim(selector);
  if (wanted.empty()) {
    return not_found;
  }

  const std::string path_prefix(kPluginSelectorPathPrefix);
  const std::string url_prefix(kPluginSelectorUrlPrefix);
  const std::string index_prefix(kPluginSelectorIndexPrefix);

  if (starts_with(wanted, path_prefix)) {
    const std::string wanted_path = wanted.substr(path_prefix.size());
    if (wanted_path.empty()) {
      return not_found;
    }
    const auto hits = collectMatches(
        entries, [&wanted_path](const PluginRegistryEntryInfo& entry) {
          return entry.path == wanted_path ||
                 (entry.path.empty() && entry.local_dev_path == wanted_path);
        });
    return hits.empty() ? not_found : finish(entries, hits);
  }

  if (starts_with(wanted, url_prefix)) {
    const std::string wanted_url = wanted.substr(url_prefix.size());
    if (wanted_url.empty()) {
      return not_found;
    }
    const auto hits = collectMatches(
        entries, [&wanted_url](const PluginRegistryEntryInfo& entry) {
          return entry.release_asset_url == wanted_url;
        });
    return hits.empty() ? not_found : finish(entries, hits);
  }

  if (starts_with(wanted, index_prefix)) {
    const std::string ordinal_text = wanted.substr(index_prefix.size());
    if (ordinal_text.empty() ||
        ordinal_text.find_first_not_of("0123456789") != std::string::npos) {
      return not_found;
    }
    const size_t ordinal = static_cast<size_t>(std::stoull(ordinal_text));
    if (ordinal >= entries.size()) {
      return not_found;
    }
    return finish(entries, {ordinal});
  }

  // Unprefixed: ids first so they stay authoritative, then the payload of the
  // prefixed forms so a pasted path or URL resolves without its prefix.
  auto hits =
      collectMatches(entries, [&wanted](const PluginRegistryEntryInfo& entry) {
        return (!entry.plugin_id.empty() && entry.plugin_id == wanted) ||
               (!entry.selector.empty() && entry.selector == wanted);
      });
  if (!hits.empty()) {
    return finish(entries, hits);
  }

  hits = collectMatches(entries, [&wanted](
                                     const PluginRegistryEntryInfo& entry) {
    return (!entry.path.empty() && entry.path == wanted) ||
           (!entry.local_dev_path.empty() && entry.local_dev_path == wanted);
  });
  if (!hits.empty()) {
    return finish(entries, hits);
  }

  hits =
      collectMatches(entries, [&wanted](const PluginRegistryEntryInfo& entry) {
        return !entry.release_asset_url.empty() &&
               entry.release_asset_url == wanted;
      });
  if (!hits.empty()) {
    return finish(entries, hits);
  }

  return not_found;
}

PluginSelectorResolution resolvePluginSelector(
    const std::vector<PluginRegistryEntryInfo>& entries,
    const std::vector<LoadedPluginInfo>& loaded_plugins,
    const std::string& selector) {
  auto resolution = resolvePluginSelector(entries, selector);
  if (resolution.status != PluginSelectorStatus::NotFound) {
    return resolution;
  }

  const std::string wanted = trim(selector);
  const auto loaded_it = std::find_if(
      loaded_plugins.begin(), loaded_plugins.end(),
      [&wanted](const LoadedPluginInfo& plugin) {
        return !plugin.plugin_id.empty() && plugin.plugin_id == wanted;
      });
  if (loaded_it == loaded_plugins.end()) {
    return resolution;
  }

  const auto hits = collectMatches(
      entries, [&loaded_it](const PluginRegistryEntryInfo& entry) {
        return entry.plugin_id.empty() && !entry.path.empty() &&
               entry.path == loaded_it->path;
      });
  if (hits.empty()) {
    return resolution;
  }
  resolution = finish(entries, hits);
  resolution.resolved_via_runtime_id = true;
  return resolution;
}

std::string describeAmbiguousPluginSelector(
    const std::string& selector, const std::vector<std::string>& candidates) {
  std::string message =
      "Selector '" + selector + "' matches more than one plugin; use one of: ";
  for (size_t i = 0; i < candidates.size(); ++i) {
    message += (i == 0 ? "" : ", ");
    message += candidates[i];
  }
  return message;
}

}  // namespace orc::presenters
