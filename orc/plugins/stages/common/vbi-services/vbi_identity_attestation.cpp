/*
 * File:        vbi_identity_attestation.cpp
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     Identity attestation reconciliation implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_identity_attestation.h"

#include <spdlog/fmt/fmt.h>

namespace orc {

std::optional<std::size_t> vbi_single_digit_neighbour(
    const VbiIdentityDigits& candidate,
    const std::vector<VbiIdentityDigits>& attested) {
  std::optional<std::size_t> found;
  for (std::size_t index = 0; index < attested.size(); ++index) {
    const VbiIdentityDigits& other = attested[index];
    if (other.size() != candidate.size()) {
      continue;
    }
    std::size_t differences = 0;
    for (std::size_t digit = 0; digit < candidate.size(); ++digit) {
      if (candidate[digit] != other[digit] && ++differences > 1) {
        break;
      }
    }
    if (differences != 1) {
      continue;
    }
    if (found.has_value()) {
      return std::nullopt;  // ambiguous: two attested identities fit equally
    }
    found = index;
  }
  return found;
}

std::string VbiIdentityReconciliation::summary(const std::string& noun) const {
  if (withheld) {
    return fmt::format(
        "no {} was ever named as transmitted, so the catalogue is left exactly "
        "as recovered.",
        noun);
  }
  if (!acted()) {
    return {};
  }
  std::string out = fmt::format(
      "{} of {} {} identities were never named as transmitted (a Hamming 8/4 "
      "byte of the identity had to be corrected every time); ",
      identities_unattested, identities_seen, noun);
  if (identities_folded > 0) {
    out += fmt::format(
        "{} of them ({} appearance(s)) were a single-digit misreading of one "
        "attested identity and were folded into it",
        identities_folded, appearances_folded);
  }
  if (identities_folded > 0 && identities_dropped > 0) {
    out += ", and ";
  }
  if (identities_dropped > 0) {
    out +=
        fmt::format("{} ({} appearance(s)) had no such identity to fold into",
                    identities_dropped, appearances_dropped);
  }
  out += '.';
  return out;
}

}  // namespace orc
