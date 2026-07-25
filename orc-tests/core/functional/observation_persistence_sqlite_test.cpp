/*
 * File:        observation_persistence_sqlite_test.cpp
 * Module:      orc-core functional tests
 * Purpose:     SQLite sidecar round-trip, warm start, GC, version purge and
 *              corruption recovery (Phase 6, Tasks 6.1-6.3)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * Functional (not unit): exercises the real SQLite backend against temporary
 * files on disk, which unit tests may not touch (AGENTS.md §4.2).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "observation_store.h"
#include "sqlite_observation_persistence.h"

namespace orc {
namespace {

NodeFingerprint fp(const std::string& v) { return NodeFingerprint{v}; }

ObservationRecordKey key(const std::string& fingerprint, uint64_t field,
                         const std::string& id, const std::string& version) {
  return ObservationRecordKey{fp(fingerprint), FieldID(field), id, version};
}

ObservationRecord all_variants_record() {
  ObservationRecord r;
  r["ns"]["i32"] = static_cast<int32_t>(-42);
  r["ns"]["i64"] = static_cast<int64_t>(9000000000LL);
  r["ns"]["dbl"] = 3.14159265358979;
  r["ns"]["str"] = std::string("hello world");
  r["ns"]["flag"] = true;
  r["other"]["k"] = static_cast<int32_t>(7);
  return r;
}

// Unique temporary sidecar path per test; removed on teardown.
class SqliteSidecarTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("orc-obs-") + info->test_suite_name() + "-" +
            info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    db_path_ = (dir_ / "observations.sqlite").string();
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
  std::string db_path_;
};

// ---------------------------------------------------------------------------
// Round-trip (all variants, empty records, replacement)
// ---------------------------------------------------------------------------

TEST_F(SqliteSidecarTest, RoundTrip_PreservesAllVariants) {
  const auto k = key("fpA", 4, "white_snr", "1.0.0");
  {
    SqliteObservationPersistence db(db_path_);
    db.save({PersistedObservation{k, all_variants_record()}});
  }
  // Reopen a fresh connection to prove durability across close/open.
  SqliteObservationPersistence db(db_path_);
  ObservationRecord loaded;
  bool found = false;
  db.load_matching({fp("fpA")},
                   [&](ObservationRecordKey lk, ObservationRecord rec) {
                     if (lk == k) {
                       loaded = std::move(rec);
                       found = true;
                     }
                   });
  ASSERT_TRUE(found);
  EXPECT_EQ(loaded, all_variants_record());
}

TEST_F(SqliteSidecarTest, EmptyRecord_RoundTripsAsPresentButEmpty) {
  const auto k = key("fpE", 0, "silent", "1.0.0");
  SqliteObservationPersistence db(db_path_);
  db.save({PersistedObservation{k, ObservationRecord{}}});

  int matches = 0;
  bool empty = false;
  db.load_matching({fp("fpE")},
                   [&](ObservationRecordKey lk, ObservationRecord rec) {
                     ++matches;
                     empty = rec.empty();
                     EXPECT_EQ(lk, k);
                   });
  EXPECT_EQ(matches, 1);  // present...
  EXPECT_TRUE(empty);     // ...and empty
}

TEST_F(SqliteSidecarTest, Save_ReplacesPreviousValuesForKey) {
  const auto k = key("fpR", 2, "id", "1.0.0");
  SqliteObservationPersistence db(db_path_);

  ObservationRecord big;
  big["ns"]["a"] = static_cast<int32_t>(1);
  big["ns"]["b"] = static_cast<int32_t>(2);
  db.save({PersistedObservation{k, big}});

  ObservationRecord small;
  small["ns"]["a"] = static_cast<int32_t>(9);  // fewer values than before
  db.save({PersistedObservation{k, small}});

  ObservationRecord loaded;
  db.load_matching({fp("fpR")},
                   [&](ObservationRecordKey, ObservationRecord rec) {
                     loaded = std::move(rec);
                   });
  EXPECT_EQ(loaded, small);  // no stale "b" left behind
}

// ---------------------------------------------------------------------------
// Task 6.2 — warm start via the store, zero observer runs for unchanged content
// ---------------------------------------------------------------------------

TEST_F(SqliteSidecarTest, StoreRoundTrip_WarmStartRestoresRecords) {
  const std::unordered_set<NodeFingerprint> reachable{fp("n1"), fp("n2")};

  // Session 1: observe and close.
  {
    ObservationStore store;
    store.set_persistence(
        std::make_shared<SqliteObservationPersistence>(db_path_));
    store.put(key("n1", 0, "white_snr", "1.0.0"), all_variants_record());
    store.put(key("n1", 1, "white_snr", "1.0.0"), all_variants_record());
    store.put(key("n2", 0, "biphase", "1.0.0"), all_variants_record());
    store.flush();
  }

  // Session 2: reopen; warm start must restore every record with no writes.
  ObservationStore store;
  store.set_persistence(
      std::make_shared<SqliteObservationPersistence>(db_path_));
  store.warm_start(reachable);

  EXPECT_TRUE(store.has(key("n1", 0, "white_snr", "1.0.0")));
  EXPECT_TRUE(store.has(key("n1", 1, "white_snr", "1.0.0")));
  EXPECT_TRUE(store.has(key("n2", 0, "biphase", "1.0.0")));
  EXPECT_EQ(*store.get(key("n1", 0, "white_snr", "1.0.0")),
            all_variants_record());
}

TEST_F(SqliteSidecarTest, WarmStart_ChangedFingerprint_LoadsNothing) {
  {
    ObservationStore store;
    store.set_persistence(
        std::make_shared<SqliteObservationPersistence>(db_path_));
    store.put(key("old_fp", 0, "white_snr", "1.0.0"), all_variants_record());
    store.flush();
  }
  // Reopen after a source/pipeline change: the node fingerprint is different,
  // so nothing matches and the affected node starts cold.
  ObservationStore store;
  store.set_persistence(
      std::make_shared<SqliteObservationPersistence>(db_path_));
  store.warm_start({fp("new_fp")});
  EXPECT_FALSE(store.has(key("old_fp", 0, "white_snr", "1.0.0")));
}

// ---------------------------------------------------------------------------
// Task 6.3 — GC, version purge, corruption recovery
// ---------------------------------------------------------------------------

TEST_F(SqliteSidecarTest, RetainOnly_RemovesOnlyUnreachable) {
  SqliteObservationPersistence db(db_path_);
  db.save(
      {PersistedObservation{key("keep", 0, "id", "v"), all_variants_record()},
       PersistedObservation{key("keep", 1, "id", "v"), all_variants_record()},
       PersistedObservation{key("gone", 0, "id", "v"), all_variants_record()}});
  ASSERT_EQ(db.record_count(), 3u);

  const std::size_t removed = db.retain_only({fp("keep")});
  EXPECT_EQ(removed, 1u);
  EXPECT_EQ(db.record_count(), 2u);

  int gone_matches = 0;
  db.load_matching({fp("gone")}, [&](ObservationRecordKey, ObservationRecord) {
    ++gone_matches;
  });
  EXPECT_EQ(gone_matches, 0);
}

TEST_F(SqliteSidecarTest, PurgeObserverVersion_LeavesOtherObserversIntact) {
  SqliteObservationPersistence db(db_path_);
  db.save({
      PersistedObservation{key("fp", 0, "white_snr", "1.0.0"),
                           all_variants_record()},  // stale
      PersistedObservation{key("fp", 1, "white_snr", "2.0.0"),
                           all_variants_record()},  // current
      PersistedObservation{key("fp", 0, "biphase", "1.0.0"),
                           all_variants_record()},  // other observer
  });

  const std::size_t removed = db.purge_observer_version("white_snr", "2.0.0");
  EXPECT_EQ(removed, 1u);
  EXPECT_EQ(db.record_count(), 2u);

  // The surviving records: current white_snr + biphase.
  EXPECT_TRUE(db.record_count() == 2u);
  int biphase = 0;
  int white_current = 0;
  int white_stale = 0;
  db.load_matching({fp("fp")}, [&](ObservationRecordKey lk, ObservationRecord) {
    if (lk.observer_id == "biphase") ++biphase;
    if (lk.observer_id == "white_snr" && lk.observer_version == "2.0.0")
      ++white_current;
    if (lk.observer_id == "white_snr" && lk.observer_version == "1.0.0")
      ++white_stale;
  });
  EXPECT_EQ(biphase, 1);
  EXPECT_EQ(white_current, 1);
  EXPECT_EQ(white_stale, 0);
}

TEST_F(SqliteSidecarTest, CorruptedDatabase_RecoversByRebuilding) {
  // Write garbage where a database is expected.
  {
    std::ofstream f(db_path_, std::ios::binary);
    f << "this is definitely not a sqlite database file, just noise!!!";
  }

  // Construction must not throw: it detects corruption and rebuilds.
  SqliteObservationPersistence db(db_path_);
  EXPECT_EQ(db.record_count(), 0u);

  // The rebuilt database is fully usable.
  const auto k = key("fp", 0, "id", "v");
  db.save({PersistedObservation{k, all_variants_record()}});
  ObservationRecord loaded;
  db.load_matching({fp("fp")},
                   [&](ObservationRecordKey, ObservationRecord rec) {
                     loaded = std::move(rec);
                   });
  EXPECT_EQ(loaded, all_variants_record());
}

}  // namespace
}  // namespace orc
