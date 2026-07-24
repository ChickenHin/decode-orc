/*
 * File:        snr_analysis_sink_deps_interface.h
 * Module:      orc-core
 * Purpose:     Interface for SNRAnalysisSinkStage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_SNR_ANALYSIS_SINK_DEPS_INTERFACE_H
#define ORC_CORE_SNR_ANALYSIS_SINK_DEPS_INTERFACE_H

#include <orc/stage/observation/observation_context_interface.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "snr_analysis_types.h"

namespace orc {
struct SNRAnalysisComputeOptions {
  std::string output_path;
  bool write_csv{false};
  SNRAnalysisMode snr_mode{SNRAnalysisMode::BOTH};
  // Analyse every Nth frame (1 = every frame). Analysed frames are
  // first, first + N, first + 2N, … and each record carries its true frame
  // number. Values < 1 are treated as 1.
  int32_t frame_interval{1};
};

struct SNRAnalysisComputeResult {
  bool success{false};
  std::string message;
  std::vector<FrameSNRStats> frame_stats;
  int32_t total_frames{0};
};

// Dependency seam for SNRAnalysisSinkStage. The concrete implementation
// (SNRAnalysisSinkStageDeps) obtains the standard observers through the host
// IObservationService injected at construction — the stage passes
// orc::plugin::get_observation_service(); unit tests either inject a mock of
// this interface or construct the concrete deps with a mock service. A null
// service (older host / direct construction) degrades gracefully: observation
// is skipped rather than crashing.
class ISNRAnalysisSinkStageDeps {
 public:
  virtual ~ISNRAnalysisSinkStageDeps() = default;

  virtual void init(TriggerProgressCallback progress_callback,
                    std::atomic<bool>* cancel_requested) = 0;

  virtual SNRAnalysisComputeResult compute_and_analyze(
      VideoFrameRepresentation* representation,
      IObservationContext& observation_context,
      SNRAnalysisComputeOptions options) = 0;

  // Thin path-opening wrapper: opens `path` for truncation and writes the
  // canonical per-frame CSV. Returns false if there is no data or the file
  // cannot be opened. The concrete deps expose a filesystem-free
  // write_csv(std::ostream&, …) formatter for unit testing.
  virtual bool write_csv(const std::string& path,
                         const std::vector<FrameSNRStats>& frame_stats) = 0;
};
}  // namespace orc

#endif  // ORC_CORE_SNR_ANALYSIS_SINK_DEPS_INTERFACE_H
