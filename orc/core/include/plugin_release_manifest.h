/*
 * File:        plugin_release_manifest.h
 * Module:      orc-core
 * Purpose:     Data model, YAML parser and host matcher for the per-release
 *              plugin manifest (orc-plugin-manifest.yaml)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#if defined(ORC_GUI_BUILD) || defined(ORC_CLI_BUILD)
#error \
    "plugin_release_manifest.h is a core-only header. Access plugin discovery through ProjectPresenter."
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace orc {

/// Manifest schema major version understood by this host. Additions within a
/// major version are non-breaking (unknown fields are ignored); a higher
/// major is parsed best-effort with a warning.
inline constexpr int kPluginReleaseManifestSchemaVersion = 1;

/// Release asset name the host looks for. A release that ships this file
/// opts into manifest-based resolution, which then takes precedence over the
/// legacy asset-name conventions.
inline constexpr const char kPluginReleaseManifestAssetName[] =
    "orc-plugin-manifest.yaml";

/**
 * @brief One binary artifact declared by a release manifest.
 *
 * `file`, `platform` and `abi` are mandatory; `toolchain_tag` and `sha256`
 * are optional but strongly recommended (without them the host can only
 * pre-check the ABI number, and downloads cannot be integrity-verified).
 */
struct PluginReleaseManifestArtifact {
  std::string file;      ///< Exact release asset filename.
  std::string platform;  ///< linux | macos | windows.
  uint32_t abi = 0;      ///< kStagePluginHostAbiVersion the binary targets.
  std::string toolchain_tag;  ///< ORC_SDK_TOOLCHAIN_TAG of the build; empty
                              ///< when not declared.
  std::string sha256;         ///< Hex SHA-256 of the asset; empty when not
                              ///< declared.
};

/**
 * @brief Parsed release manifest.
 *
 * The manifest is a declaration by the release's CI, not proof: the
 * load-time ABI/toolchain gate remains authoritative. Its purpose is to let
 * the plugin manager give a definitive compatibility verdict before anything
 * is downloaded, and to supply a digest for the download integrity check.
 */
struct PluginReleaseManifest {
  int schema_version = 0;
  std::string plugin_id;
  std::string plugin_version;  ///< Informational; the release tag stays the
                               ///< version of record.
  std::vector<PluginReleaseManifestArtifact> artifacts;
};

/**
 * @brief Outcome of parsing a manifest document.
 */
struct PluginReleaseManifestParseResult {
  bool success = false;
  PluginReleaseManifest manifest;
  std::vector<std::string> warnings;
  std::string error_message;
};

/**
 * @brief Parse a release manifest from YAML text.
 *
 * Forward-compatible: unknown fields are ignored and a newer schema major is
 * parsed best-effort with a warning. Malformed YAML, a missing/empty
 * `artifacts` list, or an artifact without `file`, `platform` or `abi` fail
 * with an error_message — a release that opts into a manifest must publish a
 * valid one.
 *
 * Thread safety: pure function, safe to call concurrently.
 */
PluginReleaseManifestParseResult parse_plugin_release_manifest_yaml(
    const std::string& yaml_text);

/**
 * @brief Verdict of matching a manifest against this host.
 */
struct ManifestArtifactResolution {
  bool found = false;  ///< An artifact for the target platform is declared.
  int index = -1;      ///< Index into manifest.artifacts when found.
  bool abi_mismatch = false;        ///< Declared ABI differs from the host's.
  bool toolchain_mismatch = false;  ///< Declared toolchain tag differs from
                                    ///< the host's (only checked when the
                                    ///< manifest declares one).
  std::string error_message;        ///< Set when !found.
};

/**
 * @brief Select the manifest artifact for the given platform and compare its
 *        declared ABI and toolchain tag against the host's.
 *
 * Thread safety: pure function, safe to call concurrently.
 */
ManifestArtifactResolution resolve_manifest_artifact(
    const PluginReleaseManifest& manifest, const std::string& target_platform,
    uint32_t host_abi, const std::string& host_toolchain_tag);

}  // namespace orc
