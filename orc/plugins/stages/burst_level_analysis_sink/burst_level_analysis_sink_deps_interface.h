/*
 * File:        burst_level_analysis_sink_deps_interface.h
 * Module:      orc-core
 * Purpose:     Interface for BurstLevelAnalysisSinkStage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_BURST_LEVEL_ANALYSIS_SINK_DEPS_INTERFACE_H
#define ORC_CORE_BURST_LEVEL_ANALYSIS_SINK_DEPS_INTERFACE_H

#include <orc/stage/observation/observation_context_interface.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "burst_level_analysis_types.h"

namespace orc {
struct BurstAnalysisComputeOptions {
  std::string output_path;
  bool write_csv{false};
};

struct BurstAnalysisComputeResult {
  bool success{false};
  std::string message;
  std::vector<FrameBurstLevelStats> frame_stats;
  int32_t total_frames{0};
};

class IBurstLevelAnalysisSinkStageDeps {
 public:
  virtual ~IBurstLevelAnalysisSinkStageDeps() = default;

  virtual void init(TriggerProgressCallback progress_callback,
                    std::atomic<bool>* cancel_requested) = 0;

  virtual BurstAnalysisComputeResult compute_and_analyze(
      VideoFrameRepresentation* representation,
      IObservationContext& observation_context,
      BurstAnalysisComputeOptions options) = 0;

  // Thin path-opening wrapper: opens `path` for truncation and writes the
  // canonical per-frame CSV. Returns false if there is no data or the file
  // cannot be opened. The concrete deps expose a filesystem-free
  // write_csv(std::ostream&, …) formatter for unit testing.
  virtual bool write_csv(
      const std::string& path,
      const std::vector<FrameBurstLevelStats>& frame_stats) = 0;
};
}  // namespace orc

#endif  // ORC_CORE_BURST_LEVEL_ANALYSIS_SINK_DEPS_INTERFACE_H
