/*
 * File:        plugin_index_client_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     Unit tests for the curated plugin index parser and client
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../orc/core/include/plugin_index_client.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "../../../orc/core/include/plugin_index.h"

namespace orc_unit_test {
namespace {

// Deterministic, network-free transport stub for the index client.
class StubFetcher : public orc::IHttpFetcher {
 public:
  orc::HttpFetchResult result;
  mutable int calls = 0;
  mutable std::string last_url;

  orc::HttpFetchResult fetch(const std::string& url) const override {
    ++calls;
    last_url = url;
    return result;
  }
};

orc::HttpFetchResult ok_body(std::string body) {
  orc::HttpFetchResult r;
  r.success = true;
  r.status_code = 200;
  r.body = std::move(body);
  return r;
}

orc::HttpFetchResult transport_error() {
  orc::HttpFetchResult r;
  r.success = false;
  r.error_message = "Failed to fetch URL: could not resolve host";
  return r;
}

const char* kValidIndex = R"yaml(
registry_schema: 2
plugins:
  - id: acme.deinterlace
    display_name: ACME Deinterlacer
    description: High quality motion-adaptive deinterlacing
    maintainer: ACME Corp
    license_spdx: GPL-3.0-or-later
    source_repo_url: https://github.com/acme/orc-plugin_deinterlace
    tags: [transform, video]
)yaml";

}  // namespace

TEST(PluginIndexParseTest, ReadsStructuredEntries) {
  const auto parsed = orc::parse_plugin_index_yaml(kValidIndex);
  ASSERT_TRUE(parsed.success);
  ASSERT_EQ(parsed.index.schema_version, 2);
  ASSERT_EQ(parsed.index.entries.size(), 1U);

  const auto& entry = parsed.index.entries.front();
  EXPECT_EQ(entry.id, "acme.deinterlace");
  EXPECT_EQ(entry.display_name, "ACME Deinterlacer");
  EXPECT_EQ(entry.license_spdx, "GPL-3.0-or-later");
  EXPECT_EQ(entry.source_repo_url,
            "https://github.com/acme/orc-plugin_deinterlace");
  ASSERT_EQ(entry.tags.size(), 2U);
  EXPECT_EQ(entry.tags[0], "transform");
}

TEST(PluginIndexParseTest, IgnoresUnknownFieldsWithinKnownSchemaMajor) {
  // A newer minor revision adds fields the host does not know; they must be
  // ignored, not rejected.
  const char* yaml = R"yaml(
registry_schema: 2
index_generated_at: 2026-07-30T00:00:00Z
plugins:
  - id: acme.tool
    display_name: Tool
    license_spdx: MIT
    source_repo_url: https://github.com/acme/orc-plugin_tool
    future_top_level_field: whatever
)yaml";
  const auto parsed = orc::parse_plugin_index_yaml(yaml);
  ASSERT_TRUE(parsed.success);
  ASSERT_EQ(parsed.index.entries.size(), 1U);
  EXPECT_EQ(parsed.index.entries.front().id, "acme.tool");
}

TEST(PluginIndexParseTest, IgnoresLegacyPinnedArtifacts) {
  // A schema-1 index pinned artifacts per platform; the entry still parses
  // (id, license, repo) and the pin list is simply ignored.
  const char* yaml = R"yaml(
registry_schema: 1
plugins:
  - id: acme.tool
    license_spdx: MIT
    source_repo_url: https://github.com/acme/orc-plugin_tool
    artifacts:
      - platform: linux
        host_abi: 8
        url: https://example.invalid/a.so
        sha256: 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
)yaml";
  const auto parsed = orc::parse_plugin_index_yaml(yaml);
  ASSERT_TRUE(parsed.success);
  EXPECT_EQ(parsed.index.schema_version, 1);
  ASSERT_EQ(parsed.index.entries.size(), 1U);
  EXPECT_EQ(parsed.index.entries.front().source_repo_url,
            "https://github.com/acme/orc-plugin_tool");
}

TEST(PluginIndexParseTest, ToleratesNewerSchemaMajorWithWarning) {
  const char* yaml = R"yaml(
registry_schema: 3
plugins:
  - id: acme.tool
    license_spdx: MIT
    source_repo_url: https://github.com/acme/orc-plugin_tool
)yaml";
  const auto parsed = orc::parse_plugin_index_yaml(yaml);
  ASSERT_TRUE(parsed.success);
  EXPECT_EQ(parsed.index.schema_version, 3);
  EXPECT_FALSE(parsed.warnings.empty());
}

TEST(PluginIndexParseTest, MalformedYamlFails) {
  const auto parsed = orc::parse_plugin_index_yaml("plugins: [ : : :");
  EXPECT_FALSE(parsed.success);
  EXPECT_FALSE(parsed.error_message.empty());
}

TEST(PluginIndexParseTest, EmptyIndexIsValid) {
  const auto parsed = orc::parse_plugin_index_yaml("registry_schema: 2\n");
  EXPECT_TRUE(parsed.success);
  EXPECT_TRUE(parsed.index.entries.empty());
}

TEST(PluginIndexParseTest, WarnsOnMissingLicenseAndRepoUrl) {
  const char* yaml = R"yaml(
registry_schema: 2
plugins:
  - id: acme.tool
    display_name: Tool
)yaml";
  const auto parsed = orc::parse_plugin_index_yaml(yaml);
  ASSERT_TRUE(parsed.success);
  EXPECT_GE(parsed.warnings.size(), 2U);  // missing license + missing repo url
}

TEST(PluginIndexSearchTest, CaseInsensitiveAcrossFields) {
  const auto parsed = orc::parse_plugin_index_yaml(kValidIndex);
  EXPECT_EQ(orc::PluginIndexClient::search(parsed.index, "DEINTERLACE").size(),
            1U);
  EXPECT_EQ(orc::PluginIndexClient::search(parsed.index, "motion").size(), 1U);
  EXPECT_EQ(orc::PluginIndexClient::search(parsed.index, "video").size(), 1U);
  EXPECT_EQ(orc::PluginIndexClient::search(parsed.index, "nomatch").size(), 0U);
  EXPECT_EQ(orc::PluginIndexClient::search(parsed.index, "").size(), 1U);
}

TEST(PluginIndexSearchTest, FindByExactId) {
  const auto parsed = orc::parse_plugin_index_yaml(kValidIndex);
  EXPECT_NE(orc::PluginIndexClient::find(parsed.index, "acme.deinterlace"),
            nullptr);
  EXPECT_EQ(orc::PluginIndexClient::find(parsed.index, "missing"), nullptr);
}

TEST(PluginIndexClientTest, FetchSuccessParsesAndWritesCache) {
  StubFetcher fetcher;
  fetcher.result = ok_body(kValidIndex);
  std::optional<std::string> saved;
  orc::PluginIndexClient client(
      fetcher, []() -> std::optional<std::string> { return std::nullopt; },
      [&saved](const std::string& body) { saved = body; });

  const auto outcome = client.refresh("https://example.invalid/index.yaml");
  EXPECT_TRUE(outcome.success);
  EXPECT_FALSE(outcome.from_cache);
  EXPECT_FALSE(outcome.offline);
  ASSERT_EQ(outcome.index.entries.size(), 1U);
  ASSERT_TRUE(saved.has_value());
  EXPECT_EQ(*saved, kValidIndex);
  EXPECT_EQ(fetcher.last_url, "https://example.invalid/index.yaml");
}

TEST(PluginIndexClientTest, NetworkFailureFallsBackToCache) {
  StubFetcher fetcher;
  fetcher.result = transport_error();
  bool saver_called = false;
  orc::PluginIndexClient client(
      fetcher,
      []() -> std::optional<std::string> { return std::string(kValidIndex); },
      [&saver_called](const std::string&) { saver_called = true; });

  const auto outcome = client.refresh("https://example.invalid/index.yaml");
  EXPECT_TRUE(outcome.success);
  EXPECT_TRUE(outcome.from_cache);
  EXPECT_TRUE(outcome.offline);
  ASSERT_EQ(outcome.index.entries.size(), 1U);
  EXPECT_FALSE(saver_called);  // never overwrite cache from a failed fetch
}

TEST(PluginIndexClientTest, NetworkFailureAndNoCacheReportsError) {
  StubFetcher fetcher;
  fetcher.result = transport_error();
  orc::PluginIndexClient client(
      fetcher, []() -> std::optional<std::string> { return std::nullopt; },
      [](const std::string&) {});

  const auto outcome = client.refresh("https://example.invalid/index.yaml");
  EXPECT_FALSE(outcome.success);
  EXPECT_TRUE(outcome.offline);
  EXPECT_FALSE(outcome.error_message.empty());
}

TEST(PluginIndexClientTest, UnparseableFetchFallsBackToCache) {
  StubFetcher fetcher;
  fetcher.result = ok_body("plugins: [ : : :");  // malformed
  orc::PluginIndexClient client(
      fetcher,
      []() -> std::optional<std::string> { return std::string(kValidIndex); },
      [](const std::string&) {});

  const auto outcome = client.refresh("https://example.invalid/index.yaml");
  EXPECT_TRUE(outcome.success);
  EXPECT_TRUE(outcome.from_cache);
  ASSERT_EQ(outcome.index.entries.size(), 1U);
}

}  // namespace orc_unit_test
