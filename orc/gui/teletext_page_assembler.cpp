/*
 * File:        teletext_page_assembler.cpp
 * Module:      orc-gui
 * Purpose:     Trailing-frame-window cache and accumulating page catalogue for
 *              the teletext preview dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_page_assembler.h"

#include <orc/support/teletext_page_decoder.h>

#include <utility>

#include "teletext_observation_presenter.h"

namespace {

using Catalogue =
    std::map<std::pair<int, int>, TeletextPageAssembler::CataloguedPage>;

// Drop the entry whose page was seen longest ago (catalogue cap enforcement).
void evictLeastRecentlySeen(Catalogue& catalogue) {
  if (catalogue.empty()) {
    return;
  }
  auto oldest = catalogue.begin();
  for (auto it = catalogue.begin(); it != catalogue.end(); ++it) {
    if (it->second.seen_frame < oldest->second.seen_frame) {
      oldest = it;
    }
  }
  catalogue.erase(oldest);
}

}  // namespace

void TeletextPageAssembler::setCurrentFrame(uint64_t frame_index) {
  // Merge what the current window holds into the catalogue before any of it
  // is evicted below: catalogue contents must not depend on whether a reader
  // happened to look between the delivery of a frame and its eviction.
  refreshCatalogue();

  // A move of at least a whole window length leaves the new window sharing no
  // frames with the old one: the accumulated catalogue no longer describes
  // anything near the previewer, so it is discarded and rebuilt from the
  // frames preceding the position jumped to. Sequential stepping (and short
  // backward steps) keep the catalogue.
  const uint64_t distance = frame_index >= current_frame_
                                ? frame_index - current_frame_
                                : current_frame_ - frame_index;
  if (distance >= kTrailingWindowFrames && !catalogue_.empty()) {
    catalogue_.clear();
    // The accumulated row copies describe the same superseded position, and
    // a service can change entirely across a seek; keeping them would let a
    // page here be built from rows recovered somewhere else.
    squasher_.clear();
    ++catalogue_revision_;
  }

  current_frame_ = frame_index;
  const uint64_t start = windowStartFrame();
  frames_.erase(frames_.begin(), frames_.lower_bound(start));
  frames_.erase(frames_.upper_bound(current_frame_), frames_.end());
  catalogue_dirty_ = true;
}

uint64_t TeletextPageAssembler::windowStartFrame() const {
  return current_frame_ >= kTrailingWindowFrames - 1
             ? current_frame_ - (kTrailingWindowFrames - 1)
             : 0;
}

std::vector<uint64_t> TeletextPageAssembler::framesNeedingData() const {
  std::vector<uint64_t> needed;
  for (uint64_t frame = windowStartFrame(); frame <= current_frame_; ++frame) {
    if (frames_.find(frame) == frames_.end()) {
      needed.push_back(frame);
    }
  }
  return needed;
}

void TeletextPageAssembler::storeFrame(
    uint64_t frame_index, orc::presenters::TeletextFieldPacketsView field1,
    orc::presenters::TeletextFieldPacketsView field2) {
  if (frame_index < windowStartFrame() || frame_index > current_frame_) {
    return;  // stale delivery from a superseded window
  }
  frames_[frame_index] = FrameData{std::move(field1), std::move(field2)};
  catalogue_dirty_ = true;
}

void TeletextPageAssembler::markFrameUnavailable(uint64_t frame_index) {
  storeFrame(frame_index, orc::presenters::TeletextFieldPacketsView{},
             orc::presenters::TeletextFieldPacketsView{});
}

bool TeletextPageAssembler::hasFrame(uint64_t frame_index) const {
  return frames_.find(frame_index) != frames_.end();
}

void TeletextPageAssembler::clear() {
  frames_.clear();
  squasher_.clear();
  if (!catalogue_.empty()) {
    catalogue_.clear();
    ++catalogue_revision_;
  }
  catalogue_dirty_ = false;
}

std::vector<TeletextPageAssembler::PageListing>
TeletextPageAssembler::cataloguedPages() const {
  refreshCatalogue();
  std::vector<PageListing> listings;
  listings.reserve(catalogue_.size());
  for (const auto& [address, entry] : catalogue_) {
    listings.push_back(PageListing{entry.magazine, entry.page_number,
                                   entry.seen_frame, entry.times_seen});
  }
  return listings;
}

const TeletextPageAssembler::CataloguedPage* TeletextPageAssembler::findPage(
    int magazine, int page_number) const {
  refreshCatalogue();
  const auto it = catalogue_.find({magazine, page_number});
  return it == catalogue_.end() ? nullptr : &it->second;
}

uint64_t TeletextPageAssembler::catalogueRevision() const {
  refreshCatalogue();
  return catalogue_revision_;
}

void TeletextPageAssembler::refreshCatalogue() const {
  if (!catalogue_dirty_) {
    return;
  }
  catalogue_dirty_ = false;

  // Decode the whole cached window from scratch: deliveries arrive out of
  // order, so the decoder — which requires monotonically non-decreasing field
  // indices — is fed from the frame-ordered cache rather than incrementally.
  // Results are merged into the catalogue, so pages whose frames have since
  // been evicted stay listed.
  orc::TeletextPageDecoder decoder;
  // Rows go into the squasher, which outlives the window: a page keeps rows
  // recovered during earlier transmissions, and repeated copies of a row
  // correct each other. Snapshots are rendered from the squashed rows.
  decoder.set_row_squasher(&squasher_);
  decoder.set_page_callback([this](const orc::TeletextPageSnapshot& snapshot) {
    const std::pair<int, int> address{snapshot.magazine, snapshot.page_number};
    const uint64_t seen_frame =
        static_cast<uint64_t>(snapshot.header_field_index) / 2;

    auto it = catalogue_.find(address);
    if (it == catalogue_.end()) {
      if (catalogue_.size() >= kMaxCataloguedPages) {
        evictLeastRecentlySeen(catalogue_);
      }
      it = catalogue_.emplace(address, CataloguedPage{}).first;
      it->second.magazine = snapshot.magazine;
      it->second.page_number = snapshot.page_number;
      it->second.seen_frame = seen_frame;
      it->second.times_seen = 1;
      it->second.counted_header_field = snapshot.header_field_index;
      it->second.page =
          orc::presenters::TeletextObservationPresenter::makePageView(snapshot);
      ++catalogue_revision_;
      return;
    }

    // The whole cached window is re-decoded on every frame change, so most
    // snapshots replay a transmission already counted. Only a header past the
    // newest one counted is a fresh appearance of the page in the carousel;
    // this also keeps "last seen" at the latest transmission when the user
    // steps backwards.
    const bool new_transmission =
        snapshot.header_field_index > it->second.counted_header_field;
    if (new_transmission) {
      it->second.counted_header_field = snapshot.header_field_index;
      it->second.seen_frame = seen_frame;
      ++it->second.times_seen;
      ++catalogue_revision_;
    } else if (it->second.page.last_field_index == snapshot.last_field_index) {
      return;  // identical re-decode of an unchanged window
    }

    // The newest decode wins outright. It is not a fragment even when the
    // trailing window clipped the transmission that produced it: the decoder
    // renders from squasher_, which holds every row copy seen since the last
    // discontinuity, so rows this transmission did not carry come from the
    // ones that did.
    it->second.page =
        orc::presenters::TeletextObservationPresenter::makePageView(snapshot);
  });

  // Candidate VBI lines per field, for packing (field, line) into one copy
  // identity. The observer scans field lines 5-21, well inside this bound.
  constexpr int64_t kFieldLineStride = 64;

  int64_t last_field_index = 0;
  for (const auto& [frame_index, data] : frames_) {
    for (const auto* field : {&data.field1, &data.field2}) {
      const int64_t field_index = static_cast<int64_t>(frame_index) * 2 +
                                  (field == &data.field2 ? 1 : 0);
      for (const auto& packet : field->packets) {
        // The (field, line) origin is this copy's identity, so the repeated
        // window rebuilds below re-seat the same copy rather than letting a
        // long-resident frame outvote the rest.
        const int64_t source = field_index * kFieldLineStride +
                               (packet.field_line % kFieldLineStride);
        decoder.process_packet(packet.bytes, field_index, source);
        last_field_index = field_index;
      }
    }
  }
  decoder.finalize(last_field_index + 1);
}
