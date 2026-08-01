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

#include <iterator>
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

// Candidate VBI lines per field, for packing (field, line) into one copy
// identity. The observer scans field lines 5-21, well inside this bound.
constexpr int64_t kFieldLineStride = 64;

// Release a consumed frame's packets. The entry itself stays, because it is
// also the record that the frame was fetched and need not be requested again.
void releasePackets(orc::presenters::TeletextFieldPacketsView& field) {
  field.packets.clear();
  field.packets.shrink_to_fit();
}

}  // namespace

TeletextPageAssembler::TeletextPageAssembler() { restartDecodeAt(0); }

TeletextPageAssembler::~TeletextPageAssembler() = default;

uint64_t TeletextPageAssembler::anchorFor(uint64_t frame_index) {
  return frame_index >= kTrailingWindowFrames - 1
             ? frame_index - (kTrailingWindowFrames - 1)
             : 0;
}

void TeletextPageAssembler::restartDecodeAt(uint64_t anchor_frame) {
  decode_anchor_ = anchor_frame;
  decode_frontier_ = anchor_frame;
  field_packet_counts_.clear();
  decoder_ = std::make_unique<orc::TeletextPageDecoder>();
  // Rows go into the squasher, which outlives any single transmission: a page
  // keeps rows recovered during earlier ones, and repeated copies of a row
  // correct each other. Snapshots are rendered from the squashed rows.
  decoder_->set_row_squasher(&squasher_);
  decoder_->set_page_callback(
      [this](const orc::TeletextPageSnapshot& snapshot) {
        mergeSnapshot(snapshot);
      });
}

void TeletextPageAssembler::setCurrentFrame(uint64_t frame_index) {
  // Merge what has already been decoded into the catalogue before anything is
  // discarded below: catalogue contents must not depend on whether a reader
  // happened to look between the delivery of a frame and its eviction.
  refreshCatalogue();

  // The decoder only moves forwards. A position it can still reach that way —
  // at or after the frames already consumed, and near enough that the window
  // covers the gap — is continuous, and the accumulated catalogue and row
  // copies keep describing the recording around the previewer. Anything else
  // is a seek: the catalogue would no longer describe anything near the new
  // position, and a service can change entirely across one, so keeping the
  // row copies would let a page here be built from rows recovered elsewhere.
  // Measured from where the previewer was, not from how far decoding has
  // got: reads can lag a long way behind a user holding down a step key, and
  // a backlog is no reason to throw away the history.
  const bool continuous = frame_index >= decode_anchor_ &&
                          frame_index < current_frame_ + kTrailingWindowFrames;
  if (!continuous) {
    const bool had_catalogue = !catalogue_.empty();
    frames_.clear();
    squasher_.clear();
    catalogue_.clear();
    restartDecodeAt(anchorFor(frame_index));
    if (had_catalogue) {
      ++catalogue_revision_;
    }
  }

  current_frame_ = frame_index;
  // Frames past the previewer are dropped after a backward step: they are not
  // wanted now and would be re-requested if it moves forward again. Frames
  // *behind* it are kept whatever the trailing length, because the decoder
  // still has to consume them in order — dropping one it has not reached
  // would stall the frontier on a frame that could never arrive.
  frames_.erase(frames_.upper_bound(current_frame_), frames_.end());
  catalogue_dirty_ = true;
}

uint64_t TeletextPageAssembler::windowStartFrame() const {
  // The span being decoded, which begins one trailing window before wherever
  // the previewer last landed discontinuously and then only grows forwards.
  return decode_anchor_;
}

std::vector<uint64_t> TeletextPageAssembler::framesNeedingData() const {
  std::vector<uint64_t> needed;
  // From the frontier rather than the window start: everything before it has
  // been decoded and released, and a frame arriving late can no longer be
  // used, so re-reading it would achieve nothing.
  const uint64_t start = decode_frontier_;
  auto cached = frames_.lower_bound(start);
  for (uint64_t frame = start;
       frame <= current_frame_ && needed.size() < kMaxFramesPerRequest;
       ++frame) {
    while (cached != frames_.end() && cached->first < frame) {
      ++cached;
    }
    if (cached != frames_.end() && cached->first == frame) {
      continue;
    }
    needed.push_back(frame);
  }
  return needed;
}

std::size_t TeletextPageAssembler::framesNeedingDataCount() const {
  const uint64_t start = decode_frontier_;
  if (current_frame_ < start) {
    return 0;
  }
  const uint64_t span = current_frame_ - start + 1;
  const auto cached = static_cast<uint64_t>(std::distance(
      frames_.lower_bound(start), frames_.upper_bound(current_frame_)));
  return static_cast<std::size_t>(span > cached ? span - cached : 0);
}

void TeletextPageAssembler::storeFrame(
    uint64_t frame_index, orc::presenters::TeletextFieldPacketsView field1,
    orc::presenters::TeletextFieldPacketsView field2) {
  if (frame_index < windowStartFrame() || frame_index > current_frame_) {
    return;  // stale delivery from a superseded window
  }
  if (frame_index < decode_frontier_) {
    return;  // the decoder has already moved past this frame
  }
  frames_[frame_index] = FrameData{std::move(field1), std::move(field2)};
  catalogue_dirty_ = true;
}

void TeletextPageAssembler::markFrameUnavailable(uint64_t frame_index) {
  storeFrame(frame_index, orc::presenters::TeletextFieldPacketsView{},
             orc::presenters::TeletextFieldPacketsView{});
}

bool TeletextPageAssembler::hasFrame(uint64_t frame_index) const {
  // Consumed frames are dropped from the cache, so "already decoded" counts
  // as fetched just as much as "still held". Frames before the anchor were
  // never fetched at all — the current decode run does not reach them.
  if (frame_index >= decode_anchor_ && frame_index < decode_frontier_) {
    return true;
  }
  return frames_.find(frame_index) != frames_.end();
}

void TeletextPageAssembler::clear() {
  frames_.clear();
  squasher_.clear();
  const bool had_catalogue = !catalogue_.empty();
  catalogue_.clear();
  restartDecodeAt(anchorFor(current_frame_));
  if (had_catalogue) {
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
                                   entry.seen_frame, entry.times_seen,
                                   entry.page.transmission_complete});
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

int TeletextPageAssembler::lostPacketsBetween(int64_t first_field,
                                              int64_t last_field) const {
  if (last_field < first_field) {
    return 0;
  }
  const auto begin = field_packet_counts_.lower_bound(first_field);
  const auto end = field_packet_counts_.upper_bound(last_field);
  if (begin == end) {
    return 0;  // nothing recorded for this span; claim nothing
  }

  // The busiest field of the transmission shows how many lines the service is
  // inserting on. Taking it from the transmission itself means a recording
  // using one line per field is not accused of losing half its packets.
  int slots_per_field = 0;
  for (auto it = begin; it != end; ++it) {
    slots_per_field = std::max(slots_per_field, it->second);
  }
  if (slots_per_field == 0) {
    return 0;
  }

  // Fields that yielded nothing at all are presumed not to carry teletext —
  // plenty of services insert into one field of each frame only, and calling
  // the other one a total loss would put a fault on every page. Only fields
  // that did yield something can be short of their slots. This under-reports
  // a field where every line was lost, which is the right way to be wrong:
  // the point of the count is to stop claiming faults that are not there.
  int64_t received = 0;
  int64_t carrying_fields = 0;
  for (auto it = begin; it != end; ++it) {
    if (it->second == 0) {
      continue;
    }
    ++carrying_fields;
    received += it->second;
  }
  const int64_t expected = slots_per_field * carrying_fields;
  return static_cast<int>(std::max<int64_t>(0, expected - received));
}

void TeletextPageAssembler::mergeSnapshot(
    const orc::TeletextPageSnapshot& snapshot) const {
  const std::pair<int, int> address{snapshot.magazine, snapshot.page_number};
  const uint64_t seen_frame =
      static_cast<uint64_t>(snapshot.header_field_index) / 2;

  // The presenter turns packets into a page; only the assembler knows which
  // VBI slots the transmission's fields gave up, so it stamps that on here.
  const auto make_page = [this](const orc::TeletextPageSnapshot& from) {
    auto page =
        orc::presenters::TeletextObservationPresenter::makePageView(from);
    page.recovery.lost_packets =
        lostPacketsBetween(from.header_field_index, from.last_field_index);
    return page;
  };

  auto it = catalogue_.find(address);
  if (it == catalogue_.end()) {
    if (catalogue_.size() >= kMaxCataloguedPages) {
      evictLeastRecentlySeen(catalogue_);
    }
    it = catalogue_.emplace(address, CataloguedPage{}).first;
    it->second.magazine = snapshot.magazine;
    it->second.page_number = snapshot.page_number;
    it->second.seen_frame = seen_frame;
    it->second.page = make_page(snapshot);
    // A page first met part-way through its transmission has still appeared
    // once; counting it only when it finishes would leave the list showing
    // zero for a page plainly on screen.
    it->second.times_seen = 1;
    it->second.counted_header_field = snapshot.header_field_index;
    ++catalogue_revision_;
    return;
  }

  // One appearance of a page yields several snapshots: fragments as its own
  // header is re-sent or as a reader peeks at it arriving, then the finished
  // page. They share a header field index, so counting only headers past the
  // newest one counted turns them into the single appearance they are.
  const bool new_appearance =
      snapshot.header_field_index > it->second.counted_header_field;
  const bool completeness_changed =
      it->second.page.transmission_complete != snapshot.transmission_complete;
  if (new_appearance) {
    it->second.counted_header_field = snapshot.header_field_index;
    it->second.seen_frame = seen_frame;
    ++it->second.times_seen;
    ++catalogue_revision_;
  } else if (it->second.page.last_field_index == snapshot.last_field_index &&
             !completeness_changed) {
    return;  // nothing new since the snapshot already held
  } else if (completeness_changed) {
    // The rows filling in are the page's own business, but a transmission
    // starting or finishing changes what the page list should say about it.
    ++catalogue_revision_;
  }

  // The newest decode wins outright. It is not a fragment even when this
  // transmission was clipped: the decoder renders from squasher_, which holds
  // every row copy seen since the last discontinuity, so rows this
  // transmission did not carry come from the ones that did.
  it->second.page = make_page(snapshot);
}

void TeletextPageAssembler::refreshCatalogue() const {
  if (!catalogue_dirty_) {
    return;
  }
  catalogue_dirty_ = false;

  // Feed every frame from the frontier that has arrived, stopping at the
  // first gap: the decoder requires monotonically non-decreasing field
  // indices, so a frame still in flight has to be waited for rather than
  // skipped. Completed pages reach the catalogue through the page callback.
  for (auto it = frames_.find(decode_frontier_);
       it != frames_.end() && it->first == decode_frontier_;
       it = frames_.find(decode_frontier_)) {
    FrameData& data = it->second;
    for (auto* field : {&data.field1, &data.field2}) {
      const int64_t field_index =
          static_cast<int64_t>(it->first) * 2 + (field == &data.field2 ? 1 : 0);
      for (const auto& packet : field->packets) {
        // The (field, line) origin is this copy's identity, so a frame that
        // is somehow decoded twice re-seats the same copy rather than letting
        // it outvote the rest.
        const int64_t source = field_index * kFieldLineStride +
                               (packet.field_line % kFieldLineStride);
        decoder_->process_packet(packet.bytes, field_index, source);
      }
      // Recorded for every decoded field, empty ones included: an empty field
      // in the middle of a page transmission is exactly what lost packets
      // look like (lostPacketsBetween()).
      field_packet_counts_[field_index] =
          static_cast<int>(field->packets.size());
      releasePackets(*field);
    }
    ++decode_frontier_;
    // The frame has been decoded and can never contribute again; the cache is
    // for frames still waiting their turn, so dropping it here is what keeps
    // memory flat over an unbounded forward run.
    frames_.erase(it);
  }
  while (field_packet_counts_.size() > kMaxFieldCountHistory) {
    field_packet_counts_.erase(field_packet_counts_.begin());
  }

  // Pages whose transmission is still in progress have not reached the
  // callback yet, and on a medium this sparse a page takes several frames to
  // arrive; showing it filling in is the honest answer, so long as it says
  // that is what it is doing (transmission_complete).
  for (const auto& snapshot : decoder_->open_page_snapshots()) {
    mergeSnapshot(snapshot);
  }
}
