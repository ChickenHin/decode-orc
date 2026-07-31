/*
 * File:        pluginbrowsemodel.cpp
 * Module:      orc-gui
 * Purpose:     Presenter-boundary model for the curated plugin browse dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "pluginbrowsemodel.h"

#include <plugin_ux_strings.h>

#include <algorithm>
#include <cctype>

namespace orc {
namespace {

std::string to_lower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool contains_ci(const std::string& haystack, const std::string& needle_lower) {
  return to_lower(haystack).find(needle_lower) != std::string::npos;
}

}  // namespace

std::vector<orc::presenters::PluginIndexEntryInfo> PluginBrowseModel::search(
    const std::string& term) const {
  const std::string needle = to_lower(term);
  if (needle.empty()) {
    return index_.entries;
  }
  std::vector<orc::presenters::PluginIndexEntryInfo> matches;
  for (const auto& entry : index_.entries) {
    bool hit = contains_ci(entry.id, needle) ||
               contains_ci(entry.display_name, needle) ||
               contains_ci(entry.description, needle);
    if (!hit) {
      for (const auto& tag : entry.tags) {
        if (contains_ci(tag, needle)) {
          hit = true;
          break;
        }
      }
    }
    if (hit) {
      matches.push_back(entry);
    }
  }
  return matches;
}

const orc::presenters::PluginRegistryEntryInfo*
PluginBrowseModel::installedEntry(const std::string& plugin_id) const {
  if (plugin_id.empty()) {
    return nullptr;
  }
  for (const auto& entry : registry_.entries) {
    if (entry.plugin_id == plugin_id) {
      return &entry;
    }
  }
  return nullptr;
}

std::string PluginBrowseModel::statusMessage() const {
  // Worded once and shared with the CLI banner, so a user who reads one
  // recognises the other.
  return plugin_ux::indexStatusMessage(index_.available, index_.offline,
                                       index_.from_cache, index_.entries.size(),
                                       index_.error_message);
}

}  // namespace orc
