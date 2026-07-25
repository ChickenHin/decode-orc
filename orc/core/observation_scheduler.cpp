/*
 * File:        observation_scheduler.cpp
 * Module:      orc-core
 * Purpose:     Background worker that computes missing observations ahead of
 *              demand, priority-ordered and cancellable
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "observation_scheduler.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "dag_executor.h"
#include "dag_frame_renderer.h"
#include "observation_store.h"

namespace orc {

// ---------------------------------------------------------------------------
// ObservationScheduler
// ---------------------------------------------------------------------------

ObservationScheduler::ObservationScheduler(
    std::unique_ptr<IObservationTaskRunner> runner,
    std::shared_ptr<const NodeFingerprintMap> fingerprints,
    std::shared_ptr<IObservationSchedulingPolicy> policy)
    : runner_(std::move(runner)),
      policy_(std::move(policy)),
      fingerprints_(std::move(fingerprints)) {}

ObservationScheduler::~ObservationScheduler() { stop(); }

void ObservationScheduler::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (running_) {
    return;
  }
  stop_requested_ = false;
  cancel_in_flight_.store(false);
  running_ = true;
  worker_ = std::thread(&ObservationScheduler::worker_loop, this);
}

void ObservationScheduler::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
    cancel_in_flight_.store(true);
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  running_ = false;
  // Abandon any work never dequeued; a stopped scheduler holds no queue. Items
  // dropped here were never started, so no completion is owed for them.
  for (auto& queue : queues_) {
    queue.clear();
  }
  has_pending_dag_update_ = false;
  pending_dag_.reset();
  pending_fingerprints_.reset();
  // Clear the workload so a later start() begins from idle. No callback is
  // fired here: no workload snapshot is delivered after shutdown.
  workload_total_frames_ = 0;
  workload_observed_frames_ = 0;
}

bool ObservationScheduler::is_running() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return running_;
}

void ObservationScheduler::submit(ObservationWorkItem item) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_) {
      return;
    }
    enqueue_locked(std::move(item));
  }
  cv_.notify_one();
  emit_workload();
}

void ObservationScheduler::submit_batch(
    std::vector<ObservationWorkItem> items) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_) {
      return;
    }
    for (auto& item : items) {
      enqueue_locked(std::move(item));
    }
  }
  cv_.notify_all();
  emit_workload();
}

void ObservationScheduler::enqueue_locked(ObservationWorkItem item) {
  workload_total_frames_ += item.frames.count();
  const int idx = static_cast<int>(item.priority);
  queues_[idx].push_back(std::move(item));
}

void ObservationScheduler::on_project_loaded(
    const ObservationSchedulingContext& context) {
  if (!policy_) {
    return;
  }
  submit_batch(policy_->plan_sweep(context));
}

void ObservationScheduler::on_preview_moved(
    const ObservationSchedulingContext& context) {
  if (!policy_) {
    return;
  }
  submit_batch(policy_->plan_prefetch(context));
}

void ObservationScheduler::on_invalidation(
    const ObservationSchedulingContext& context) {
  if (!policy_) {
    return;
  }
  submit_batch(policy_->plan_invalidation(context));
}

NodeFingerprint ObservationScheduler::request_interactive(
    NodeID node_id, FrameID frame_id,
    const ObservationSchedulingContext& context) {
  if (!policy_) {
    return {};
  }
  ObservationWorkItem item =
      policy_->plan_interactive(node_id, frame_id, context);
  NodeFingerprint fingerprint = item.fingerprint;
  submit(std::move(item));
  return fingerprint;
}

void ObservationScheduler::on_dag_changed(
    std::shared_ptr<const DAG> dag,
    std::shared_ptr<const NodeFingerprintMap> fingerprints) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fingerprints_ = fingerprints;
    has_pending_dag_update_ = true;
    pending_dag_ = std::move(dag);
    pending_fingerprints_ = fingerprints;

    // Abort the in-flight item if its content identity is gone in the new map.
    if (has_in_flight_ && fingerprints_) {
      const auto it = fingerprints_->find(in_flight_node_);
      const bool stale = (it == fingerprints_->end()) ||
                         (it->second != in_flight_fingerprint_);
      if (stale) {
        cancel_in_flight_.store(true);
      }
    }

    purge_stale_locked();
  }
  cv_.notify_all();
  // A purge shrinks the outstanding workload; publish it, then drop to idle if
  // nothing remains (invalidation that clears the queue resets the aggregate).
  emit_workload();
  reset_workload_if_drained();
}

void ObservationScheduler::purge_stale_locked() {
  if (!fingerprints_) {
    return;
  }
  for (auto& queue : queues_) {
    std::deque<ObservationWorkItem> kept;
    for (auto& item : queue) {
      if (fingerprint_current(item, *fingerprints_)) {
        kept.push_back(std::move(item));
      } else {
        // Purged queued items never ran, so remove their whole frame count from
        // the outstanding total (clamped so it can never underflow).
        const std::uint64_t count = item.frames.count();
        workload_total_frames_ -= std::min(workload_total_frames_, count);
      }
    }
    queue = std::move(kept);
  }
}

bool ObservationScheduler::fingerprint_current(const ObservationWorkItem& item,
                                               const NodeFingerprintMap& map) {
  const auto it = map.find(item.node_id);
  return it != map.end() && it->second == item.fingerprint;
}

void ObservationScheduler::set_progress_callback(ProgressCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  progress_cb_ = std::move(callback);
}

void ObservationScheduler::set_completion_callback(
    CompletionCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  completion_cb_ = std::move(callback);
}

ObservationScheduler::ProgressCallback ObservationScheduler::progress_callback()
    const {
  std::lock_guard<std::mutex> lock(mutex_);
  return progress_cb_;
}

ObservationScheduler::CompletionCallback
ObservationScheduler::completion_callback() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return completion_cb_;
}

void ObservationScheduler::set_workload_callback(WorkloadCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  workload_cb_ = std::move(callback);
}

ObservationScheduler::WorkloadCallback ObservationScheduler::workload_callback()
    const {
  std::lock_guard<std::mutex> lock(mutex_);
  return workload_cb_;
}

ObservationWorkload ObservationScheduler::workload() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return build_workload_locked();
}

ObservationWorkload ObservationScheduler::build_workload_locked() const {
  ObservationWorkload w;
  w.frames_total = workload_total_frames_;
  w.frames_observed =
      std::min(workload_observed_frames_, workload_total_frames_);
  w.active = workload_total_frames_ > 0;
  if (workload_total_frames_ > 0) {
    // Rounded percentage, clamped to 100 (observed never exceeds total).
    const std::uint64_t pct =
        (w.frames_observed * 100 + workload_total_frames_ / 2) /
        workload_total_frames_;
    w.percent_complete = static_cast<int>(pct > 100 ? 100 : pct);
  }
  // Distinct nodes with pending work: queued items plus the in-flight item.
  std::unordered_set<NodeID> nodes;
  for (const auto& queue : queues_) {
    for (const auto& item : queue) {
      nodes.insert(item.node_id);
    }
  }
  if (has_in_flight_) {
    nodes.insert(in_flight_node_);
  }
  w.outstanding_nodes = nodes.size();
  return w;
}

void ObservationScheduler::emit_workload() {
  WorkloadCallback cb;
  ObservationWorkload snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_ || !workload_cb_) {
      return;
    }
    cb = workload_cb_;
    snapshot = build_workload_locked();
  }
  cb(snapshot);
}

void ObservationScheduler::reset_workload_if_drained() {
  WorkloadCallback cb;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_requested_) {
      return;
    }
    if (has_in_flight_ || has_pending_dag_update_) {
      return;
    }
    for (const auto& queue : queues_) {
      if (!queue.empty()) {
        return;
      }
    }
    // Nothing outstanding: return to idle.
    workload_total_frames_ = 0;
    workload_observed_frames_ = 0;
    cb = workload_cb_;
  }
  if (cb) {
    cb(ObservationWorkload{});
  }
}

std::size_t ObservationScheduler::queued_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t total = 0;
  for (const auto& queue : queues_) {
    total += queue.size();
  }
  return total;
}

ObservationScheduler::NextAction ObservationScheduler::take_next(
    bool blocking) {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    if (stop_requested_) {
      return {};
    }

    if (has_pending_dag_update_) {
      NextAction action;
      action.kind = NextAction::Kind::kDagUpdate;
      action.dag = pending_dag_;
      action.fingerprints = pending_fingerprints_;
      has_pending_dag_update_ = false;
      pending_dag_.reset();
      pending_fingerprints_.reset();
      return action;
    }

    for (auto& queue : queues_) {
      if (!queue.empty()) {
        NextAction action;
        action.kind = NextAction::Kind::kItem;
        action.item = std::move(queue.front());
        queue.pop_front();

        has_in_flight_ = true;
        in_flight_node_ = action.item.node_id;
        in_flight_fingerprint_ = action.item.fingerprint;
        cancel_in_flight_.store(false);
        return action;
      }
    }

    if (!blocking) {
      return {};
    }
    cv_.wait(lock);
  }
}

void ObservationScheduler::process_item(const ObservationWorkItem& item) {
  const std::uint64_t total = item.frames.count();
  std::uint64_t observed = 0;
  bool cancelled = false;
  bool failed = false;

  const ProgressCallback prog_cb = progress_callback();

  if (!item.frames.empty()) {
    for (FrameID frame = item.frames.first;; ++frame) {
      if (cancel_in_flight_.load()) {
        cancelled = true;
        break;
      }
      try {
        runner_->observe_frame(item.node_id, item.fingerprint, frame,
                               item.observer_ids);
      } catch (...) {
        // A stage/decode error fails the item but never the worker.
        failed = true;
        break;
      }
      ++observed;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++workload_observed_frames_;
      }
      emit_workload();
      if (prog_cb) {
        prog_cb(ObservationProgress{item.node_id, observed, total});
      }
      if (frame == item.frames.last) {
        break;  // guards against wrap-around when last == UINT64_MAX
      }
    }
  }

  const CompletionCallback comp_cb = completion_callback();
  if (comp_cb) {
    ObservationCompletion completion;
    completion.node_id = item.node_id;
    completion.fingerprint = item.fingerprint;
    completion.priority = item.priority;
    completion.frames_observed = observed;
    completion.frames_total = total;
    completion.succeeded = !failed && !cancelled && observed == total;
    completion.cancelled = cancelled;
    comp_cb(completion);
  }
}

void ObservationScheduler::worker_loop() {
  for (;;) {
    NextAction action = take_next(/*blocking=*/true);
    switch (action.kind) {
      case NextAction::Kind::kNone:
        return;  // stop requested
      case NextAction::Kind::kDagUpdate:
        runner_->update_dag(std::move(action.dag),
                            std::move(action.fingerprints));
        break;
      case NextAction::Kind::kItem:
        process_item(action.item);
        {
          std::lock_guard<std::mutex> lock(mutex_);
          has_in_flight_ = false;
        }
        reset_workload_if_drained();
        break;
    }
  }
}

bool ObservationScheduler::process_one_for_testing() {
  NextAction action = take_next(/*blocking=*/false);
  switch (action.kind) {
    case NextAction::Kind::kNone:
      return false;
    case NextAction::Kind::kDagUpdate:
      runner_->update_dag(std::move(action.dag),
                          std::move(action.fingerprints));
      return true;
    case NextAction::Kind::kItem:
      process_item(action.item);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        has_in_flight_ = false;
      }
      reset_workload_if_drained();
      return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// RendererObservationTaskRunner
// ---------------------------------------------------------------------------

RendererObservationTaskRunner::RendererObservationTaskRunner(
    std::shared_ptr<const DAG> dag,
    std::shared_ptr<const NodeFingerprintMap> fingerprints,
    std::shared_ptr<ObservationStore> store,
    std::shared_ptr<IObservationService> service)
    : store_(std::move(store)), service_(std::move(service)) {
  renderer_ = std::make_unique<DAGFrameRenderer>(std::move(dag));
  if (service_) {
    renderer_->set_observation_service(service_);
  }
  renderer_->set_observation_store(store_, std::move(fingerprints));
}

RendererObservationTaskRunner::~RendererObservationTaskRunner() = default;

void RendererObservationTaskRunner::observe_frame(
    NodeID node_id, const NodeFingerprint& /*fingerprint*/, FrameID frame_id,
    const std::vector<std::string>& /*observer_ids*/) {
  // The renderer's store-backed observer pass writes every standard observer's
  // records for this frame, keyed by the node fingerprint the renderer holds
  // (kept in sync via update_dag). observer_ids is advisory — see the header.
  const FrameRenderResult result =
      renderer_->render_frame_at_node(node_id, frame_id);
  if (!result.is_valid) {
    // Surface as a work-item failure so the scheduler reports it and moves on.
    throw DAGFrameRenderError(result.error_message);
  }
}

void RendererObservationTaskRunner::update_dag(
    std::shared_ptr<const DAG> dag,
    std::shared_ptr<const NodeFingerprintMap> fingerprints) {
  renderer_->update_dag(std::move(dag));
  renderer_->set_observation_store(store_, std::move(fingerprints));
}

// ---------------------------------------------------------------------------
// DefaultObservationSchedulingPolicy
// ---------------------------------------------------------------------------

DefaultObservationSchedulingPolicy::DefaultObservationSchedulingPolicy(
    std::vector<std::string> observer_ids,
    std::vector<std::string> stateful_ids, FrameID prefetch_radius)
    : stateful_ids_(std::move(stateful_ids)),
      prefetch_radius_(prefetch_radius) {
  const std::unordered_set<std::string> stateful(stateful_ids_.begin(),
                                                 stateful_ids_.end());
  for (auto& id : observer_ids) {
    if (stateful.find(id) == stateful.end()) {
      stateless_ids_.push_back(std::move(id));
    }
  }
}

NodeFingerprint DefaultObservationSchedulingPolicy::fingerprint_of(
    NodeID node_id, const ObservationSchedulingContext& context) {
  if (!context.fingerprints) {
    return {};
  }
  const auto it = context.fingerprints->find(node_id);
  return it != context.fingerprints->end() ? it->second : NodeFingerprint{};
}

void DefaultObservationSchedulingPolicy::emit_node_items(
    NodeID node_id, const NodeFingerprint& fingerprint, FrameIDRange frames,
    ObservationPriority priority, std::vector<ObservationWorkItem>& out) const {
  if (frames.empty()) {
    return;
  }
  if (!stateless_ids_.empty()) {
    ObservationWorkItem item;
    item.node_id = node_id;
    item.fingerprint = fingerprint;
    item.frames = frames;
    item.observer_ids = stateless_ids_;
    item.stateful = false;
    item.priority = priority;
    out.push_back(std::move(item));
  }
  if (!stateful_ids_.empty()) {
    ObservationWorkItem item;
    item.node_id = node_id;
    item.fingerprint = fingerprint;
    item.frames = frames;
    item.observer_ids = stateful_ids_;
    item.stateful = true;
    item.priority = priority;
    out.push_back(std::move(item));
  }
}

std::vector<ObservationWorkItem> DefaultObservationSchedulingPolicy::plan_sweep(
    const ObservationSchedulingContext& context) {
  std::vector<ObservationWorkItem> out;
  if (context.total_frames == 0) {
    return out;
  }
  const FrameIDRange whole{0, context.total_frames - 1};
  for (const NodeID node : context.nodes_of_interest) {
    emit_node_items(node, fingerprint_of(node, context), whole,
                    ObservationPriority::kSweep, out);
  }
  return out;
}

std::vector<ObservationWorkItem>
DefaultObservationSchedulingPolicy::plan_prefetch(
    const ObservationSchedulingContext& context) {
  std::vector<ObservationWorkItem> out;
  const FrameID centre = context.preview_position;
  const FrameID first =
      centre > prefetch_radius_ ? centre - prefetch_radius_ : 0;
  FrameID last = centre + prefetch_radius_;
  if (context.total_frames > 0) {
    last = std::min<FrameID>(last, context.total_frames - 1);
  }
  const FrameIDRange window{first, last};
  for (const NodeID node : context.nodes_of_interest) {
    emit_node_items(node, fingerprint_of(node, context), window,
                    ObservationPriority::kPrefetch, out);
  }
  return out;
}

std::vector<ObservationWorkItem>
DefaultObservationSchedulingPolicy::plan_invalidation(
    const ObservationSchedulingContext& context) {
  std::vector<ObservationWorkItem> out;
  if (context.total_frames == 0) {
    return out;
  }
  const FrameIDRange whole{0, context.total_frames - 1};
  for (const NodeID node : context.changed_nodes) {
    emit_node_items(node, fingerprint_of(node, context), whole,
                    ObservationPriority::kSweep, out);
  }
  return out;
}

ObservationWorkItem DefaultObservationSchedulingPolicy::plan_interactive(
    NodeID node_id, FrameID frame_id,
    const ObservationSchedulingContext& context) {
  ObservationWorkItem item;
  item.node_id = node_id;
  item.fingerprint = fingerprint_of(node_id, context);
  item.frames = FrameIDRange{frame_id, frame_id};
  item.observer_ids = stateless_ids_;
  item.observer_ids.insert(item.observer_ids.end(), stateful_ids_.begin(),
                           stateful_ids_.end());
  item.stateful = false;  // single frame — ordering is irrelevant
  item.priority = ObservationPriority::kInteractive;
  return item;
}

}  // namespace orc
