/*
 * File:        vbi_identity_attestation.h
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     Separates the page and record identities a service really
 *              transmitted from the ones Hamming 8/4 mis-correction invented
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_SERVICES_VBI_IDENTITY_ATTESTATION_H
#define ORC_VBI_SERVICES_VBI_IDENTITY_ATTESTATION_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace orc {

/**
 * @brief A service identity written out as fixed-width hexadecimal digits
 *
 * The digits a receiver was actually sent, one entry apiece and in transmission
 * order: for World System Teletext the magazine, the two page-number digits and
 * the four sub-code digits (ETSI EN 300 706 §9.3.1.1, §9.3.1.2); for NABTS the
 * three packet-address digits, the nine record-address digits and the version
 * (CEA-516 §3.2.3, §5.2.5, §5.2.7.2).
 *
 * Digits rather than a packed number because that is the granularity the damage
 * has: every one of them is carried by its own Hamming 8/4 byte, so a burst
 * that defeats the code moves exactly one of them and leaves the rest as
 * transmitted.
 */
using VbiIdentityDigits = std::vector<uint8_t>;

/**
 * @brief What reconciling a catalogue's identities came to
 *
 * Reported rather than kept quiet: the pass removes entries a reader would
 * otherwise have browsed, and a count that appears in the run's log and report
 * is the difference between a decoder that pruned a damaged catalogue and one
 * that lost pages.
 */
struct VbiIdentityReconciliation {
  /// Identities the catalogue held before the pass.
  uint32_t identities_seen = 0;
  /// Of those, ones no transmission ever named as transmitted.
  uint32_t identities_unattested = 0;
  /// Unattested identities folded into the attested one they were a
  /// single-digit misreading of.
  uint32_t identities_folded = 0;
  /// Unattested identities removed with nowhere to fold them.
  uint32_t identities_dropped = 0;
  /// Appearances carried over by the folds, and appearances removed with the
  /// identities that were dropped.
  uint64_t appearances_folded = 0;
  uint64_t appearances_dropped = 0;
  /// True when the pass stood aside because nothing in the catalogue was
  /// attested — see vbi_identity_reconciliation_applies().
  bool withheld = false;

  /// Whether the pass changed anything.
  bool acted() const { return identities_folded + identities_dropped > 0; }

  /// Human-readable summary for a stage report. Empty where nothing was found.
  std::string summary(const std::string& noun) const;
};

/**
 * @brief Whether a catalogue holds the evidence the reconciliation needs
 *
 * The rule says an identity no transmission ever named as transmitted is not an
 * identity. That reading is only available where *some* identity was named as
 * transmitted: a recording so damaged, or so short, that nothing at all arrived
 * uncorrected offers no baseline to judge the rest against, and the honest
 * answer there is to leave the catalogue as it stands rather than to empty it.
 *
 * @param attested_identities Identities with at least one attested appearance
 */
inline bool vbi_identity_reconciliation_applies(
    std::size_t attested_identities) {
  return attested_identities > 0;
}

/**
 * @brief The one attested identity @p candidate is a single-digit misreading of
 *
 * @param candidate Digits of the unattested identity
 * @param attested  Digits of every attested identity, all the same width
 * @return Index into @p attested, or std::nullopt
 *
 * Exactly one digit different, because that is the shape of the damage: the
 * digit sits in its own Hamming 8/4 byte and a burst long enough to defeat the
 * code carries that byte onto a neighbouring codeword without touching any
 * other. Two digits apart is two independent bursts inside one header, which is
 * possible but is no longer evidence of anything in particular.
 *
 * A candidate with several such neighbours is left alone. Real services number
 * their pages densely — a magazine holding both 003 and 007 is ordinary — so a
 * misreading that could have come from either says nothing about which, and
 * folding it into whichever came first in the map would be arbitrary.
 *
 * Linear in the number of attested identities, which is the size of the service
 * rather than the length of the recording.
 */
std::optional<std::size_t> vbi_single_digit_neighbour(
    const VbiIdentityDigits& candidate,
    const std::vector<VbiIdentityDigits>& attested);

}  // namespace orc

#endif  // ORC_VBI_SERVICES_VBI_IDENTITY_ATTESTATION_H
