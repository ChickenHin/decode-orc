/*
 * File:        observation_cache_retention.h
 * Module:      orc-core
 * Purpose:     Retention policy for the per-source observation sidecar cache
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// Host-internal cache maintenance. Only orc-core and orc-presenters may
// include this header; GUI/CLI code must go through presenters.
#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/observation_cache_retention.h. Use a presenter instead."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/observation_cache_retention.h. Use a presenter instead."
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace orc {

/**
 * @brief Suffix identifying an observation sidecar database.
 *
 * Only files ending in this are ever considered for eviction, so anything else
 * a user leaves in the cache directory is left strictly alone.
 */
inline constexpr const char* kSidecarSuffix = ".orc-obs.sqlite";

/**
 * @brief One sidecar the retention pass may evict.
 */
struct SidecarEntry {
  std::string path;  ///< Absolute path of the main database file.
  /// When the sidecar was last used. Sidecars are stamped on open (see
  /// touchSidecar), so this is a genuine last-use time rather than a
  /// last-write time — a session that only reads a complete cache still
  /// renews it.
  std::chrono::system_clock::time_point last_used{};
  /// Combined size of the database and its companion files, for reporting.
  std::uintmax_t bytes = 0;
};

/**
 * @brief How much of the observation sidecar cache to keep.
 *
 * Sidecars are content-addressed per source, so they accumulate: every disc
 * ever opened leaves one behind, and a feature-length source can reach several
 * gigabytes. Nothing else prunes them — the store's own garbage collection
 * only ever works *inside* the database belonging to the open project — so
 * without this the directory grows without bound.
 *
 * The two limits answer different failure modes: the age limit retires sources
 * the user has moved on from, and the entry limit bounds the working set for
 * someone who cycles through many sources faster than the age limit retires
 * them. A sidecar is evicted when it fails either one.
 */
struct SidecarRetentionPolicy {
  /// Keep at most this many sidecars, most recently used first. 0 disables the
  /// count limit.
  std::size_t max_entries = 10;
  /// Evict sidecars unused for longer than this. Zero disables the age limit.
  std::chrono::seconds max_age = std::chrono::hours(24 * 30);
};

/**
 * @brief Choose which sidecars to evict. Pure: no filesystem, no clock.
 *
 * @p entries need not be sorted. @p keep_path is never evicted whatever the
 * limits say — it is the sidecar the caller is about to open, and evicting it
 * would delete the cache the session is about to fill. Everything else is
 * evicted when it is older than the age limit, or when it ranks beyond
 * max_entries by last use (most recent first). @p keep_path occupies one of
 * those slots whether or not it is in @p entries, so the limit describes the
 * cache as it will be, not as it was.
 *
 * Ties on last-use are broken by path so the outcome is deterministic.
 *
 * @return Paths to remove, in eviction order (oldest first).
 */
std::vector<std::string> selectSidecarsToEvict(
    std::vector<SidecarEntry> entries, const SidecarRetentionPolicy& policy,
    const std::string& keep_path, std::chrono::system_clock::time_point now);

/**
 * @brief List the sidecars in @p directory, newest use first.
 *
 * Last use is taken as the newest modification time across the database and
 * its SQLite companions (-wal/-shm/-journal), so a sidecar last touched only
 * by write-ahead logging is not mistaken for stale. A missing or unreadable
 * directory yields an empty list rather than an error: cache maintenance must
 * never be able to fail an application start.
 */
std::vector<SidecarEntry> listSidecars(const std::string& directory);

/**
 * @brief Delete @p path and its SQLite companion files.
 *
 * @return Bytes reclaimed (0 if nothing could be removed). Failures are logged
 * and swallowed — a sidecar held open by another instance, or on a read-only
 * volume, simply survives to be reconsidered next time.
 */
std::uintmax_t removeSidecar(const std::string& path);

/**
 * @brief Stamp @p path as used now, so the age limit measures last use.
 *
 * Called when a sidecar is opened. Renewing on open is also what keeps a
 * sidecar that another instance is using out of reach of the limits above: an
 * open sidecar is always among the most recently used, so neither limit
 * selects it. Failures are ignored (the stamp is an optimisation, not a
 * correctness requirement).
 */
void touchSidecar(const std::string& path);

/**
 * @brief Apply @p policy to @p directory, sparing @p keep_path.
 *
 * Convenience composition of the three functions above, intended to be called
 * immediately before a sidecar is opened. Never throws.
 *
 * @return Number of sidecars removed.
 */
std::size_t enforceSidecarRetention(const std::string& directory,
                                    const SidecarRetentionPolicy& policy,
                                    const std::string& keep_path);

}  // namespace orc
