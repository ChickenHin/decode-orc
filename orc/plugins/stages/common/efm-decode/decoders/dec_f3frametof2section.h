/*
 * File:        dec_f3frametof2section.h
 * Purpose:     efm-decoder-f2 - EFM T-values to F2 Section decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef DEC_F3FRAMETOF2SECTION_H
#define DEC_F3FRAMETOF2SECTION_H

#include <cstdint>
#include <queue>
#include <vector>

#include "decoders.h"
#include "section.h"
#include "subcode.h"

class F3FrameToF2Section : public Decoder {
 public:
  F3FrameToF2Section();
  void pushFrame(const F3Frame& data);
  void pushFrame(F3Frame&& data);
  F2Section popSection();
  bool isReady() const;

  // F3 frames consumed without output before the first F2 section was
  // emitted (section-sync acquisition cost). Together with the other
  // head-loss counters this lets the caller compute how much input time
  // elapsed before decoded output began (audio/video sync alignment, issue
  // #231). Constant once the first section has been output.
  uint64_t headLostF3Frames() const { return m_headLostF3Frames; }

  void showStatistics() const;

 private:
  void processStateMachine();
  void outputSection(bool showAddress);

  // Adds |frames| to the head-loss statistic while no F2 section has been
  // output yet; a no-op afterwards (later losses are mid-stream damage, not
  // an origin shift).
  void noteHeadLoss(uint64_t frames);

  std::queue<F2Section> m_outputBuffer;

  std::vector<F3Frame> m_internalBuffer;
  std::vector<F3Frame> m_sectionFrames;

  int32_t m_badSyncCounter;
  SectionMetadata m_lastSectionMetadata;

  // State machine states
  enum State {
    ExpectingInitialSync,
    ExpectingSync,
    HandleValid,
    HandleOvershoot,
    HandleUndershoot,
    LostSync
  };

  State m_currentState;

  // State machine state processing functions
  State expectingInitialSync();
  State expectingSync();
  State handleValid();
  State handleUndershoot();
  State handleOvershoot();
  State lostSync();

  // Statistics
  uint64_t m_inputF3Frames;
  uint64_t m_presyncDiscardedF3Frames;
  uint64_t m_goodSync0;
  uint64_t m_missingSync0;
  uint64_t m_undershootSync0;
  uint64_t m_overshootSync0;
  uint64_t m_discardedF3Frames;
  uint64_t m_paddedF3Frames;
  uint64_t m_lostSyncCounter;
  uint64_t m_headLostF3Frames;
  bool m_firstSectionOutput;
};

#endif  // DEC_F3FRAMETOF2SECTION_H