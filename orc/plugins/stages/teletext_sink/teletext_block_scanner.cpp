/*
 * File:        teletext_block_scanner.cpp
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Block-parallel frame scanning implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_block_scanner.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace orc {

size_t resolve_worker_count(uint64_t frame_count, int32_t requested) {
  uint64_t wanted = static_cast<uint64_t>(std::max(requested, 0));
  if (wanted == 0) {
    unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) {
      hardware = 4;  // hardware_concurrency() is allowed to say it cannot tell
    }
    wanted = hardware;
  }
  const uint64_t capped = std::min(wanted, frame_count);
  return static_cast<size_t>(std::max<uint64_t>(1, capped));
}

void slice_block(const VideoFrameRepresentation& representation,
                 const TeletextFrameSlicer& slicer, FrameID first_frame,
                 uint64_t first_frame_index, uint64_t frame_count,
                 const TeletextScanSnapshot& snapshot, size_t worker_count,
                 const std::atomic<bool>* cancel_requested,
                 std::vector<TeletextFieldScan>& out) {
  std::atomic<uint64_t> next_frame{0};
  std::mutex failure_mutex;
  std::string failure;

  const auto run = [&] {
    try {
      for (;;) {
        const uint64_t offset =
            next_frame.fetch_add(1, std::memory_order_relaxed);
        if (offset >= frame_count) {
          return;
        }
        if (cancel_requested != nullptr && cancel_requested->load()) {
          return;  // The caller discards the whole block, so stop taking work.
        }
        // A whole frame at a time rather than a field: both fields read the
        // same frame from the source, so taking them together turns what would
        // be two fetches on two threads into one fetch and a cache hit.
        const FrameID frame_id = first_frame + static_cast<FrameID>(offset);
        const size_t base = static_cast<size_t>(offset) * kFieldsPerFrame;
        for (size_t field_idx = 0; field_idx < kFieldsPerFrame; ++field_idx) {
          slicer.slice_field(representation, frame_id, field_idx,
                             first_frame_index + offset, snapshot,
                             out[base + field_idx]);
        }
      }
    } catch (const std::exception& e) {
      const std::lock_guard<std::mutex> lock(failure_mutex);
      if (failure.empty()) {
        failure = e.what();
      }
      // Drain the queue so the other workers stop rather than finish a block
      // whose result is about to be thrown away.
      next_frame.store(frame_count, std::memory_order_relaxed);
    }
  };

  if (worker_count <= 1) {
    run();
  } else {
    // One fewer thread than workers: this one takes a share too, which saves a
    // thread creation per block and keeps a single-core machine honest.
    std::vector<std::thread> threads;
    threads.reserve(worker_count - 1);
    for (size_t i = 0; i + 1 < worker_count; ++i) {
      threads.emplace_back(run);
    }
    run();
    for (auto& thread : threads) {
      thread.join();
    }
  }

  if (!failure.empty()) {
    throw std::runtime_error(failure);
  }
}

}  // namespace orc
