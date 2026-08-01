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

namespace orc {

namespace {

// Pick the winning value for one byte position across |copies|.
//
// Odd parity (ETSI EN 300 706 §8.1) detects every single-bit error, so a byte
// that fails it is known to be corrupt: the vote is held among the
// parity-clean candidates whenever there are any, and only falls back to all
// candidates when every copy of this position is damaged. Ties go to the
// value from the most recently added copy: with only two differing copies
// there is no majority to find, and the newer transmission is what a plain
// Level 1 decoder would be showing.
uint8_t vote(const std::vector<TeletextRowBytes>& copies, size_t position) {
  const auto tally = [&](bool parity_clean_only) -> std::optional<uint8_t> {
    // 256 counters is cheaper than a map for a vote this small, and keeping
    // the last-seen index gives a deterministic tie-break.
    std::array<uint16_t, 256> counts{};
    std::array<size_t, 256> last_seen{};
    bool any = false;
    for (size_t i = 0; i < copies.size(); ++i) {
      const uint8_t value = copies[i][position];
      if (parity_clean_only && !teletext_odd_parity_valid(value)) {
        continue;
      }
      any = true;
      ++counts[value];
      last_seen[value] = i;
    }
    if (!any) {
      return std::nullopt;
    }
    uint8_t best = 0;
    uint16_t best_count = 0;
    size_t best_last = 0;
    for (int value = 0; value < 256; ++value) {
      if (counts[value] == 0) {
        continue;
      }
      const auto candidate = static_cast<uint8_t>(value);
      if (counts[value] > best_count ||
          (counts[value] == best_count && last_seen[value] > best_last)) {
        best = candidate;
        best_count = counts[value];
        best_last = last_seen[value];
      }
    }
    return best;
  };

  if (const auto clean = tally(/*parity_clean_only=*/true)) {
    return *clean;
  }
  return tally(/*parity_clean_only=*/false).value_or(copies[0][position]);
}

}  // namespace

void TeletextRowSquasher::add_row(const TeletextPageKey& key, int row,
                                  const TeletextRowBytes& bytes,
                                  int64_t source) {
  if (row < 1 || row >= static_cast<int>(
                            std::tuple_size<decltype(PageRows::rows)>::value)) {
    return;  // header row and enhancement packets are not squashed
  }

  auto it = pages_.find(key);
  if (it == pages_.end()) {
    enforce_page_bound();
    it = pages_.emplace(key, PageRows{}).first;
  }
  it->second.last_touched = ++touch_counter_;

  RowCopies& copies = it->second.rows[static_cast<size_t>(row)];

  // Re-reading the same recovered line replaces its earlier copy: a previewer
  // that rebuilds a sliding window would otherwise let a long-resident frame
  // outvote the rest purely by being read more often.
  const auto existing =
      std::find(copies.sources.begin(), copies.sources.end(), source);
  if (existing != copies.sources.end()) {
    copies.copies[static_cast<size_t>(
        std::distance(copies.sources.begin(), existing))] = bytes;
    return;
  }

  if (copies.sources.size() >= options_.max_copies_per_row) {
    copies.sources.erase(copies.sources.begin());
    copies.copies.erase(copies.copies.begin());
  }
  copies.sources.push_back(source);
  copies.copies.push_back(bytes);
}

std::optional<TeletextRowBytes> TeletextRowSquasher::squashed_row(
    const TeletextPageKey& key, int row) const {
  const auto it = pages_.find(key);
  if (it == pages_.end() || row < 1 ||
      row >=
          static_cast<int>(std::tuple_size<decltype(PageRows::rows)>::value)) {
    return std::nullopt;
  }
  const RowCopies& copies = it->second.rows[static_cast<size_t>(row)];
  if (copies.copies.empty()) {
    return std::nullopt;
  }
  if (copies.copies.size() == 1) {
    return copies.copies.front();  // a vote of one is the copy itself
  }

  TeletextRowBytes result{};
  for (size_t position = 0; position < kTeletextRowBytes; ++position) {
    result[position] = vote(copies.copies, position);
  }
  return result;
}

void TeletextRowSquasher::erase_page(const TeletextPageKey& key) {
  pages_.erase(key);
}

size_t TeletextRowSquasher::copy_count(const TeletextPageKey& key,
                                       int row) const {
  const auto it = pages_.find(key);
  if (it == pages_.end() || row < 1 ||
      row >=
          static_cast<int>(std::tuple_size<decltype(PageRows::rows)>::value)) {
    return 0;
  }
  return it->second.rows[static_cast<size_t>(row)].copies.size();
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
