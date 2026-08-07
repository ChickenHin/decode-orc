/*
 * File:        teletext_analysis_sink_deps.h
 * Module:      orc-stage-plugin-teletext_analysis_sink
 * Purpose:     TeletextAnalysisSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_ANALYSIS_SINK_DEPS_H
#define ORC_TELETEXT_ANALYSIS_SINK_DEPS_H

#include <orc/stage/triggerable_stage.h>
#include <orc/support/teletext_recovery_stats.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "teletext_analysis_sink_deps_interface.h"
#include "teletext_squash_stats.h"

namespace orc {

class IStageServices;
class IFileWriterUint8;

/**
 * @brief Runs the analysis pass: frame loop, packet writer, page catalogue.
 *
 * One linear pass over the frame range slices every candidate VBI line with
 * TeletextFrameSlicer, writes the recovered packets as the service's flat
 * stream (.t42 on 625 lines, .t34 on 525), and feeds the same packets to a
 * page decoder whose snapshots build the catalogue the stage tool displays.
 *
 * Nothing is read from or written to the observation store: the stage owns its
 * decoding end to end, which is what lets its tuning parameters mean anything.
 */
class TeletextAnalysisSinkDeps : public ITeletextAnalysisSinkStageDeps {
 public:
  // |stage_services| may be null (direct in-process construction in tests);
  // the run then fails for want of a writer factory.
  explicit TeletextAnalysisSinkDeps(IStageServices* stage_services)
      : stage_services_(stage_services) {}

  void init(TriggerProgressCallback progress_callback,
            std::atomic<bool>* cancel_requested) override;

  TeletextAnalysisSinkResult analyse(
      const VideoFrameRepresentation* representation,
      const TeletextAnalysisSinkOptions& options) override;

 private:
  // Assemble the run's diagnostic report: what was exported, how recovery
  // went, and what combining repeated rows changed. Always built — it costs a
  // string, and a run that recovered little is exactly when it is worth
  // reading.
  static std::string build_report(const TeletextAnalysisSinkOptions& options,
                                  const TeletextAnalysisSinkResult& result,
                                  uint64_t total_frames,
                                  const TeletextRecoveryStats& stats,
                                  const TeletextSquashStats& squash_stats);

  // Write |result.report| to <output>.txt when the option asks for it,
  // stamping result.report_path on success. A failure here is logged and
  // otherwise ignored: the packet stream is the product, the report is a note
  // about it.
  void write_report(const TeletextAnalysisSinkOptions& options,
                    TeletextAnalysisSinkResult& result) const;

  IStageServices* stage_services_{nullptr};
  TriggerProgressCallback progress_callback_;
  std::atomic<bool>* cancel_requested_{nullptr};
};

}  // namespace orc

#endif  // ORC_TELETEXT_ANALYSIS_SINK_DEPS_H
