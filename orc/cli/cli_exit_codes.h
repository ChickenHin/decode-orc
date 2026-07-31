/*
 * File:        cli_exit_codes.h
 * Module:      orc-cli
 * Purpose:     Exit-code contract shared by the scriptable subcommands
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_CLI_EXIT_CODES_H
#define ORC_CLI_EXIT_CODES_H

namespace orc {
namespace cli {

// A script must be able to tell "you asked for something that does not exist"
// from "the network was down" from "you declined to trust a binary", because
// each calls for a different response. The table is documented in
// docs/cli-user-guide/overview.md.

/// The command did what was asked.
inline constexpr int kExitSuccess = 0;

/// Bad arguments, or a failure with no more specific code.
inline constexpr int kExitUsage = 1;

/// The named plugin, stage or index entry does not exist, or a selector
/// matched more than one entry and the command refused to guess.
inline constexpr int kExitNotFound = 2;

/// The curated index or a release could not be reached.
inline constexpr int kExitIndexUnavailable = 3;

/// Trust was required and not granted, so nothing was recorded.
inline constexpr int kExitTrustDeclined = 4;

}  // namespace cli
}  // namespace orc

#endif  // ORC_CLI_EXIT_CODES_H
