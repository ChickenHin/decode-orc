/*
 * File:        teletext_sink_stage_deps.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     TeletextSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_SINK_STAGE_DEPS_H
#define ORC_TELETEXT_SINK_STAGE_DEPS_H

#include <orc/stage/observation/observation_service_interface.h>
#include <orc/stage/triggerable_stage.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "teletext_sink_stage_deps_interface.h"

namespace orc {

class IStageServices;
class IFileWriterUint8;

/**
 * @brief Runs the T42 export: observer session, frame loop, packet writer.
 *
 * With the default slicer options (exact framing, MRAG required) packets come
 * from the host "teletext" observer, so results already recorded in the
 * host's provenance store are reused via the per-frame coverage skip. With
 * non-default options the host observer's fixed configuration no longer
 * matches, so the deps slices the candidate lines itself with a locally
 * configured TeletextSlicer (the support tier exists precisely so the sink
 * can compile it in — design §4.1) and bypasses the coverage skip.
 */
class TeletextSinkStageDeps : public ITeletextSinkStageDeps {
 public:
  // Both services may be null (older host, or direct in-process construction
  // in tests): a null stage_services fails the export (no writer factory); a
  // null observation_service falls back to whatever teletext observations are
  // already present in the context.
  TeletextSinkStageDeps(IStageServices* stage_services,
                        IObservationService* observation_service)
      : stage_services_(stage_services),
        observation_service_(observation_service) {}

  void init(TriggerProgressCallback progress_callback,
            std::atomic<bool>* cancel_requested) override;

  TeletextSinkResult export_t42(const VideoFrameRepresentation* representation,
                                IObservationContext& observation_context,
                                const TeletextSinkOptions& options) override;

 private:
  IStageServices* stage_services_{nullptr};
  IObservationService* observation_service_{nullptr};
  TriggerProgressCallback progress_callback_;
  std::atomic<bool>* cancel_requested_{nullptr};
};

}  // namespace orc

#endif  // ORC_TELETEXT_SINK_STAGE_DEPS_H
