/*
 * File:        sqlite_observation_persistence.cpp
 * Module:      orc-core
 * Purpose:     SQLite-backed observation sidecar stored beside a project
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "sqlite_observation_persistence.h"

#include <orc/support/logging.h>
#include <sqlite3.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace orc {

namespace {

// Value-type tags stored in the value_type column. Persisted numerically, so
// their values are part of the on-disk format and must never be renumbered
// without a schema-version bump.
enum ValueType : int {
  kInt32 = 0,
  kInt64 = 1,
  kDouble = 2,
  kString = 3,
  kBool = 4,
  kEmptyRecord = 5,  // sentinel: observer ran, produced no values
};

// Serialise a variant value to its (type tag, text) on-disk representation.
// Doubles use %.17g so IEEE-754 doubles round-trip exactly.
std::pair<int, std::string> encode_value(const ObservationValue& value) {
  return std::visit(
      [](const auto& v) -> std::pair<int, std::string> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, int32_t>) {
          return {kInt32, std::to_string(v)};
        } else if constexpr (std::is_same_v<T, int64_t>) {
          return {kInt64, std::to_string(v)};
        } else if constexpr (std::is_same_v<T, double>) {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%.17g", v);
          return {kDouble, std::string(buf)};
        } else if constexpr (std::is_same_v<T, std::string>) {
          return {kString, v};
        } else {  // bool
          return {kBool, v ? std::string("1") : std::string("0")};
        }
      },
      value);
}

// Reconstruct a variant value from its stored (type tag, text) representation.
ObservationValue decode_value(int value_type, const std::string& text) {
  switch (value_type) {
    case kInt32:
      return static_cast<int32_t>(std::stol(text));
    case kInt64:
      return static_cast<int64_t>(std::stoll(text));
    case kDouble:
      return std::stod(text);
    case kBool:
      return text == "1";
    case kString:
    default:
      return text;
  }
}

// RAII wrapper for a prepared statement, finalised on scope exit.
class Statement {
 public:
  Statement(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      stmt_ = nullptr;
    }
  }
  ~Statement() {
    if (stmt_) sqlite3_finalize(stmt_);
  }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  explicit operator bool() const { return stmt_ != nullptr; }
  sqlite3_stmt* get() const { return stmt_; }

 private:
  sqlite3_stmt* stmt_ = nullptr;
};

// Execute a simple statement with no result rows. Returns true on success.
bool exec(sqlite3* db, const char* sql) {
  return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
  sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()),
                    SQLITE_TRANSIENT);
}

std::string column_text(sqlite3_stmt* stmt, int index) {
  const auto* text = sqlite3_column_text(stmt, index);
  if (!text) return {};
  return reinterpret_cast<const char*>(text);
}

}  // namespace

SqliteObservationPersistence::SqliteObservationPersistence(std::string db_path)
    : db_path_(std::move(db_path)) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!try_open_and_verify()) {
    // Corrupt file or schema mismatch: discard and rebuild rather than fail.
    ORC_LOG_WARN(
        "ObservationSidecar: '{}' is unusable (corrupt or wrong schema); "
        "rebuilding",
        db_path_);
    rebuild();
  }
}

SqliteObservationPersistence::~SqliteObservationPersistence() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

void SqliteObservationPersistence::configure_connection() {
  // Performance configuration for this workload: a large (multi-GB),
  // regenerable cache written in frequent small batches by the write-behind
  // thread and read via indexed point/streaming queries.
  //
  //  - WAL: commits append to one log instead of creating and deleting a
  //    rollback-journal file per transaction — the write-behind path commits
  //    continuously during sweeps and triggers.
  //  - synchronous=NORMAL: with WAL this cannot corrupt the database on an
  //    application crash; an OS/power failure may lose the most recent
  //    transactions, which for a cache of recomputable observations merely
  //    means recomputing a few frames. FULL's per-commit fsyncs buy nothing
  //    here.
  //  - 64 MiB page cache + 1 GiB mmap window: point reads (read-through) and
  //    fingerprint streams (warm-up, merge) over a database far larger than
  //    SQLite's ~2 MiB default cache.
  //  - temp_store=MEMORY: GC's NOT-IN scratch structures stay off disk.
  //  - busy_timeout: a second connection (another app instance sharing the
  //    per-source quick cache) waits briefly instead of failing SQLITE_BUSY.
  exec(db_, "PRAGMA journal_mode=WAL");
  exec(db_, "PRAGMA synchronous=NORMAL");
  exec(db_, "PRAGMA cache_size=-65536");
  exec(db_, "PRAGMA mmap_size=1073741824");
  exec(db_, "PRAGMA temp_store=MEMORY");
  sqlite3_busy_timeout(db_, 5000);
}

bool SqliteObservationPersistence::try_open_and_verify() {
  if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    return false;
  }

  configure_connection();

  // A garbage file opens lazily; the first real statements surface corruption
  // (SQLITE_NOTADB / SQLITE_CORRUPT), and every failure path here funnels into
  // rebuild(). A full integrity_check would read the entire multi-GB B-tree on
  // every open — deliberately NOT done: this is a regenerable cache, and any
  // deeper corruption that slips past these probes surfaces as per-operation
  // errors that degrade to cache misses (recompute), never wrong data.
  if (!ensure_schema()) return false;

  // Cheap root-page probe of the main table (reads at most one page).
  {
    Statement probe(db_, "SELECT 1 FROM observation_record LIMIT 1");
    if (!probe) return false;
    const int rc = sqlite3_step(probe.get());
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) return false;
  }

  // Reject an existing database written by a newer/older schema version.
  Statement stmt(db_, "SELECT value FROM schema_meta WHERE key = 'version'");
  if (!stmt) return false;
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return false;
  return column_text(stmt.get(), 0) == std::to_string(kSchemaVersion);
}

void SqliteObservationPersistence::rebuild() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
  std::error_code ec;
  std::filesystem::remove(db_path_, ec);  // best effort; ignore failure

  if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
    const std::string msg =
        db_ ? sqlite3_errmsg(db_) : "unable to open observation sidecar";
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    throw std::runtime_error("ObservationSidecar: " + msg);
  }
  configure_connection();
  if (!ensure_schema()) {
    throw std::runtime_error(
        "ObservationSidecar: failed to initialise schema for '" + db_path_ +
        "'");
  }
}

bool SqliteObservationPersistence::ensure_schema() {
  if (!exec(db_,
            "CREATE TABLE IF NOT EXISTS observation_record("
            "  node_fingerprint TEXT NOT NULL,"
            "  field_id INTEGER NOT NULL,"
            "  observer_id TEXT NOT NULL,"
            "  observer_version TEXT NOT NULL,"
            "  namespace TEXT NOT NULL,"
            "  key TEXT NOT NULL,"
            "  value_type INTEGER NOT NULL,"
            "  value TEXT NOT NULL,"
            "  PRIMARY KEY (node_fingerprint, field_id, observer_id,"
            "               observer_version, namespace, key))")) {
    return false;
  }
  if (!exec(db_,
            "CREATE INDEX IF NOT EXISTS idx_obs_fingerprint"
            "  ON observation_record(node_fingerprint)")) {
    return false;
  }
  if (!exec(db_,
            "CREATE INDEX IF NOT EXISTS idx_obs_observer"
            "  ON observation_record(observer_id)")) {
    return false;
  }
  if (!exec(db_,
            "CREATE TABLE IF NOT EXISTS schema_meta("
            "  key TEXT PRIMARY KEY, value TEXT NOT NULL)")) {
    return false;
  }
  const std::string stamp_version =
      "INSERT OR IGNORE INTO schema_meta(key, value) VALUES ('version', '" +
      std::to_string(kSchemaVersion) + "')";
  return exec(db_, stamp_version.c_str());
}

void SqliteObservationPersistence::save(
    const std::vector<PersistedObservation>& records) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_ || records.empty()) return;

  Statement del(
      db_,
      "DELETE FROM observation_record WHERE node_fingerprint = ?"
      " AND field_id = ? AND observer_id = ? AND observer_version = ?");
  Statement ins(db_,
                "INSERT INTO observation_record(node_fingerprint, field_id,"
                " observer_id, observer_version, namespace, key, value_type,"
                " value) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
  if (!del || !ins) {
    ORC_LOG_ERROR("ObservationSidecar: failed to prepare save statements");
    return;
  }

  exec(db_, "BEGIN IMMEDIATE");
  for (const auto& item : records) {
    const auto& key = item.key;
    const auto field = static_cast<sqlite3_int64>(key.field_id.value());

    // Replace any existing rows for this key so a re-observation never leaves
    // stale values behind.
    sqlite3_reset(del.get());
    bind_text(del.get(), 1, key.fingerprint.value);
    sqlite3_bind_int64(del.get(), 2, field);
    bind_text(del.get(), 3, key.observer_id);
    bind_text(del.get(), 4, key.observer_version);
    sqlite3_step(del.get());

    auto insert_row = [&](const std::string& ns, const std::string& k,
                          int value_type, const std::string& value) {
      sqlite3_reset(ins.get());
      bind_text(ins.get(), 1, key.fingerprint.value);
      sqlite3_bind_int64(ins.get(), 2, field);
      bind_text(ins.get(), 3, key.observer_id);
      bind_text(ins.get(), 4, key.observer_version);
      bind_text(ins.get(), 5, ns);
      bind_text(ins.get(), 6, k);
      sqlite3_bind_int(ins.get(), 7, value_type);
      bind_text(ins.get(), 8, value);
      sqlite3_step(ins.get());
    };

    if (item.record.empty()) {
      // Present-but-empty record: store a single sentinel row.
      insert_row("", "", kEmptyRecord, "");
      continue;
    }
    for (const auto& [ns, keys] : item.record) {
      for (const auto& [k, v] : keys) {
        const auto [value_type, text] = encode_value(v);
        insert_row(ns, k, value_type, text);
      }
    }
  }
  exec(db_, "COMMIT");
}

void SqliteObservationPersistence::load_matching(
    const std::unordered_set<NodeFingerprint>& keep,
    const std::function<void(ObservationRecordKey, ObservationRecord)>& sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_ || keep.empty()) return;

  // Small keep sets stream just the wanted fingerprints through the
  // idx_obs_fingerprint index — a single-fingerprint warm-up on a multi-GB
  // sidecar must not pay a full-table scan. Larger sets (whole-map warm
  // start) fall back to one ordered scan with the filter applied in the loop.
  std::string sql =
      "SELECT node_fingerprint, field_id, observer_id,"
      " observer_version, namespace, key, value_type, value"
      " FROM observation_record";
  constexpr std::size_t kIndexedKeepLimit = 8;
  const bool indexed = keep.size() <= kIndexedKeepLimit;
  if (indexed) {
    sql += " WHERE node_fingerprint IN (";
    for (std::size_t i = 0; i < keep.size(); ++i) {
      sql += i == 0 ? "?" : ",?";
    }
    sql += ")";
  }
  sql += " ORDER BY node_fingerprint, field_id, observer_id, observer_version";

  Statement stmt(db_, sql.c_str());
  if (!stmt) return;
  if (indexed) {
    int slot = 1;
    for (const auto& fp : keep) {
      bind_text(stmt.get(), slot++, fp.value);
    }
  }

  // Rows arrive grouped by record key; accumulate a record and emit it when the
  // key changes.
  bool have_current = false;
  ObservationRecordKey current_key;
  ObservationRecord current_record;
  bool current_kept = false;

  auto flush_current = [&]() {
    if (have_current && current_kept) {
      sink(current_key, std::move(current_record));
    }
    current_record.clear();
    have_current = false;
  };

  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    ObservationRecordKey row_key{
        NodeFingerprint{column_text(stmt.get(), 0)},
        FieldID(static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 1))),
        column_text(stmt.get(), 2), column_text(stmt.get(), 3)};

    if (!have_current || row_key != current_key) {
      flush_current();
      current_key = row_key;
      have_current = true;
      current_kept = keep.find(current_key.fingerprint) != keep.end();
    }
    if (!current_kept) continue;

    const std::string ns = column_text(stmt.get(), 4);
    const std::string k = column_text(stmt.get(), 5);
    const int value_type = sqlite3_column_int(stmt.get(), 6);
    if (value_type == kEmptyRecord) continue;  // sentinel: leave record empty
    current_record[ns][k] =
        decode_value(value_type, column_text(stmt.get(), 7));
  }
  flush_current();
}

std::optional<ObservationRecord> SqliteObservationPersistence::load_one(
    const ObservationRecordKey& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return std::nullopt;

  Statement stmt(db_,
                 "SELECT namespace, key, value_type, value"
                 " FROM observation_record"
                 " WHERE node_fingerprint = ? AND field_id = ?"
                 " AND observer_id = ? AND observer_version = ?");
  if (!stmt) return std::nullopt;

  bind_text(stmt.get(), 1, key.fingerprint.value);
  sqlite3_bind_int64(stmt.get(), 2,
                     static_cast<sqlite3_int64>(key.field_id.value()));
  bind_text(stmt.get(), 3, key.observer_id);
  bind_text(stmt.get(), 4, key.observer_version);

  bool found = false;
  ObservationRecord record;
  while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
    found = true;
    const int value_type = sqlite3_column_int(stmt.get(), 2);
    if (value_type == kEmptyRecord) continue;  // sentinel: present-but-empty
    record[column_text(stmt.get(), 0)][column_text(stmt.get(), 1)] =
        decode_value(value_type, column_text(stmt.get(), 3));
  }
  if (!found) return std::nullopt;
  return record;
}

std::string SqliteObservationPersistence::get_meta(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return {};
  Statement stmt(db_, "SELECT value FROM schema_meta WHERE key = ?");
  if (!stmt) return {};
  bind_text(stmt.get(), 1, "app:" + key);
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return {};
  return column_text(stmt.get(), 0);
}

void SqliteObservationPersistence::set_meta(const std::string& key,
                                            const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return;
  Statement stmt(db_,
                 "INSERT OR REPLACE INTO schema_meta(key, value)"
                 " VALUES (?, ?)");
  if (!stmt) return;
  bind_text(stmt.get(), 1, "app:" + key);
  bind_text(stmt.get(), 2, value);
  sqlite3_step(stmt.get());
}

std::size_t SqliteObservationPersistence::merge_from(
    const std::string& source_db_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_ || source_db_path.empty()) return 0;

  std::error_code ec;
  if (!std::filesystem::exists(source_db_path, ec) || ec) return 0;

  Statement attach(db_, "ATTACH DATABASE ? AS merge_src");
  if (!attach) return 0;
  bind_text(attach.get(), 1, source_db_path);
  if (sqlite3_step(attach.get()) != SQLITE_DONE) {
    ORC_LOG_WARN("ObservationSidecar: cannot attach '{}' for merge",
                 source_db_path);
    return 0;
  }

  std::size_t inserted = 0;
  {
    // Count guard: a source with no more rows than we already hold cannot
    // contribute anything new through INSERT OR IGNORE on the full PK — the
    // common case after a completed merge, kept cheap.
    sqlite3_int64 own_rows = 0;
    sqlite3_int64 src_rows = 0;
    Statement own_count(db_, "SELECT COUNT(*) FROM observation_record");
    Statement src_count(db_,
                        "SELECT COUNT(*) FROM merge_src.observation_record");
    const bool counts_ok =
        own_count && sqlite3_step(own_count.get()) == SQLITE_ROW &&
        (own_rows = sqlite3_column_int64(own_count.get(), 0), true) &&
        src_count && sqlite3_step(src_count.get()) == SQLITE_ROW &&
        (src_rows = sqlite3_column_int64(src_count.get(), 0), true);

    if (counts_ok && src_rows > 0 && src_rows > own_rows) {
      exec(db_, "BEGIN IMMEDIATE");
      // Adopt whole records only: a record (one observer's output for one
      // field) is atomic, so rows are copied only when the target holds NO
      // rows for that record key. Row-level INSERT OR IGNORE would blend rows
      // from two different observations of the same record.
      if (exec(db_,
               "INSERT INTO observation_record"
               " SELECT s.* FROM merge_src.observation_record AS s"
               " WHERE NOT EXISTS ("
               "   SELECT 1 FROM observation_record AS t"
               "   WHERE t.node_fingerprint = s.node_fingerprint"
               "     AND t.field_id = s.field_id"
               "     AND t.observer_id = s.observer_id"
               "     AND t.observer_version = s.observer_version)")) {
        inserted = static_cast<std::size_t>(sqlite3_changes(db_));
      }
      exec(db_, "COMMIT");
    }
  }

  exec(db_, "DETACH DATABASE merge_src");
  return inserted;
}

std::size_t SqliteObservationPersistence::retain_only(
    const std::unordered_set<NodeFingerprint>& keep) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return 0;

  // Collect the distinct fingerprints present, then delete those not kept.
  std::vector<std::string> to_delete;
  {
    Statement stmt(db_,
                   "SELECT DISTINCT node_fingerprint FROM observation_record");
    if (!stmt) return 0;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      std::string fp = column_text(stmt.get(), 0);
      if (keep.find(NodeFingerprint{fp}) == keep.end()) {
        to_delete.push_back(std::move(fp));
      }
    }
  }
  if (to_delete.empty()) return 0;

  const std::size_t before = distinct_record_count_locked();
  Statement del(db_,
                "DELETE FROM observation_record WHERE node_fingerprint = ?");
  if (!del) return 0;
  exec(db_, "BEGIN IMMEDIATE");
  for (const auto& fp : to_delete) {
    sqlite3_reset(del.get());
    bind_text(del.get(), 1, fp);
    sqlite3_step(del.get());
  }
  exec(db_, "COMMIT");
  return before - distinct_record_count_locked();
}

std::size_t SqliteObservationPersistence::purge_observer_version(
    const std::string& observer_id, const std::string& current_version) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return 0;

  Statement del(db_,
                "DELETE FROM observation_record WHERE observer_id = ?"
                " AND observer_version <> ?");
  if (!del) return 0;
  const std::size_t before = distinct_record_count_locked();
  bind_text(del.get(), 1, observer_id);
  bind_text(del.get(), 2, current_version);
  if (sqlite3_step(del.get()) != SQLITE_DONE) return 0;
  return before - distinct_record_count_locked();
}

std::size_t SqliteObservationPersistence::distinct_record_count_locked() {
  if (!db_) return 0;
  Statement stmt(db_,
                 "SELECT COUNT(*) FROM (SELECT DISTINCT node_fingerprint,"
                 " field_id, observer_id, observer_version"
                 " FROM observation_record)");
  if (!stmt) return 0;
  if (sqlite3_step(stmt.get()) != SQLITE_ROW) return 0;
  return static_cast<std::size_t>(sqlite3_column_int64(stmt.get(), 0));
}

std::size_t SqliteObservationPersistence::record_count() {
  std::lock_guard<std::mutex> lock(mutex_);
  return distinct_record_count_locked();
}

}  // namespace orc
