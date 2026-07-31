/*
 * File:        command_stages.h
 * Module:      orc-cli
 * Purpose:     Stage introspection subcommand (list / info / help)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

namespace orc {
namespace cli {

/**
 * @brief Execute the `stages` subcommand.
 *
 * Gives a script-only user the stage introspection the GUI offers through its
 * Add Stage menu, parameter dialog and help dialog: which stages this build can
 * run, what each one takes, and the instructions shipped beside it.
 *
 * @param argc Argument count.
 * @param argv Arguments, with argv[0] = the program name (for usage messages)
 *             and argv[1] = the subcommand.
 * @return Exit code from the contract in cli_exit_codes.h.
 */
int stages_command(int argc, char* argv[]);

}  // namespace cli
}  // namespace orc
