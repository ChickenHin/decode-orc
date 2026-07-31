/*
 * File:        plugin_release_manifest_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     Unit tests for the release manifest YAML parser and the
 *              host ABI/toolchain matcher
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../orc/core/include/plugin_release_manifest.h"

#include <gtest/gtest.h>

namespace orc_unit_test {

using orc::parse_plugin_release_manifest_yaml;
using orc::PluginReleaseManifest;
using orc::PluginReleaseManifestArtifact;
using orc::resolve_manifest_artifact;

namespace {

constexpr char kValidManifest[] = R"yaml(
manifest_schema: 1
plugin_id: org.example.stage.demo
plugin_version: 1.2.3
artifacts:
  - file: orc-plugin_demo_linux.so
    platform: linux
    abi: 12
    toolchain_tag: gcc14/libstdc++
    sha256: 91ba329876d3df13772f051878ef7071721ed8f3e547ff6bfe6e4bd36c088c68
  - file: orc-plugin_demo_macos.dylib
    platform: macos
    abi: 12
    toolchain_tag: clang17/libc++
    sha256: 16dbdebc7ee615ac7d759972f38a8e92479f1a985dcc93ee355e7a36c32cf12c
)yaml";

PluginReleaseManifest makeManifest() {
  PluginReleaseManifest manifest;
  manifest.schema_version = 1;
  manifest.plugin_id = "org.example.stage.demo";
  PluginReleaseManifestArtifact artifact;
  artifact.file = "orc-plugin_demo_linux.so";
  artifact.platform = "linux";
  artifact.abi = 12;
  artifact.toolchain_tag = "gcc14/libstdc++";
  artifact.sha256 = "abc123";
  manifest.artifacts.push_back(artifact);
  return manifest;
}

}  // namespace

// --- Parsing ---------------------------------------------------------------

TEST(PluginReleaseManifestTest, Parse_AcceptsValidManifest) {
  const auto result = parse_plugin_release_manifest_yaml(kValidManifest);
  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_EQ(result.manifest.schema_version, 1);
  EXPECT_EQ(result.manifest.plugin_id, "org.example.stage.demo");
  EXPECT_EQ(result.manifest.plugin_version, "1.2.3");
  ASSERT_EQ(result.manifest.artifacts.size(), 2u);
  EXPECT_EQ(result.manifest.artifacts[0].file, "orc-plugin_demo_linux.so");
  EXPECT_EQ(result.manifest.artifacts[0].platform, "linux");
  EXPECT_EQ(result.manifest.artifacts[0].abi, 12u);
  EXPECT_EQ(result.manifest.artifacts[0].toolchain_tag, "gcc14/libstdc++");
  EXPECT_EQ(result.manifest.artifacts[0].sha256,
            "91ba329876d3df13772f051878ef7071721ed8f3e547ff6bfe6e4bd36c088c68");
  EXPECT_TRUE(result.warnings.empty());
}

TEST(PluginReleaseManifestTest, Parse_IgnoresUnknownFields) {
  const auto result = parse_plugin_release_manifest_yaml(R"yaml(
manifest_schema: 1
plugin_id: org.example.stage.demo
future_field: whatever
artifacts:
  - file: orc-plugin_demo_linux.so
    platform: linux
    abi: 12
    another_future_field: 42
)yaml");
  ASSERT_TRUE(result.success) << result.error_message;
  ASSERT_EQ(result.manifest.artifacts.size(), 1u);
}

TEST(PluginReleaseManifestTest, Parse_WarnsOnNewerSchemaMajor) {
  const auto result = parse_plugin_release_manifest_yaml(R"yaml(
manifest_schema: 99
artifacts:
  - file: orc-plugin_demo_linux.so
    platform: linux
    abi: 12
)yaml");
  ASSERT_TRUE(result.success) << result.error_message;
  ASSERT_FALSE(result.warnings.empty());
  EXPECT_NE(result.warnings.front().find("newer"), std::string::npos);
}

TEST(PluginReleaseManifestTest, Parse_WarnsWhenArtifactHasNoDigest) {
  const auto result = parse_plugin_release_manifest_yaml(R"yaml(
artifacts:
  - file: orc-plugin_demo_linux.so
    platform: linux
    abi: 12
)yaml");
  ASSERT_TRUE(result.success) << result.error_message;
  ASSERT_FALSE(result.warnings.empty());
  EXPECT_NE(result.warnings.front().find("sha256"), std::string::npos);
}

TEST(PluginReleaseManifestTest, Parse_RejectsMalformedYaml) {
  const auto result = parse_plugin_release_manifest_yaml("{ not: [valid");
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
}

TEST(PluginReleaseManifestTest, Parse_RejectsNonMappingRoot) {
  const auto result = parse_plugin_release_manifest_yaml("- just\n- a\n- list");
  EXPECT_FALSE(result.success);
}

TEST(PluginReleaseManifestTest, Parse_RejectsMissingArtifacts) {
  const auto result = parse_plugin_release_manifest_yaml(R"yaml(
manifest_schema: 1
plugin_id: org.example.stage.demo
)yaml");
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("artifacts"), std::string::npos);
}

TEST(PluginReleaseManifestTest, Parse_RejectsArtifactMissingRequiredField) {
  const auto result = parse_plugin_release_manifest_yaml(R"yaml(
artifacts:
  - file: orc-plugin_demo_linux.so
    platform: linux
)yaml");
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_message.find("required"), std::string::npos);
}

// --- Host matching ---------------------------------------------------------

TEST(PluginReleaseManifestTest, Resolve_MatchesCompatibleArtifact) {
  const auto resolution =
      resolve_manifest_artifact(makeManifest(), "linux", 12, "gcc14/libstdc++");
  ASSERT_TRUE(resolution.found);
  EXPECT_EQ(resolution.index, 0);
  EXPECT_FALSE(resolution.abi_mismatch);
  EXPECT_FALSE(resolution.toolchain_mismatch);
}

TEST(PluginReleaseManifestTest, Resolve_FlagsAbiMismatch) {
  const auto resolution =
      resolve_manifest_artifact(makeManifest(), "linux", 13, "gcc14/libstdc++");
  ASSERT_TRUE(resolution.found);
  EXPECT_TRUE(resolution.abi_mismatch);
  EXPECT_FALSE(resolution.toolchain_mismatch);
}

TEST(PluginReleaseManifestTest, Resolve_FlagsToolchainMismatch) {
  const auto resolution =
      resolve_manifest_artifact(makeManifest(), "linux", 12, "clang17/libc++");
  ASSERT_TRUE(resolution.found);
  EXPECT_FALSE(resolution.abi_mismatch);
  EXPECT_TRUE(resolution.toolchain_mismatch);
}

TEST(PluginReleaseManifestTest, Resolve_SkipsToolchainCheckWhenUndeclared) {
  auto manifest = makeManifest();
  manifest.artifacts[0].toolchain_tag.clear();
  const auto resolution =
      resolve_manifest_artifact(manifest, "linux", 12, "clang17/libc++");
  ASSERT_TRUE(resolution.found);
  EXPECT_FALSE(resolution.toolchain_mismatch);
}

TEST(PluginReleaseManifestTest, Resolve_ReportsMissingPlatform) {
  const auto resolution = resolve_manifest_artifact(makeManifest(), "windows",
                                                    12, "msvc19/msvc-stl");
  EXPECT_FALSE(resolution.found);
  EXPECT_NE(resolution.error_message.find("windows"), std::string::npos);
}

}  // namespace orc_unit_test
