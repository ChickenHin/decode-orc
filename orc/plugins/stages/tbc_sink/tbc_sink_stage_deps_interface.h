/*
 * File:        tbc_sink_stage_deps_interface.h
 * Module:      orc-core
 * Purpose:     Interface for 'TBC Sink Stage dependencies'
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_TBC_SINK_STAGE_DEPS_INTERFACE_H
#define ORC_CORE_TBC_SINK_STAGE_DEPS_INTERFACE_H

#include <orc/stage/observation/observation_context_interface.h>
#include <orc/stage/video_frame_representation.h>

#include <cstddef>
#include <string>

namespace orc {
class ITBCSinkStageDeps {
 public:
  virtual ~ITBCSinkStageDeps() = default;

  // Writes the .tbc, its .tbc.db metadata, and the .pcm / .efm sidecars the
  // input carries.  |audio_channel_pair| selects which pipeline channel pair
  // becomes the analogue audio sidecar; it is ignored when the input has no
  // audio, and out-of-range values fall back to the lowest pair.
  virtual bool write_tbc_and_metadata(
      const VideoFrameRepresentation* representation,
      const std::string& tbc_path, size_t audio_channel_pair,
      IObservationContext& observation_context) = 0;
};
}  // namespace orc

#endif  // ORC_CORE_TBC_SINK_STAGE_DEPS_INTERFACE_H
