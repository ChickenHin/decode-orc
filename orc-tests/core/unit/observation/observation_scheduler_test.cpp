/*
 * File:        observation_scheduler_test.cpp
 * Module:      orc-tests/core/unit/observation
 * Purpose:     Unit tests for the background ObservationScheduler, its default
 *              scheduling policy, and progress/completion reporting
 *
 * All tests drive the scheduler through a mocked IObservationTaskRunner and a
 * scripted IObservationSchedulingPolicy — no filesystem, network, clock, or
 * pipeline execution. Deterministic queue-logic tests use the synchronous
 * process_one_for_testing() driver; the two threading tests use start()/stop()
 * with a latch-gated runner (condition-variable predicates, never sleeps).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "observation_scheduler.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace orc {
namespace {

NodeFingerprint fp(const std::string& value) { return NodeFingerprint{value}; }

// Records every observe_frame() call in order. Optionally throws for a chosen
// (node, frame) to exercise item-failure handling, and optionally gates the
// first N calls behind a latch so a test can observe a genuinely in-flight
// item.
class MockTaskRunner final : public IObservationTaskRunner {
 public:
  struct Call {
    NodeID node;
    FrameID frame;
  };

  void observe_frame(NodeID node, const NodeFingerprint& /*fingerprint*/,
                     FrameID frame,
                     const std::vector<std::string>& /*ids*/) override {
    std::unique_lock<std::mutex> lock(mutex_);
    calls_.push_back({node, frame});
    entered_ += 1;
    entered_cv_.notify_all();
    if (throw_on_ && throw_on_->first == node && throw_on_->second == frame) {
      throw std::runtime_error("injected observe failure");
    }
    if (gated_) {
      release_cv_.wait(lock, [&] { return released_; });
    }
  }

  void update_dag(
      std::shared_ptr<const DAG> /*dag*/,
      std::shared_ptr<const NodeFingerprintMap> /*fingerprints*/) override {
    std::lock_guard<std::mutex> lock(mutex_);
    dag_updates_ += 1;
  }

  // --- inspection ---
  std::vector<Call> calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_;
  }
  std::size_t call_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_.size();
  }
  int dag_updates() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dag_updates_;
  }

  // --- configuration ---
  void throw_on(NodeID node, FrameID frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    throw_on_ = std::make_pair(node, frame);
  }
  void arm_gate() {
    std::lock_guard<std::mutex> lock(mutex_);
    gated_ = true;
    released_ = false;
  }
  void wait_until_entered(int n) {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_cv_.wait(lock, [&] { return entered_ >= n; });
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    release_cv_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<Call> calls_;
  int dag_updates_ = 0;
  int entered_ = 0;
  std::optional<std::pair<NodeID, FrameID>> throw_on_;
  bool gated_ = false;
  bool released_ = true;
  std::condition_variable entered_cv_;
  std::condition_variable release_cv_;
};

// Returns canned plans and records how it was called.
class ScriptedPolicy final : public IObservationSchedulingPolicy {
 public:
  std::vector<ObservationWorkItem> sweep_plan;
  std::vector<ObservationWorkItem> prefetch_plan;
  std::vector<ObservationWorkItem> invalidation_plan;
  ObservationWorkItem interactive_template;

  int sweep_calls = 0;
  int prefetch_calls = 0;
  int invalidation_calls = 0;
  int interactive_calls = 0;
  ObservationSchedulingContext last_context;
  NodeID last_interactive_node;
  FrameID last_interactive_frame = 0;

  std::vector<ObservationWorkItem> plan_sweep(
      const ObservationSchedulingContext& c) override {
    ++sweep_calls;
    last_context = c;
    return sweep_plan;
  }
  std::vector<ObservationWorkItem> plan_prefetch(
      const ObservationSchedulingContext& c) override {
    ++prefetch_calls;
    last_context = c;
    return prefetch_plan;
  }
  std::vector<ObservationWorkItem> plan_invalidation(
      const ObservationSchedulingContext& c) override {
    ++invalidation_calls;
    last_context = c;
    return invalidation_plan;
  }
  ObservationWorkItem plan_interactive(
      NodeID node, FrameID frame,
      const ObservationSchedulingContext& c) override {
    ++interactive_calls;
    last_context = c;
    last_interactive_node = node;
    last_interactive_frame = frame;
    ObservationWorkItem item = interactive_template;
    item.node_id = node;
    item.frames = FrameIDRange{frame, frame};
    item.priority = ObservationPriority::kInteractive;
    return item;
  }
};

// One-frame item helper.
ObservationWorkItem frame_item(NodeID node, NodeFingerprint fingerprint,
                               FrameID frame, ObservationPriority priority) {
  ObservationWorkItem item;
  item.node_id = node;
  item.fingerprint = std::move(fingerprint);
  item.frames = FrameIDRange{frame, frame};
  item.priority = priority;
  return item;
}

std::unique_ptr<ObservationScheduler> make_scheduler(
    MockTaskRunner** runner_out,
    std::shared_ptr<const NodeFingerprintMap> fingerprints = nullptr,
    std::shared_ptr<IObservationSchedulingPolicy> policy = nullptr) {
  auto runner = std::make_unique<MockTaskRunner>();
  *runner_out = runner.get();
  return std::make_unique<ObservationScheduler>(
      std::move(runner), std::move(fingerprints), std::move(policy));
}

// Shared sink for the multi-worker (pool) tests: every worker's runner records
// its observed (node, frame) pairs here, and — when a barrier target is set —
// each observe arrives at a barrier so a test can prove K frames run at once.
struct PoolSink {
  std::mutex mutex;
  std::condition_variable observed_cv;
  std::vector<std::pair<NodeID::value_type, FrameID>> observed;
  int runners_created = 0;

  std::mutex bmutex;
  std::condition_variable bcv;
  int arrived = 0;
  int target = 0;  // 0 disables the barrier
};

class PoolMockRunner final : public IObservationTaskRunner {
 public:
  explicit PoolMockRunner(PoolSink* sink) : sink_(sink) {}

  void observe_frame(NodeID node, const NodeFingerprint& /*fingerprint*/,
                     FrameID frame,
                     const std::vector<std::string>& /*ids*/) override {
    {
      std::lock_guard<std::mutex> lock(sink_->mutex);
      sink_->observed.emplace_back(node.value(), frame);
      sink_->observed_cv.notify_all();
    }
    if (sink_->target > 0) {
      std::unique_lock<std::mutex> lock(sink_->bmutex);
      ++sink_->arrived;
      sink_->bcv.notify_all();
      sink_->bcv.wait_for(lock, std::chrono::seconds(5),
                          [&] { return sink_->arrived >= sink_->target; });
    }
  }

  void update_dag(
      std::shared_ptr<const DAG> /*dag*/,
      std::shared_ptr<const NodeFingerprintMap> /*fingerprints*/) override {}

 private:
  PoolSink* sink_;
};

std::unique_ptr<ObservationScheduler> make_pool_scheduler(
    PoolSink* sink, unsigned workers,
    std::shared_ptr<IObservationSchedulingPolicy> policy = nullptr) {
  ObservationScheduler::TaskRunnerFactory factory = [sink]() {
    ++sink->runners_created;
    return std::make_unique<PoolMockRunner>(sink);
  };
  return std::make_unique<ObservationScheduler>(std::move(factory), workers,
                                                nullptr, std::move(policy));
}

// ---------------------------------------------------------------------------
// Task 4.2 — scheduler core
// ---------------------------------------------------------------------------

TEST(ObservationScheduler, ProcessesHigherPriorityWorkFirst) {
  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner);

  // Submit in reverse priority order; expect processing in priority order.
  scheduler->submit(
      frame_item(NodeID(1), fp("a"), 10, ObservationPriority::kSweep));
  scheduler->submit(
      frame_item(NodeID(2), fp("b"), 20, ObservationPriority::kPrefetch));
  scheduler->submit(
      frame_item(NodeID(3), fp("c"), 30, ObservationPriority::kInteractive));

  while (scheduler->process_one_for_testing()) {
  }

  const auto calls = runner->calls();
  ASSERT_EQ(calls.size(), 3u);
  EXPECT_EQ(calls[0].node, NodeID(3));  // interactive
  EXPECT_EQ(calls[1].node, NodeID(2));  // prefetch
  EXPECT_EQ(calls[2].node, NodeID(1));  // sweep
}

TEST(ObservationScheduler, PreservesFifoWithinAPriorityClass) {
  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner);

  for (FrameID f = 0; f < 4; ++f) {
    scheduler->submit(
        frame_item(NodeID(1), fp("a"), f, ObservationPriority::kSweep));
  }
  while (scheduler->process_one_for_testing()) {
  }

  const auto calls = runner->calls();
  ASSERT_EQ(calls.size(), 4u);
  for (FrameID f = 0; f < 4; ++f) {
    EXPECT_EQ(calls[f].frame, f);
  }
}

TEST(ObservationScheduler, ObservesStatefulItemFramesInAscendingOrder) {
  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner);

  ObservationWorkItem item;
  item.node_id = NodeID(1);
  item.fingerprint = fp("a");
  item.frames = FrameIDRange{0, 4};
  item.stateful = true;
  item.priority = ObservationPriority::kSweep;
  scheduler->submit(item);

  while (scheduler->process_one_for_testing()) {
  }

  const auto calls = runner->calls();
  ASSERT_EQ(calls.size(), 5u);
  for (FrameID f = 0; f < 5; ++f) {
    EXPECT_EQ(calls[f].frame, f);
  }
}

TEST(ObservationScheduler, DropsQueuedItemsWithStaleFingerprintOnDagChange) {
  auto old_map = std::make_shared<NodeFingerprintMap>();
  (*old_map)[NodeID(1)] = fp("n1_v1");
  (*old_map)[NodeID(2)] = fp("n2_v1");

  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner, old_map);

  scheduler->submit(
      frame_item(NodeID(1), fp("n1_v1"), 0, ObservationPriority::kSweep));
  scheduler->submit(
      frame_item(NodeID(2), fp("n2_v1"), 0, ObservationPriority::kSweep));
  EXPECT_EQ(scheduler->queued_count(), 2u);

  // Node 1's content changed; node 2 unchanged.
  auto new_map = std::make_shared<NodeFingerprintMap>();
  (*new_map)[NodeID(1)] = fp("n1_v2");
  (*new_map)[NodeID(2)] = fp("n2_v1");
  scheduler->on_dag_changed(/*dag=*/nullptr, new_map);

  EXPECT_EQ(scheduler->queued_count(), 1u);

  // Draining also applies the pending DAG update (forwarded to the runner).
  while (scheduler->process_one_for_testing()) {
  }

  const auto calls = runner->calls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].node, NodeID(2));  // only the still-valid item ran
  EXPECT_EQ(runner->dag_updates(), 1);
}

TEST(ObservationScheduler, ItemFailureDoesNotStopSubsequentWork) {
  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner);
  runner->throw_on(NodeID(1), 0);

  int completions = 0;
  bool first_succeeded = true;
  scheduler->set_completion_callback([&](const ObservationCompletion& c) {
    ++completions;
    if (c.node_id == NodeID(1)) {
      first_succeeded = c.succeeded;
    }
  });

  scheduler->submit(
      frame_item(NodeID(1), fp("a"), 0, ObservationPriority::kSweep));
  scheduler->submit(
      frame_item(NodeID(2), fp("b"), 0, ObservationPriority::kSweep));

  while (scheduler->process_one_for_testing()) {
  }

  EXPECT_EQ(completions, 2);
  EXPECT_FALSE(first_succeeded);  // the failing item is reported failed
  // The second item still ran despite the first throwing.
  const auto calls = runner->calls();
  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[1].node, NodeID(2));
}

TEST(ObservationScheduler, StartThenStopJoinsCleanly) {
  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner);

  scheduler->start();
  EXPECT_TRUE(scheduler->is_running());
  scheduler->stop();
  EXPECT_FALSE(scheduler->is_running());

  // Idempotent: a second stop() is a harmless no-op.
  scheduler->stop();
  EXPECT_FALSE(scheduler->is_running());
}

TEST(ObservationScheduler, DagChangeAbortsInFlightStaleItem) {
  auto old_map = std::make_shared<NodeFingerprintMap>();
  (*old_map)[NodeID(1)] = fp("v1");

  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner, old_map);
  runner->arm_gate();  // block inside the first observe_frame

  std::atomic<int> completions{0};
  ObservationCompletion last{};
  scheduler->set_completion_callback([&](const ObservationCompletion& c) {
    last = c;
    completions.fetch_add(1);
  });

  ObservationWorkItem item;
  item.node_id = NodeID(1);
  item.fingerprint = fp("v1");
  item.frames = FrameIDRange{0, 9};  // long run
  item.stateful = true;
  scheduler->start();
  scheduler->submit(item);

  runner->wait_until_entered(1);  // frame 0 is now genuinely in-flight

  // Node 1's content changes: the in-flight item is now stale.
  auto new_map = std::make_shared<NodeFingerprintMap>();
  (*new_map)[NodeID(1)] = fp("v2");
  scheduler->on_dag_changed(nullptr, new_map);

  runner->release();  // let frame 0 finish; the loop must then abort
  scheduler->stop();

  EXPECT_EQ(completions.load(), 1);
  EXPECT_TRUE(last.cancelled);
  EXPECT_LT(last.frames_observed, last.frames_total);
}

// ---------------------------------------------------------------------------
// Multi-worker pool
// ---------------------------------------------------------------------------

TEST(ObservationScheduler, PoolObservesDistinctItemsConcurrently) {
  constexpr unsigned kWorkers = 4;
  PoolSink sink;
  sink.target = static_cast<int>(kWorkers);  // barrier: all K observe at once

  auto scheduler = make_pool_scheduler(&sink, kWorkers);
  EXPECT_EQ(scheduler->worker_count(), static_cast<std::size_t>(kWorkers));
  EXPECT_EQ(sink.runners_created, static_cast<int>(kWorkers));

  scheduler->start();
  // One independent single-frame item per worker (single-frame items are never
  // chunked), so each worker takes exactly one.
  for (unsigned i = 0; i < kWorkers; ++i) {
    scheduler->submit(
        frame_item(NodeID(i + 1), fp("a"), 0, ObservationPriority::kSweep));
  }

  // The barrier can only open if all K frames are inside observe_frame at the
  // same time — impossible without K genuinely-parallel worker threads.
  {
    std::unique_lock<std::mutex> lock(sink.bmutex);
    ASSERT_TRUE(sink.bcv.wait_for(lock, std::chrono::seconds(5), [&] {
      return sink.arrived >= static_cast<int>(kWorkers);
    }));
  }
  scheduler->stop();

  EXPECT_EQ(sink.observed.size(), static_cast<std::size_t>(kWorkers));
}

TEST(ObservationScheduler, PoolChunksStatelessRangeWithFullCoverage) {
  constexpr unsigned kWorkers = 4;
  constexpr FrameID kFrames = 40;
  PoolSink sink;  // no barrier

  auto scheduler = make_pool_scheduler(&sink, kWorkers);

  // A single stateless whole-range item: the pool must split it into disjoint
  // chunks and observe every frame exactly once (order across chunks is free).
  ObservationWorkItem item;
  item.node_id = NodeID(1);
  item.fingerprint = fp("a");
  item.frames = FrameIDRange{0, kFrames - 1};
  item.stateful = false;

  scheduler->start();
  scheduler->submit(item);

  {
    std::unique_lock<std::mutex> lock(sink.mutex);
    ASSERT_TRUE(sink.observed_cv.wait_for(lock, std::chrono::seconds(10), [&] {
      return sink.observed.size() >= kFrames;
    }));
  }
  scheduler->stop();

  std::set<FrameID> frames;
  for (const auto& [node, frame] : sink.observed) {
    EXPECT_EQ(node, NodeID(1).value());
    frames.insert(frame);
  }
  // Exactly the full range, with no duplicated frames across chunks.
  EXPECT_EQ(sink.observed.size(), static_cast<std::size_t>(kFrames));
  EXPECT_EQ(frames.size(), static_cast<std::size_t>(kFrames));
  EXPECT_EQ(*frames.begin(), 0u);
  EXPECT_EQ(*frames.rbegin(), kFrames - 1);
}

// Large stateless items are split into bounded chunks (max ~128 frames each)
// so workers return to the priority queues frequently — an interactive request
// arriving mid-sweep is only ever behind the in-flight chunks, never behind a
// whole node's range.
TEST(ObservationScheduler, LargeStatelessItemsAreCappedForPreemption) {
  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner);

  ObservationWorkItem item;
  item.node_id = NodeID(1);
  item.fingerprint = fp("a");
  item.frames = FrameIDRange{0, 999};  // 1000 frames
  item.stateful = false;
  scheduler->submit(item);

  // 1000 frames under a 128-frame cap => at least 8 queued chunks even on a
  // single-worker scheduler.
  EXPECT_GE(scheduler->queued_count(), 8u);

  // Coverage is exact: every frame observed exactly once across the chunks.
  while (scheduler->process_one_for_testing()) {
  }
  EXPECT_EQ(runner->call_count(), 1000u);
}

// A stateful whole-range item must NOT be chunked: it stays a single item so
// its frames are observed in ascending order on one worker.
TEST(ObservationScheduler, PoolDoesNotChunkStatefulItems) {
  constexpr unsigned kWorkers = 4;
  constexpr FrameID kFrames = 12;
  PoolSink sink;

  auto scheduler = make_pool_scheduler(&sink, kWorkers);

  ObservationWorkItem item;
  item.node_id = NodeID(1);
  item.fingerprint = fp("a");
  item.frames = FrameIDRange{0, kFrames - 1};
  item.stateful = true;

  scheduler->start();
  scheduler->submit(item);

  {
    std::unique_lock<std::mutex> lock(sink.mutex);
    ASSERT_TRUE(sink.observed_cv.wait_for(lock, std::chrono::seconds(10), [&] {
      return sink.observed.size() >= kFrames;
    }));
  }
  scheduler->stop();

  ASSERT_EQ(sink.observed.size(), static_cast<std::size_t>(kFrames));
  // One undivided item on one worker → strictly ascending frame order.
  for (FrameID f = 0; f < kFrames; ++f) {
    EXPECT_EQ(sink.observed[f].second, f);
  }
}

// ---------------------------------------------------------------------------
// Task 4.3 — scheduling policy (scripted + default)
// ---------------------------------------------------------------------------

TEST(ObservationScheduler, ProjectLoadEnqueuesPolicySweep) {
  auto policy = std::make_shared<ScriptedPolicy>();
  policy->sweep_plan = {
      frame_item(NodeID(1), fp("a"), 0, ObservationPriority::kSweep),
      frame_item(NodeID(2), fp("b"), 0, ObservationPriority::kSweep)};

  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner, nullptr, policy);

  ObservationSchedulingContext ctx;
  scheduler->on_project_loaded(ctx);

  EXPECT_EQ(policy->sweep_calls, 1);
  EXPECT_EQ(scheduler->queued_count(), 2u);
}

TEST(ObservationScheduler, InvalidationEnqueuesOnlyChangedNodes) {
  auto policy = std::make_shared<ScriptedPolicy>();
  policy->invalidation_plan = {
      frame_item(NodeID(7), fp("x"), 0, ObservationPriority::kSweep)};

  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner, nullptr, policy);

  ObservationSchedulingContext ctx;
  ctx.changed_nodes = {NodeID(7)};
  scheduler->on_invalidation(ctx);

  EXPECT_EQ(policy->invalidation_calls, 1);
  ASSERT_EQ(policy->last_context.changed_nodes.size(), 1u);
  EXPECT_EQ(policy->last_context.changed_nodes[0], NodeID(7));

  while (scheduler->process_one_for_testing()) {
  }
  const auto calls = runner->calls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].node, NodeID(7));
}

TEST(ObservationScheduler, PrefetchFollowsPreviewPosition) {
  auto policy = std::make_shared<ScriptedPolicy>();
  policy->prefetch_plan = {
      frame_item(NodeID(1), fp("a"), 100, ObservationPriority::kPrefetch)};

  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner, nullptr, policy);

  ObservationSchedulingContext ctx;
  ctx.preview_position = 100;
  scheduler->on_preview_moved(ctx);

  EXPECT_EQ(policy->prefetch_calls, 1);
  EXPECT_EQ(policy->last_context.preview_position, 100u);
  EXPECT_EQ(scheduler->queued_count(), 1u);
}

TEST(ObservationScheduler, InteractiveRequestEnqueuesAtTopPriority) {
  auto policy = std::make_shared<ScriptedPolicy>();
  policy->interactive_template.fingerprint = fp("live");

  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner, nullptr, policy);

  // Pre-load lower-priority work; the interactive frame must still run first.
  scheduler->submit(
      frame_item(NodeID(9), fp("z"), 0, ObservationPriority::kSweep));

  ObservationSchedulingContext ctx;
  const NodeFingerprint keyed =
      scheduler->request_interactive(NodeID(5), 42, ctx);
  EXPECT_EQ(keyed, fp("live"));
  EXPECT_EQ(policy->interactive_calls, 1);
  EXPECT_EQ(policy->last_interactive_frame, 42u);

  while (scheduler->process_one_for_testing()) {
  }
  const auto calls = runner->calls();
  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0].node, NodeID(5));  // interactive ran before the sweep
  EXPECT_EQ(calls[0].frame, 42u);
}

TEST(DefaultObservationSchedulingPolicy, SweepSplitsStatelessAndStatefulItems) {
  DefaultObservationSchedulingPolicy policy({"white_snr", "closed_caption"},
                                            {"closed_caption"});

  auto map = std::make_shared<NodeFingerprintMap>();
  (*map)[NodeID(1)] = fp("n1");

  ObservationSchedulingContext ctx;
  ctx.fingerprints = map;
  ctx.total_frames = 50;
  ctx.nodes_of_interest = {NodeID(1)};

  const auto items = policy.plan_sweep(ctx);
  ASSERT_EQ(items.size(), 2u);

  const ObservationWorkItem* stateless = nullptr;
  const ObservationWorkItem* stateful = nullptr;
  for (const auto& item : items) {
    (item.stateful ? stateful : stateless) = &item;
  }
  ASSERT_NE(stateless, nullptr);
  ASSERT_NE(stateful, nullptr);

  EXPECT_EQ(stateless->observer_ids, std::vector<std::string>{"white_snr"});
  EXPECT_EQ(stateful->observer_ids, std::vector<std::string>{"closed_caption"});
  EXPECT_EQ(stateless->frames.first, 0u);
  EXPECT_EQ(stateless->frames.last, 49u);
  EXPECT_EQ(stateful->fingerprint, fp("n1"));
  EXPECT_EQ(stateless->priority, ObservationPriority::kSweep);
}

TEST(DefaultObservationSchedulingPolicy, PrefetchClampsWindowToPreview) {
  DefaultObservationSchedulingPolicy policy({"white_snr"}, {},
                                            /*prefetch_radius=*/10);

  auto map = std::make_shared<NodeFingerprintMap>();
  (*map)[NodeID(1)] = fp("n1");

  ObservationSchedulingContext ctx;
  ctx.fingerprints = map;
  ctx.total_frames = 1000;
  ctx.preview_position = 5;  // near the start: window clamps at 0
  ctx.nodes_of_interest = {NodeID(1)};

  const auto items = policy.plan_prefetch(ctx);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].frames.first, 0u);  // 5 - 10 clamped to 0
  EXPECT_EQ(items[0].frames.last, 15u);  // 5 + 10
  EXPECT_EQ(items[0].priority, ObservationPriority::kPrefetch);
}

TEST(DefaultObservationSchedulingPolicy, InvalidationTargetsOnlyChangedNodes) {
  DefaultObservationSchedulingPolicy policy({"white_snr"}, {});

  auto map = std::make_shared<NodeFingerprintMap>();
  (*map)[NodeID(1)] = fp("n1");
  (*map)[NodeID(2)] = fp("n2");

  ObservationSchedulingContext ctx;
  ctx.fingerprints = map;
  ctx.total_frames = 10;
  ctx.nodes_of_interest = {NodeID(1), NodeID(2)};
  ctx.changed_nodes = {NodeID(2)};

  const auto items = policy.plan_invalidation(ctx);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].node_id, NodeID(2));
  EXPECT_EQ(items[0].fingerprint, fp("n2"));
}

// ---------------------------------------------------------------------------
// Task 4.4 — progress and completion reporting
// ---------------------------------------------------------------------------

TEST(ObservationScheduler, ReportsMonotonicProgressPerFrame) {
  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner);

  std::vector<ObservationProgress> updates;
  scheduler->set_progress_callback(
      [&](const ObservationProgress& p) { updates.push_back(p); });

  ObservationWorkItem item;
  item.node_id = NodeID(1);
  item.fingerprint = fp("a");
  item.frames = FrameIDRange{0, 3};  // four frames
  scheduler->submit(item);

  while (scheduler->process_one_for_testing()) {
  }

  ASSERT_EQ(updates.size(), 4u);
  for (std::size_t i = 0; i < updates.size(); ++i) {
    EXPECT_EQ(updates[i].frames_observed, i + 1);  // 1,2,3,4 — monotonic
    EXPECT_EQ(updates[i].frames_total, 4u);
    EXPECT_EQ(updates[i].node_id, NodeID(1));
  }
}

TEST(ObservationScheduler, CompletionFiresExactlyOncePerItem) {
  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner);

  int completions = 0;
  ObservationCompletion last{};
  scheduler->set_completion_callback([&](const ObservationCompletion& c) {
    ++completions;
    last = c;
  });

  scheduler->submit(
      frame_item(NodeID(1), fp("a"), 0, ObservationPriority::kSweep));
  scheduler->submit(
      frame_item(NodeID(2), fp("b"), 0, ObservationPriority::kSweep));
  scheduler->submit(
      frame_item(NodeID(3), fp("c"), 0, ObservationPriority::kSweep));

  while (scheduler->process_one_for_testing()) {
  }

  EXPECT_EQ(completions, 3);
  EXPECT_TRUE(last.succeeded);
  EXPECT_FALSE(last.cancelled);
  EXPECT_EQ(last.frames_observed, last.frames_total);
}

TEST(ObservationScheduler, NoCallbacksFireAfterShutdown) {
  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner);

  std::atomic<bool> shut_down{false};
  std::atomic<int> completions{0};
  scheduler->set_completion_callback([&](const ObservationCompletion&) {
    // Must never run once stop() has joined the worker.
    EXPECT_FALSE(shut_down.load());
    completions.fetch_add(1);
  });

  scheduler->start();
  for (FrameID f = 0; f < 20; ++f) {
    scheduler->submit(
        frame_item(NodeID(1), fp("a"), f, ObservationPriority::kSweep));
  }
  scheduler->stop();
  shut_down.store(true);

  // Dropped-queued items never complete, so at most one-per-started-item fired.
  EXPECT_LE(completions.load(), 20);
  // Submitting after shutdown is a no-op and triggers no callback.
  scheduler->submit(
      frame_item(NodeID(2), fp("b"), 0, ObservationPriority::kSweep));
  EXPECT_EQ(scheduler->queued_count(), 0u);
}

// ---------------------------------------------------------------------------
// Task 5.4 — workload aggregation for the status-line progress indicator
// ---------------------------------------------------------------------------

TEST(ObservationScheduler, WorkloadPercentStaysBoundedAndEndsIdleOnDrain) {
  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner);

  std::vector<ObservationWorkload> snapshots;
  scheduler->set_workload_callback(
      [&](const ObservationWorkload& w) { snapshots.push_back(w); });

  ObservationWorkItem item;
  item.node_id = NodeID(1);
  item.fingerprint = fp("a");
  item.frames = FrameIDRange{0, 3};  // four frames
  scheduler->submit(item);

  // Enqueue makes the workload active at 0%.
  ASSERT_FALSE(snapshots.empty());
  EXPECT_TRUE(snapshots.front().active);
  EXPECT_EQ(snapshots.front().frames_total, 4u);
  EXPECT_EQ(snapshots.front().percent_complete, 0);
  EXPECT_EQ(snapshots.front().outstanding_nodes, 1u);

  while (scheduler->process_one_for_testing()) {
  }

  // Every snapshot stays in range and observed never exceeds total.
  bool saw_active_progress = false;
  for (const auto& w : snapshots) {
    EXPECT_GE(w.percent_complete, 0);
    EXPECT_LE(w.percent_complete, 100);
    EXPECT_LE(w.frames_observed, w.frames_total);
    if (w.active && w.percent_complete > 0) {
      saw_active_progress = true;
    }
  }
  EXPECT_TRUE(saw_active_progress);

  // Draining the queue returns the aggregate to idle exactly.
  const ObservationWorkload final = scheduler->workload();
  EXPECT_FALSE(final.active);
  EXPECT_EQ(final.frames_total, 0u);
  EXPECT_EQ(final.frames_observed, 0u);
  EXPECT_EQ(final.percent_complete, 0);
  EXPECT_EQ(final.outstanding_nodes, 0u);
  ASSERT_FALSE(snapshots.empty());
  EXPECT_FALSE(snapshots.back().active);  // last emitted snapshot is idle
}

TEST(ObservationScheduler, EnqueueAfterDrainReactivatesWorkloadAtZeroPercent) {
  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner);

  scheduler->submit(
      frame_item(NodeID(1), fp("a"), 0, ObservationPriority::kSweep));
  ASSERT_TRUE(scheduler->process_one_for_testing());  // observe the one frame

  // Queue drained -> idle.
  EXPECT_FALSE(scheduler->workload().active);

  // A fresh ten-frame item makes it active again, starting at 0%.
  ObservationWorkItem big;
  big.node_id = NodeID(2);
  big.fingerprint = fp("b");
  big.frames = FrameIDRange{0, 9};
  scheduler->submit(big);

  const ObservationWorkload w = scheduler->workload();
  EXPECT_TRUE(w.active);
  EXPECT_EQ(w.frames_total, 10u);
  EXPECT_EQ(w.frames_observed, 0u);
  EXPECT_EQ(w.percent_complete, 0);
  EXPECT_EQ(w.outstanding_nodes, 1u);
}

TEST(ObservationScheduler, InvalidationPurgeResetsWorkloadForStaleWork) {
  auto old_map = std::make_shared<NodeFingerprintMap>();
  (*old_map)[NodeID(1)] = fp("v1");

  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner, old_map);

  std::vector<ObservationWorkload> snapshots;
  scheduler->set_workload_callback(
      [&](const ObservationWorkload& w) { snapshots.push_back(w); });

  ObservationWorkItem item;
  item.node_id = NodeID(1);
  item.fingerprint = fp("v1");
  item.frames = FrameIDRange{0, 9};
  scheduler->submit(item);
  EXPECT_TRUE(scheduler->workload().active);

  // Node 1's content changes: the queued item is now stale and purged.
  auto new_map = std::make_shared<NodeFingerprintMap>();
  (*new_map)[NodeID(1)] = fp("v2");
  scheduler->on_dag_changed(nullptr, new_map);

  const ObservationWorkload after = scheduler->workload();
  EXPECT_FALSE(after.active);
  EXPECT_EQ(after.frames_total, 0u);
  EXPECT_EQ(after.outstanding_nodes, 0u);
  ASSERT_FALSE(snapshots.empty());
  EXPECT_FALSE(snapshots.back().active);
}

TEST(ObservationScheduler, NoWorkloadCallbackFiresAfterShutdown) {
  MockTaskRunner* runner = nullptr;
  auto scheduler = make_scheduler(&runner);

  std::atomic<bool> shut_down{false};
  scheduler->set_workload_callback(
      [&](const ObservationWorkload&) { EXPECT_FALSE(shut_down.load()); });

  scheduler->start();
  for (FrameID f = 0; f < 20; ++f) {
    scheduler->submit(
        frame_item(NodeID(1), fp("a"), f, ObservationPriority::kSweep));
  }
  scheduler->stop();
  shut_down.store(true);

  // Submitting after shutdown is a no-op and triggers no workload callback.
  scheduler->submit(
      frame_item(NodeID(2), fp("b"), 0, ObservationPriority::kSweep));
}

}  // namespace
}  // namespace orc
