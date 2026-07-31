/*
 * File:        plugin_selector_test.cpp
 * Module:      orc-tests/core/unit
 * Purpose:     Unit tests for plugin selector derivation and resolution
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "plugin_selector.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace orc::presenters {
namespace {

PluginRegistryEntryInfo makeEntry(const std::string& plugin_id,
                                  const std::string& path,
                                  const std::string& release_asset_url = {}) {
  PluginRegistryEntryInfo entry;
  entry.plugin_id = plugin_id;
  entry.path = path;
  entry.release_asset_url = release_asset_url;
  return entry;
}

// A registry holding one of everything a real one can hold: an ordinary
// plugin, a locally added entry that has no id yet, a core plugin, and a
// remote entry whose binary has not been downloaded.
std::vector<PluginRegistryEntryInfo> makeMixedRegistry() {
  std::vector<PluginRegistryEntryInfo> entries = {
      makeEntry("com.example.plugin", "/plugins/example.so"),
      makeEntry("", "/plugins/unnamed.so"),
      makeEntry("decode-orc.stage.tbc_source", "/usr/lib/orc/tbc_source.so"),
      makeEntry("", "", "https://example.invalid/releases/plugin.so"),
  };
  entries[2].is_core_plugin = true;
  entries[3].artifact_source = "github_release_asset";

  for (size_t i = 0; i < entries.size(); ++i) {
    entries[i].selector = makePluginSelector(entries[i], i);
  }
  return entries;
}

TEST(PluginSelectorTest, IdIsTheSelectorWhenPresent) {
  EXPECT_EQ(makePluginSelector("com.example.plugin", "/plugins/example.so", ""),
            "com.example.plugin");
}

TEST(PluginSelectorTest, IdLessEntryIsAddressedByItsPath) {
  EXPECT_EQ(makePluginSelector("", "/plugins/unnamed.so", ""),
            "path:/plugins/unnamed.so");
}

TEST(PluginSelectorTest, EntryWithOnlyAnAssetUrlIsAddressedByThatUrl) {
  EXPECT_EQ(makePluginSelector("", "", "https://example.invalid/plugin.so"),
            "url:https://example.invalid/plugin.so");
}

TEST(PluginSelectorTest, EveryEntryGetsANonEmptySelector) {
  const auto entries = makeMixedRegistry();
  for (const auto& entry : entries) {
    EXPECT_FALSE(entry.selector.empty());
    // Placeholder text is banned from selector fields.
    EXPECT_EQ(entry.selector.find('<'), std::string::npos);
  }
}

// An entry with no identity at all (a hand-edited registry) still has to be
// addressable, or the user cannot remove it.
TEST(PluginSelectorTest, IdentityLessEntryFallsBackToItsPosition) {
  PluginRegistryEntryInfo entry;
  EXPECT_EQ(makePluginSelector(entry, 3), "index:3");
}

TEST(PluginSelectorTest, ResolvesById) {
  const auto entries = makeMixedRegistry();
  const auto resolution = resolvePluginSelector(entries, "com.example.plugin");
  ASSERT_EQ(resolution.status, PluginSelectorStatus::Resolved);
  EXPECT_EQ(resolution.entry_index, 0U);
  EXPECT_EQ(resolution.entry.plugin_id, "com.example.plugin");
}

TEST(PluginSelectorTest, ResolvesByPathSelector) {
  const auto entries = makeMixedRegistry();
  const auto resolution =
      resolvePluginSelector(entries, "path:/plugins/unnamed.so");
  ASSERT_EQ(resolution.status, PluginSelectorStatus::Resolved);
  EXPECT_EQ(resolution.entry_index, 1U);
}

// A path pasted out of the listing's `path:` field resolves without the user
// re-assembling the prefix.
TEST(PluginSelectorTest, ResolvesByBarePath) {
  const auto entries = makeMixedRegistry();
  const auto resolution = resolvePluginSelector(entries, "/plugins/unnamed.so");
  ASSERT_EQ(resolution.status, PluginSelectorStatus::Resolved);
  EXPECT_EQ(resolution.entry_index, 1U);
}

TEST(PluginSelectorTest, ResolvesByUrlSelector) {
  const auto entries = makeMixedRegistry();
  const auto resolution = resolvePluginSelector(
      entries, "url:https://example.invalid/releases/plugin.so");
  ASSERT_EQ(resolution.status, PluginSelectorStatus::Resolved);
  EXPECT_EQ(resolution.entry_index, 3U);
}

TEST(PluginSelectorTest, ResolvesByBareAssetUrl) {
  const auto entries = makeMixedRegistry();
  const auto resolution = resolvePluginSelector(
      entries, "https://example.invalid/releases/plugin.so");
  ASSERT_EQ(resolution.status, PluginSelectorStatus::Resolved);
  EXPECT_EQ(resolution.entry_index, 3U);
}

TEST(PluginSelectorTest, EverySelectorPrintedResolvesBackToItsEntry) {
  const auto entries = makeMixedRegistry();
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto resolution = resolvePluginSelector(entries, entries[i].selector);
    ASSERT_EQ(resolution.status, PluginSelectorStatus::Resolved)
        << "selector: " << entries[i].selector;
    EXPECT_EQ(resolution.entry_index, i);
  }
}

TEST(PluginSelectorTest, UnknownSelectorIsNotFound) {
  const auto entries = makeMixedRegistry();
  EXPECT_EQ(resolvePluginSelector(entries, "com.example.missing").status,
            PluginSelectorStatus::NotFound);
  EXPECT_EQ(resolvePluginSelector(entries, "").status,
            PluginSelectorStatus::NotFound);
}

// Two entries sharing a path must not be resolved by guessing; the caller is
// told which unambiguous selectors to choose from.
TEST(PluginSelectorTest, SharedPathIsAmbiguousAndListsBothCandidates) {
  std::vector<PluginRegistryEntryInfo> entries = {
      makeEntry("com.example.first", "/plugins/shared.so"),
      makeEntry("", "/plugins/shared.so"),
  };
  for (size_t i = 0; i < entries.size(); ++i) {
    entries[i].selector = makePluginSelector(entries[i], i);
  }

  const auto resolution = resolvePluginSelector(entries, "/plugins/shared.so");
  ASSERT_EQ(resolution.status, PluginSelectorStatus::Ambiguous);
  ASSERT_EQ(resolution.candidates.size(), 2U);
  EXPECT_EQ(resolution.candidates[0], "com.example.first");
  EXPECT_EQ(resolution.candidates[1], "path:/plugins/shared.so");

  const std::string message = describeAmbiguousPluginSelector(
      "/plugins/shared.so", resolution.candidates);
  EXPECT_NE(message.find("com.example.first"), std::string::npos);
  EXPECT_NE(message.find("path:/plugins/shared.so"), std::string::npos);
}

// Two id-less entries on the same path would print the same selector, so the
// positional form is offered for the one that cannot be named by it.
TEST(PluginSelectorTest, IndistinguishableEntriesOfferPositionalCandidates) {
  std::vector<PluginRegistryEntryInfo> entries = {
      makeEntry("", "/plugins/shared.so"),
      makeEntry("", "/plugins/shared.so"),
  };
  for (size_t i = 0; i < entries.size(); ++i) {
    entries[i].selector = makePluginSelector(entries[i], i);
  }

  const auto resolution =
      resolvePluginSelector(entries, "path:/plugins/shared.so");
  ASSERT_EQ(resolution.status, PluginSelectorStatus::Ambiguous);
  ASSERT_EQ(resolution.candidates.size(), 2U);
  EXPECT_EQ(resolution.candidates[1], "index:1");

  const auto positional = resolvePluginSelector(entries, "index:1");
  ASSERT_EQ(positional.status, PluginSelectorStatus::Resolved);
  EXPECT_EQ(positional.entry_index, 1U);
}

// A plugin added by file before its id was known lists under its runtime id,
// so that id must resolve onto the id-less registry row recording its path —
// through the same overload every command uses, so `remove --dry-run` and
// `remove` can never disagree about it.
TEST(PluginSelectorTest, RuntimeIdResolvesOntoTheIdLessRowByPath) {
  const auto entries = makeMixedRegistry();

  LoadedPluginInfo loaded;
  loaded.plugin_id = "com.example.runtime";
  loaded.path = "/plugins/unnamed.so";

  const auto resolution =
      resolvePluginSelector(entries, {loaded}, "com.example.runtime");
  ASSERT_EQ(resolution.status, PluginSelectorStatus::Resolved);
  EXPECT_EQ(resolution.entry_index, 1U);
  EXPECT_TRUE(resolution.resolved_via_runtime_id);

  // A registry match never reports the runtime tier, and an id no loaded
  // plugin carries stays not-found.
  const auto direct =
      resolvePluginSelector(entries, {loaded}, "com.example.plugin");
  ASSERT_EQ(direct.status, PluginSelectorStatus::Resolved);
  EXPECT_FALSE(direct.resolved_via_runtime_id);
  EXPECT_EQ(
      resolvePluginSelector(entries, {loaded}, "com.example.absent").status,
      PluginSelectorStatus::NotFound);
}

// An id must win over a path that happens to spell the same string.
TEST(PluginSelectorTest, IdsOutrankPathsForUnprefixedSelectors) {
  std::vector<PluginRegistryEntryInfo> entries = {
      makeEntry("/plugins/example.so", "/plugins/other.so"),
      makeEntry("com.example.second", "/plugins/example.so"),
  };
  for (size_t i = 0; i < entries.size(); ++i) {
    entries[i].selector = makePluginSelector(entries[i], i);
  }

  const auto resolution = resolvePluginSelector(entries, "/plugins/example.so");
  ASSERT_EQ(resolution.status, PluginSelectorStatus::Resolved);
  EXPECT_EQ(resolution.entry_index, 0U);
}

}  // namespace
}  // namespace orc::presenters
