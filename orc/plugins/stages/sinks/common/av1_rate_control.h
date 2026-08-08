/*
 * File:        av1_rate_control.h
 * Module:      orc-stage-plugin-video-sink
 * Purpose:     Resolves AV1 encoder rate-control precedence
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_PLUGIN_VIDEO_SINK_AV1_RATE_CONTROL_H
#define ORC_STAGE_PLUGIN_VIDEO_SINK_AV1_RATE_CONTROL_H

namespace orc {

// Selects AV1 rate control. Lossless overrides a requested bitrate, which in
// turn overrides CRF quality mode when the bitrate is non-zero.
enum class Av1RateControl {
  kLossless,
  kCrfQuality,
  kTargetBitrate,
};

// Resolves the shared Video Sink rate-control parameters for software AV1
// encoders. This function is pure and thread-safe.
constexpr Av1RateControl resolve_av1_rate_control(bool use_lossless_mode,
                                                  int encoder_bitrate) {
  if (use_lossless_mode) {
    return Av1RateControl::kLossless;
  }
  if (encoder_bitrate > 0) {
    return Av1RateControl::kTargetBitrate;
  }
  return Av1RateControl::kCrfQuality;
}

}  // namespace orc

#endif  // ORC_STAGE_PLUGIN_VIDEO_SINK_AV1_RATE_CONTROL_H
