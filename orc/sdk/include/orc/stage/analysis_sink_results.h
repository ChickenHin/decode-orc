/*
 * File:        analysis_sink_results.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Interfaces for accessing analysis sink stage results across
 *              shared library boundaries
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_CORE_ANALYSIS_SINK_RESULTS_H
#define ORC_CORE_ANALYSIS_SINK_RESULTS_H

#include <orc/stage/common_types.h>

#include <cstdint>
#include <vector>

namespace orc {

// The host reaches an analysis sink's results by dynamic_cast on the stage
// pointer it holds. Cast to one of these interfaces, never to a concrete
// plugin class: plugins are loaded RTLD_LOCAL, so a class defined in both the
// plugin and the host ends up with two distinct type_info objects and the cast
// fails (observed on macOS first, but the arrangement is what is fragile, not
// the platform).
//
// Keep these pure abstract interfaces — no data members, no non-virtual
// members, declared here and nowhere else — so that both sides agree on the
// vtable layout and the cast has only the type name to reconcile.

class IDropoutAnalysisResults {
 public:
  virtual bool has_results() const = 0;
  virtual const std::vector<FrameDropoutStats>& frame_stats() const = 0;
  virtual int32_t total_frames() const = 0;
  virtual ~IDropoutAnalysisResults() = default;
};

class ISNRAnalysisResults {
 public:
  virtual bool has_results() const = 0;
  virtual const std::vector<FrameSNRStats>& frame_stats() const = 0;
  virtual int32_t total_frames() const = 0;
  virtual ~ISNRAnalysisResults() = default;
};

class IBurstLevelAnalysisResults {
 public:
  virtual bool has_results() const = 0;
  virtual const std::vector<FrameBurstLevelStats>& frame_stats() const = 0;
  virtual int32_t total_frames() const = 0;
  virtual ~IBurstLevelAnalysisResults() = default;
};

}  // namespace orc

#endif  // ORC_CORE_ANALYSIS_SINK_RESULTS_H
