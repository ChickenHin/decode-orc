/*
 * File:        plugin_index_client.h
 * Module:      orc-core
 * Purpose:     Fetch, cache, resolve and search the curated plugin index
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#if defined(ORC_GUI_BUILD) || defined(ORC_CLI_BUILD)
#error \
    "plugin_index_client.h is a core-only header. Access plugin discovery through ProjectPresenter."
#endif

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "http_fetcher.h"
#include "plugin_index.h"

namespace orc {

/**
 * @brief Fetches the curated plugin index and resolves compatible artifacts.
 *
 * The client is transport- and storage-agnostic: an IHttpFetcher performs the
 * network GET, and two callbacks handle the last-good on-disk cache. This keeps
 * the fetch/parse/fallback logic unit-testable with no filesystem or network.
 * Search and lookup are pure static helpers; release resolution happens
 * against each entry's source repository (see PluginRemoteLoader).
 */
class PluginIndexClient {
 public:
  /// Loads the cached index YAML (nullopt when no cache exists).
  using CacheLoader = std::function<std::optional<std::string>()>;
  /// Persists freshly fetched index YAML as the new last-good cache.
  using CacheSaver = std::function<void(const std::string&)>;

  PluginIndexClient(const IHttpFetcher& fetcher, CacheLoader cache_loader,
                    CacheSaver cache_saver);

  struct RefreshResult {
    bool success = false;     ///< An index (fresh or cached) is available.
    bool from_cache = false;  ///< The returned index came from the cache.
    bool offline = false;     ///< The network fetch failed.
    PluginIndex index;
    std::vector<std::string> warnings;
    std::string error_message;
  };

  /**
   * @brief Fetch the index from `index_url`, falling back to the cache.
   *
   * On a successful fetch+parse the fresh copy is written to the cache. On a
   * network or parse failure the last-good cache is returned when present.
   */
  RefreshResult refresh(const std::string& index_url) const;

  /// Case-insensitive substring search over id, name, description and tags.
  static std::vector<PluginIndexEntry> search(const PluginIndex& index,
                                              const std::string& term);

  /// Find an entry by exact id, or nullptr.
  static const PluginIndexEntry* find(const PluginIndex& index,
                                      const std::string& id);

  /// Default raw index URL (override with ORC_PLUGIN_INDEX_URL).
  static std::string default_index_url();

  /// Default cache file path for the last-good index copy.
  static std::string default_cache_path();

 private:
  const IHttpFetcher& fetcher_;
  CacheLoader cache_loader_;
  CacheSaver cache_saver_;
};

}  // namespace orc
