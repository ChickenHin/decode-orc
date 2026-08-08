/*
 * File:        nabts_sink_deps.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Concrete NabtsSinkStage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_SINK_DEPS_H
#define ORC_NABTS_SINK_DEPS_H

#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "nabts_scan_state.h"
#include "nabts_sink_deps_interface.h"

namespace orc {

class IStageServices;
class TeletextRecoveryStats;

/**
 * @brief The NABTS sink's real work: one pass over the frame range
 *
 * Recovers the System C data lines of every frame of the input, writes the
 * recovered packets as a flat 33-byte packet stream, and reports how the
 * recovery went.
 *
 * Thread safety: analyse() slices on several threads internally but is itself
 * single-threaded from the caller's point of view — one call at a time per
 * instance.
 */
class NabtsSinkDeps : public INabtsSinkStageDeps {
 public:
  explicit NabtsSinkDeps(IStageServices* stage_services)
      : stage_services_(stage_services) {}

  void init(TriggerProgressCallback progress_callback,
            std::atomic<bool>* cancel_requested) override;

  NabtsSinkResult analyse(const VideoFrameRepresentation* representation,
                          const NabtsSinkOptions& options) override;

 private:
  // The run's diagnostic report: what was exported and how the recovery went.
  std::string build_report(const NabtsSinkOptions& options,
                           const NabtsSinkResult& result, uint64_t total_frames,
                           const TeletextRecoveryStats& stats,
                           const NabtsScanState& scan_state) const;

  // Write |result.report| beside the packet stream. Never fails the export.
  void write_report(const NabtsSinkOptions& options,
                    NabtsSinkResult& result) const;

  IStageServices* stage_services_{nullptr};
  TriggerProgressCallback progress_callback_;
  std::atomic<bool>* cancel_requested_{nullptr};
};

}  // namespace orc

#endif  // ORC_NABTS_SINK_DEPS_H
