/*
 * File:        nabts_test_builders.h
 * Module:      orc-tests/core/unit/stages/nabts_sink
 * Purpose:     Build well-formed NABTS packets, data groups and record headers
 *              for the tests to take apart again
 *
 * Everything here encodes rather than decodes: the tests assert that the
 * decoders read back what the standard says these bytes mean, so a builder that
 * shared code with its decoder would prove nothing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "nabts_packet.h"
#include "nabts_record.h"
#include "vbi-services/teletext_slicer.h"

namespace orc_unit_test {
namespace nabts {

/// One Hamming 8/4 protected byte carrying |nibble| (CEA-516 §3.2.2). The
/// nibble's most significant bit is b8, which is the convention the whole
/// standard uses — see nabts_packet.cpp.
inline uint8_t hamming(uint8_t nibble) {
  return orc::teletext_hamming84_encode(nibble);
}

/// |value| with odd parity in b8, which CEA-516 §3.3 requires of every data
/// byte of a type-zero data group. Only the low seven bits of |value| are used.
inline uint8_t parity(uint8_t value) {
  uint8_t byte = static_cast<uint8_t>(value & 0x7F);
  int ones = 0;
  for (int bit = 0; bit < 7; ++bit) {
    ones += (byte >> bit) & 0x1;
  }
  if ((ones % 2) == 0) {
    byte = static_cast<uint8_t>(byte | 0x80);  // b8 makes the count odd
  }
  return byte;
}

/// A packet structure byte (CEA-516 §3.2.5): b2 synchronizing, b4 not full,
/// b8/b6 the suffix code.
inline uint8_t packet_structure(bool synchronizing, bool not_full,
                                orc::NabtsSuffixKind suffix) {
  uint8_t code = 0;
  switch (suffix) {
    case orc::NabtsSuffixKind::kNone:
      code = 0;
      break;
    case orc::NabtsSuffixKind::kLongitudinal:
      code = 1;
      break;
    case orc::NabtsSuffixKind::kLongitudinalPlusReserved:
      code = 2;
      break;
    case orc::NabtsSuffixKind::kBundle:
      code = 3;
      break;
  }
  // b8 is the more significant of the two suffix bits, and the nibble's most
  // significant bit; b2 is its least.
  const uint8_t b8 = static_cast<uint8_t>((code >> 1) & 0x1);
  const uint8_t b6 = static_cast<uint8_t>(code & 0x1);
  return static_cast<uint8_t>((b8 << 3) | (b6 << 2) |
                              (static_cast<uint8_t>(not_full) << 1) |
                              static_cast<uint8_t>(synchronizing));
}

/// One whole 33-byte data packet.
///
/// |data| is the data block as it should read after decoding, padded with
/// odd-parity nulls to the length |suffix| leaves for it and truncated if
/// longer. The suffix bytes are computed so the longitudinal check passes
/// (§3.4), which is what makes a deliberately corrupted copy of one of these a
/// meaningful test.
inline std::vector<uint8_t> make_packet(uint16_t channel, uint8_t continuity,
                                        bool synchronizing,
                                        orc::NabtsSuffixKind suffix,
                                        const std::vector<uint8_t>& data,
                                        bool not_full = false) {
  std::vector<uint8_t> packet(orc::kNabtsPacketBytes, 0);

  // §3.2.1: P1 P2 P3 CI PS, P1 the most significant address digit.
  packet[0] = hamming(static_cast<uint8_t>((channel >> 8) & 0xF));
  packet[1] = hamming(static_cast<uint8_t>((channel >> 4) & 0xF));
  packet[2] = hamming(static_cast<uint8_t>(channel & 0xF));
  packet[3] = hamming(static_cast<uint8_t>(continuity & 0xF));
  packet[4] = hamming(packet_structure(synchronizing, not_full, suffix));

  const size_t block = orc::nabts_data_block_bytes(suffix);
  for (size_t i = 0; i < block; ++i) {
    packet[orc::kNabtsPrefixBytes + i] =
        i < data.size() ? data[i] : parity(0x00);
  }

  // §3.4: the longitudinal byte makes the exclusive-or of the data block and
  // the whole suffix 0xFF. It is the last byte of the packet in both the
  // one-byte and the two-byte case.
  if (suffix == orc::NabtsSuffixKind::kLongitudinal ||
      suffix == orc::NabtsSuffixKind::kLongitudinalPlusReserved) {
    if (suffix == orc::NabtsSuffixKind::kLongitudinalPlusReserved) {
      // The byte before the longitudinal one is "subject to further study" and
      // is covered by the check without being read from.
      packet[orc::kNabtsPacketBytes - 2] = parity(0x55);
    }
    uint8_t longitudinal = 0xFF;
    for (size_t i = orc::kNabtsPrefixBytes; i < orc::kNabtsPacketBytes - 1;
         ++i) {
      longitudinal = static_cast<uint8_t>(longitudinal ^ packet[i]);
    }
    packet[orc::kNabtsPacketBytes - 1] = longitudinal;
  }

  // A bundle packet is all suffix (§3.4) and its protection method is reserved,
  // so its bytes carry only the odd parity §3.4 requires of them.
  if (suffix == orc::NabtsSuffixKind::kBundle) {
    for (size_t i = orc::kNabtsPrefixBytes; i < orc::kNabtsPacketBytes; ++i) {
      packet[i] = parity(static_cast<uint8_t>(i));
    }
  }

  return packet;
}

/// The eight Hamming bytes of a data group header (CEA-516 §4.2.1).
inline std::vector<uint8_t> make_group_header(
    uint8_t type, uint16_t further_blocks, uint16_t final_block_bytes,
    uint8_t continuity = 0, uint8_t repetition = 0, uint8_t routing = 0) {
  return {
      hamming(type),
      hamming(continuity),
      hamming(repetition),
      // §4.2.5 and §4.2.6 concatenate each pair, the first the more
      // significant.
      hamming(static_cast<uint8_t>((further_blocks >> 4) & 0xF)),
      hamming(static_cast<uint8_t>(further_blocks & 0xF)),
      hamming(static_cast<uint8_t>((final_block_bytes >> 4) & 0xF)),
      hamming(static_cast<uint8_t>(final_block_bytes & 0xF)),
      hamming(routing),
  };
}

/// A record header designator (CEA-516 §5.2.3).
inline uint8_t record_designator(bool address_extension, bool link,
                                 bool classification, bool header_extension) {
  return static_cast<uint8_t>((static_cast<uint8_t>(header_extension) << 3) |
                              (static_cast<uint8_t>(classification) << 2) |
                              (static_cast<uint8_t>(link) << 1) |
                              static_cast<uint8_t>(address_extension));
}

/// A record link pair L1, L2 (CEA-516 §5.2.6).
inline std::vector<uint8_t> make_link(uint8_t order, bool more) {
  // L1 b8 says whether more follow; the seven order bits are L1 b6 b4 b2 then
  // L2 b8 b6 b4 b2, L1 the more significant.
  const uint8_t l1 = static_cast<uint8_t>((static_cast<uint8_t>(more) << 3) |
                                          ((order >> 4) & 0x7));
  const uint8_t l2 = static_cast<uint8_t>(order & 0xF);
  return {hamming(l1), hamming(l2)};
}

/// A classification-sequence flag nibble from its four flags, b8 first.
inline uint8_t flags(bool b8, bool b6, bool b4, bool b2) {
  return static_cast<uint8_t>(
      (static_cast<uint8_t>(b8) << 3) | (static_cast<uint8_t>(b6) << 2) |
      (static_cast<uint8_t>(b4) << 1) | static_cast<uint8_t>(b2));
}

/// A classification sequence carrying the first group's six flag bytes
/// (CEA-516 §5.2.7.1): one pointer byte announcing all three pairs, then
/// Y11 … Y16.
inline std::vector<uint8_t> make_classification(uint8_t y11, uint8_t y12,
                                                uint8_t y13, uint8_t y14,
                                                uint8_t y15, uint8_t y16) {
  // Pointer byte Y01: b2 gives YN1/YN2, b4 gives YN3/YN4, b6 gives YN5/YN6,
  // and b8 clear ends the sequence.
  const uint8_t pointer = flags(/*b8=*/false, /*b6=*/true, /*b4=*/true,
                                /*b2=*/true);
  return {hamming(pointer), hamming(y11), hamming(y12), hamming(y13),
          hamming(y14),     hamming(y15), hamming(y16)};
}

/// A header extension field (CEA-516 §5.2.8): EI, ES, then |data| nibbles.
inline std::vector<uint8_t> make_header_extension(
    uint8_t meaning, std::initializer_list<uint8_t> data, bool more = false) {
  // §5.2.8.2: b6 b4 b2 carry the meaning, b6 the most significant, and b8 says
  // whether another field follows.
  std::vector<uint8_t> out{
      hamming(static_cast<uint8_t>((static_cast<uint8_t>(more) << 3) |
                                   ((meaning & 0x7) << 0))),
      hamming(static_cast<uint8_t>(data.size() & 0xF)),
  };
  for (const uint8_t nibble : data) {
    out.push_back(hamming(nibble));
  }
  return out;
}

/// A short record address A1 A2 A3 (CEA-516 §5.2.4) from three hex digits.
inline std::vector<uint8_t> make_short_address(uint16_t address) {
  return {hamming(static_cast<uint8_t>((address >> 8) & 0xF)),
          hamming(static_cast<uint8_t>((address >> 4) & 0xF)),
          hamming(static_cast<uint8_t>(address & 0xF))};
}

/// A long record address A1 … A9 (CEA-516 §5.2.5) from nine hex digits, the
/// first the most significant.
inline std::vector<uint8_t> make_long_address(uint64_t address) {
  std::vector<uint8_t> out;
  out.reserve(orc::kNabtsLongAddressNibbles);
  for (int shift = 32; shift >= 0; shift -= 4) {
    out.push_back(hamming(static_cast<uint8_t>((address >> shift) & 0xF)));
  }
  return out;
}

/// Append |tail| to |head|.
inline std::vector<uint8_t>& operator+=(std::vector<uint8_t>& head,
                                        const std::vector<uint8_t>& tail) {
  head.insert(head.end(), tail.begin(), tail.end());
  return head;
}

/// Odd-parity data bytes from a run of seven-bit values.
inline std::vector<uint8_t> parity_bytes(
    std::initializer_list<uint8_t> values) {
  std::vector<uint8_t> out;
  out.reserve(values.size());
  for (const uint8_t value : values) {
    out.push_back(parity(value));
  }
  return out;
}

}  // namespace nabts
}  // namespace orc_unit_test
