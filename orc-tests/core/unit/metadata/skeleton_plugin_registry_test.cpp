/*
 * File:        skeleton_plugin_registry_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     Contract tests for the canonical skeleton plugin registry:
 *              YAML parsing, artifact naming, and cache-path formation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include "../../../orc/core/include/plugin_remote_loader.h"
#include "../../../orc/core/include/stage_plugin_registry.h"

namespace orc_unit_test {

TEST(SkeletonPluginRegistryTest,
     Registry_AcceptsSkeletonReleaseMetadataWithLocalOverride) {
  const std::string skeleton_registry_yaml = R"yaml(
version: 2
plugins:
  - plugin_id: org.decodeorc.stage.skeleton_passthrough
    plugin_version: 0.1.0
    artifact_source: github_release_asset
    release_asset_url: https://github.com/decode-orc/orc-plugin_skeleton/releases/download/v0.1.0/orc-plugin_skeleton_passthrough_linux.so
    release_asset_name: orc-plugin_skeleton_passthrough_linux.so
    target_platform: linux
    source_repo_url: https://github.com/decode-orc/orc-plugin_skeleton
    release_tag: v0.1.0
    local_dev_path: /tmp/orc-plugin_skeleton_passthrough_linux.so
    enabled: true
    trust_state: untrusted
    license_spdx: GPL-3.0-or-later
)yaml";

  const auto result = orc::StagePluginRegistry::parse_yaml(
      skeleton_registry_yaml, "<skeleton-registry-test>");

  EXPECT_EQ(result.registry_path, "<skeleton-registry-test>");
  ASSERT_TRUE(result.warnings.empty());
  ASSERT_EQ(result.entries.size(), 1U);
  EXPECT_EQ(result.entries[0].plugin_id,
            "org.decodeorc.stage.skeleton_passthrough");
  EXPECT_EQ(result.entries[0].plugin_version, "0.1.0");
  EXPECT_EQ(result.entries[0].release_tag, "v0.1.0");
  EXPECT_EQ(result.entries[0].source_repo_url,
            "https://github.com/decode-orc/orc-plugin_skeleton");
  EXPECT_EQ(result.entries[0].path,
            "/tmp/orc-plugin_skeleton_passthrough_linux.so");
}

TEST(SkeletonPluginRegistryTest, SkeletonArtifactNames_MatchNamingConvention) {
  // These are the actual published artifacts on GitHub Releases
  const std::vector<std::string> artifacts = {
      "orc-plugin_skeleton_passthrough_linux.so",
      "orc-plugin_skeleton_passthrough_macos.dylib",
      "orc-plugin_skeleton_passthrough_windows.dll"};

  for (const auto& artifact : artifacts) {
    // Validate cache path can be constructed
    const auto path =
        orc::PluginRemoteLoader::get_cache_path(artifact, "linux");
    EXPECT_FALSE(path.empty());
    EXPECT_TRUE(path.find("plugin-cache") != std::string::npos);
    EXPECT_TRUE(path.find(artifact) != std::string::npos);
  }
}

TEST(SkeletonPluginRegistryTest, CachePaths_MatchSkeletonArtifacts) {
  const std::vector<std::string> skeleton_artifacts = {
      "orc-plugin_skeleton_passthrough_linux.so",
      "orc-plugin_skeleton_passthrough_macos.dylib",
      "orc-plugin_skeleton_passthrough_windows.dll"};

  for (const auto& artifact : skeleton_artifacts) {
    const auto path =
        orc::PluginRemoteLoader::get_cache_path(artifact, "linux");
    EXPECT_FALSE(path.empty());
    EXPECT_TRUE(path.find("plugin-cache") != std::string::npos);
    EXPECT_TRUE(path.find(artifact) != std::string::npos);
  }
}

}  // namespace orc_unit_test
