/*
 * File:        closed_caption_observer.h
 * Module:      orc-core
 * Purpose:     Closed caption observer (EIA-608 line 21/22)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <observer.h>

namespace orc {

/**
 * @brief Observer for EIA-608 closed captions (525-line line 21).
 *
 * Observations (namespace "closed_caption"):
 * - present (bool, optional): true when a caption pair was decoded
 * - data0 / data1 (int32, optional): the two EIA-608 bytes
 * - parity0_valid / parity1_valid (bool, optional): per-byte parity validity
 */
class ClosedCaptionObserver : public Observer {
 public:
  ClosedCaptionObserver() = default;
  ~ClosedCaptionObserver() override = default;

  std::string observer_name() const override { return "ClosedCaptionObserver"; }
  std::string observer_version() const override { return "1.1.0"; }

  // EIA-608 captions are carried on line 21 of 525-line systems only
  // [CTA-608-E §4.1]. 625-line PAL has no equivalent line-21 service, so the
  // decode can only ever waste work and record present=false there.
  bool applies_to(const SourceParameters& params) const override {
    return params.system == VideoSystem::NTSC ||
           params.system == VideoSystem::PAL_M;
  }

  void process_frame(const VideoFrameRepresentation& representation,
                     FrameID frame_id, IObservationContext& context) override;

  std::vector<ObservationKey> get_provided_observations() const override;

 private:
  struct DecodedCaption {
    uint8_t data0 = 0;
    uint8_t data1 = 0;
    bool parity_valid0 = false;
    bool parity_valid1 = false;
  };

  bool decode_line(const int16_t* line_data, size_t sample_count,
                   int16_t zero_crossing, size_t colorburst_end,
                   double samples_per_bit, DecodedCaption& decoded) const;
};

}  // namespace orc
