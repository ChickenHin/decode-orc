/*
 * File:        dropout_analysis_sink_deps_interface.h
 * Module:      orc-core
 * Purpose:     Interface for DropoutAnalysisSinkStage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_DROPOUT_ANALYSIS_SINK_DEPS_INTERFACE_H
#define ORC_CORE_DROPOUT_ANALYSIS_SINK_DEPS_INTERFACE_H

#include <orc/stage/observation/observation_context_interface.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dropout_analysis_types.h"

namespace orc {
struct DropoutAnalysisComputeOptions {
  std::string output_path;
  bool write_csv{false};
  DropoutAnalysisMode mode{DropoutAnalysisMode::FULL_FIELD};
};

struct DropoutAnalysisComputeResult {
  bool success{false};
  std::string message;
  std::vector<FrameDropoutStats> frame_stats;
  int32_t total_frames{0};
};

class IDropoutAnalysisSinkStageDeps {
 public:
  virtual ~IDropoutAnalysisSinkStageDeps() = default;

  virtual void init(TriggerProgressCallback progress_callback,
                    std::atomic<bool>* cancel_requested) = 0;

  virtual DropoutAnalysisComputeResult compute_and_analyze(
      VideoFrameRepresentation* representation,
      IObservationContext& observation_context,
      DropoutAnalysisComputeOptions options) = 0;

  // Thin path-opening wrapper: opens `path` for truncation and writes the
  // canonical per-frame CSV. Returns false if there is no data or the file
  // cannot be opened. The concrete deps expose a filesystem-free
  // write_csv(std::ostream&, …) formatter for unit testing.
  virtual bool write_csv(const std::string& path,
                         const std::vector<FrameDropoutStats>& frame_stats) = 0;
};
}  // namespace orc

#endif  // ORC_CORE_DROPOUT_ANALYSIS_SINK_DEPS_INTERFACE_H
