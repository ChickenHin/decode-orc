/*
 * File:        observation_store.h
 * Module:      orc-core
 * Purpose:     Provenance-keyed, memory-budgeted store of observer output
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// Host-internal observation store. Only orc-core and orc-presenters may include
// this header; GUI/CLI code must go through presenters.
#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/observation_store.h. Use a presenter instead."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/observation_store.h. Use a presenter instead."
#endif

#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context_interface.h>

#include <cstddef>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "frame_provenance.h"

namespace orc {

/**
 * @brief Identity of one stored observation record.
 *
 * A record holds everything a single observer wrote for a single field of a
 * single frame-content. Keying on the node's provenance fingerprint (rather
 * than a live DAG node id) is what lets records survive DAG rebuilds and,
 * later, whole sessions: an unchanged pipeline produces the same fingerprint,
 * so the record still matches. The observer version is part of the key so
 * bumping an observer naturally invalidates only that observer's records.
 */
struct ObservationRecordKey {
  NodeFingerprint fingerprint;  ///< Provenance of the field's parent node.
  FieldID field_id;         ///< Derived field id (frame_id * 2 + field_idx).
  std::string observer_id;  ///< Stable observer id (e.g. "white_snr").
  std::string observer_version;  ///< Observer version string.

  bool operator==(const ObservationRecordKey& other) const {
    return field_id == other.field_id && fingerprint == other.fingerprint &&
           observer_id == other.observer_id &&
           observer_version == other.observer_version;
  }
  bool operator!=(const ObservationRecordKey& other) const {
    return !(*this == other);
  }
};

/**
 * @brief The namespaced observation values one observer wrote for one field.
 *
 * Shape mirrors IObservationContext::get_all_observations(): namespace -> key
 * -> value. An empty record is a valid, distinct state ("this observer ran for
 * this field and produced nothing"), so a stored empty record still reports
 * has() == true and suppresses re-running.
 */
using ObservationRecord =
    std::map<std::string, std::map<std::string, ObservationValue>>;

/**
 * @brief Thread-safe, memory-budgeted store of provenance-keyed observations.
 *
 * Records are keyed by ObservationRecordKey so they are tied to frame *content*
 * rather than to a live DAG node. The store is the sole thread-safe handoff
 * point between the (single-threaded) renderer/scheduler and any reader.
 *
 * Eviction: an approximate byte budget bounds memory. put() and retain_only()
 * evict least-recently-used records once usage exceeds the budget. get(),
 * load_into() and put() mark a record most-recently-used; has() does not (it is
 * a pure existence probe, matching LRUCache::contains()).
 *
 * Thread safety (coding standards §5.3.3): every public method is safe to call
 * concurrently from any thread; all state is guarded by an internal mutex.
 */
class ObservationStore {
 public:
  /// Default memory budget (64 MiB of approximate record payload).
  static constexpr std::size_t kDefaultMemoryBudgetBytes =
      static_cast<std::size_t>(64) * 1024 * 1024;

  explicit ObservationStore(
      std::size_t memory_budget_bytes = kDefaultMemoryBudgetBytes);

  // Non-copyable, non-movable (holds a mutex).
  ObservationStore(const ObservationStore&) = delete;
  ObservationStore& operator=(const ObservationStore&) = delete;
  ObservationStore(ObservationStore&&) = delete;
  ObservationStore& operator=(ObservationStore&&) = delete;

  /// True if a record is stored for @p key (does not affect LRU order).
  bool has(const ObservationRecordKey& key) const;

  /// Return the stored record for @p key, or nullopt on miss. Marks the record
  /// most-recently-used on a hit.
  std::optional<ObservationRecord> get(const ObservationRecordKey& key);

  /// Store (or replace) the record for @p key and mark it most-recently-used.
  /// Evicts least-recently-used records if the byte budget is exceeded, but
  /// never evicts the record just inserted.
  void put(const ObservationRecordKey& key, ObservationRecord record);

  /// Load the record for @p key into @p context at its field id. Returns false
  /// on miss (context left untouched). Marks the record most-recently-used.
  bool load_into(const ObservationRecordKey& key,
                 IObservationContext& context) const;

  /// Drop every record whose fingerprint is not in @p keep, then evict
  /// least-recently-used records until usage is within @p budget. Also updates
  /// the active budget to @p budget.
  void retain_only(const std::unordered_set<NodeFingerprint>& keep,
                   std::size_t budget);

  /// Number of stored records.
  std::size_t size() const;

  /// Approximate payload size of all stored records, in bytes.
  std::size_t memory_usage_bytes() const;

  /// Active memory budget in bytes.
  std::size_t memory_budget_bytes() const;

  /// Remove all records (budget unchanged).
  void clear();

 private:
  struct Hash {
    std::size_t operator()(const ObservationRecordKey& key) const noexcept;
  };

  struct Entry {
    ObservationRecordKey key;
    ObservationRecord record;
    std::size_t bytes;
  };

  // Estimate the approximate payload size of a record (used for the budget).
  static std::size_t estimate_bytes(const ObservationRecordKey& key,
                                    const ObservationRecord& record);

  // Move the entry referenced by @p it to the front of the LRU list. Caller
  // holds mutex_.
  void touch(std::list<Entry>::iterator it) const;

  // Evict least-recently-used entries until usage_bytes_ <= budget_bytes_,
  // keeping at least @p keep_at_least entries. Caller holds mutex_.
  void evict_to_budget(std::size_t keep_at_least);

  mutable std::mutex mutex_;
  std::size_t budget_bytes_;
  std::size_t usage_bytes_ = 0;

  // LRU list (front = most recently used) plus an index into it. Marked mutable
  // so const probes (get/load_into) can refresh LRU order under the lock.
  mutable std::list<Entry> lru_;
  mutable std::unordered_map<ObservationRecordKey, std::list<Entry>::iterator,
                             Hash>
      index_;
};

}  // namespace orc
