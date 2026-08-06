/*
 * File:        teletext_observer.h
 * Module:      orc-core
 * Purpose:     WST teletext observer (VBI data-line T42 packet recovery)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <observer.h>
#include <orc/support/teletext_slicer.h>

namespace orc {

/**
 * @brief WST (System B) teletext observer.
 *
 * Slices the candidate VBI lines of each field with TeletextSlicer and
 * records the recovered T42 packets as observations in the "teletext"
 * namespace, keyed by derived FieldID (frame_id * 2 + field_idx):
 *   - present    (BOOL):   at least one valid packet recovered in the field
 *   - line_count (INT32):  number of candidate lines that yielded packets
 *   - t42_<n>    (STRING): the packet recovered from 0-based field line <n> as
 *                          two hex chars per byte, followed by one more per
 *                          byte when the detector could measure its per-byte
 *                          confidence; the key is absent when the line carried
 *                          no data
 *
 * Both television systems ITU-R BT.653 defines System B on are covered, and
 * the string's length is what says which one produced it (84 or 126 characters
 * for the 42-byte 625-line packet, 68 or 102 for the 34-byte 525-line one —
 * teletext_hex_to_observed_packet decodes either):
 *
 *   - 625 lines (PAL): ETSI EN 300 706. 6,9375 Mbit/s, 42-byte packet, field
 *     lines 5-21.
 *   - 525 lines (NTSC, PAL_M): ITU-R BT.653 Table 1b. 5,727272 Mbit/s,
 *     34-byte packet, field lines 9-20.
 *
 * NABTS (BT.653 System C, the service the US networks carried) shares those
 * 525 lines and that bit rate but not the framing code, so its lines are seen
 * and rejected rather than decoded.
 *
 * Stateless: each field is sliced independently.
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
  //
  // Deliberately still 1.2.0 now that 525-line systems are observed too. What
  // this build recovers from a 625-line field is bit-for-bit what 1.2.0
  // recovered — same window, same bit rate, same packet length — so a bump
  // would throw away every stored PAL sweep to no purpose. The 525-line
  // sources need no bump either: they were outside applies_to() before, so
  // they have no stored teletext records, and a sweep-complete marker is only
  // honoured when the store holds a record for every observer the node's
  // parameters make applicable — which this observer now is.
  std::string observer_version() const override { return "1.2.0"; }

  // ITU-R BT.653 System B, on both the television systems it is defined for:
  // 625 lines (ETSI EN 300 706, PAL) and 525 lines (Table 1b, NTSC and PAL_M).
  bool applies_to(const SourceParameters& params) const override {
    return params.system == VideoSystem::PAL ||
           params.system == VideoSystem::NTSC ||
           params.system == VideoSystem::PAL_M;
  }

  void process_frame(const VideoFrameRepresentation& representation,
                     FrameID frame_id, IObservationContext& context) override;

  std::vector<ObservationKey> get_provided_observations() const override;

 private:
  // One slicer per television system: each carries its own sample rate, bit
  // rate, packet length and data-timing window, and all are cheap enough to
  // build once here rather than per frame.
  TeletextSlicer slicer_pal_;
  TeletextSlicer slicer_ntsc_;
  TeletextSlicer slicer_palm_;
};

}  // namespace orc
