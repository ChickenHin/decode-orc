/*
 * File:        plugin_index_client.cpp
 * Module:      orc-core
 * Purpose:     Fetch, cache, resolve and search the curated plugin index
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "include/plugin_index_client.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <utility>

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

std::string env_or_empty(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

std::filesystem::path resolve_default_config_dir() {
#if defined(_WIN32)
  const std::string appdata = env_or_empty("APPDATA");
  if (!appdata.empty()) {
    return std::filesystem::path(appdata) / "decode-orc";
  }
  const std::string userprofile = env_or_empty("USERPROFILE");
  if (!userprofile.empty()) {
    return std::filesystem::path(userprofile) / "AppData" / "Roaming" /
           "decode-orc";
  }
#else
  const std::string xdg_config_home = env_or_empty("XDG_CONFIG_HOME");
  if (!xdg_config_home.empty()) {
    return std::filesystem::path(xdg_config_home) / "decode-orc";
  }
  const std::string home = env_or_empty("HOME");
  if (!home.empty()) {
    return std::filesystem::path(home) / ".config" / "decode-orc";
  }
#endif
  return std::filesystem::current_path() / ".decode-orc";
}

}  // namespace

PluginIndexClient::PluginIndexClient(const IHttpFetcher& fetcher,
                                     CacheLoader cache_loader,
                                     CacheSaver cache_saver)
    : fetcher_(fetcher),
      cache_loader_(std::move(cache_loader)),
      cache_saver_(std::move(cache_saver)) {}

PluginIndexClient::RefreshResult PluginIndexClient::refresh(
    const std::string& index_url) const {
  RefreshResult result;

  const HttpFetchResult response = fetcher_.fetch(index_url);
  if (response.success) {
    const PluginIndexParseResult parsed =
        parse_plugin_index_yaml(response.body);
    if (parsed.success) {
      if (cache_saver_) {
        cache_saver_(response.body);
      }
      result.success = true;
      result.index = parsed.index;
      result.warnings = parsed.warnings;
      return result;
    }
    // A fetched-but-unparseable index falls through to the cached copy.
    result.warnings.push_back(parsed.error_message);
  }

  // Network or parse failure: fall back to the last-good cached copy.
  result.offline = true;
  if (cache_loader_) {
    if (const std::optional<std::string> cached = cache_loader_()) {
      const PluginIndexParseResult parsed = parse_plugin_index_yaml(*cached);
      if (parsed.success) {
        result.success = true;
        result.from_cache = true;
        result.index = parsed.index;
        for (auto& w : parsed.warnings) {
          result.warnings.push_back(std::move(w));
        }
        return result;
      }
    }
  }

  result.error_message =
      response.success
          ? "Fetched plugin index could not be parsed and no valid cache exists"
          : response.error_message;
  return result;
}

std::vector<PluginIndexEntry> PluginIndexClient::search(
    const PluginIndex& index, const std::string& term) {
  const std::string needle = to_lower(term);
  if (needle.empty()) {
    return index.entries;
  }
  std::vector<PluginIndexEntry> matches;
  for (const auto& entry : index.entries) {
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

const PluginIndexEntry* PluginIndexClient::find(const PluginIndex& index,
                                                const std::string& id) {
  for (const auto& entry : index.entries) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

std::string PluginIndexClient::default_index_url() {
  const std::string override_url = env_or_empty("ORC_PLUGIN_INDEX_URL");
  if (!override_url.empty()) {
    return override_url;
  }
  // The curated index currently lives alongside the host in the decode-orc
  // repository; it is read from the default branch head so merges publish
  // immediately. This is expected to move to a dedicated repository later.
  return "https://raw.githubusercontent.com/decode-orc/decode-orc/main/"
         "orc-plugin-registry/index.yaml";
}

std::string PluginIndexClient::default_cache_path() {
  return (resolve_default_config_dir() / "plugin-index-cache.yaml").string();
}

}  // namespace orc
