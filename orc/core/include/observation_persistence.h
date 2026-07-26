/*
 * File:        observation_persistence.h
 * Module:      orc-core
 * Purpose:     Interface for durable, provenance-keyed observation storage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// Host-internal persistence interface. Only orc-core and orc-presenters may
// include this header; GUI/CLI code must go through presenters.
#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/observation_persistence.h. Use a presenter instead."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/observation_persistence.h. Use a presenter instead."
#endif

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "frame_provenance.h"
#include "observation_store.h"

namespace orc {

/**
 * @brief One observation record staged for persistence.
 *
 * Mirrors the in-memory (key, record) pair the ObservationStore holds. The
 * record is copied so the write-behind path can outlive the caller's data.
 */
struct PersistedObservation {
  ObservationRecordKey key;
  ObservationRecord record;
};

/**
 * @brief Durable sidecar storage for provenance-keyed observation records.
 *
 * The ObservationStore uses an implementation of this interface to survive
 * application restarts: completed records are written behind the interactive
 * path and reloaded on project open for fingerprints that still match. Keeping
 * the contract abstract lets the store be unit-tested without a real database.
 *
 * Thread-safety: implementations must be safe to call concurrently from any
 * thread (the store drives save() from a background writer thread while
 * warm_start()/GC run on the caller's thread). Each method is individually
 * atomic with respect to the others.
 */
class IObservationPersistence {
 public:
  virtual ~IObservationPersistence() = default;

  /**
   * @brief Durably store a batch of records (insert or replace on key).
   *
   * Replacing a key overwrites the previously persisted values for that key in
   * full, so a re-observation that produces fewer values does not leave stale
   * rows. An empty record is stored as a distinct present-but-empty state.
   */
  virtual void save(const std::vector<PersistedObservation>& records) = 0;

  /**
   * @brief Stream back every persisted record whose fingerprint is in @p keep.
   *
   * @p sink is invoked once per matching record with the reconstructed key and
   * record. Records whose fingerprint is absent from @p keep are skipped (they
   * belong to a different pipeline/source content).
   */
  virtual void load_matching(
      const std::unordered_set<NodeFingerprint>& keep,
      const std::function<void(ObservationRecordKey, ObservationRecord)>&
          sink) = 0;

  /**
   * @brief Delete every persisted record whose fingerprint is not in @p keep.
   *
   * @return Number of records removed.
   */
  virtual std::size_t retain_only(
      const std::unordered_set<NodeFingerprint>& keep) = 0;

  /**
   * @brief Delete records for @p observer_id whose stored version differs from
   *        @p current_version (an observer version bump invalidates only that
   *        observer's records, leaving other observers' records intact).
   *
   * @return Number of records removed.
   */
  virtual std::size_t purge_observer_version(
      const std::string& observer_id, const std::string& current_version) = 0;

  /**
   * @brief Read / write a small named metadata value (maintenance stamps).
   *
   * Used to remember what maintenance has already been applied to the durable
   * data — e.g. the observer-version set the last purge ran with, or the
   * fingerprint set the last GC retained — so an unchanged reopen skips
   * whole-database scans entirely. Defaults: no storage (get returns "",
   * set is a no-op), which simply means the maintenance runs every time.
   */
  virtual std::string get_meta(const std::string& /*key*/) { return {}; }
  virtual void set_meta(const std::string& /*key*/,
                        const std::string& /*value*/) {}

  /**
   * @brief Read back a single persisted record by exact key.
   *
   * Backs the in-memory store's read-through: a record evicted from memory by
   * the budget is reloaded from durable storage instead of being recomputed.
   * Returns an engaged optional holding an empty record for the
   * present-but-empty state, and nullopt when the key was never persisted.
   *
   * Default: nullopt (an implementation that only archives need not support
   * point reads; the store then treats eviction as a genuine miss).
   */
  virtual std::optional<ObservationRecord> load_one(
      const ObservationRecordKey& /*key*/) {
    return std::nullopt;
  }
};

}  // namespace orc
