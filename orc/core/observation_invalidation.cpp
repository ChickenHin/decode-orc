/*
 * File:        observation_invalidation.cpp
 * Module:      orc-core
 * Purpose:     Fingerprint-map diffing and retention window for observation
 *              change propagation and store garbage collection
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "observation_invalidation.h"

#include <algorithm>

namespace orc {

ObservationInvalidation diff_node_fingerprints(
    const NodeFingerprintMap& old_map, const NodeFingerprintMap& new_map) {
  ObservationInvalidation result;

  // Changed nodes: present in the new map with a fingerprint that differs from
  // (or is absent in) the old map. Downstream descendants are captured
  // automatically because their fingerprints compose over the changed input.
  for (const auto& [id, fingerprint] : new_map) {
    auto old_it = old_map.find(id);
    if (old_it == old_map.end() || old_it->second != fingerprint) {
      result.changed_nodes.push_back(id);
    }
  }
  std::sort(result.changed_nodes.begin(), result.changed_nodes.end());

  // Removed fingerprints: content identities reachable before but not now. A
  // fingerprint value still carried by any node in the new map is kept.
  std::unordered_set<NodeFingerprint> new_fingerprints;
  new_fingerprints.reserve(new_map.size());
  for (const auto& [id, fingerprint] : new_map) {
    new_fingerprints.insert(fingerprint);
  }

  std::unordered_set<NodeFingerprint> already_recorded;
  for (const auto& [id, fingerprint] : old_map) {
    if (new_fingerprints.count(fingerprint) == 0 &&
        already_recorded.insert(fingerprint).second) {
      result.removed_fingerprints.push_back(fingerprint);
    }
  }
  std::sort(result.removed_fingerprints.begin(),
            result.removed_fingerprints.end());

  return result;
}

ObservationRetentionWindow::ObservationRetentionWindow(std::size_t capacity)
    : capacity_(capacity) {}

void ObservationRetentionWindow::record_unreachable(
    const std::vector<NodeFingerprint>& removed) {
  for (const auto& fingerprint : removed) {
    // Drop any existing copy so the fingerprint re-arms at the newest position
    // instead of occupying two slots.
    auto existing = std::find(window_.begin(), window_.end(), fingerprint);
    if (existing != window_.end()) {
      window_.erase(existing);
    }
    window_.push_back(fingerprint);
  }
  while (window_.size() > capacity_) {
    window_.pop_front();
  }
}

std::unordered_set<NodeFingerprint> ObservationRetentionWindow::retain_set(
    const NodeFingerprintMap& current) const {
  std::unordered_set<NodeFingerprint> keep;
  keep.reserve(current.size() + window_.size());
  for (const auto& [id, fingerprint] : current) {
    keep.insert(fingerprint);
  }
  for (const auto& fingerprint : window_) {
    keep.insert(fingerprint);
  }
  return keep;
}

}  // namespace orc
