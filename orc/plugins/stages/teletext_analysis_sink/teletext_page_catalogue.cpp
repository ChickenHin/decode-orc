/*
 * File:        teletext_page_catalogue.cpp
 * Module:      orc-stage-plugin-teletext_analysis_sink
 * Purpose:     Bounded teletext page catalogue implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_page_catalogue.h"

#include <algorithm>
#include <utility>

namespace orc {

TeletextPageCatalogue::TeletextPageCatalogue(std::size_t max_pages)
    : max_pages_(max_pages == 0 ? 1 : max_pages) {}

void TeletextPageCatalogue::merge(const TeletextPageSnapshot& snapshot,
                                  uint64_t frame_id) {
  const std::pair<int, int> key{snapshot.magazine, snapshot.page_number};

  auto it = pages_.find(key);
  const bool is_new = (it == pages_.end());
  if (is_new) {
    Entry fresh;
    fresh.page.magazine = snapshot.magazine;
    fresh.page.page_number = snapshot.page_number;
    fresh.page.first_seen_frame = frame_id;
    it = pages_.emplace(key, std::move(fresh)).first;
  }

  Entry& entry = it->second;
  entry.last_touched = ++touch_counter_;

  // The content is always replaced: with a row squasher attached the decoder
  // renders every snapshot from every copy recovered so far, so the newest
  // assembly is the best one.
  entry.page.page = snapshot;
  entry.page.last_seen_frame = frame_id;

  // Sticky subtitle flag — the service telling the receiver which page carries
  // the subtitles. A page transmitted without C6 between captions is still the
  // subtitle page.
  if (snapshot.subtitle) {
    entry.page.subtitle = true;
  }

  // One appearance per distinct header transmission. A rolling header (ETSI EN
  // 300 706 §9.3.1.4) closes the page mid-transmission and re-emits it under
  // the same header field index, which is what keeps this from counting the
  // same appearance twice.
  if (snapshot.header_field_index != entry.counted_header_field) {
    entry.counted_header_field = snapshot.header_field_index;
    ++entry.page.times_seen;
  }

  if (is_new) {
    enforce_page_bound();
  }
}

void TeletextPageCatalogue::enforce_page_bound() {
  while (pages_.size() > max_pages_) {
    auto oldest = pages_.begin();
    for (auto it = pages_.begin(); it != pages_.end(); ++it) {
      if (it->second.last_touched < oldest->second.last_touched) {
        oldest = it;
      }
    }
    pages_.erase(oldest);
    truncated_ = true;
  }
}

std::vector<TeletextCataloguedPage> TeletextPageCatalogue::pages() const {
  std::vector<TeletextCataloguedPage> out;
  out.reserve(pages_.size());
  for (const auto& [key, entry] : pages_) {
    out.push_back(entry.page);
  }
  return out;
}

}  // namespace orc
