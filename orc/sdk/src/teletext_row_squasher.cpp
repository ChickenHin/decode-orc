/*
 * File:        teletext_row_squasher.cpp
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     Combine repeated copies of a teletext page row into one
 *              best-estimate row ("squashing")
 *
 * Squashing is an idea from vhs-teletext by Alistair Buxton (ali1234),
 * https://github.com/ali1234/vhs-teletext, with thanks — independently
 * implemented here (see teletext_row_squasher.h).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/support/teletext_page_decoder.h>
#include <orc/support/teletext_row_squasher.h>

#include <algorithm>
#include <array>

namespace orc {

void TeletextRowSquasher::add_row(const TeletextPageKey& key, int row,
                                  const TeletextRowBytes& bytes, int64_t source,
                                  const TeletextRowConfidence* confidence,
                                  size_t first_column, size_t column_count) {
  if (row < 0 || row >= static_cast<int>(
                            std::tuple_size<decltype(PageRows::rows)>::value)) {
    return;  // enhancement packets are not squashed
  }
  first_column = std::min(first_column, kTeletextRowBytes);
  column_count = std::min(column_count, kTeletextRowBytes - first_column);
  if (column_count == 0) {
    return;
  }
  if (row == 0 && first_column == 0) {
    // The header packet's own display bytes carry a live clock, which differs
    // between transmissions of the same page by design; only columns a separate
    // packet carries can be combined (see the class comment).
    return;
  }

  auto it = pages_.find(key);
  if (it == pages_.end()) {
    enforce_page_bound();
    it = pages_.emplace(key, PageRows{}).first;
  }
  it->second.last_touched = ++touch_counter_;

  RowCopies& copies = it->second.rows[static_cast<size_t>(row)];

  TeletextRowConfidence weights;
  if (confidence != nullptr) {
    weights = *confidence;
  } else {
    weights.fill(1.0F);
  }

  // Re-reading the same recovered line replaces its earlier copy: a previewer
  // that rebuilds a sliding window would otherwise let a long-resident frame
  // outvote the rest purely by being read more often. A source is only the same
  // copy when it speaks for the same columns — one packet of a service that
  // splits a row across two contributes to both halves under the same id.
  for (size_t index = 0; index < copies.sources.size(); ++index) {
    if (copies.sources[index] != source ||
        copies.first_column[index] != first_column) {
      continue;
    }
    copies.copies[index] = bytes;
    copies.confidences[index] = weights;
    copies.column_count[index] = static_cast<uint8_t>(column_count);
    return;  // recency deliberately unchanged: a re-read, not a transmission
  }

  if (copies.sources.size() >= options_.max_copies_per_row &&
      !copies.sources.empty()) {
    // Overwrite the oldest slot in place (see RowCopies::seq), among the copies
    // speaking for these columns: the two halves of a split row each keep their
    // own history, so a busy half cannot evict the other's only copy.
    size_t oldest = copies.seq.size();
    for (size_t i = 0; i < copies.seq.size(); ++i) {
      if (copies.first_column[i] != first_column) {
        continue;
      }
      if (oldest == copies.seq.size() || copies.seq[i] < copies.seq[oldest]) {
        oldest = i;
      }
    }
    if (oldest < copies.seq.size()) {
      copies.sources[oldest] = source;
      copies.copies[oldest] = bytes;
      copies.confidences[oldest] = weights;
      copies.first_column[oldest] = static_cast<uint8_t>(first_column);
      copies.column_count[oldest] = static_cast<uint8_t>(column_count);
      copies.seq[oldest] = copies.next_seq++;
      return;
    }
    // No copy of these columns yet; let the row grow by one rather than drop
    // the contribution, so a half that arrives late is not locked out.
  }
  copies.sources.push_back(source);
  copies.copies.push_back(bytes);
  copies.confidences.push_back(weights);
  copies.first_column.push_back(static_cast<uint8_t>(first_column));
  copies.column_count.push_back(static_cast<uint8_t>(column_count));
  copies.seq.push_back(copies.next_seq++);
}

std::optional<TeletextRowBytes> TeletextRowSquasher::squashed_row(
    const TeletextPageKey& key, int row, TeletextRowCoverage* covered) const {
  if (covered != nullptr) {
    covered->fill(false);
  }
  const auto it = pages_.find(key);
  if (it == pages_.end() || row < 0 ||
      row >=
          static_cast<int>(std::tuple_size<decltype(PageRows::rows)>::value)) {
    return std::nullopt;
  }
  const RowCopies& copies = it->second.rows[static_cast<size_t>(row)];
  if (copies.copies.empty()) {
    return std::nullopt;
  }
  if (copies.copies.size() == 1) {
    // A vote of one is the copy itself.
    if (covered != nullptr) {
      const size_t first = copies.first_column.front();
      const size_t count = copies.column_count.front();
      std::fill(covered->begin() + static_cast<std::ptrdiff_t>(first),
                covered->begin() + static_cast<std::ptrdiff_t>(first + count),
                true);
    }
    return copies.copies.front();
  }

  // Pick the winning value at each byte position across the copies.
  //
  // Odd parity (ETSI EN 300 706 §8.1) detects every single-bit error, so a
  // byte that fails it is known to be corrupt: the vote is held among the
  // parity-clean candidates whenever there are any, and only falls back to all
  // candidates when every copy of this position is damaged. Within that, each
  // copy contributes how sure the recovery chain was of the byte (1 where it
  // cannot say), so a value read cleanly outweighs the same number of copies of
  // one the detector nearly decided the other way. Ties go to the value from
  // the most recently added copy: with only two differing copies there is no
  // majority to find, and the newer transmission is what a plain Level 1
  // decoder would be showing.
  //
  // This runs for every byte of every row of every page rendered — a previewer
  // renders per frame stepped, and the sink's rewrite pass asks once per
  // packet of the whole recording — so the tally is one pass over the copies
  // per position. The accumulators are per byte value, epoch-marked by
  // position so nothing is cleared between positions.
  const size_t copy_count = copies.copies.size();
  std::array<double, 256> weight_of;
  std::array<uint64_t, 256> newest_of;
  std::array<uint8_t, 256> seen_at{};  // epoch marks: position + 1, 0 = never
  std::array<uint8_t, 256> distinct;   // values seen at this position

  TeletextRowBytes result{};
  for (size_t position = 0; position < kTeletextRowBytes; ++position) {
    const uint8_t epoch = static_cast<uint8_t>(position + 1);
    size_t distinct_count = 0;
    for (size_t j = 0; j < copy_count; ++j) {
      // A copy votes only within the columns it spoke for: the two halves of a
      // split row are combined independently.
      if (position < copies.first_column[j] ||
          position >= static_cast<size_t>(copies.first_column[j]) +
                          copies.column_count[j]) {
        continue;
      }
      const uint8_t value = copies.copies[j][position];
      if (seen_at[value] != epoch) {
        seen_at[value] = epoch;
        weight_of[value] = 0.0;
        newest_of[value] = 0;
        distinct[distinct_count++] = value;
      }
      weight_of[value] += static_cast<double>(copies.confidences[j][position]);
      newest_of[value] = std::max(newest_of[value], copies.seq[j]);
    }

    bool found = false;
    bool best_clean = false;
    uint8_t best = 0;
    double best_weight = 0.0;
    uint64_t best_newest = 0;
    for (size_t k = 0; k < distinct_count; ++k) {
      const uint8_t value = distinct[k];
      const bool clean = teletext_odd_parity_valid(value);
      if (found) {
        if (best_clean && !clean) {
          continue;  // a value known corrupt never beats a parity-clean one
        }
        if (clean == best_clean && !(weight_of[value] > best_weight ||
                                     (weight_of[value] == best_weight &&
                                      newest_of[value] > best_newest))) {
          continue;
        }
      }
      found = true;
      best = value;
      best_clean = clean;
      best_weight = weight_of[value];
      best_newest = newest_of[value];
    }
    result[position] = best;
    if (covered != nullptr) {
      (*covered)[position] = found;
    }
  }
  return result;
}

void TeletextRowSquasher::erase_page(const TeletextPageKey& key) {
  pages_.erase(key);
}

size_t TeletextRowSquasher::copy_count(const TeletextPageKey& key,
                                       int row) const {
  const auto it = pages_.find(key);
  if (it == pages_.end() || row < 0 ||
      row >=
          static_cast<int>(std::tuple_size<decltype(PageRows::rows)>::value)) {
    return 0;
  }
  const RowCopies& copies = it->second.rows[static_cast<size_t>(row)];
  size_t count = 0;
  for (const uint8_t first : copies.first_column) {
    count += (first == 0) ? 1 : 0;
  }
  return count;
}

void TeletextRowSquasher::clear() {
  pages_.clear();
  touch_counter_ = 0;
}

void TeletextRowSquasher::enforce_page_bound() {
  if (pages_.size() < options_.max_pages) {
    return;
  }
  auto oldest = pages_.begin();
  for (auto it = pages_.begin(); it != pages_.end(); ++it) {
    if (it->second.last_touched < oldest->second.last_touched) {
      oldest = it;
    }
  }
  pages_.erase(oldest);
}

}  // namespace orc
