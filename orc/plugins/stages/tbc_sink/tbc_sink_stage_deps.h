/*
 * File:        tbc_sink_stage_deps.h
 * Module:      orc-core
 * Purpose:     TBC Sink Stage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_TBC_SINK_STAGE_DEPS_H
#define ORC_CORE_TBC_SINK_STAGE_DEPS_H

#include <orc/stage/observation/observation_context_interface.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tbc_metadata_writer_interface.h"
#include "tbc_sink_stage_deps_interface.h"

namespace orc {
class IStageServices;

class TBCSinkStageDeps : public ITBCSinkStageDeps {
 public:
  TBCSinkStageDeps(IStageServices* stage_services,
                   std::shared_ptr<ITBCMetadataWriter> metadata_writer)
      : stage_services_(stage_services),
        metadata_writer_(std::move(metadata_writer)) {}

  void init(TriggerProgressCallback progress_callback,
            std::atomic<bool>* pIsProcessing,
            std::atomic<bool>* pCancelRequested);

  bool write_tbc_and_metadata(
      const VideoFrameRepresentation* representation,
      const std::string& tbc_path, size_t audio_channel_pair,
      IObservationContext& observation_context) override;

 private:
  // Resample the gathered 48 kHz pipeline audio to the sidecar's 44100 Hz,
  // narrow it to signed 16-bit, and write |pcm_path|.  |field_pairs| is the
  // per-field layout already recorded in the metadata; the payload is trimmed
  // or silence-padded to match it exactly.
  bool write_pcm_sidecar(const std::vector<int32_t>& audio_stream,
                         const std::vector<int32_t>& field_pairs,
                         const std::string& pcm_path);

  TriggerProgressCallback progress_callback_;
  std::atomic<bool>* pIsProcessing_{};
  std::atomic<bool>* pCancelRequested_{};
  IStageServices* stage_services_{};
  std::shared_ptr<ITBCMetadataWriter> metadata_writer_;
};
}  // namespace orc

#endif  // ORC_CORE_TBC_SINK_STAGE_DEPS_H
