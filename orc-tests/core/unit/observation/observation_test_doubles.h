/*
 * File:        observation_test_doubles.h
 * Module:      orc-core tests
 * Purpose:     Shared test doubles for observation store / read-through tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/frame_id.h>
#include <orc/stage/observation/observation_context_interface.h>
#include <orc/stage/observation/observation_service_interface.h>
#include <orc/stage/video_frame_representation.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace orc {
namespace test {

// Minimal VideoFrameRepresentation stub. The spy observation service never
// reads samples from it, so every accessor returns a trivial value.
class FakeVideoFrameRepresentation final : public VideoFrameRepresentation {
 public:
  FrameIDRange frame_range() const override { return {0, 0}; }
  size_t frame_count() const override { return 1; }
  bool has_frame(FrameID id) const override { return id == 0; }
  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID /*id*/) const override {
    return std::nullopt;
  }
  const sample_type* get_frame(FrameID /*id*/) const override {
    return nullptr;
  }
  std::vector<sample_type> get_frame_copy(FrameID /*id*/) const override {
    return {};
  }
};

// Spy IObservationService that records how many times each observer id was run
// and writes deterministic, type-diverse observations into the context. An
// observer id of "silent" writes nothing (to exercise empty-record caching).
class SpyObservationService final : public IObservationService {
 public:
  std::vector<ObserverInfo> observers;
  mutable int total_runs = 0;
  mutable std::map<std::string, int> runs_by_id;

  std::vector<ObserverInfo> available_observers() const override {
    return observers;
  }

  std::unique_ptr<IObserverHandle> create_observer(
      const std::string& /*observer_id*/) const override {
    return nullptr;
  }

  bool run_observer(const std::string& observer_id,
                    const VideoFrameRepresentation& /*representation*/,
                    FrameID frame_id,
                    IObservationContext& context) const override {
    ++total_runs;
    ++runs_by_id[observer_id];

    if (observer_id == "silent") {
      return true;  // Intentionally writes nothing.
    }

    // Populate both fields of the frame with one value of every variant type,
    // deterministically derived from the id/frame/field.
    for (FieldID::value_type field_idx = 0; field_idx < 2; ++field_idx) {
      const FieldID field(frame_id * 2 + field_idx);
      context.set(field, observer_id, "i32",
                  static_cast<int32_t>(frame_id * 10 + field_idx));
      context.set(field, observer_id, "i64",
                  static_cast<int64_t>(frame_id) * 1000 +
                      static_cast<int64_t>(field_idx));
      context.set(field, observer_id, "dbl",
                  1.5 + static_cast<double>(field_idx));
      context.set(field, observer_id, "str",
                  observer_id + ":" + std::to_string(frame_id) + ":" +
                      std::to_string(field_idx));
      context.set(field, observer_id, "flag", field_idx == 0);
    }
    return true;
  }
};

// Convenience: build an ObserverInfo with id + version (provided_observations
// is unused by the read-through pass).
inline ObserverInfo make_observer_info(std::string id, std::string version) {
  ObserverInfo info;
  info.id = std::move(id);
  info.version = std::move(version);
  return info;
}

}  // namespace test
}  // namespace orc
