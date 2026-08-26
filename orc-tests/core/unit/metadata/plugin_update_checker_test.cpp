/*
 * File:        plugin_update_checker_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     Unit tests for the plugin latest-release update checker
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../orc/core/include/plugin_update_checker.h"

#include <gtest/gtest.h>

#include <string>

namespace orc_unit_test {
namespace {

using orc::PluginUpdateChecker;

// Deterministic, network-free transport stub for the update checker.
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

const char* kReleaseJson = R"json({
  "tag_name": "v1.0.6",
  "name": "Release 1.0.6",
  "assets": []
})json";

// --- API URL derivation -----------------------------------------------------

TEST(PluginUpdateChecker_ApiUrl, DerivesLatestReleaseUrlFromRepoUrl) {
  const auto url = PluginUpdateChecker::github_latest_release_api_url(
      "https://github.com/decode-orc/orc-plugin_skeleton");
  ASSERT_TRUE(url.has_value());
  EXPECT_EQ(*url,
            "https://api.github.com/repos/decode-orc/orc-plugin_skeleton/"
            "releases/latest");
}

TEST(PluginUpdateChecker_ApiUrl, StripsDotGitSuffixAndTrailingPath) {
  EXPECT_EQ(*PluginUpdateChecker::github_latest_release_api_url(
                "https://github.com/acme/plugin.git"),
            "https://api.github.com/repos/acme/plugin/releases/latest");
  EXPECT_EQ(*PluginUpdateChecker::github_latest_release_api_url(
                "https://github.com/acme/plugin/releases"),
            "https://api.github.com/repos/acme/plugin/releases/latest");
  EXPECT_EQ(*PluginUpdateChecker::github_latest_release_api_url(
                "https://github.com/acme/plugin/"),
            "https://api.github.com/repos/acme/plugin/releases/latest");
}

TEST(PluginUpdateChecker_ApiUrl, RejectsNonGitHubOrMalformedUrls) {
  EXPECT_FALSE(PluginUpdateChecker::github_latest_release_api_url(
                   "https://gitlab.com/acme/plugin")
                   .has_value());
  EXPECT_FALSE(
      PluginUpdateChecker::github_latest_release_api_url("").has_value());
  EXPECT_FALSE(PluginUpdateChecker::github_latest_release_api_url(
                   "https://github.com/owner-only")
                   .has_value());
}

// --- Tag parsing and version normalisation ----------------------------------

TEST(PluginUpdateChecker_Parse, ExtractsTagNameFromReleaseJson) {
  EXPECT_EQ(PluginUpdateChecker::parse_latest_release_tag(kReleaseJson),
            "v1.0.6");
  EXPECT_EQ(PluginUpdateChecker::parse_latest_release_tag("{}"), "");
}

TEST(PluginUpdateChecker_Parse, NormalizeStripsLeadingVOnlyBeforeDigits) {
  EXPECT_EQ(PluginUpdateChecker::normalize_version("v1.0.6"), "1.0.6");
  EXPECT_EQ(PluginUpdateChecker::normalize_version("V2.0"), "2.0");
  EXPECT_EQ(PluginUpdateChecker::normalize_version(" 1.2.3 "), "1.2.3");
  EXPECT_EQ(PluginUpdateChecker::normalize_version("vintage"), "vintage");
  EXPECT_EQ(PluginUpdateChecker::normalize_version(""), "");
}

// --- Version comparison -----------------------------------------------------

TEST(PluginUpdateChecker_Compare, ComparesNumericSegments) {
  EXPECT_LT(PluginUpdateChecker::compare_versions("1.0.5", "1.0.6"), 0);
  EXPECT_GT(PluginUpdateChecker::compare_versions("1.0.10", "1.0.9"), 0);
  EXPECT_EQ(PluginUpdateChecker::compare_versions("1.0.5", "1.0.5"), 0);
  EXPECT_EQ(PluginUpdateChecker::compare_versions("1.0", "1.0.0"), 0);
  EXPECT_LT(PluginUpdateChecker::compare_versions("1.9", "2.0"), 0);
}

TEST(PluginUpdateChecker_Compare, NonNumericRemaindersFallBackToStrings) {
  EXPECT_LT(PluginUpdateChecker::compare_versions("1.0.5-rc1", "1.0.5-rc2"), 0);
  EXPECT_NE(PluginUpdateChecker::compare_versions("1.0.5", "1.0.5-rc1"), 0);
}

// --- End-to-end check with stubbed transport --------------------------------

TEST(PluginUpdateChecker_Check, ReportsUpToDateWhenInstalledMatchesLatest) {
  StubFetcher fetcher;
  fetcher.result = ok_body(kReleaseJson);
  const PluginUpdateChecker checker(fetcher);

  const auto result = checker.check("https://github.com/acme/plugin", "1.0.6");
  EXPECT_EQ(result.status, PluginUpdateChecker::Status::UpToDate);
  EXPECT_EQ(result.latest_tag, "v1.0.6");
  EXPECT_EQ(result.latest_version, "1.0.6");
  EXPECT_EQ(fetcher.last_url,
            "https://api.github.com/repos/acme/plugin/releases/latest");
}

TEST(PluginUpdateChecker_Check, ReportsUpToDateWhenInstalledIsNewer) {
  StubFetcher fetcher;
  fetcher.result = ok_body(kReleaseJson);
  const PluginUpdateChecker checker(fetcher);

  const auto result = checker.check("https://github.com/acme/plugin", "1.1.0");
  EXPECT_EQ(result.status, PluginUpdateChecker::Status::UpToDate);
}

TEST(PluginUpdateChecker_Check, ReportsUpdateAvailableWhenLatestIsNewer) {
  StubFetcher fetcher;
  fetcher.result = ok_body(kReleaseJson);
  const PluginUpdateChecker checker(fetcher);

  // Installed version may carry the tag's 'v' prefix; comparison normalises.
  const auto result = checker.check("https://github.com/acme/plugin", "v1.0.5");
  EXPECT_EQ(result.status, PluginUpdateChecker::Status::UpdateAvailable);
  EXPECT_EQ(result.latest_version, "1.0.6");
}

TEST(PluginUpdateChecker_Check, ReportsUnreachableOnTransportFailure) {
  StubFetcher fetcher;
  fetcher.result = transport_error();
  const PluginUpdateChecker checker(fetcher);

  const auto result = checker.check("https://github.com/acme/plugin", "1.0.5");
  EXPECT_EQ(result.status, PluginUpdateChecker::Status::Unreachable);
  EXPECT_FALSE(result.message.empty());
}

TEST(PluginUpdateChecker_Check, ReportsUnreachableWhenNoTagInResponse) {
  StubFetcher fetcher;
  fetcher.result = ok_body(R"json({"message": "Not Found"})json");
  const PluginUpdateChecker checker(fetcher);

  const auto result = checker.check("https://github.com/acme/plugin", "1.0.5");
  EXPECT_EQ(result.status, PluginUpdateChecker::Status::Unreachable);
}

TEST(PluginUpdateChecker_Check, ReportsUnknownWhenInstalledVersionMissing) {
  StubFetcher fetcher;
  fetcher.result = ok_body(kReleaseJson);
  const PluginUpdateChecker checker(fetcher);

  const auto result = checker.check("https://github.com/acme/plugin", "");
  EXPECT_EQ(result.status, PluginUpdateChecker::Status::Unknown);
  EXPECT_EQ(result.latest_version, "1.0.6");
}

TEST(PluginUpdateChecker_Check, ReportsNotApplicableWithoutGitHubRepo) {
  StubFetcher fetcher;
  const PluginUpdateChecker checker(fetcher);

  EXPECT_EQ(checker.check("", "1.0.0").status,
            PluginUpdateChecker::Status::NotApplicable);
  EXPECT_EQ(checker.check("https://example.com/repo", "1.0.0").status,
            PluginUpdateChecker::Status::NotApplicable);
  EXPECT_EQ(fetcher.calls, 0);
}

}  // namespace
}  // namespace orc_unit_test
