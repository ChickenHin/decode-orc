/*
 * File:        store_backed_observation_context.cpp
 * Module:      orc-core
 * Purpose:     Observation context that loads stored records per field on
 *              first access instead of pre-loading a whole recording
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "store_backed_observation_context.h"

#include <cstddef>
#include <utility>

namespace orc {

namespace {

// Derived field id convention used throughout the host: field = frame * 2 +
// field_idx (top field 0, bottom field 1).
constexpr FieldID::value_type kFieldsPerFrame = 2;

}  // namespace

StoreBackedObservationContext::StoreBackedObservationContext(
    IObservationContext& inner, ObservationStore& store,
    NodeFingerprint fingerprint, std::vector<ObserverInfo> observers,
    FrameIDRange frame_range)
    : inner_(&inner),
      store_(&store),
      fingerprint_(std::move(fingerprint)),
      observers_(std::move(observers)),
      frame_range_(frame_range) {
  if (!frame_range_.empty()) {
    loaded_.resize(frame_range_.count() * kFieldsPerFrame, false);
  }
}

size_t StoreBackedObservationContext::slot_of(FieldID field_id) const {
  const FieldID::value_type first_field = frame_range_.first * kFieldsPerFrame;
  const FieldID::value_type value = field_id.value();
  if (frame_range_.empty() || value < first_field ||
      value - first_field >= loaded_.size()) {
    return SIZE_MAX;
  }
  return static_cast<size_t>(value - first_field);
}

void StoreBackedObservationContext::ensure_loaded(FieldID field_id) const {
  if (cleared_ || fingerprint_.value.empty()) {
    return;
  }
  const size_t slot = slot_of(field_id);
  if (slot == SIZE_MAX || loaded_[slot]) {
    return;
  }
  // Marked before loading: a field whose records are absent has still been
  // asked about, and asking the store again cannot change the answer.
  loaded_[slot] = true;

  for (const auto& observer : observers_) {
    if (!observer.stateless && !stateful_fully_covered(observer)) {
      continue;
    }
    store_->load_into({fingerprint_, field_id, observer.id, observer.version},
                      *inner_);
  }
}

bool StoreBackedObservationContext::stateful_fully_covered(
    const ObserverInfo& observer) const {
  const auto it = stateful_covered_.find(observer.id);
  if (it != stateful_covered_.end()) {
    return it->second;
  }
  bool covered = true;
  for (FrameID frame = frame_range_.first;; ++frame) {
    const FieldID top(frame * kFieldsPerFrame);
    const FieldID bottom(frame * kFieldsPerFrame + 1);
    if (!store_->has_stored(
            {fingerprint_, top, observer.id, observer.version}) ||
        !store_->has_stored(
            {fingerprint_, bottom, observer.id, observer.version})) {
      covered = false;
      break;
    }
    if (frame == frame_range_.last) {
      break;
    }
  }
  stateful_covered_.emplace(observer.id, covered);
  return covered;
}

void StoreBackedObservationContext::set(FieldID field_id,
                                        const std::string& namespace_,
                                        const std::string& key,
                                        const ObservationValue& value) {
  // Load before writing so a stored record and a freshly-computed value merge
  // exactly as they did when everything was pre-loaded up front.
  ensure_loaded(field_id);
  inner_->set(field_id, namespace_, key, value);
}

std::optional<ObservationValue> StoreBackedObservationContext::get(
    FieldID field_id, const std::string& namespace_,
    const std::string& key) const {
  ensure_loaded(field_id);
  return inner_->get(field_id, namespace_, key);
}

bool StoreBackedObservationContext::has(FieldID field_id,
                                        const std::string& namespace_,
                                        const std::string& key) const {
  ensure_loaded(field_id);
  return inner_->has(field_id, namespace_, key);
}

std::vector<std::string> StoreBackedObservationContext::get_keys(
    FieldID field_id, const std::string& namespace_) const {
  ensure_loaded(field_id);
  return inner_->get_keys(field_id, namespace_);
}

std::vector<std::string> StoreBackedObservationContext::get_namespaces(
    FieldID field_id) const {
  ensure_loaded(field_id);
  return inner_->get_namespaces(field_id);
}

std::map<std::string, std::map<std::string, ObservationValue>>
StoreBackedObservationContext::get_all_observations(FieldID field_id) const {
  ensure_loaded(field_id);
  return inner_->get_all_observations(field_id);
}

void StoreBackedObservationContext::clear() {
  inner_->clear();
  cleared_ = true;
}

void StoreBackedObservationContext::clear_field(FieldID field_id) {
  // Mark the field loaded so a later read does not resurrect the cleared
  // records from the store.
  const size_t slot = slot_of(field_id);
  if (slot != SIZE_MAX) {
    loaded_[slot] = true;
  }
  inner_->clear_field(field_id);
}

void StoreBackedObservationContext::register_schema(
    const std::vector<ObservationKey>& keys) {
  inner_->register_schema(keys);
}

void StoreBackedObservationContext::clear_schema() { inner_->clear_schema(); }

}  // namespace orc
