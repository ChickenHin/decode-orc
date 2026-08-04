/*
 * File:        vbi_line_reader.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Turns a raw VBI byte stream into indexed line records
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_LINE_READER_H
#define ORC_VBI_LINE_READER_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vbi_byte_source.h"
#include "vbi_source_format.h"

namespace orc {

// One stored line record that carries the data service, with the index tuple
// and source byte range it came from.
//
// The byte range is what closes the provenance loop (design §1.1): because
// the record index is preserved exactly as stored and records are never
// dropped or reordered, a packet recovered downstream resolves back to these
// bytes as a pure index relation.
struct VBILineRecord {
  uint64_t frame_index = 0;   // 0-based stored frame in the capture
  uint32_t field_index = 0;   // 0 = first stored field, 1 = second
  uint32_t record_index = 0;  // 0-based record within the stored field

  uint64_t source_byte_offset = 0;  // offset of sample 0 in the raw stream
  uint64_t source_byte_length = 0;  // valid samples only, padding excluded

  // Raw sample values, padding excluded.  Held as doubles because every
  // consumer above the reader (level estimation, correlation, resampling)
  // works in the continuous domain; no scaling or conditioning is applied
  // here.
  std::vector<double> samples;
};

// Every data-service record of one stored frame, in stored order: the first
// stored field's records followed by the second's.
struct VBIFrameRecords {
  uint64_t frame_index = 0;

  // The bt8x8 driver's per-frame sequence number, when the format declares a
  // frame-counter trailer.  Interpreted little-endian; the kernel writes
  // machine endianness and every platform these captures come from is
  // little-endian (design §3.3).
  std::optional<uint32_t> frame_counter;

  std::vector<VBILineRecord> lines;
};

// Reads fixed-stride line records out of a raw VBI byte stream.
//
// The reader is generic over the container descriptor and holds no format
// knowledge of its own: record stride, padding exclusion, field selection and
// the frame trailer all come from VBISourceFormat.  It performs no signal
// processing whatsoever.
//
// Not thread-safe: it borrows the byte source, which holds a stream position.
class VBILineReader {
 public:
  // The referenced byte source must outlive the reader.
  VBILineReader(VBISourceFormat format, IVBIByteSource& byte_source);

  const VBISourceFormat& format() const { return format_; }

  uint64_t bytes_per_frame() const { return format_.bytes_per_frame(); }

  // Whole stored frames available, or nullopt when the transport cannot
  // report its size without decoding everything.
  std::optional<uint64_t> frame_count() const;

  // True when the stream ends part-way through a frame.  This is always a
  // configuration error rather than something to truncate silently; it is
  // reported as such by validate_vbi_source_config().
  bool has_partial_trailing_frame() const;

  // Read every data-service record of one stored frame.  Returns false with
  // an error message on a short read (a truncated final frame) or a sample
  // format whose decode path is not implemented.
  bool read_frame(uint64_t frame_index, VBIFrameRecords& out_records,
                  std::string& error_message) const;

  // Read one stored frame's counter without decoding its records.
  //
  // Frame-drop detection needs the counter of a frame and nothing else, and
  // asking for the samples as well would decode 64 KiB to read four bytes.
  // out_counter is empty when the format declares no counter trailer, which
  // is not an error: it is the answer that the source cannot report drops
  // (design §6.3).
  bool read_frame_counter(uint64_t frame_index,
                          std::optional<uint32_t>& out_counter,
                          std::string& error_message) const;

 private:
  VBISourceFormat format_;
  IVBIByteSource* byte_source_;
};

// Decode one record's worth of sample words into raw sample values.
// record_bytes must hold at least sample_count words of the given format.
// Returns false with an error message for sample formats that have no decode
// path yet.
bool decode_vbi_samples(VBISampleFormat sample_format,
                        const uint8_t* record_bytes, uint32_t sample_count,
                        std::vector<double>& out_samples,
                        std::string& error_message);

}  // namespace orc

#endif  // ORC_VBI_LINE_READER_H
