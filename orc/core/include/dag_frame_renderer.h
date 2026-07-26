/*
 * File:        dag_frame_renderer.h
 * Module:      orc-core
 * Purpose:     Frame rendering at DAG nodes using VideoFrameRepresentation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/frame_id.h>
#include <orc/stage/node_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/observation/observation_service_interface.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/lru_cache.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "core_observation_service.h"
#include "dag_executor.h"
#include "observation_store.h"

namespace orc {

// Run the standard observer pass for one frame, with optional read-through
// caching against an ObservationStore keyed by the node's provenance.
//
// For each observer the two derived field records (frame_id*2 and frame_id*2+1)
// are considered:
//   - When both @p fingerprint and @p store are non-null and both records are
//     present, the stored values are loaded into @p context and the observer is
//     not run.
//   - Otherwise the observer runs; when caching is enabled its output for both
//     fields is written back to the store (an empty record is stored too, so a
//     later probe still hits).
//
// When either @p fingerprint or @p store is null the observers always run and
// nothing is stored — behaviour identical to the pre-store renderer.
//
// Thread safety: reentrant with respect to distinct @p context objects; @p
// store is the sole shared state and is internally synchronised.
void run_frame_observer_pass(const IObservationService& service,
                             const std::vector<ObserverInfo>& observers,
                             const VideoFrameRepresentation& representation,
                             FrameID frame_id,
                             const NodeFingerprint* fingerprint,
                             ObservationStore* store,
                             IObservationContext& context);

// Alias-aware variant: @p fingerprints lists every provenance the frame's
// content is known under — the observed node's own fingerprint first, followed
// by the fingerprints of upstream nodes the frame passes through byte-identical
// (per VideoFrameRepresentation::video_passthrough_source()). All entries key
// the same content, so:
//   - A store hit under ANY listed fingerprint satisfies the pass without
//     running the observer, and the records are copied to every other listed
//     fingerprint still missing them (so future lookups keyed to any node in
//     the pass-through chain hit directly — including across sessions via the
//     persistence sidecar).
//   - On a miss everywhere the observer runs once and its records are stored
//     under every listed fingerprint.
// An empty @p fingerprints (or null @p store) disables caching entirely.
void run_frame_observer_pass(const IObservationService& service,
                             const std::vector<ObserverInfo>& observers,
                             const VideoFrameRepresentation& representation,
                             FrameID frame_id,
                             const std::vector<NodeFingerprint>& fingerprints,
                             ObservationStore* store,
                             IObservationContext& context);

// Exception thrown during DAG frame rendering.
class DAGFrameRenderError : public std::runtime_error {
 public:
  explicit DAGFrameRenderError(const std::string& msg)
      : std::runtime_error(msg) {}
};

// Result of rendering a specific frame at a DAG node.
struct FrameRenderResult {
  // The VideoFrameRepresentation produced at the specified node.
  // Contains all frames from the source; callers address individual frames by
  // FrameID through the VFR API.
  VideoFrameRepresentationPtr representation;

  // True if the result is valid and can be used.
  bool is_valid = false;

  // Error message when is_valid is false (empty when valid).
  std::string error_message;

  // The node that was rendered.
  NodeID node_id;

  // The frame that was requested.
  FrameID frame_id = 0;

  // True if this result was returned from the LRU cache.
  bool from_cache = false;
};

// DAGFrameRenderer — on-demand frame access at arbitrary DAG nodes.
//
// Executes the DAG up to a specified node and returns the
// VideoFrameRepresentation produced there, keyed by FrameID in an LRU cache.
// Consumers address individual frames through the VFR API.
//
// Replaces DAGFieldRenderer for the CVBS_U10_4FSC pipeline.
//
// Thread safety: not thread-safe. Use from a single thread only.
class DAGFrameRenderer {
 public:
  // Construct a renderer for the given DAG.  Throws DAGFrameRenderError if dag
  // is null or invalid.
  explicit DAGFrameRenderer(std::shared_ptr<const DAG> dag);

  ~DAGFrameRenderer() = default;

  // Non-copyable, non-movable (contains stateful executor and cache).
  DAGFrameRenderer(const DAGFrameRenderer&) = delete;
  DAGFrameRenderer& operator=(const DAGFrameRenderer&) = delete;
  DAGFrameRenderer(DAGFrameRenderer&&) = delete;
  DAGFrameRenderer& operator=(DAGFrameRenderer&&) = delete;

  // -------------------------------------------------------------------------
  // Frame Rendering API
  // -------------------------------------------------------------------------

  // Render the representation at node_id and return it, verifying that
  // frame_id is present.  Results are LRU-cached; calling update_dag()
  // invalidates all cached entries.
  //
  // The representation is NOT executed per-frame: the whole source runs once
  // and the VFR is cached.  Individual frames are accessed by FrameID through
  // the representation's get_frame() / get_line() API.
  FrameRenderResult render_frame_at_node(NodeID node_id, FrameID frame_id);

  // True if node_id exists in the current DAG.
  bool has_node(NodeID node_id) const;

  // -------------------------------------------------------------------------
  // DAG Change Tracking
  // -------------------------------------------------------------------------

  // Replace the DAG and clear all cached render results.  Increments the
  // internal DAG version so stale FrameRenderResult objects are detectable.
  void update_dag(std::shared_ptr<const DAG> new_dag);

  // Returns the observation context populated during the most recent
  // render_frame_at_node() execution.
  const ObservationContext& get_observation_context() const;

  // Attach a shared, provenance-keyed ObservationStore and the fingerprint map
  // for the current DAG. When both are set, the observer pass reads
  // observations through the store (skipping observers whose records are
  // already present) and writes fresh results back. Pass nulls to disable
  // read-through caching. The store is owned externally so it can outlive
  // individual renderers and survive DAG rebuilds; the fingerprint map must
  // match the renderer's current DAG.
  void set_observation_store(
      std::shared_ptr<ObservationStore> store,
      std::shared_ptr<const NodeFingerprintMap> fingerprints);

  // Override the observation service used by the observer pass (for testing
  // with a spy/mock). Passing nullptr restores the default host
  // CoreObservationService. Re-enumerates the observer inventory from the
  // active service.
  void set_observation_service(std::shared_ptr<IObservationService> service);

  // -------------------------------------------------------------------------
  // Cache Management
  // -------------------------------------------------------------------------

  // Discard all cached render results without incrementing the DAG version.
  void clear_cache();

  // Enable or disable result caching.  When disabled every call re-executes
  // the DAG.
  void set_cache_enabled(bool enabled) { cache_enabled_ = enabled; }
  bool is_cache_enabled() const { return cache_enabled_; }

 private:
  std::shared_ptr<const DAG> dag_;
  uint64_t dag_version_;
  bool cache_enabled_;

  // Cache key: (node_id, frame_id value, dag_version)
  struct CacheKey {
    NodeID node_id;
    uint64_t frame_id_value;
    uint64_t dag_version;

    bool operator==(const CacheKey& o) const noexcept {
      return dag_version == o.dag_version && node_id == o.node_id &&
             frame_id_value == o.frame_id_value;
    }
  };

  struct CacheKeyHash {
    std::size_t operator()(const CacheKey& k) const noexcept {
      std::size_t h1 = std::hash<NodeID>{}(k.node_id);
      std::size_t h2 = std::hash<uint64_t>{}(k.frame_id_value);
      std::size_t h3 = std::hash<uint64_t>{}(k.dag_version);
      return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
  };

  static constexpr size_t kMaxCachedRenders = 500;
  LRUCache<CacheKey, FrameRenderResult, CacheKeyHash> render_cache_;

  std::unique_ptr<DAGExecutor> executor_;

  // Host-owned observation service backing the standard observer pass. The
  // registry it enumerates (see CoreObservationService) is the single source
  // of truth for observer identity, so adding an observer to the registry
  // extends the frame-render pass with no edit here. observers_ caches the
  // enumeration (id + version) so available_observers() (which constructs each
  // observer) runs once rather than per rendered frame.
  CoreObservationService default_observation_service_;
  std::shared_ptr<IObservationService> observation_service_override_;
  std::vector<ObserverInfo> observers_;

  // Optional provenance-keyed read-through cache. Both must be set for caching
  // to engage; either being null disables it (renderer behaves as before).
  std::shared_ptr<ObservationStore> observation_store_;
  std::shared_ptr<const NodeFingerprintMap> node_fingerprints_;

  // The observation service currently in effect (override if set, else
  // default).
  const IObservationService& observation_service() const {
    if (observation_service_override_) {
      return *observation_service_override_;
    }
    return default_observation_service_;
  }

  // (Re)populate observers_ from the active observation service.
  void refresh_observers();

  mutable std::map<NodeID, size_t> node_index_;
  mutable bool node_index_valid_;

  void ensure_node_index() const;

  FrameRenderResult execute_to_node(NodeID node_id, FrameID frame_id);
};

}  // namespace orc
