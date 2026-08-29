/*
 * File:        eia608_service_demux.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     EIA-608 data-channel / caption-vs-text service demultiplexer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_EIA608_SERVICE_DEMUX_H
#define ORC_EIA608_SERVICE_DEMUX_H

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace orc {

/**
 * @brief One of the eight EIA-608 services multiplexed onto line 21
 *
 * Line 21 of each field carries a single byte-pair stream shared by four
 * services [CTA-608-E §6]. Field 1 carries CC1, CC2, T1 and T2; field 2
 * carries CC3, CC4, T3 and T4. A stream read without demultiplexing
 * interleaves whichever of them the recording used — captions spliced into
 * the middle of a text-service page and vice versa.
 */
enum class EIA608Service : uint8_t {
  CC1,  ///< Field 1, data channel 1, captioning — the primary caption service
  CC2,  ///< Field 1, data channel 2, captioning
  T1,   ///< Field 1, data channel 1, text service
  T2,   ///< Field 1, data channel 2, text service
  CC3,  ///< Field 2, data channel 1, captioning
  CC4,  ///< Field 2, data channel 2, captioning
  T3,   ///< Field 2, data channel 1, text service
  T4,   ///< Field 2, data channel 2, text service
};

/// Canonical short name ("CC1", "T2", ...)
const char* eia608_service_name(EIA608Service service);

/// Parse a service name. Accepts the canonical short names and the "TEXT1"
/// spelling the parameter surfaces use for T1-T4. Case-sensitive.
std::optional<EIA608Service> eia608_service_from_name(const std::string& name);

/// Field the service is carried on: 0 for the first field of the frame
/// (CC1/CC2/T1/T2), 1 for the second (CC3/CC4/T3/T4).
int eia608_service_field(EIA608Service service);

/// True for the text services (T1-T4), which carry a rolling page of text
/// rather than captions timed to the picture.
bool eia608_service_is_text(EIA608Service service);

/**
 * @brief Routes the line 21 byte-pair stream to a single service
 *
 * Place in front of an EIA608Decoder (or an SCC writer) and pass on only the
 * pairs accept() returns true for.
 *
 * The mux has two levels [CTA-608-E §6.2, §7.3]:
 *
 * - **Data channel.** Bit 3 of the first byte of every *control* pair selects
 *   the channel: 0x10-0x17 is channel 1, 0x18-0x1F is channel 2. Character
 *   pairs carry no channel of their own and belong to the channel of the last
 *   control pair seen in that field.
 * - **Captioning vs. text.** Within a channel, RCL (0x20), RU2/RU3/RU4
 *   (0x25-0x27) and RDC (0x29) select the caption service; TR (0x2A) and RTD
 *   (0x2B) select the text service. The selection persists until the next such
 *   code, so it too applies to the character pairs that follow.
 *
 * Both fields are tracked independently, as the standard requires, so one
 * instance can be fed a whole frame's worth of pairs.
 *
 * Optionally also suppresses the duplicate transmission of control codes: an
 * encoder sends every control pair twice in succession so that a pair lost to
 * noise still arrives, and a receiver must act on only the first of two
 * identical consecutive ones [CTA-608-E §7.3]. This is applied *after*
 * routing, because the duplicate follows immediately within its own service —
 * pairs belonging to the other services of the field interleave between them.
 * Leave it off when capturing the stream verbatim (an SCC file records what
 * was transmitted, and its consumers de-duplicate for themselves).
 */
class EIA608ServiceDemux {
 public:
  explicit EIA608ServiceDemux(EIA608Service target = EIA608Service::CC1,
                              bool suppress_repeated_controls = true);

  /**
   * @brief Route one byte pair
   *
   * @param field_index 0 for the first field of the frame, 1 for the second
   * @param byte1       First byte, parity bit already stripped (7-bit)
   * @param byte2       Second byte, parity bit already stripped (7-bit)
   * @return True when the pair belongs to the target service and should be
   *         passed on. Routing state is updated either way, so every pair of
   *         the stream must be offered, including those that are rejected.
   */
  bool accept(int field_index, uint8_t byte1, uint8_t byte2);

  /// Service the pair last offered to accept() belongs to, or nullopt for a
  /// pair that belongs to none (null padding, or an XDS packet byte).
  std::optional<EIA608Service> last_service() const { return last_service_; }

  /// Forget the channel/mode selection and the duplicate-control memory. Use
  /// when the stream is picked up somewhere other than where it was left.
  void reset();

  EIA608Service target() const { return target_; }

 private:
  /// Channel and captioning/text selection for one field
  struct FieldState {
    /// 0 = data channel 1, 1 = data channel 2
    int channel = 0;
    /// Text service selected, indexed by channel
    bool text_mode[2] = {false, false};
  };

  EIA608Service target_;
  bool suppress_repeated_controls_;
  FieldState fields_[2];
  std::optional<EIA608Service> last_service_;

  // Previous accepted pair, for duplicate-control suppression.
  bool have_previous_ = false;
  uint8_t previous_byte1_ = 0;
  uint8_t previous_byte2_ = 0;
};

}  // namespace orc

#endif  // ORC_EIA608_SERVICE_DEMUX_H
