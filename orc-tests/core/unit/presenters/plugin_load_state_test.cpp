/*
 * File:        plugin_load_state_test.cpp
 * Module:      orc-tests/core/unit
 * Purpose:     Unit tests for the presenter-computed plugin load state
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "plugin_load_state.h"

#include <gtest/gtest.h>
#include <plugin_ux_strings.h>

namespace orc::presenters {
namespace {

// A registered, trusted, enabled plugin whose binary is present. Each test
// perturbs exactly one field so the state ranking is exercised in isolation.
PluginRegistryEntryInfo makeLoadableEntry() {
  PluginRegistryEntryInfo entry;
  entry.plugin_id = "com.example.plugin";
  entry.path = "/plugins/example.so";
  entry.artifact_source = "local_path";
  entry.enabled = true;
  entry.trust_state = "trusted";
  entry.path_exists = true;
  entry.abi_compatible = true;
  entry.required_host_abi = 12;
  entry.host_abi_version = 12;
  return entry;
}

TEST(PluginLoadStateTest, TrustedEnabledPresentEntryWillLoad) {
  const auto entry = makeLoadableEntry();
  EXPECT_EQ(computePluginLoadState(entry), PluginLoadState::WillLoad);
}

TEST(PluginLoadStateTest, ClearedEnabledFlagReportsDisabled) {
  auto entry = makeLoadableEntry();
  entry.enabled = false;
  EXPECT_EQ(computePluginLoadState(entry), PluginLoadState::Disabled);
}

TEST(PluginLoadStateTest, UntrustedEntryReportsNotTrusted) {
  auto entry = makeLoadableEntry();
  entry.trust_state = "untrusted";
  EXPECT_EQ(computePluginLoadState(entry), PluginLoadState::NotTrusted);
}

// Ticking Enabled in the GUI is what grants trust, so an entry the user has
// enabled but not yet confirmed must still report NotTrusted.
TEST(PluginLoadStateTest, EnabledButUntrustedEntryReportsNotTrusted) {
  auto entry = makeLoadableEntry();
  entry.enabled = true;
  entry.trust_state = "untrusted";
  EXPECT_EQ(computePluginLoadState(entry), PluginLoadState::NotTrusted);
}

TEST(PluginLoadStateTest, AbiMismatchReportsAbiMismatchWithBothVersions) {
  auto entry = makeLoadableEntry();
  entry.required_host_abi = 11;
  entry.host_abi_version = 12;
  entry.abi_compatible = false;

  std::string detail;
  EXPECT_EQ(computePluginLoadState(entry, &detail),
            PluginLoadState::AbiMismatch);
  EXPECT_NE(detail.find("11"), std::string::npos);
  EXPECT_NE(detail.find("12"), std::string::npos);
}

// An untrusted, disabled entry with the wrong ABI still reports the ABI
// problem: trusting or enabling it would not make it load.
TEST(PluginLoadStateTest, AbiMismatchOutranksTrustAndEnabledFlags) {
  auto entry = makeLoadableEntry();
  entry.abi_compatible = false;
  entry.enabled = false;
  entry.trust_state = "untrusted";
  EXPECT_EQ(computePluginLoadState(entry), PluginLoadState::AbiMismatch);
}

TEST(PluginLoadStateTest, MissingLocalBinaryReportsFileMissing) {
  auto entry = makeLoadableEntry();
  entry.path_exists = false;

  std::string detail;
  EXPECT_EQ(computePluginLoadState(entry, &detail),
            PluginLoadState::FileMissing);
  EXPECT_NE(detail.find(entry.path), std::string::npos);
}

// A remote entry's path is a download cache location: an absent file means
// "not fetched yet", which is not a broken entry.
TEST(PluginLoadStateTest, RemoteEntryWithUnfetchedBinaryIsNotFileMissing) {
  auto entry = makeLoadableEntry();
  entry.artifact_source = "github_release_asset";
  entry.release_asset_url = "https://example.invalid/plugin.so";
  entry.path.clear();
  entry.path_exists = false;
  EXPECT_EQ(computePluginLoadState(entry), PluginLoadState::WillLoad);
}

TEST(PluginLoadStateTest, CorePluginReportsCore) {
  auto entry = makeLoadableEntry();
  entry.is_core_plugin = true;
  EXPECT_EQ(computePluginLoadState(entry), PluginLoadState::Core);
}

// Core plugins ship with the host and are gated at build time, so they are
// exempt from the registry's ABI and trust gates.
TEST(PluginLoadStateTest, CorePluginIsExemptFromAbiAndTrustGates) {
  auto entry = makeLoadableEntry();
  entry.is_core_plugin = true;
  entry.abi_compatible = false;
  entry.trust_state = "untrusted";
  entry.enabled = false;
  EXPECT_EQ(computePluginLoadState(entry), PluginLoadState::Core);
}

TEST(PluginLoadStateTest, LabelsComeFromTheSharedStringsHeader) {
  EXPECT_STREQ(pluginLoadStateLabel(PluginLoadState::WillLoad),
               plugin_ux::kLoadStateWillLoadLabel);
  EXPECT_STREQ(pluginLoadStateLabel(PluginLoadState::NotTrusted),
               plugin_ux::kLoadStateNotTrustedLabel);
  EXPECT_STREQ(pluginLoadStateLabel(PluginLoadState::AbiMismatch),
               plugin_ux::kLoadStateAbiMismatchLabel);
}

TEST(PluginLoadStateTest, StateIdentifiersAreStableLowercaseTokens) {
  EXPECT_STREQ(pluginLoadStateId(PluginLoadState::WillLoad), "will_load");
  EXPECT_STREQ(pluginLoadStateId(PluginLoadState::Disabled), "disabled");
  EXPECT_STREQ(pluginLoadStateId(PluginLoadState::NotTrusted), "not_trusted");
  EXPECT_STREQ(pluginLoadStateId(PluginLoadState::AbiMismatch), "abi_mismatch");
  EXPECT_STREQ(pluginLoadStateId(PluginLoadState::FileMissing), "file_missing");
  EXPECT_STREQ(pluginLoadStateId(PluginLoadState::Core), "core");
}

TEST(PluginLoadStateTest, SummaryJoinsLabelAndDetail) {
  const std::string summary = pluginLoadStateSummary(
      PluginLoadState::NotTrusted, plugin_ux::kLoadStateNotTrustedDetail);
  EXPECT_EQ(summary, std::string(plugin_ux::kLoadStateNotTrustedLabel) +
                         plugin_ux::kLabelDetailSeparator +
                         plugin_ux::kLoadStateNotTrustedDetail);

  EXPECT_EQ(pluginLoadStateSummary(PluginLoadState::Core, std::string()),
            plugin_ux::kLoadStateCoreLabel);
}

}  // namespace
}  // namespace orc::presenters
