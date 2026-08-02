/*
 * File:        teletext_observer.h
 * Module:      orc-core
 * Purpose:     PAL WST teletext observer (VBI data-line T42 packet recovery)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <observer.h>
#include <orc/support/teletext_slicer.h>

namespace orc {

/**
 * @brief PAL WST (System B) teletext observer.
 *
 * Slices the candidate VBI lines of each field with TeletextSlicer and
 * records the recovered 42-byte T42 packets as observations in the
 * "teletext" namespace, keyed by derived FieldID (frame_id * 2 + field_idx):
 *   - present    (BOOL):   at least one valid packet recovered in the field
 *   - line_count (INT32):  number of candidate lines that yielded packets
 *   - t42_<n>    (STRING): 84 hex chars for 0-based field line <n>; the key
 *                          is absent when the line carried no data
 *
 * PAL only (ETSI EN 300 706 System B); non-PAL frames produce no
 * observations. Stateless: each field is sliced independently.
 */
class TeletextObserver : public Observer {
 public:
  TeletextObserver();
  ~TeletextObserver() override = default;

  std::string observer_name() const override { return "TeletextObserver"; }
  std::string observer_version() const override { return "1.0.0"; }

  void process_frame(const VideoFrameRepresentation& representation,
                     FrameID frame_id, IObservationContext& context) override;

  std::vector<ObservationKey> get_provided_observations() const override;

 private:
  TeletextSlicer slicer_;
};

}  // namespace orc
