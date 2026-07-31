/*
 * File:        plugin_release_manifest.cpp
 * Module:      orc-core
 * Purpose:     Data model, YAML parser and host matcher for the per-release
 *              plugin manifest (orc-plugin-manifest.yaml)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "include/plugin_release_manifest.h"

#include <yaml-cpp/yaml.h>

namespace orc {

PluginReleaseManifestParseResult parse_plugin_release_manifest_yaml(
    const std::string& yaml_text) {
  PluginReleaseManifestParseResult result;

  YAML::Node root;
  try {
    root = YAML::Load(yaml_text);
  } catch (const YAML::Exception& e) {
    result.error_message =
        std::string("Failed to parse release manifest YAML: ") + e.what();
    return result;
  }

  if (!root || !root.IsMap()) {
    result.error_message = "Release manifest root is not a mapping";
    return result;
  }

  result.manifest.schema_version =
      root["manifest_schema"].as<int>(kPluginReleaseManifestSchemaVersion);
  if (result.manifest.schema_version > kPluginReleaseManifestSchemaVersion) {
    result.warnings.push_back(
        "Release manifest uses schema version " +
        std::to_string(result.manifest.schema_version) +
        " which is newer than this host understands (" +
        std::to_string(kPluginReleaseManifestSchemaVersion) +
        "); unknown fields are ignored");
  }

  result.manifest.plugin_id = root["plugin_id"].as<std::string>("");
  result.manifest.plugin_version = root["plugin_version"].as<std::string>("");

  const YAML::Node artifacts_node = root["artifacts"];
  if (!artifacts_node || !artifacts_node.IsSequence() ||
      artifacts_node.size() == 0) {
    result.error_message =
        "Release manifest declares no artifacts (an `artifacts` sequence with "
        "at least one entry is required)";
    return result;
  }

  for (const auto& node : artifacts_node) {
    PluginReleaseManifestArtifact artifact;
    artifact.file = node["file"].as<std::string>("");
    artifact.platform = node["platform"].as<std::string>("");
    artifact.abi = node["abi"].as<uint32_t>(0);
    artifact.toolchain_tag = node["toolchain_tag"].as<std::string>("");
    artifact.sha256 = node["sha256"].as<std::string>("");

    if (artifact.file.empty() || artifact.platform.empty() ||
        artifact.abi == 0) {
      result.error_message =
          "Release manifest artifact is missing a required field (`file`, "
          "`platform` and a non-zero `abi` are required)";
      return result;
    }
    if (artifact.sha256.empty()) {
      result.warnings.push_back("Release manifest artifact '" + artifact.file +
                                "' declares no sha256; the download cannot be "
                                "integrity-verified");
    }
    result.manifest.artifacts.push_back(std::move(artifact));
  }

  result.success = true;
  return result;
}

ManifestArtifactResolution resolve_manifest_artifact(
    const PluginReleaseManifest& manifest, const std::string& target_platform,
    uint32_t host_abi, const std::string& host_toolchain_tag) {
  ManifestArtifactResolution resolution;

  for (size_t i = 0; i < manifest.artifacts.size(); ++i) {
    if (manifest.artifacts[i].platform == target_platform) {
      resolution.found = true;
      resolution.index = static_cast<int>(i);
      break;
    }
  }

  if (!resolution.found) {
    resolution.error_message =
        "The release manifest lists no artifact for platform '" +
        target_platform + "'";
    return resolution;
  }

  const PluginReleaseManifestArtifact& artifact =
      manifest.artifacts[static_cast<size_t>(resolution.index)];
  resolution.abi_mismatch = artifact.abi != host_abi;
  // The toolchain tag is only comparable when the manifest declares one; the
  // load-time gate still enforces exact equality either way.
  resolution.toolchain_mismatch = !artifact.toolchain_tag.empty() &&
                                  artifact.toolchain_tag != host_toolchain_tag;
  return resolution;
}

}  // namespace orc
