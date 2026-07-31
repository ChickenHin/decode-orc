/*
 * File:        plugin_artifact_name_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     Unit tests for release-artifact name validation and ABI-tag
 *              parsing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../orc/core/include/plugin_artifact_name.h"

#include <gtest/gtest.h>

namespace orc_unit_test {

using orc::parse_release_asset_name;

// --- Validation / parsing --------------------------------------------------

TEST(PluginArtifactNameTest, ParsesLegacyUntaggedName) {
  const auto parsed = parse_release_asset_name("orc-plugin_my-stage_linux.so");
  EXPECT_TRUE(parsed.valid);
  EXPECT_EQ(parsed.ext, "so");
  EXPECT_FALSE(parsed.has_abi);
}

TEST(PluginArtifactNameTest, ParsesAbiTaggedName) {
  const auto parsed =
      parse_release_asset_name("orc-plugin_my-stage_macos_abi8.dylib");
  EXPECT_TRUE(parsed.valid);
  EXPECT_EQ(parsed.ext, "dylib");
  EXPECT_TRUE(parsed.has_abi);
  EXPECT_EQ(parsed.abi, 8U);
}

TEST(PluginArtifactNameTest, ParsesWindowsDll) {
  const auto parsed =
      parse_release_asset_name("orc-plugin_stage_windows_abi12.dll");
  EXPECT_TRUE(parsed.valid);
  EXPECT_EQ(parsed.ext, "dll");
  EXPECT_TRUE(parsed.has_abi);
  EXPECT_EQ(parsed.abi, 12U);
}

TEST(PluginArtifactNameTest, RejectsWrongPrefix) {
  EXPECT_FALSE(orc::is_valid_release_asset_name("plugin_my-stage_linux.so"));
}

TEST(PluginArtifactNameTest, RejectsWrongExtension) {
  EXPECT_FALSE(orc::is_valid_release_asset_name("orc-plugin_stage_linux.txt"));
}

TEST(PluginArtifactNameTest, RejectsMissingPlatformSegment) {
  // Only a single segment after the prefix — no stage/platform split.
  EXPECT_FALSE(orc::is_valid_release_asset_name("orc-plugin_stage.so"));
}

}  // namespace orc_unit_test
