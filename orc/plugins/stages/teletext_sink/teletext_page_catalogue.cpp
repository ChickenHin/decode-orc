/*
 * File:        teletext_page_catalogue.cpp
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Bounded teletext page catalogue implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_page_catalogue.h"

#include <algorithm>
#include <utility>

namespace orc {

TeletextPageCatalogue::TeletextPageCatalogue(std::size_t max_subpages,
                                             std::size_t max_subpages_per_page)
    : max_subpages_(max_subpages == 0 ? 1 : max_subpages),
      max_subpages_per_page_(
          max_subpages_per_page == 0 ? 1 : max_subpages_per_page) {}

void TeletextPageCatalogue::merge(const TeletextPageSnapshot& snapshot,
                                  uint64_t frame_id) {
  const std::pair<int, int> page_key{snapshot.magazine, snapshot.page_number};
  Entry& entry = pages_[page_key];

  // Sticky subtitle flag — the service telling the receiver which page carries
  // the subtitles. A page transmitted without C6 between captions is still the
  // subtitle page, and the flag belongs to the page rather than to whichever
  // sub-page of it happened to carry a caption.
  if (snapshot.subtitle) {
    entry.subtitle = true;
  }

  // The sub-code identifies which sub-page of the set this is (ETSI EN 300 706
  // §9.3.1.2, Annex A.1). It is the identity the row squasher combined its
  // copies under, so a sub-page here holds exactly what was combined for it.
  auto sub_it = entry.subpages.find(snapshot.subcode);
  const bool is_new = (sub_it == entry.subpages.end());
  if (is_new) {
    SubEntry fresh;
    fresh.subpage.subcode = snapshot.subcode;
    fresh.subpage.first_seen_frame = frame_id;
    sub_it = entry.subpages.emplace(snapshot.subcode, std::move(fresh)).first;
    ++subpage_count_;
  }

  SubEntry& sub_entry = sub_it->second;
  sub_entry.last_touched = ++touch_counter_;

  // The content is always replaced: with a row squasher attached the decoder
  // renders every snapshot from every copy recovered so far, so the newest
  // assembly is the best one.
  sub_entry.subpage.page = snapshot;
  sub_entry.subpage.last_seen_frame = frame_id;

  // One appearance per distinct header transmission. A rolling header (ETSI EN
  // 300 706 §9.3.1.4) closes the page mid-transmission and re-emits it under
  // the same header field index, which is what keeps this from counting the
  // same appearance twice.
  if (snapshot.header_field_index != sub_entry.counted_header_field) {
    sub_entry.counted_header_field = snapshot.header_field_index;
    ++sub_entry.subpage.times_seen;
  }

  if (is_new) {
    enforce_subpage_bounds(entry);
  }
}

std::map<int, TeletextPageCatalogue::SubEntry>::iterator
TeletextPageCatalogue::oldest_subpage(Entry& entry) {
  auto oldest = entry.subpages.begin();
  for (auto it = entry.subpages.begin(); it != entry.subpages.end(); ++it) {
    if (it->second.last_touched < oldest->second.last_touched) {
      oldest = it;
    }
  }
  return oldest;
}

void TeletextPageCatalogue::enforce_subpage_bounds(Entry& page_entry) {
  // Per-page cap first: a page whose sub-codes churn loses its own oldest
  // sub-pages rather than the rest of the service's.
  while (page_entry.subpages.size() > max_subpages_per_page_) {
    page_entry.subpages.erase(oldest_subpage(page_entry));
    --subpage_count_;
    truncated_ = true;
  }

  while (subpage_count_ > max_subpages_) {
    auto oldest_page = pages_.begin();
    auto oldest_sub = oldest_subpage(oldest_page->second);
    for (auto page_it = pages_.begin(); page_it != pages_.end(); ++page_it) {
      if (page_it->second.subpages.empty()) {
        continue;
      }
      const auto candidate = oldest_subpage(page_it->second);
      if (candidate->second.last_touched < oldest_sub->second.last_touched) {
        oldest_page = page_it;
        oldest_sub = candidate;
      }
    }
    oldest_page->second.subpages.erase(oldest_sub);
    --subpage_count_;
    truncated_ = true;
    // A page number is only in the catalogue for the sub-pages it carried.
    if (oldest_page->second.subpages.empty()) {
      pages_.erase(oldest_page);
    }
  }
}

std::vector<TeletextCataloguedPage> TeletextPageCatalogue::pages() const {
  std::vector<TeletextCataloguedPage> out;
  out.reserve(pages_.size());
  for (const auto& [key, entry] : pages_) {
    if (entry.subpages.empty()) {
      continue;  // cannot happen: a page is erased with its last sub-page
    }
    TeletextCataloguedPage page;
    page.magazine = key.first;
    page.page_number = key.second;
    page.subtitle = entry.subtitle;
    page.subpages.reserve(entry.subpages.size());

    // The page-level figures cover the whole set, so that a table of pages
    // reads the same whether or not a page turns out to be a multi-page set.
    bool first = true;
    for (const auto& [subcode, sub_entry] : entry.subpages) {
      const auto& subpage = sub_entry.subpage;
      page.times_seen += subpage.times_seen;
      page.first_seen_frame =
          first ? subpage.first_seen_frame
                : std::min(page.first_seen_frame, subpage.first_seen_frame);
      page.last_seen_frame =
          first ? subpage.last_seen_frame
                : std::max(page.last_seen_frame, subpage.last_seen_frame);
      first = false;
      page.subpages.push_back(subpage);
    }
    out.push_back(std::move(page));
  }
  return out;
}

}  // namespace orc
