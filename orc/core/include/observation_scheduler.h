/*
 * File:        observation_scheduler.h
 * Module:      orc-core
 * Purpose:     Background worker that computes missing observations ahead of
 *              demand, priority-ordered and cancellable
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// Host-internal background scheduler. Only orc-core and orc-presenters may
// include this header; GUI/CLI code must go through presenters.
#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/observation_scheduler.h. Use a presenter instead."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/observation_scheduler.h. Use a presenter instead."
#endif

#include <orc/stage/frame_id.h>
#include <orc/stage/node_id.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "frame_provenance.h"

namespace orc {

class DAG;                  // dag_executor.h
class DAGFrameRenderer;     // dag_frame_renderer.h
class ObservationStore;     // observation_store.h
class IObservationService;  // observation_service_interface.h

/**
 * @brief Priority classes ordering background observation work.
 *
 * Lower numeric value = higher priority. Interactive requests (a frame a user
 * is looking at right now) preempt prefetch (frames near the preview position)
 * which preempts the background sweep of whole nodes. Within one class, work is
 * processed in submission order (FIFO).
 */
enum class ObservationPriority : int {
  kInteractive = 0,
  kPrefetch = 1,
  kSweep = 2,
};

/**
 * @brief One unit of background observation work.
 *
 * Describes a contiguous frame range to observe at a single DAG node with a
 * given observer set. The @ref fingerprint is captured from the node's
 * provenance at enqueue time so the scheduler can drop the item if a later DAG
 * change makes it stale (the node's content identity changed).
 */
struct ObservationWorkItem {
  NodeID node_id;                         ///< Node to render/observe at.
  NodeFingerprint fingerprint;            ///< Node provenance at enqueue time.
  FrameIDRange frames;                    ///< Inclusive frame range to cover.
  std::vector<std::string> observer_ids;  ///< Observer set (advisory; see
                                          ///< IObservationTaskRunner).
  /// True when the observer set includes a cross-frame (stateful) observer, so
  /// the scheduler must observe this item's frames strictly in ascending order.
  bool stateful = false;
  ObservationPriority priority = ObservationPriority::kSweep;
};

/**
 * @brief Per-node progress emitted while a work item is processed.
 *
 * @c frames_observed is cumulative within a single work item and increases
 * monotonically; @c frames_total is that item's frame count.
 */
struct ObservationProgress {
  NodeID node_id;
  std::uint64_t frames_observed = 0;
  std::uint64_t frames_total = 0;
};

/**
 * @brief Terminal report emitted exactly once per dequeued work item.
 *
 * Fires whether the item completed, failed, or was cancelled mid-flight. Items
 * that are dropped from the queue before ever being dequeued (stale-fingerprint
 * purge, or shutdown) never emit a completion.
 */
struct ObservationCompletion {
  NodeID node_id;
  NodeFingerprint fingerprint;
  ObservationPriority priority = ObservationPriority::kSweep;
  std::uint64_t frames_observed = 0;
  std::uint64_t frames_total = 0;
  bool succeeded = false;  ///< True iff every frame was observed without error.
  bool cancelled = false;  ///< True iff a DAG change / shutdown aborted it.
};

/**
 * @brief Performs the render + observe work for one frame at one node.
 *
 * This is the seam that lets the scheduler run against a mocked backend in unit
 * tests. The production implementation (RendererObservationTaskRunner) owns a
 * DAGFrameRenderer and writes observed values into a shared ObservationStore.
 *
 * Threading: every method is invoked only on the scheduler's single worker
 * thread, honouring DAGFrameRenderer's single-thread contract; implementations
 * need no internal synchronisation. observe_frame() may throw — the scheduler
 * catches at the work-item boundary and marks the item failed, so a stage or
 * decode error never terminates the process.
 */
class IObservationTaskRunner {
 public:
  virtual ~IObservationTaskRunner() = default;

  /**
   * @brief Observe @p frame_id at @p node_id, keyed by @p fingerprint.
   *
   * @param node_id      Node to render and observe.
   * @param fingerprint  Provenance used to key stored records.
   * @param frame_id     Frame to observe.
   * @param observer_ids Requested observer set. Advisory: an implementation
   *                     may observe the full standard set and rely on the
   *                     store's read-through to skip already-present records.
   */
  virtual void observe_frame(NodeID node_id, const NodeFingerprint& fingerprint,
                             FrameID frame_id,
                             const std::vector<std::string>& observer_ids) = 0;

  /**
   * @brief Adopt a new DAG and fingerprint map (called on the worker thread).
   *
   * Invoked before any work item for the new DAG is processed, so the runner's
   * renderer is always consistent with the fingerprints keying the store.
   */
  virtual void update_dag(
      std::shared_ptr<const DAG> dag,
      std::shared_ptr<const NodeFingerprintMap> fingerprints) = 0;
};

/**
 * @brief Inputs a scheduling policy uses to decide what to enqueue.
 *
 * A snapshot assembled by the caller (presenter) at the moment of an event.
 */
struct ObservationSchedulingContext {
  /// Current fingerprint map; a policy looks up each node's fingerprint here.
  std::shared_ptr<const NodeFingerprintMap> fingerprints;
  /// Preview position (frame) the prefetch window is centred on.
  FrameID preview_position = 0;
  /// Total frame count of the source (for clamping ranges); 0 if unknown.
  std::uint64_t total_frames = 0;
  /// Nodes worth observing: the previewed node plus any with open dialogs.
  std::vector<NodeID> nodes_of_interest;
  /// Nodes whose fingerprint just changed (from an ObservationInvalidation).
  std::vector<NodeID> changed_nodes;
};

/**
 * @brief Decides which work items to enqueue for each scheduling event.
 *
 * Injected so the enqueue strategy is testable in isolation and swappable. The
 * scheduler owns no policy of its own; the caller drives the on_* event methods
 * with a context snapshot.
 *
 * Threading: called on the caller's thread (not the worker). Implementations
 * must be pure with respect to the passed context (no hidden shared state).
 */
class IObservationSchedulingPolicy {
 public:
  virtual ~IObservationSchedulingPolicy() = default;

  /// Full background sweep, e.g. on project load. Lowest priority.
  virtual std::vector<ObservationWorkItem> plan_sweep(
      const ObservationSchedulingContext& context) = 0;

  /// Prefetch window around the current preview position. Medium priority.
  virtual std::vector<ObservationWorkItem> plan_prefetch(
      const ObservationSchedulingContext& context) = 0;

  /// Re-enqueue only the changed nodes' frames after an invalidation.
  virtual std::vector<ObservationWorkItem> plan_invalidation(
      const ObservationSchedulingContext& context) = 0;

  /// The single interactively-requested frame. Highest priority.
  virtual ObservationWorkItem plan_interactive(
      NodeID node_id, FrameID frame_id,
      const ObservationSchedulingContext& context) = 0;
};

/**
 * @brief Background observation scheduler.
 *
 * Owns one worker thread that drains a priority-ordered queue of work items,
 * driving the injected IObservationTaskRunner one frame at a time. Interactive
 * work preempts prefetch preempts sweep; within a class, FIFO. Stateful items
 * are observed in ascending frame order.
 *
 * DAG changes: on_dag_changed() atomically adopts the new fingerprint map,
 * drops every queued item whose node fingerprint no longer matches, and aborts
 * the in-flight item if it became stale. Requeuing new work against the new map
 * is the policy's job (via on_invalidation()).
 *
 * Progress/completion callbacks fire on the worker thread; a presenter marshals
 * them to the UI thread. No callback fires after stop() has joined the worker.
 *
 * Thread safety (coding standards §5.3.3): submit*, on_dag_changed, the on_*
 * policy events, callback setters, start and stop are all safe to call from any
 * thread. Internal state is guarded by a mutex; the runner and callbacks run
 * only on the worker thread.
 */
class ObservationScheduler {
 public:
  using ProgressCallback = std::function<void(const ObservationProgress&)>;
  using CompletionCallback = std::function<void(const ObservationCompletion&)>;

  /**
   * @param runner        Backend that renders + observes frames (required).
   * @param fingerprints  Initial fingerprint map used for staleness checks.
   * @param policy        Optional enqueue policy driving the on_* events.
   */
  ObservationScheduler(
      std::unique_ptr<IObservationTaskRunner> runner,
      std::shared_ptr<const NodeFingerprintMap> fingerprints,
      std::shared_ptr<IObservationSchedulingPolicy> policy = nullptr);

  ~ObservationScheduler();

  ObservationScheduler(const ObservationScheduler&) = delete;
  ObservationScheduler& operator=(const ObservationScheduler&) = delete;
  ObservationScheduler(ObservationScheduler&&) = delete;
  ObservationScheduler& operator=(ObservationScheduler&&) = delete;

  /// Launch the worker thread. Idempotent; a no-op once running.
  void start();

  /// Signal shutdown, abort in-flight work, and join the worker. Idempotent;
  /// safe to call from the destructor. No callback fires after it returns.
  void stop();

  /// True while the worker thread is running.
  bool is_running() const;

  // ---- Direct submission ---------------------------------------------------

  /// Enqueue one work item. Ignored (dropped) if the scheduler is stopping.
  void submit(ObservationWorkItem item);

  /// Enqueue a batch of work items in order.
  void submit_batch(std::vector<ObservationWorkItem> items);

  // ---- Policy-driven events ------------------------------------------------

  /// Project loaded: enqueue the policy's sweep plan.
  void on_project_loaded(const ObservationSchedulingContext& context);

  /// Preview moved: enqueue the policy's prefetch plan.
  void on_preview_moved(const ObservationSchedulingContext& context);

  /// Invalidation: enqueue the policy's re-observation plan for changed nodes.
  void on_invalidation(const ObservationSchedulingContext& context);

  /// Interactive request: enqueue the policy's single-frame plan at top
  /// priority. Returns the fingerprint the item was keyed with (empty if no
  /// policy is set, in which case nothing is enqueued).
  NodeFingerprint request_interactive(
      NodeID node_id, FrameID frame_id,
      const ObservationSchedulingContext& context);

  // ---- DAG change ----------------------------------------------------------

  /**
   * @brief Adopt a new DAG / fingerprint map and drop stale work.
   *
   * Queued items whose node fingerprint no longer matches @p fingerprints are
   * removed; the in-flight item is aborted if stale. The runner adopts the new
   * DAG on the worker thread before the next item runs.
   */
  void on_dag_changed(std::shared_ptr<const DAG> dag,
                      std::shared_ptr<const NodeFingerprintMap> fingerprints);

  // ---- Callbacks -----------------------------------------------------------

  void set_progress_callback(ProgressCallback callback);
  void set_completion_callback(CompletionCallback callback);

  // ---- Introspection / testing --------------------------------------------

  /// Total number of items currently queued across all priority classes.
  std::size_t queued_count() const;

  /**
   * @brief Process at most one pending action synchronously, for tests.
   *
   * Must only be used when the worker thread is NOT running (i.e. start() was
   * not called). Applies a pending DAG update or processes the next work item
   * exactly as the worker loop would, then returns. Returns true if it did
   * work, false if there was nothing pending.
   */
  bool process_one_for_testing();

 private:
  static constexpr int kPriorityClasses = 3;

  // A control action taken by the worker (or the test driver): either a DAG
  // update to forward to the runner, or a work item to process.
  struct NextAction {
    enum class Kind { kNone, kDagUpdate, kItem } kind = Kind::kNone;
    ObservationWorkItem item;
    std::shared_ptr<const DAG> dag;
    std::shared_ptr<const NodeFingerprintMap> fingerprints;
  };

  // True if @p item's fingerprint still matches the node in @p map.
  static bool fingerprint_current(const ObservationWorkItem& item,
                                  const NodeFingerprintMap& map);

  void worker_loop();

  // Pop the next action under the lock. When @p blocking, waits until there is
  // work, a DAG update, or a stop request; returns kNone only on stop. When not
  // blocking, returns kNone immediately if nothing is pending.
  NextAction take_next(bool blocking);

  // Execute one dequeued work item: observe its frames in order, honouring
  // cancellation, and emit progress + a single completion. Never throws.
  void process_item(const ObservationWorkItem& item);

  void enqueue_locked(ObservationWorkItem item);
  void purge_stale_locked();

  ProgressCallback progress_callback() const;
  CompletionCallback completion_callback() const;

  std::unique_ptr<IObservationTaskRunner> runner_;
  std::shared_ptr<IObservationSchedulingPolicy> policy_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;

  // FIFO queue per priority class (index == ObservationPriority value).
  std::deque<ObservationWorkItem> queues_[kPriorityClasses];

  std::shared_ptr<const NodeFingerprintMap> fingerprints_;

  // A pending DAG update to forward to the runner on the worker thread.
  bool has_pending_dag_update_ = false;
  std::shared_ptr<const DAG> pending_dag_;
  std::shared_ptr<const NodeFingerprintMap> pending_fingerprints_;

  // In-flight item bookkeeping for staleness-driven abort.
  bool has_in_flight_ = false;
  NodeID in_flight_node_;
  NodeFingerprint in_flight_fingerprint_;
  std::atomic<bool> cancel_in_flight_{false};

  bool stop_requested_ = false;
  bool running_ = false;
  std::thread worker_;

  ProgressCallback progress_cb_;
  CompletionCallback completion_cb_;
};

/**
 * @brief Production IObservationTaskRunner backed by a DAGFrameRenderer.
 *
 * Renders each requested frame at its node; the renderer's built-in
 * store-backed observer pass writes every standard observer's output for that
 * frame into the shared ObservationStore (read-through skips already-present
 * records). The observer_ids on a work item are therefore advisory — coverage
 * is the full standard set.
 *
 * Threading: not thread-safe; used only on the scheduler's worker thread.
 */
class RendererObservationTaskRunner final : public IObservationTaskRunner {
 public:
  /**
   * @param dag           Initial DAG to render against.
   * @param fingerprints  Fingerprint map matching @p dag (keys the store).
   * @param store         Shared, provenance-keyed observation store.
   * @param service       Optional observation service override (nullptr uses
   *                      the host CoreObservationService).
   */
  RendererObservationTaskRunner(
      std::shared_ptr<const DAG> dag,
      std::shared_ptr<const NodeFingerprintMap> fingerprints,
      std::shared_ptr<ObservationStore> store,
      std::shared_ptr<IObservationService> service = nullptr);

  ~RendererObservationTaskRunner() override;

  void observe_frame(NodeID node_id, const NodeFingerprint& fingerprint,
                     FrameID frame_id,
                     const std::vector<std::string>& observer_ids) override;

  void update_dag(
      std::shared_ptr<const DAG> dag,
      std::shared_ptr<const NodeFingerprintMap> fingerprints) override;

 private:
  std::shared_ptr<ObservationStore> store_;
  std::shared_ptr<IObservationService> service_;
  std::unique_ptr<DAGFrameRenderer> renderer_;
};

/**
 * @brief Default scheduling policy: whole-node sweeps, a prefetch window, and
 *        changed-node re-observation.
 *
 * Splits the observer inventory into stateless and stateful sets so stateless
 * frames may be chunked while stateful observers get one ascending-order item.
 *
 * Thread safety: immutable after construction; safe to share across threads.
 */
class DefaultObservationSchedulingPolicy final
    : public IObservationSchedulingPolicy {
 public:
  /// Default half-width (frames) of the prefetch window around the preview.
  static constexpr FrameID kDefaultPrefetchRadius = 24;

  /**
   * @param observer_ids   Stable ids of every standard observer.
   * @param stateful_ids   Subset of @p observer_ids that are stateful.
   * @param prefetch_radius Half-width of the prefetch window in frames.
   */
  DefaultObservationSchedulingPolicy(
      std::vector<std::string> observer_ids,
      std::vector<std::string> stateful_ids,
      FrameID prefetch_radius = kDefaultPrefetchRadius);

  std::vector<ObservationWorkItem> plan_sweep(
      const ObservationSchedulingContext& context) override;
  std::vector<ObservationWorkItem> plan_prefetch(
      const ObservationSchedulingContext& context) override;
  std::vector<ObservationWorkItem> plan_invalidation(
      const ObservationSchedulingContext& context) override;
  ObservationWorkItem plan_interactive(
      NodeID node_id, FrameID frame_id,
      const ObservationSchedulingContext& context) override;

 private:
  // Look up a node's fingerprint in the context (empty if absent).
  static NodeFingerprint fingerprint_of(
      NodeID node_id, const ObservationSchedulingContext& context);

  // Build the stateless + stateful work items covering @p frames at @p node.
  void emit_node_items(NodeID node_id, const NodeFingerprint& fingerprint,
                       FrameIDRange frames, ObservationPriority priority,
                       std::vector<ObservationWorkItem>& out) const;

  std::vector<std::string> stateless_ids_;
  std::vector<std::string> stateful_ids_;
  FrameID prefetch_radius_;
};

}  // namespace orc
