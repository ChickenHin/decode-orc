#!/usr/bin/env bash
#
# File:        check_plugin_index.sh
# Module:      cmake
# Purpose:     Gate the curated plugin index and prove each validation failure
#              class (offline)
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# Runs the plugin-index validator over the live index (must pass) and every
# fixture (good must pass, each bad must fail), so the validation rules stay
# honest. Offline only; URL reachability and digest matching run in the
# registry PR workflow. Kept bash-3.2 compatible.

set -u

SOURCE_DIR="${1:-.}"
REGISTRY_DIR="${SOURCE_DIR}/orc-plugin-registry"
VALIDATOR="${REGISTRY_DIR}/tools/validate_index.py"
TESTS_DIR="${REGISTRY_DIR}/tests"

PYTHON="${PYTHON:-python3}"

if ! command -v "${PYTHON}" >/dev/null 2>&1; then
  echo "SKIP: ${PYTHON} not available; cannot validate plugin index" >&2
  exit 0
fi

if [ ! -f "${VALIDATOR}" ]; then
  echo "FAIL: validator not found at ${VALIDATOR}" >&2
  exit 1
fi

status=0

expect_pass() {
  # $1 = file
  if ! "${PYTHON}" "${VALIDATOR}" "$1" >/dev/null 2>&1; then
    echo "FAIL: expected '$1' to validate, but it did not" >&2
    "${PYTHON}" "${VALIDATOR}" "$1" >&2
    status=1
  else
    echo "PASS: '$1' is valid"
  fi
}

expect_fail() {
  # $1 = file
  if "${PYTHON}" "${VALIDATOR}" "$1" >/dev/null 2>&1; then
    echo "FAIL: expected '$1' to be rejected, but it validated" >&2
    status=1
  else
    echo "PASS: '$1' was correctly rejected"
  fi
}

# The live index must always be valid.
expect_pass "${REGISTRY_DIR}/index.yaml"

# Good fixtures.
expect_pass "${TESTS_DIR}/valid.yaml"
expect_pass "${TESTS_DIR}/newer-minor-schema.yaml"

# One fixture per failure class.
expect_fail "${TESTS_DIR}/bad-schema.yaml"
expect_fail "${TESTS_DIR}/missing-license.yaml"
expect_fail "${TESTS_DIR}/missing-repo-url.yaml"
expect_fail "${TESTS_DIR}/pinned-artifacts.yaml"

if [ "${status}" -ne 0 ]; then
  echo "Plugin index validation gate FAILED" >&2
fi
exit "${status}"
