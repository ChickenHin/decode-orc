/*
 * File:        audio_head_offset.h
 * Purpose:     efm-decoder-audio - head pad/trim of the decoded audio stream
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef AUDIO_HEAD_OFFSET_H
#define AUDIO_HEAD_OFFSET_H

#include <algorithm>
#include <cstdint>
#include <vector>

// Applies a one-shot head adjustment to a sequentially written interleaved
// stereo int16 stream (audio/video sync alignment, issue #231). A positive
// offset prepends that many silent stereo pairs before the first decoded
// pair; a negative offset drops the first |offset| decoded pairs. Shared by
// WriterWav and WriterRaw so both output forms shift identically.
//
// Thread safety: none; owned and driven by a single writer instance.
class AudioHeadOffset {
 public:
  // Must be called before the first apply(); later pairs are untouched.
  void setOffsetPairs(int64_t pairs) {
    if (pairs >= 0) {
      m_padPairsPending = static_cast<uint64_t>(pairs);
      m_trimPairsPending = 0;
    } else {
      m_trimPairsPending = static_cast<uint64_t>(-pairs);
      m_padPairsPending = 0;
    }
  }

  // Mutates one section-sized interleaved stereo buffer in stream order:
  // drops still-pending head pairs from its front, or prepends the pending
  // silence to the first buffer. May leave the buffer empty while a large
  // trim is being consumed.
  void apply(std::vector<int16_t>& sectionData) {
    if (m_trimPairsPending > 0) {
      const uint64_t pairsInSection =
          static_cast<uint64_t>(sectionData.size()) / 2;
      const uint64_t trim = std::min(m_trimPairsPending, pairsInSection);
      sectionData.erase(
          sectionData.begin(),
          sectionData.begin() + static_cast<std::ptrdiff_t>(trim * 2));
      m_trimPairsPending -= trim;
    } else if (m_padPairsPending > 0) {
      sectionData.insert(sectionData.begin(),
                         static_cast<size_t>(m_padPairsPending) * 2, 0);
      m_padPairsPending = 0;
    }
  }

 private:
  uint64_t m_padPairsPending{0};
  uint64_t m_trimPairsPending{0};
};

#endif  // AUDIO_HEAD_OFFSET_H
