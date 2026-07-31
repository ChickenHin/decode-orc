/*
 * File:        sqlite_observation_persistence.h
 * Module:      orc-core
 * Purpose:     SQLite-backed observation sidecar stored beside a project
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// Host-internal persistence backend. Only orc-core and orc-presenters may
// include this header; GUI/CLI code must go through presenters.
#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/sqlite_observation_persistence.h. Use a presenter instead."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/sqlite_observation_persistence.h. Use a presenter instead."
#endif

#include <cstddef>
#include <mutex>
#include <string>

#include "observation_persistence.h"

// Forward-declared so the SQLite headers stay confined to the .cpp and never
// leak into the presenter layer that includes this file.
struct sqlite3;

namespace orc {

/**
 * @brief SQLite implementation of IObservationPersistence.
 *
 * Schema (schema version 1), a single denormalised record table plus a
 * schema-version table:
 *
 * @code
 *   CREATE TABLE observation_record(
 *     node_fingerprint TEXT    NOT NULL,  -- provenance hash of the parent node
 *     field_id         INTEGER NOT NULL,  -- FieldID value (frame*2 +
 * field_idx) observer_id      TEXT    NOT NULL,  -- stable observer id (e.g.
 * white_snr) observer_version TEXT    NOT NULL,  -- observer version string
 *     namespace        TEXT    NOT NULL,  -- observation namespace
 *     key              TEXT    NOT NULL,  -- observation key
 *     value_type       INTEGER NOT NULL,  -- ValueType tag (see .cpp)
 *     value            TEXT    NOT NULL,  -- textual, type-tagged value
 *     PRIMARY KEY (node_fingerprint, field_id, observer_id, observer_version,
 *                  namespace, key));
 *   CREATE TABLE schema_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
 * @endcode
 *
 * A present-but-empty record (an observer that ran and produced nothing) is
 * stored as a single sentinel row with an empty namespace/key and the
 * kEmptyRecord value type, so it round-trips as distinct from an absent record.
 *
 * Lifecycle: on construction the database beside the project is opened, or
 * created if absent. A corrupt file or a schema-version mismatch is discarded
 * and rebuilt (a warning is logged; construction never throws for a recoverable
 * on-disk problem), so opening a project can never fail on the sidecar.
 *
 * Thread-safety (coding standards §5.3.3): every public method is guarded by an
 * internal mutex, so all database access is serialised and the instance is safe
 * to share across the store's writer thread and the caller's thread.
 */
class SqliteObservationPersistence : public IObservationPersistence {
 public:
  /// Current on-disk schema version. Bumping it invalidates older sidecars.
  static constexpr int kSchemaVersion = 1;

  /// Open (or create) the sidecar database at @p db_path. Recovers by
  /// rebuilding on corruption or schema mismatch; throws only if a usable
  /// database cannot be opened at all.
  explicit SqliteObservationPersistence(std::string db_path);
  ~SqliteObservationPersistence() override;

  // Non-copyable, non-movable (owns a database handle and a mutex).
  SqliteObservationPersistence(const SqliteObservationPersistence&) = delete;
  SqliteObservationPersistence& operator=(const SqliteObservationPersistence&) =
      delete;
  SqliteObservationPersistence(SqliteObservationPersistence&&) = delete;
  SqliteObservationPersistence& operator=(SqliteObservationPersistence&&) =
      delete;

  void save(const std::vector<PersistedObservation>& records) override;
  void load_matching(
      const std::unordered_set<NodeFingerprint>& keep,
      const std::function<void(ObservationRecordKey, ObservationRecord)>& sink)
      override;
  std::optional<ObservationRecord> load_one(
      const ObservationRecordKey& key) override;
  bool exists(const ObservationRecordKey& key) override;

  // Maintenance stamps, stored in the schema_meta table under an "app:" key
  // prefix (the bare "version" key is reserved for the schema version).
  std::string get_meta(const std::string& key) override;
  void set_meta(const std::string& key, const std::string& value) override;

  /**
   * @brief Merge records from another sidecar database into this one.
   *
   * Inserts every row of @p source_db_path's observation_record table that
   * this database does not already hold (matched on the full primary key).
   * Used to adopt the per-source quick-project cache when a project is saved
   * and reopened, so observations computed before the save are not lost. A
   * no-op (returning 0) when the source does not exist, cannot be attached,
   * or holds no rows this database lacks; guarded by row counts so repeated
   * calls after a completed merge are cheap.
   *
   * @return Number of rows inserted.
   */
  std::size_t merge_from(const std::string& source_db_path);
  std::size_t retain_only(
      const std::unordered_set<NodeFingerprint>& keep) override;
  std::size_t purge_observer_version(
      const std::string& observer_id,
      const std::string& current_version) override;

  /// Number of distinct records currently persisted (test/introspection aid).
  std::size_t record_count();

 private:
  // Open the database and ensure the schema is present and current. Returns
  // false if the file is unusable (corrupt / not a database / wrong schema),
  // in which case the caller rebuilds from scratch. Caller holds mutex_.
  bool try_open_and_verify();

  // Apply the per-connection performance configuration (WAL journal,
  // synchronous=NORMAL, enlarged page cache, mmap window, in-memory temp
  // store, busy timeout). Called immediately after every successful
  // sqlite3_open; see the implementation for the workload rationale.
  void configure_connection();

  // Discard any existing file and recreate an empty, current-schema database.
  // Caller holds mutex_.
  void rebuild();

  // Create the tables if missing and stamp the schema version. Caller holds
  // mutex_. Returns false on any SQLite error.
  bool ensure_schema();

  // Count distinct (fingerprint, field, observer, version) records. Caller
  // holds mutex_. Used to report how many whole records a GC operation removed
  // (each record spans several value rows).
  std::size_t distinct_record_count_locked();

  std::string db_path_;
  std::mutex mutex_;
  sqlite3* db_ = nullptr;
};

}  // namespace orc
