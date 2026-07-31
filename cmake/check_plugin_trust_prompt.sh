#!/usr/bin/env bash
# File:        check_plugin_trust_prompt.sh
# Purpose:     Gate: every plugin command that lets a binary run asks for the
#              trust confirmation first, never blocks on an answer that cannot
#              arrive, and names --yes when there is no terminal to ask on.
#
# Usage:       check_plugin_trust_prompt.sh <path-to-orc-cli>
#
# Registry-only: the fixture registry is built under a redirected
# HOME/XDG_CONFIG_HOME and no subcommand exercised here touches the network or
# the curated index. Every invocation reads stdin from /dev/null, so a command
# that decided to wait for input would fail the gate rather than hang it.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Simon Inns

set -u

ORC_CLI="${1:-}"
if [[ -z "$ORC_CLI" || ! -x "$ORC_CLI" ]]; then
    echo "Usage: $0 <path-to-orc-cli>" >&2
    exit 1
fi

# Exit code 4 = trust was required and not granted (see orc/cli/cli_exit_codes.h).
readonly EXIT_TRUST_DECLINED=4

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home"
mkdir -p "$HOME"

case "$(uname -s)" in
    Darwin) PLUGIN_EXT=".dylib" ;;
    MINGW*|MSYS*|CYGWIN*) PLUGIN_EXT=".dll" ;;
    *) PLUGIN_EXT=".so" ;;
esac

FAILURES=0

fail() {
    echo "FAIL: $*" >&2
    FAILURES=$((FAILURES + 1))
}

PLUGIN="$WORK_DIR/trust_fixture${PLUGIN_EXT}"
touch "$PLUGIN"
SELECTOR="com.example.trustgate"

# --- Adding grants trust, so it must be confirmed -----------------------------

OUTPUT="$("$ORC_CLI" plugins add "$PLUGIN" --id "$SELECTOR" </dev/null 2>&1)"
STATUS=$?
if [[ "$STATUS" -ne "$EXIT_TRUST_DECLINED" ]]; then
    fail "'plugins add' without --yes exited $STATUS, expected $EXIT_TRUST_DECLINED"
fi
if ! echo "$OUTPUT" | grep -q -- "--yes"; then
    fail "'plugins add' without a terminal did not name --yes: $OUTPUT"
fi
if "$ORC_CLI" plugins list </dev/null | grep -q "$SELECTOR"; then
    fail "'plugins add' recorded an entry without a trust confirmation"
fi

if ! "$ORC_CLI" plugins add "$PLUGIN" --id "$SELECTOR" --version 1.0.0 --yes \
        </dev/null >/dev/null; then
    fail "'plugins add --yes' did not add the fixture plugin"
fi

# Confirmed on the way in means the entry loads at the next launch.
if ! "$ORC_CLI" plugins list </dev/null | grep -q "status:.*Enabled"; then
    fail "'plugins add --yes' left the entry in a non-loading state"
fi

# --- Withdrawing trust grants nothing, so it must not prompt ------------------

if ! "$ORC_CLI" plugins untrust "$SELECTOR" </dev/null >/dev/null 2>&1; then
    fail "'plugins untrust' asked for a confirmation it does not need"
fi
if ! "$ORC_CLI" plugins list </dev/null | grep -q "status:.*Not trusted yet"; then
    fail "'plugins untrust' did not leave the entry untrusted"
fi

# --- Enabling an untrusted entry is a trust grant -----------------------------

OUTPUT="$("$ORC_CLI" plugins enable "$SELECTOR" </dev/null 2>&1)"
STATUS=$?
if [[ "$STATUS" -ne "$EXIT_TRUST_DECLINED" ]]; then
    fail "'plugins enable' on an untrusted entry exited $STATUS, expected $EXIT_TRUST_DECLINED"
fi
if ! echo "$OUTPUT" | grep -q -- "--yes"; then
    fail "'plugins enable' without a terminal did not name --yes: $OUTPUT"
fi
if ! "$ORC_CLI" plugins list </dev/null | grep -q "status:.*Not trusted yet"; then
    fail "'plugins enable' granted trust without a confirmation"
fi

if ! "$ORC_CLI" plugins enable "$SELECTOR" --yes </dev/null >/dev/null; then
    fail "'plugins enable --yes' did not enable the entry"
fi
if ! "$ORC_CLI" plugins list </dev/null | grep -q "status:.*Enabled"; then
    fail "'plugins enable --yes' did not grant trust"
fi

# --- Disabling takes nothing away from the user, so it must not prompt --------

if ! "$ORC_CLI" plugins disable "$SELECTOR" </dev/null >/dev/null; then
    fail "'plugins disable' asked for a confirmation it does not need"
fi

if [[ "$FAILURES" -eq 0 ]]; then
    echo "Plugin trust confirmation: passed"
    exit 0
fi

echo "Plugin trust confirmation: failed ($FAILURES violation(s))" >&2
exit 1
