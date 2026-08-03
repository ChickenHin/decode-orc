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
 *   - t42_<n>    (STRING): 84 hex chars for 0-based field line <n>, followed
 *                          by 42 more when the detector could measure its
 *                          per-byte confidence; the key is absent when the
 *                          line carried no data
 *
 * PAL only (ETSI EN 300 706 System B); non-PAL frames produce no
 * observations. Stateless: each field is sliced independently.
 */
class TeletextObserver : public Observer {
 public:
  TeletextObserver();
  ~TeletextObserver() override = default;

  std::string observer_name() const override { return "TeletextObserver"; }
  // 1.1.0: the slicer runs TeletextDetector::kAuto, so a band-limited source
  // (consumer VHS) now recovers packets where 1.0.0 recovered none. The bump
  // is what makes the host discard those stored "nothing here" results and
  // the sweep-complete markers that would otherwise keep them in service.
  //
  // 1.2.0: the MLSE detector reads a band-limited line far better than 1.1.0
  // did (a fractionally-spaced branch metric, a terminated trellis and a
  // decision-directed channel refit), it repairs display bytes that fail the
  // odd parity of ETSI EN 300 706 §8.1 by flipping the bit it was least sure
  // of, and it records that per-byte confidence alongside the packet. A 1.1.0
  // observation is still a valid packet, which is why it is worth saying why
  // the bump discards it anyway: it holds worse bytes than this build recovers
  // from the same line, and it carries no confidence, so every row it
  // contributes votes at full weight against copies that measured themselves.
  // Mixing the two would let the older, unweighted copies win.
  std::string observer_version() const override { return "1.2.0"; }

  void process_frame(const VideoFrameRepresentation& representation,
                     FrameID frame_id, IObservationContext& context) override;

  std::vector<ObservationKey> get_provided_observations() const override;

 private:
  TeletextSlicer slicer_;
};

}  // namespace orc
