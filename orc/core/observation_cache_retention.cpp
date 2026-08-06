/*
 * File:        observation_cache_retention.cpp
 * Module:      orc-core
 * Purpose:     Retention policy for the per-source observation sidecar cache
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "observation_cache_retention.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <system_error>

namespace orc {

namespace {

// SQLite's companions to a database file. They are evicted with their parent
// and counted as evidence of recent use.
constexpr std::array<const char*, 3> kCompanionSuffixes = {"-wal", "-shm",
                                                           "-journal"};

bool has_sidecar_suffix(const std::string& filename) {
  const std::string suffix(kSidecarSuffix);
  return filename.size() > suffix.size() &&
         filename.compare(filename.size() - suffix.size(), suffix.size(),
                          suffix) == 0;
}

// file_clock's epoch is unspecified and std::chrono::clock_cast is not
// available on every toolchain this builds with, so both clocks are read once
// per listing and file timestamps are converted against that pair. The only
// error is the microseconds between the two now() calls — irrelevant against
// limits measured in days.
struct ClockAnchor {
  std::chrono::system_clock::time_point system =
      std::chrono::system_clock::now();
  std::filesystem::file_time_type file =
      std::filesystem::file_time_type::clock::now();

  // Last write time of @p path, or the epoch when it cannot be read (which
  // reads as maximally stale — a file whose timestamp is unreadable is a
  // better eviction candidate than one that is demonstrably fresh).
  std::chrono::system_clock::time_point write_time_or_epoch(
      const std::filesystem::path& path) const {
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
      return std::chrono::system_clock::time_point{};
    }
    return system +
           std::chrono::duration_cast<std::chrono::system_clock::duration>(
               stamp - file);
  }
};

std::uintmax_t size_or_zero(const std::filesystem::path& path) {
  std::error_code ec;
  const auto bytes = std::filesystem::file_size(path, ec);
  return ec ? 0 : bytes;
}

}  // namespace

std::vector<std::string> selectSidecarsToEvict(
    std::vector<SidecarEntry> entries, const SidecarRetentionPolicy& policy,
    const std::string& keep_path, std::chrono::system_clock::time_point now) {
  // The sidecar about to be opened is never a candidate, and it is not
  // examined for age either: the caller is about to use it.
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [&keep_path](const SidecarEntry& e) {
                                 return e.path == keep_path;
                               }),
                entries.end());

  // Most recently used first; path breaks ties so the result is deterministic.
  std::sort(entries.begin(), entries.end(),
            [](const SidecarEntry& a, const SidecarEntry& b) {
              if (a.last_used != b.last_used) {
                return a.last_used > b.last_used;
              }
              return a.path < b.path;
            });

  // The kept sidecar occupies a slot even when it does not exist yet, so the
  // limit describes the cache the caller is about to leave behind.
  const std::size_t reserved = keep_path.empty() ? 0 : 1;

  std::vector<std::string> evict;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const bool too_old = policy.max_age.count() > 0 &&
                         entries[i].last_used + policy.max_age < now;
    const bool beyond_limit =
        policy.max_entries > 0 && i + reserved >= policy.max_entries;
    if (too_old || beyond_limit) {
      evict.push_back(entries[i].path);
    }
  }
  // Oldest first, so a partially-failing pass still reclaims the least useful
  // sidecars.
  std::reverse(evict.begin(), evict.end());
  return evict;
}

std::vector<SidecarEntry> listSidecars(const std::string& directory) {
  std::vector<SidecarEntry> entries;
  std::error_code ec;
  std::filesystem::directory_iterator it(directory, ec);
  if (ec) {
    return entries;  // absent or unreadable: nothing to maintain
  }
  const ClockAnchor clocks;
  for (const auto& item : it) {
    if (!item.is_regular_file(ec) || ec) {
      continue;
    }
    if (!has_sidecar_suffix(item.path().filename().string())) {
      continue;
    }
    SidecarEntry entry;
    entry.path = item.path().string();
    entry.last_used = clocks.write_time_or_epoch(item.path());
    entry.bytes = size_or_zero(item.path());
    // A sidecar whose database file has not been rewritten may still have been
    // used: WAL traffic lands in the companions. Take the newest stamp of the
    // set, and count their bytes toward what eviction would reclaim.
    for (const char* suffix : kCompanionSuffixes) {
      const std::filesystem::path companion(entry.path + suffix);
      if (!std::filesystem::exists(companion, ec) || ec) {
        continue;
      }
      entry.last_used =
          std::max(entry.last_used, clocks.write_time_or_epoch(companion));
      entry.bytes += size_or_zero(companion);
    }
    entries.push_back(std::move(entry));
  }
  return entries;
}

std::uintmax_t removeSidecar(const std::string& path) {
  std::uintmax_t reclaimed = 0;
  std::error_code ec;

  // Companions first: a database file removed on its own would leave an
  // orphaned -wal that a later open could mistake for recoverable state.
  for (const char* suffix : kCompanionSuffixes) {
    const std::filesystem::path companion(path + suffix);
    const auto bytes = size_or_zero(companion);
    if (std::filesystem::remove(companion, ec) && !ec) {
      reclaimed += bytes;
    }
  }

  const auto bytes = size_or_zero(path);
  if (!std::filesystem::remove(path, ec) || ec) {
    ORC_LOG_WARN("ObservationCache: could not evict '{}' ({})", path,
                 ec ? ec.message() : std::string("file already gone"));
    return reclaimed;
  }
  return reclaimed + bytes;
}

void touchSidecar(const std::string& path) {
  std::error_code ec;
  std::filesystem::last_write_time(
      path, std::filesystem::file_time_type::clock::now(), ec);
  // Best effort: an unstampable sidecar simply ages by its write time.
}

std::size_t enforceSidecarRetention(const std::string& directory,
                                    const SidecarRetentionPolicy& policy,
                                    const std::string& keep_path) {
  try {
    const auto evict =
        selectSidecarsToEvict(listSidecars(directory), policy, keep_path,
                              std::chrono::system_clock::now());
    if (evict.empty()) {
      return 0;
    }
    std::uintmax_t reclaimed = 0;
    for (const auto& path : evict) {
      reclaimed += removeSidecar(path);
    }
    ORC_LOG_INFO(
        "ObservationCache: evicted {} sidecar(s) from '{}', reclaiming {} MB "
        "(keeping at most {} entries, {} days)",
        evict.size(), directory, reclaimed / (std::uintmax_t{1024} * 1024),
        policy.max_entries,
        std::chrono::duration_cast<std::chrono::hours>(policy.max_age).count() /
            24);
    return evict.size();
  } catch (const std::exception& e) {
    // Cache maintenance must never be able to fail an application start.
    ORC_LOG_WARN("ObservationCache: retention pass failed ({}); continuing",
                 e.what());
    return 0;
  }
}

}  // namespace orc
