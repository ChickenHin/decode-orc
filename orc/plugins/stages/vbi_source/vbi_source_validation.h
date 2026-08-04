/*
 * File:        vbi_source_validation.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Fail-fast configuration checks for raw VBI captures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_SOURCE_VALIDATION_H
#define ORC_VBI_SOURCE_VALIDATION_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vbi_source_format.h"

namespace orc {

// What the transport declares about the stream, as opposed to what the
// container descriptor configures.
//
// Only bits_per_sample is usable.  A FLAC wrapper's sample rate is a
// conventional placeholder (48000) and carries no VBI timing information at
// all, so it is recorded here purely so that diagnostics can say it was
// ignored on purpose (design §3.3).
struct VBITransportHints {
  std::optional<uint32_t> bits_per_sample;
  std::optional<uint32_t> declared_sample_rate_hz;
};

// Validate a container descriptor, and the stream it is about to be applied
// to, against everything that can be checked without signal processing.
//
// Returns one message per violation, each naming the offending parameter, so
// a wrong configuration is reported in full rather than one field at a time.
// An empty result means the configuration is consistent.
//
// decoded_stream_bytes is the decoded length of the capture after any
// transport unwrapping, or nullopt when the transport cannot report it.
std::vector<std::string> validate_vbi_source_config(
    const VBISourceFormat& format, std::optional<uint64_t> decoded_stream_bytes,
    const VBITransportHints& transport_hints = VBITransportHints{});

}  // namespace orc

#endif  // ORC_VBI_SOURCE_VALIDATION_H
