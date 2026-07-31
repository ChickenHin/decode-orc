#!/usr/bin/env bash
# File:        check_plugin_ux_parity.sh
# Purpose:     Gate: the plugin/stage UX capability manifest and the two front
#              ends agree — every recorded CLI form is still offered by
#              `orc-cli plugins --help` / `orc-cli stages --help`, every
#              subcommand those help texts offer is recorded, and every
#              capability names both a CLI form and a GUI control or says why
#              it has only one.
#
# Usage:       check_plugin_ux_parity.sh <path-to-orc-cli> <repo-root>
#
# Runs in the default unit lane: reads two --help texts and the source tree,
# with no network, no registry and no GUI.
# Kept bash-3.2 compatible.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Simon Inns

set -u

ORC_CLI="${1:-}"
REPO_ROOT="${2:-}"
if [[ -z "$ORC_CLI" || ! -x "$ORC_CLI" || -z "$REPO_ROOT" || ! -d "$REPO_ROOT" ]]; then
    echo "Usage: $0 <path-to-orc-cli> <repo-root>" >&2
    exit 1
fi

MANIFEST="$REPO_ROOT/orc/plugin_ux_capabilities.yaml"
if [[ ! -f "$MANIFEST" ]]; then
    echo "FAIL: capability manifest not found at $MANIFEST" >&2
    exit 1
fi

FAILURES=0

fail() {
    echo "FAIL: $*" >&2
    FAILURES=$((FAILURES + 1))
}

# --- Help texts --------------------------------------------------------------
#
# Both groups print their usage on stderr and exit 0 for --help.

PLUGINS_HELP="$("$ORC_CLI" plugins --help 2>&1)"
STAGES_HELP="$("$ORC_CLI" stages --help 2>&1)"

if [[ -z "$PLUGINS_HELP" ]]; then
    fail "'plugins --help' printed nothing"
fi
if [[ -z "$STAGES_HELP" ]]; then
    fail "'stages --help' printed nothing"
fi

help_text_for_group() {
    case "$1" in
        plugins) printf '%s' "$PLUGINS_HELP" ;;
        stages) printf '%s' "$STAGES_HELP" ;;
        *) printf '' ;;
    esac
}

# The subcommand block ends at the first blank line after the header.
# A cli_help entry names the start of the help line that offers it, so a
# passing mention in prose ("...'updates' and 'doctor'...") cannot stand in for
# the command actually being offered.
help_offers() {
    help_text_for_group "$1" | awk -v want="$2" '
        { line = $0; sub(/^[[:space:]]+/, "", line)
          if (index(line, want) == 1) { found = 1 } }
        END { exit found ? 0 : 1 }
    '
}

subcommands_of() {
    help_text_for_group "$1" | awk '
        /^Subcommands:/ { inside = 1; next }
        inside && /^[[:space:]]*$/ { inside = 0 }
        inside && NF { print $1 }
    ' | sort -u
}

# --- Manifest ----------------------------------------------------------------
#
# The manifest is written to one strict shape (see its header comment), so it
# is read with awk rather than pulling in a YAML library the default lane would
# then depend on. Records are emitted one per line, fields in this order:
#   id, cli, cli_group, cli_help, gui, gui_evidence, has_reason
#
# The separator is US (0x1f) rather than a tab: `read` treats whitespace
# separators as collapsible, which would silently shift an empty field.

RECORDS="$(awk '
    function strip(v) {
        sub(/^[[:space:]]+/, "", v)
        sub(/[[:space:]]+$/, "", v)
        if (v ~ /^".*"$/) { v = substr(v, 2, length(v) - 2) }
        return v
    }
    function flush() {
        if (id != "") {
            printf "%s\037%s\037%s\037%s\037%s\037%s\037%s\n",
                   id, cli, cli_group, cli_help, gui, gui_evidence, has_reason
        }
        id = ""; cli = ""; cli_group = ""; cli_help = ""
        gui = ""; gui_evidence = ""; has_reason = 0; last_key = ""
    }
    /^[[:space:]]*#/ { next }
    /^  - id:[[:space:]]/ {
        flush()
        id = strip(substr($0, index($0, ":") + 1))
        last_key = "id"
        next
    }
    /^    [a-z_]+:/ {
        key = $0
        sub(/^[[:space:]]+/, "", key)
        sub(/:.*$/, "", key)
        value = strip(substr($0, index($0, ":") + 1))
        last_key = key
        if (key == "cli") { cli = value }
        else if (key == "cli_group") { cli_group = value }
        else if (key == "cli_help") { cli_help = value }
        else if (key == "gui") { gui = value }
        else if (key == "gui_evidence") { gui_evidence = value }
        else if (key == "reason") { if (value != "" && value != ">-" && value != ">") { has_reason = 1 } }
        next
    }
    /^      [^[:space:]]/ {
        if (last_key == "reason") { has_reason = 1 }
        next
    }
    END { flush() }
' "$MANIFEST")"

if [[ -z "$RECORDS" ]]; then
    fail "the capability manifest declares no capabilities"
fi

SEEN_IDS=""
CLAIMED=""

while IFS=$'\037' read -r id cli cli_group cli_help gui gui_evidence has_reason; do
    [[ -z "$id" ]] && continue

    # --- Shape of the record -------------------------------------------------

    case " $SEEN_IDS " in
        *" $id "*) fail "capability id '$id' is declared more than once" ;;
    esac
    SEEN_IDS="$SEEN_IDS $id"

    if [[ -z "$cli" ]]; then
        fail "$id: no 'cli' field (use 'none' when there is no CLI form)"
        continue
    fi
    if [[ -z "$gui" ]]; then
        fail "$id: no 'gui' field (use 'none' when there is no GUI control)"
        continue
    fi
    if [[ "$cli" == "none" && "$gui" == "none" ]]; then
        fail "$id: neither front end offers this capability"
        continue
    fi
    if [[ ( "$cli" == "none" || "$gui" == "none" ) && "$has_reason" != "1" ]]; then
        fail "$id: only one front end offers this, and no 'reason' says why"
    fi

    # --- The CLI side still exists -------------------------------------------

    if [[ "$cli" == "none" ]]; then
        if [[ "$cli_group" != "none" ]]; then
            fail "$id: cli is 'none' but cli_group is '$cli_group'"
        fi
    else
        case "$cli_group" in
            plugins|stages) ;;
            *) fail "$id: cli_group must be 'plugins' or 'stages', got '$cli_group'"
               continue ;;
        esac
        if [[ -z "$cli_help" ]]; then
            fail "$id: no 'cli_help' substring to look for in '$cli_group --help'"
        elif ! help_offers "$cli_group" "$cli_help"; then
            fail "$id: 'orc-cli $cli_group --help' no longer offers '$cli_help'"
        fi
        CLAIMED="$CLAIMED
$cli_group|$cli"
    fi

    # --- The GUI side still exists -------------------------------------------
    #
    # Evidence is searched in orc/gui only: a constant that merely exists in
    # the shared strings header proves nothing about the control still being
    # built, so deleting the control must remove the evidence too.

    if [[ "$gui" != "none" ]]; then
        evidence="${gui_evidence:-$gui}"
        if ! grep -rqF -- "$evidence" "$REPO_ROOT/orc/gui"; then
            fail "$id: no GUI control matching '$evidence' in orc/gui"
        fi
    fi
done <<EOF
$RECORDS
EOF

# --- Nothing the CLI offers is missing from the manifest ----------------------
#
# A capability that grows on one side only is caught here: a new subcommand is
# unrecorded until the manifest names its GUI counterpart too.

for group in plugins stages; do
    while IFS= read -r subcommand; do
        [[ -z "$subcommand" ]] && continue
        if ! printf '%s\n' "$CLAIMED" \
                | grep -qE "^$group\|(.* )?$group $subcommand( |\$)"; then
            fail "'$group $subcommand' is offered by --help but no capability records it"
        fi
    done <<EOF
$(subcommands_of "$group")
EOF
done

# --- The top-level help lists every subcommand --------------------------------
#
# `orc-cli --help` is the first text a user reads; it once went stale, silently
# omitting half the plugins subcommands. Every subcommand a group's own help
# offers must be visible there too.

TOP_HELP="$("$ORC_CLI" --help 2>&1)"
if [[ -z "$TOP_HELP" ]]; then
    fail "'orc-cli --help' printed nothing"
fi
for group in plugins stages; do
    while IFS= read -r subcommand; do
        [[ -z "$subcommand" ]] && continue
        if ! printf '%s\n' "$TOP_HELP" \
                | grep -qE "^[[:space:]]+$group $subcommand([^[:alnum:]]|\$)"; then
            fail "'$group $subcommand' is missing from the top-level --help"
        fi
    done <<EOF
$(subcommands_of "$group")
EOF
done

if [[ "$FAILURES" -eq 0 ]]; then
    echo "Plugin UX capability parity: passed"
    exit 0
fi

echo "Plugin UX capability parity: failed ($FAILURES violation(s))" >&2
exit 1
