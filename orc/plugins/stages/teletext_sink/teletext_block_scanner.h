/*
 * File:        teletext_block_scanner.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Block-parallel frame scanning for VBI teletext recovery
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_BLOCK_SCANNER_H
#define ORC_TELETEXT_BLOCK_SCANNER_H

#include <orc/stage/common_types.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "teletext_frame_slicer.h"
#include "teletext_scan_state.h"

namespace orc {

// Fields per frame. Named because it is the stride of the block buffer as well
// as the loop bound, and the two must not drift apart.
constexpr size_t kFieldsPerFrame = 2;

// Frames sliced per block, first and last. Blocks exist because the pass
// learns as it goes (see TeletextScanSnapshot): every frame of a block is
// sliced against one frozen view of what is known, and the block is then
// emitted in order, which is what advances it.
//
// So the size trades learning latency against everything else. Early blocks
// are small because that is when there is most to learn — a recording's data
// phase is pinned within the first few dozen frames rather than the first few
// hundred — and they grow because a large block amortises the per-block
// thread creation and leaves the workers less often idle at its tail. The
// ceiling is a memory bound: a block holds every line it sliced, at about
// 1,6 kB each, so 128 frames of a 625-line window is roughly 7 MB.
constexpr uint64_t kFirstScanBlockFrames = 16;
constexpr uint64_t kMaxScanBlockFrames = 128;

// Threads to slice with: |requested|, or one per hardware thread when that is
// 0. Never more than there are frames to slice, and never fewer than one —
// which runs everything on the calling thread and starts none.
size_t resolve_worker_count(uint64_t frame_count, int32_t requested);

/**
 * @brief Slice one block of frames, in parallel
 *
 * Recovering a line reads that line and nothing else, so the fields of a block
 * are independent and are handed out to |worker_count| threads as they come
 * free. VideoFrameRepresentation's const accessors are documented safe to call
 * concurrently, which is what lets them all read one representation.
 *
 * |out| is indexed by frame offset within the block times kFieldsPerFrame plus
 * the field index, so the caller can walk it back in the strict temporal order
 * emission needs. Each frame owns its own elements, so no two threads touch
 * the same one and no locking is needed on the hot path.
 *
 * Throws if a worker did, once they have all been joined — an exception
 * escaping a std::thread would otherwise take the process with it.
 */
void slice_block(const VideoFrameRepresentation& representation,
                 const TeletextFrameSlicer& slicer, FrameID first_frame,
                 uint64_t first_frame_index, uint64_t frame_count,
                 const TeletextScanSnapshot& snapshot, size_t worker_count,
                 const std::atomic<bool>* cancel_requested,
                 std::vector<TeletextFieldScan>& out);

}  // namespace orc

#endif  // ORC_TELETEXT_BLOCK_SCANNER_H
