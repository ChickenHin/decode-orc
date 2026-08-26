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
#include <cstddef>
#include <utility>

namespace orc {

uint64_t teletext_subpage_lost_packets(
    const TeletextCataloguedSubPage& subpage) {
  uint64_t lost = 0;
  const auto& copies = subpage.page.row_copies;
  // Row 0 is the header, which carries a live clock and is never squashed, so
  // its copy count is always zero and says nothing.
  for (std::size_t row = 1; row < copies.size(); ++row) {
    const auto arrived = static_cast<uint64_t>(std::max(0, copies[row]));
    // A row that never arrived is unknowable; more copies than appearances is
    // a row re-sent inside one transmission, not negative loss.
    if (arrived == 0 || arrived >= subpage.times_seen) {
      continue;
    }
    lost += subpage.times_seen - arrived;
  }
  return lost;
}

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
    if (snapshot.identity_attested) {
      ++sub_entry.subpage.times_attested;
    }
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

namespace {

// A page's identity written out digit by digit, in transmission order: the
// magazine, the two page-number digits (ETSI EN 300 706 §9.3.1.1) and the four
// sub-code digits S1 to S4 (§9.3.1.2). Seven digits, each of which arrived in
// its own Hamming 8/4 byte and so can be moved on its own.
//
// The sub-code is unpacked back into the four fields the header carries rather
// than compared as the packed value TeletextPageSnapshot holds: S2 occupies
// three bits and S4 two, so a single digit of the packed form spans two of the
// transmitted fields and a single-digit test over it would be testing something
// the transmission never had.
VbiIdentityDigits page_identity_digits(int magazine, int page_number,
                                       int subcode) {
  VbiIdentityDigits digits;
  digits.reserve(7);
  digits.push_back(static_cast<uint8_t>(magazine & 0xF));
  digits.push_back(static_cast<uint8_t>((page_number >> 4) & 0xF));
  digits.push_back(static_cast<uint8_t>(page_number & 0xF));
  digits.push_back(static_cast<uint8_t>(subcode & 0xF));          // S1
  digits.push_back(static_cast<uint8_t>((subcode >> 4) & 0x7));   // S2
  digits.push_back(static_cast<uint8_t>((subcode >> 7) & 0xF));   // S3
  digits.push_back(static_cast<uint8_t>((subcode >> 11) & 0x3));  // S4
  return digits;
}

}  // namespace

VbiIdentityReconciliation TeletextPageCatalogue::reconcile_identities() {
  // One identity per sub-page: the sub-code is part of what a header names, so
  // a misread S1 duplicates a sub-page inside a page number that is itself
  // perfectly attested.
  struct Located {
    std::pair<int, int> page_key;
    int subcode = 0;
    VbiIdentityDigits digits;
  };

  VbiIdentityReconciliation out;
  std::vector<Located> attested;
  std::vector<Located> unattested;
  for (const auto& [page_key, entry] : pages_) {
    for (const auto& [subcode, sub_entry] : entry.subpages) {
      Located located{
          page_key, subcode,
          page_identity_digits(page_key.first, page_key.second, subcode)};
      if (sub_entry.subpage.times_attested > 0) {
        attested.push_back(std::move(located));
      } else {
        unattested.push_back(std::move(located));
      }
    }
  }
  out.identities_seen =
      static_cast<uint32_t>(attested.size() + unattested.size());
  out.identities_unattested = static_cast<uint32_t>(unattested.size());

  if (!vbi_identity_reconciliation_applies(attested.size())) {
    // Nothing arrived as transmitted, so there is no baseline and the rule has
    // nothing to say. Leaving the catalogue as recovered is the honest answer;
    // emptying it would be the confident one.
    out.withheld = out.identities_unattested > 0;
    return out;
  }

  std::vector<VbiIdentityDigits> attested_digits;
  attested_digits.reserve(attested.size());
  for (const Located& located : attested) {
    attested_digits.push_back(located.digits);
  }

  for (const Located& located : unattested) {
    auto page_it = pages_.find(located.page_key);
    if (page_it == pages_.end()) {
      continue;
    }
    auto sub_it = page_it->second.subpages.find(located.subcode);
    if (sub_it == page_it->second.subpages.end()) {
      continue;
    }
    const TeletextCataloguedSubPage& misread = sub_it->second.subpage;

    const auto neighbour =
        vbi_single_digit_neighbour(located.digits, attested_digits);
    bool folded = false;
    if (neighbour.has_value()) {
      const Located& target = attested[*neighbour];
      auto target_page = pages_.find(target.page_key);
      if (target_page != pages_.end()) {
        auto target_sub = target_page->second.subpages.find(target.subcode);
        if (target_sub != target_page->second.subpages.end()) {
          // The appearances were appearances of the target: the carousel
          // brought that page round and this recording misread its number on
          // the way past. Its rows stay behind — the squasher combined them
          // under the misread number, so there is nothing here to merge — but
          // counting the appearances keeps times_seen a property of the service
          // rather than of the damage.
          TeletextCataloguedSubPage& kept = target_sub->second.subpage;
          kept.times_seen += misread.times_seen;
          kept.first_seen_frame =
              std::min(kept.first_seen_frame, misread.first_seen_frame);
          kept.last_seen_frame =
              std::max(kept.last_seen_frame, misread.last_seen_frame);
          folded = true;
        }
      }
    }

    if (folded) {
      ++out.identities_folded;
      out.appearances_folded += misread.times_seen;
    } else {
      ++out.identities_dropped;
      out.appearances_dropped += misread.times_seen;
    }

    page_it->second.subpages.erase(sub_it);
    --subpage_count_;
    // A page number is only in the catalogue for the sub-pages it carried.
    if (page_it->second.subpages.empty()) {
      pages_.erase(page_it);
    }
  }
  return out;
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
      page.times_attested += subpage.times_attested;
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
