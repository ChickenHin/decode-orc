/*
 * File:        observation_store_persistence_test.cpp
 * Module:      orc-core tests
 * Purpose:     Unit tests for ObservationStore write-behind + warm start
 *              against a mock IObservationPersistence (Tasks 6.1-6.3)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "observation_persistence.h"
#include "observation_store.h"

namespace orc {
namespace {

NodeFingerprint fp(const std::string& v) { return NodeFingerprint{v}; }

ObservationRecordKey key(const std::string& fingerprint, uint64_t field,
                         const std::string& id, const std::string& version) {
  return ObservationRecordKey{fp(fingerprint), FieldID(field), id, version};
}

ObservationRecord record_with(int32_t v) {
  ObservationRecord r;
  r["ns"]["v"] = v;
  return r;
}

// In-memory persistence double. Thread-safe because the store drives save()
// from its background writer thread while the test thread reads.
class FakePersistence : public IObservationPersistence {
 public:
  void save(const std::vector<PersistedObservation>& records) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++save_calls_;
    for (const auto& item : records) {
      store_[flat_key(item.key)] = {item.key, item.record};
      ++saved_records_;
    }
  }

  void load_matching(
      const std::unordered_set<NodeFingerprint>& keep,
      const std::function<void(ObservationRecordKey, ObservationRecord)>& sink)
      override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [flat, entry] : store_) {
      if (keep.find(entry.first.fingerprint) != keep.end()) {
        sink(entry.first, entry.second);
      }
    }
  }

  std::size_t retain_only(
      const std::unordered_set<NodeFingerprint>& keep) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t removed = 0;
    for (auto it = store_.begin(); it != store_.end();) {
      if (keep.find(it->second.first.fingerprint) == keep.end()) {
        it = store_.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
    return removed;
  }

  std::size_t purge_observer_version(
      const std::string& observer_id,
      const std::string& current_version) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t removed = 0;
    for (auto it = store_.begin(); it != store_.end();) {
      if (it->second.first.observer_id == observer_id &&
          it->second.first.observer_version != current_version) {
        it = store_.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
    return removed;
  }

  std::optional<ObservationRecord> load_one(
      const ObservationRecordKey& k) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++load_one_calls_;
    auto it = store_.find(flat_key(k));
    if (it == store_.end()) {
      return std::nullopt;
    }
    return it->second.second;
  }

  std::size_t record_count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return store_.size();
  }
  int load_one_calls() {
    std::lock_guard<std::mutex> lock(mutex_);
    return load_one_calls_;
  }
  int save_calls() {
    std::lock_guard<std::mutex> lock(mutex_);
    return save_calls_;
  }
  int saved_records() {
    std::lock_guard<std::mutex> lock(mutex_);
    return saved_records_;
  }

 private:
  static std::string flat_key(const ObservationRecordKey& k) {
    return k.fingerprint.value + "|" + std::to_string(k.field_id.value()) +
           "|" + k.observer_id + "|" + k.observer_version;
  }

  std::mutex mutex_;
  std::map<std::string, std::pair<ObservationRecordKey, ObservationRecord>>
      store_;
  int save_calls_ = 0;
  int saved_records_ = 0;
  int load_one_calls_ = 0;
};

// ---------------------------------------------------------------------------
// Read-through: memory is a cache over the sidecar — budget eviction reloads
// instead of recomputing
// ---------------------------------------------------------------------------

TEST(ObservationStorePersistence_EvictedRecord_ReloadsFromSidecar,
     ReadThrough) {
  auto persistence = std::make_shared<FakePersistence>();
  // A budget of one byte keeps only the most recent record resident, so every
  // earlier record is evicted from memory (but stays durable in the sidecar).
  ObservationStore store(/*memory_budget_bytes=*/1);
  store.set_persistence(persistence);

  const auto evicted = key("A", 0, "obs", "1");
  const auto resident = key("A", 1, "obs", "1");
  store.put(evicted, record_with(1));
  store.put(resident, record_with(2));
  store.flush();  // both durably saved; only `resident` is still in memory

  // has() reads through and re-installs the evicted record…
  EXPECT_TRUE(store.has(evicted));
  EXPECT_GE(persistence->load_one_calls(), 1);

  // …and get() serves the reloaded values.
  const auto rec = store.get(evicted);
  ASSERT_TRUE(rec.has_value());
  EXPECT_EQ(std::get<int32_t>((*rec).at("ns").at("v")), 1);

  // A key never persisted is a genuine miss.
  EXPECT_FALSE(store.has(key("A", 2, "obs", "1")));
}

TEST(ObservationStorePersistence_EvictionWithoutSidecar_IsAGenuineMiss,
     ReadThrough) {
  ObservationStore store(/*memory_budget_bytes=*/1);  // no persistence attached

  const auto evicted = key("A", 0, "obs", "1");
  store.put(evicted, record_with(1));
  store.put(key("A", 1, "obs", "1"), record_with(2));  // evicts the first

  EXPECT_FALSE(store.has(evicted));
  EXPECT_FALSE(store.get(evicted).has_value());
}

// ---------------------------------------------------------------------------
// Write-behind
// ---------------------------------------------------------------------------

TEST(ObservationStorePersistence_Put_WritesThroughToPersistence, WriteBehind) {
  auto persistence = std::make_shared<FakePersistence>();
  ObservationStore store;
  store.set_persistence(persistence);

  store.put(key("a", 0, "white_snr", "1.0.0"), record_with(1));
  store.put(key("a", 1, "white_snr", "1.0.0"), record_with(2));
  store.flush();

  EXPECT_EQ(persistence->saved_records(), 2);
  EXPECT_EQ(persistence->record_count(), 2u);
}

TEST(ObservationStorePersistence_WithoutPersistence_NeverPersists,
     WriteBehind) {
  // Default store has no persistence: put()/flush() are pure in-memory.
  ObservationStore store;
  store.put(key("a", 0, "id", "v"), record_with(1));
  store.flush();  // no-op, must not hang
  EXPECT_TRUE(store.has(key("a", 0, "id", "v")));
}

TEST(ObservationStorePersistence_Flush_DrainsAllQueuedRecords, WriteBehind) {
  auto persistence = std::make_shared<FakePersistence>();
  ObservationStore store;
  store.set_persistence(persistence);

  constexpr int kCount = 500;
  for (int i = 0; i < kCount; ++i) {
    store.put(key("fp", static_cast<uint64_t>(i), "id", "v"), record_with(i));
  }
  store.flush();
  EXPECT_EQ(persistence->saved_records(), kCount);
}

TEST(ObservationStorePersistence_Destructor_FlushesPending, WriteBehind) {
  auto persistence = std::make_shared<FakePersistence>();
  {
    ObservationStore store;
    store.set_persistence(persistence);
    for (int i = 0; i < 100; ++i) {
      store.put(key("fp", static_cast<uint64_t>(i), "id", "v"), record_with(i));
    }
    // No explicit flush: destruction must drain the queue.
  }
  EXPECT_EQ(persistence->record_count(), 100u);
}

// ---------------------------------------------------------------------------
// Warm start
// ---------------------------------------------------------------------------

TEST(ObservationStorePersistence_WarmStart_LoadsMatchingFingerprints, Warm) {
  auto persistence = std::make_shared<FakePersistence>();
  {
    ObservationStore writer;
    writer.set_persistence(persistence);
    writer.put(key("keep", 0, "id", "v"), record_with(10));
    writer.put(key("drop", 0, "id", "v"), record_with(20));
    writer.flush();
  }

  ObservationStore reader;
  reader.set_persistence(persistence);
  reader.warm_start({fp("keep")});

  EXPECT_TRUE(reader.has(key("keep", 0, "id", "v")));
  EXPECT_EQ((*reader.get(key("keep", 0, "id", "v")))["ns"]["v"],
            ObservationValue(static_cast<int32_t>(10)));

  // warm_start() governs what is pre-installed in memory, not what is
  // reachable: a record outside the warm set is still found via sidecar
  // read-through (content-addressed keys make it correct for whoever asks;
  // truly unreachable fingerprints are removed by the sidecar GC instead).
  EXPECT_TRUE(reader.has(key("drop", 0, "id", "v")));
  EXPECT_EQ(std::get<int32_t>(
                (*reader.get(key("drop", 0, "id", "v"))).at("ns").at("v")),
            20);
}

TEST(ObservationStorePersistence_WarmStart_DoesNotRePersist, Warm) {
  auto persistence = std::make_shared<FakePersistence>();
  {
    ObservationStore writer;
    writer.set_persistence(persistence);
    writer.put(key("keep", 0, "id", "v"), record_with(10));
    writer.flush();
  }
  const int saves_before = persistence->save_calls();

  ObservationStore reader;
  reader.set_persistence(persistence);
  reader.warm_start({fp("keep")});
  reader.flush();

  // Warm-loaded records are already durable; they must not be re-queued.
  EXPECT_EQ(persistence->save_calls(), saves_before);
}

// ---------------------------------------------------------------------------
// Lifecycle (Task 6.3) — forwarding to persistence
// ---------------------------------------------------------------------------

TEST(ObservationStorePersistence_GcPersistence_DropsUnreachable, Lifecycle) {
  auto persistence = std::make_shared<FakePersistence>();
  ObservationStore store;
  store.set_persistence(persistence);
  store.put(key("keep", 0, "id", "v"), record_with(1));
  store.put(key("gone", 0, "id", "v"), record_with(2));
  store.flush();

  store.gc_persistence({fp("keep")});
  EXPECT_EQ(persistence->record_count(), 1u);
}

TEST(ObservationStorePersistence_PurgeObserverVersion_OnlyThatObserver,
     Lifecycle) {
  auto persistence = std::make_shared<FakePersistence>();
  ObservationStore store;
  store.set_persistence(persistence);
  store.put(key("fp", 0, "white_snr", "1.0.0"), record_with(1));
  store.put(key("fp", 0, "white_snr", "2.0.0"), record_with(2));  // newer
  store.put(key("fp", 0, "biphase", "1.0.0"), record_with(3));
  store.flush();

  store.purge_observer_version("white_snr", "2.0.0");

  // Old white_snr record removed; current white_snr and biphase intact.
  EXPECT_EQ(persistence->record_count(), 2u);
}

TEST(ObservationStorePersistence_SetNullptr_StopsWriterCleanly, Lifecycle) {
  auto persistence = std::make_shared<FakePersistence>();
  ObservationStore store;
  store.set_persistence(persistence);
  store.put(key("a", 0, "id", "v"), record_with(1));
  store.set_persistence(nullptr);  // flushes and stops the writer

  EXPECT_EQ(persistence->record_count(), 1u);
  // Subsequent puts are in-memory only.
  store.put(key("b", 0, "id", "v"), record_with(2));
  store.flush();
  EXPECT_EQ(persistence->record_count(), 1u);
  EXPECT_TRUE(store.has(key("b", 0, "id", "v")));
}

}  // namespace
}  // namespace orc
