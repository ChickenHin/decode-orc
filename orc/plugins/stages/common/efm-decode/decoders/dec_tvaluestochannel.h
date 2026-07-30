/*
 * File:        dec_tvaluestochannel.h
 * Purpose:     efm-decoder-f2 - EFM T-values to F2 Section decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef DEC_TVALUESTOCHANNEL_H
#define DEC_TVALUESTOCHANNEL_H

#include <cstdint>
#include <queue>
#include <vector>

#include "decoders.h"

class TvaluesToChannel : public Decoder {
 public:
  TvaluesToChannel();
  void pushFrame(const std::vector<uint8_t>& data);
  std::vector<uint8_t> popFrame();
  bool isReady() const;

  // Channel bits discarded before the first channel frame was emitted (the
  // initial sync-acquisition cost). Together with the downstream head-loss
  // counters this lets the caller compute how much input time elapsed before
  // decoded output began (audio/video sync alignment, issue #231). Constant
  // once the first frame has been output.
  uint64_t headDiscardedBits() const { return m_headDiscardedBits; }

  void showStatistics() const;

 private:
  void processStateMachine();
  void attemptToFixOvershootFrame(std::vector<uint8_t>& frameData);
  void attemptToFixUndershootFrame(uint32_t startIndex, uint32_t endIndex,
                                   std::vector<uint8_t>& frameData);
  uint32_t countBits(const std::vector<uint8_t>& data,
                     int32_t startPosition = 0, int32_t endPosition = -1);

  // State machine states
  enum State {
    ExpectingInitialSync,
    ExpectingSync,
    HandleOvershoot,
    HandleUndershoot
  };

  // Notes bits discarded while no channel frame has been emitted yet (feeds
  // m_headDiscardedBits; a no-op once decoding has started).
  void noteHeadDiscard(int32_t startPosition, int32_t endPosition);

  // Statistics (P-10: 64-bit so cumulative T-value counters do not wrap on a
  // long capture - a 32-bit T-value count wraps after ~95 minutes of audio).
  uint64_t m_consumedTValues;
  uint64_t m_discardedTValues;
  uint64_t m_channelFrameCount;
  uint64_t m_headDiscardedBits;

  uint64_t m_perfectFrames;
  uint64_t m_longFrames;
  uint64_t m_shortFrames;

  uint64_t m_overshootSyncs;
  uint64_t m_undershootSyncs;
  uint64_t m_perfectSyncs;

  State m_currentState;
  std::vector<uint8_t> m_internalBuffer;

  std::queue<std::vector<uint8_t>> m_outputBuffer;

  uint64_t m_tvalueDiscardCount;

  State expectingInitialSync();
  State expectingSync();
  State handleUndershoot();
  State handleOvershoot();
};

#endif  // DEC_TVALUESTOCHANNEL_H