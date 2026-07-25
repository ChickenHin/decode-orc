/*
 * File:        observation_invalidation.h
 * Module:      orc-core
 * Purpose:     Fingerprint-map diffing and retention window for observation
 *              change propagation and store garbage collection
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// Host-internal change-propagation machinery. Only orc-core and orc-presenters
// may include this header; GUI/CLI code must go through presenters.
#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/observation_invalidation.h. Use a presenter instead."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/observation_invalidation.h. Use a presenter instead."
#endif

#include <orc/stage/node_id.h>

#include <cstddef>
#include <deque>
#include <unordered_set>
#include <vector>

#include "frame_provenance.h"

namespace orc {

/**
 * @brief The change signal produced by diffing two node-fingerprint maps.
 *
 * Because fingerprints compose recursively over a node's inputs, a change on
 * one node necessarily alters that node's fingerprint and every transitive
 * descendant's. Diffing therefore captures downstream propagation for free —
 * no separate graph walk is required.
 */
struct ObservationInvalidation {
  /// Nodes whose fingerprint changed or that are newly present in the new map,
  /// sorted by NodeID for deterministic delivery. These are the nodes whose
  /// stored observations are now stale and must be recomputed.
  std::vector<NodeID> changed_nodes;

  /// Content identities (fingerprints) reachable in the old map but no longer
  /// present as any node's fingerprint in the new map, sorted for determinism.
  /// These are the store keys that become garbage-collection candidates.
  std::vector<NodeFingerprint> removed_fingerprints;

  /// True when nothing changed (a no-op rebuild).
  bool empty() const {
    return changed_nodes.empty() && removed_fingerprints.empty();
  }
};

/**
 * @brief Diff two node-fingerprint maps into an ObservationInvalidation.
 *
 * A node is "changed" when it is present in @p new_map with a fingerprint that
 * differs from @p old_map, or when it is present in @p new_map but absent from
 * @p old_map (a newly-added node). Nodes removed entirely contribute nothing to
 * @c changed_nodes (there is nothing left to compute for them) but their now
 * unreachable fingerprints appear in @c removed_fingerprints.
 *
 * A fingerprint counts as removed only when no node in @p new_map still carries
 * that value: a content identity reachable via any node must be retained.
 *
 * Deterministic: identical maps yield an empty diff; outputs are sorted.
 * Complexity: O(V) with V = node count (hash lookups).
 */
ObservationInvalidation diff_node_fingerprints(
    const NodeFingerprintMap& old_map, const NodeFingerprintMap& new_map);

/**
 * @brief A bounded FIFO window of recently-unreachable node fingerprints.
 *
 * When a project edit makes a content identity unreachable, its stored
 * observations should not be evicted immediately: an undo restores that
 * identity and the cached observations should still be available (zero
 * recompute). The window remembers the most recent @c capacity() unreachable
 * fingerprints so they are retained across the garbage-collection call until
 * they age out.
 *
 * Thread safety: not synchronised. Intended to be owned by a single presenter
 * and mutated only from the thread that rebuilds the DAG.
 */
class ObservationRetentionWindow {
 public:
  /// Default number of unreachable fingerprints kept for the undo window.
  static constexpr std::size_t kDefaultCapacity = 128;

  explicit ObservationRetentionWindow(std::size_t capacity = kDefaultCapacity);

  /// Record fingerprints that just became unreachable. A fingerprint already in
  /// the window is moved to the most-recent position (re-armed) rather than
  /// duplicated; the oldest entries are dropped once @c capacity() is exceeded.
  void record_unreachable(const std::vector<NodeFingerprint>& removed);

  /// Build the retain set for ObservationStore::retain_only(): every currently
  /// reachable fingerprint in @p current plus every fingerprint still held in
  /// the window.
  std::unordered_set<NodeFingerprint> retain_set(
      const NodeFingerprintMap& current) const;

  /// Number of fingerprints currently held in the window.
  std::size_t size() const { return window_.size(); }

  /// Maximum number of fingerprints the window retains.
  std::size_t capacity() const { return capacity_; }

 private:
  std::size_t capacity_;
  std::deque<NodeFingerprint> window_;  ///< front = oldest, back = newest.
};

}  // namespace orc
