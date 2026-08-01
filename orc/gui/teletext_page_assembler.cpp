/*
 * File:        teletext_page_assembler.cpp
 * Module:      orc-gui
 * Purpose:     Trailing-frame-window cache and Level 1 page assembly for the
 *              teletext preview dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_page_assembler.h"

#include <orc/support/teletext_page_decoder.h>

#include "teletext_observation_presenter.h"

void TeletextPageAssembler::setCurrentFrame(uint64_t frame_index) {
  current_frame_ = frame_index;
  const uint64_t start = windowStartFrame();
  frames_.erase(frames_.begin(), frames_.lower_bound(start));
  frames_.erase(frames_.upper_bound(current_frame_), frames_.end());
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
}

bool TeletextPageAssembler::hasFrame(uint64_t frame_index) const {
  return frames_.find(frame_index) != frames_.end();
}

void TeletextPageAssembler::clear() { frames_.clear(); }

std::optional<orc::presenters::TeletextPageView>
TeletextPageAssembler::assemblePage(int magazine, int page_number) const {
  orc::TeletextPageDecoder decoder;
  std::optional<orc::TeletextPageSnapshot> latest;
  decoder.set_page_callback(
      [&latest, magazine, page_number](const orc::TeletextPageSnapshot& page) {
        if (page.magazine == magazine && page.page_number == page_number) {
          latest = page;
        }
      });

  int64_t last_field_index = 0;
  for (const auto& [frame_index, data] : frames_) {
    for (const auto* field : {&data.field1, &data.field2}) {
      const int64_t field_index = static_cast<int64_t>(frame_index) * 2 +
                                  (field == &data.field2 ? 1 : 0);
      for (const auto& packet : field->packets) {
        decoder.process_packet(packet.bytes, field_index);
        last_field_index = field_index;
      }
    }
  }
  decoder.finalize(last_field_index + 1);

  if (!latest) {
    return std::nullopt;
  }
  return orc::presenters::TeletextObservationPresenter::makePageView(*latest);
}
