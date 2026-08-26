/*
 * File:        vbi_transport.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Raw-file and FLAC-wrapped byte transports for VBI captures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_TRANSPORT_H
#define ORC_VBI_TRANSPORT_H

#include <cstdint>
#include <memory>
#include <string>

#include "vbi_byte_source.h"

namespace orc {

// The four magic bytes at offset 0 of a FLAC stream.
extern const char kFlacStreamMarker[5];

// Does the buffer start with the FLAC stream marker?  This is step 1 of the
// format-detection ladder (design §8.1) and the only thing that decides
// whether a capture needs unwrapping.
bool has_flac_stream_marker(const uint8_t* buffer, size_t length);

// Open a capture for reading, transparently unwrapping FLAC.
//
// FLAC is used on these captures as a generic lossless byte compressor, so
// the decoded byte sequence is identical to the raw file and every consumer
// above IVBIByteSource sees the same thing either way (design §3.3).
// Returns nullptr with an error message when the file cannot be opened or
// its transport is not supported.
std::unique_ptr<IVBIByteSource> open_vbi_byte_source(
    const std::string& path, std::string& error_message);

}  // namespace orc

#endif  // ORC_VBI_TRANSPORT_H
