/*
 * File:        core_observation_service.h
 * Module:      orc-core
 * Purpose:     Host implementation of the plugin-facing IObservationService
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/frame_id.h>
#include <orc/stage/observation/observation_context_interface.h>
#include <orc/stage/observation/observation_service_interface.h>
#include <orc/stage/orc_source_parameters.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace orc {

class VideoFrameRepresentation;

/**
 * @brief Host-owned observation service backed by the standard observer set.
 *
 * The registry mapping observer id -> concrete observer factory is the single
 * source of truth for observer identity (see core_observation_service.cpp). The
 * class is stateless: it owns no per-run data, so a single instance can back
 * every plugin. Unknown ids fail cleanly — create_observer() returns nullptr
 * and run_observer() returns false; nothing throws across the plugin boundary.
 *
 * Thread-safety: all methods are const and reentrant; the object may be shared
 * across threads. Per-handle threading rules are documented on IObserverHandle.
 */
class CoreObservationService final : public IObservationService {
 public:
  std::vector<ObserverInfo> available_observers() const override;

  std::unique_ptr<IObserverHandle> create_observer(
      const std::string& observer_id) const override;

  bool run_observer(const std::string& observer_id,
                    const VideoFrameRepresentation& representation,
                    FrameID frame_id,
                    IObservationContext& context) const override;
};

// True when the standard observer identified by @p observer_id can ever
// produce observations for a source with video parameters @p params (e.g.
// "teletext" is PAL-only, "fm_code"/"white_flag" are NTSC-only). Core-private
// applicability query — deliberately NOT part of the SDK service surface.
// Unknown ids (e.g. observers injected by a test service) are always
// applicable. Thread-safe.
bool standard_observer_applies(const std::string& observer_id,
                               const SourceParameters& params);

// The subset of @p observers applicable to @p params. A disengaged @p params
// (video parameters unavailable) applies every observer: filtering must fail
// open, because the observer pass, the runner's store fast path and the
// presenter's coverage probes all have to agree on the same set — a site that
// filtered while another did not would make frames look permanently
// uncovered. Thread-safe.
std::vector<ObserverInfo> filter_applicable_observers(
    const std::vector<ObserverInfo>& observers,
    const std::optional<SourceParameters>& params);

}  // namespace orc
