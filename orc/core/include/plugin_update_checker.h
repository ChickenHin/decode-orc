/*
 * File:        plugin_update_checker.h
 * Module:      orc-core
 * Purpose:     Check installed remote plugins against their latest release
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#if defined(ORC_GUI_BUILD) || defined(ORC_CLI_BUILD)
#error \
    "plugin_update_checker.h is a core-only header. Access update checks through ProjectPresenter."
#endif

#include <optional>
#include <string>

#include "http_fetcher.h"

namespace orc {

/**
 * @brief Queries a plugin's source repository for its latest published release
 * and compares it against the locally installed version.
 *
 * The check is transport-agnostic: an injected IHttpFetcher performs the
 * GitHub API GET, so the fetch/parse/compare logic is unit-testable without
 * network access. All parsing and comparison helpers are pure static
 * functions.
 *
 * Thread safety: the class holds only a reference to the fetcher; safety is
 * that of the injected fetcher.
 */
class PluginUpdateChecker {
 public:
  explicit PluginUpdateChecker(const IHttpFetcher& fetcher)
      : fetcher_(fetcher) {}

  enum class Status {
    UpToDate,         ///< Installed version >= latest published release.
    UpdateAvailable,  ///< A newer release is published upstream.
    Unreachable,      ///< The release information could not be fetched.
    Unknown,          ///< Latest release fetched, but no installed version to
                      ///< compare against.
    NotApplicable,    ///< The plugin has no supported source repository URL.
  };

  struct Result {
    Status status = Status::NotApplicable;
    std::string latest_tag;      ///< Raw release tag (e.g. "v1.0.6").
    std::string latest_version;  ///< Tag normalised for display ("1.0.6").
    std::string message;         ///< Failure detail when Unreachable.
  };

  /**
   * @brief Check one plugin's source repository for a newer release.
   *
   * @param source_repo_url GitHub repository URL recorded for the plugin.
   * @param installed_version Locally installed version ("1.0.5" or "v1.0.5").
   */
  Result check(const std::string& source_repo_url,
               const std::string& installed_version) const;

  /**
   * @brief Derive the GitHub latest-release API URL for a repository URL.
   * @return nullopt when the URL is not a recognisable GitHub repository.
   */
  static std::optional<std::string> github_latest_release_api_url(
      const std::string& source_repo_url);

  /// Extract "tag_name" from a GitHub release JSON document (empty if absent).
  static std::string parse_latest_release_tag(const std::string& release_json);

  /// Strip a leading 'v'/'V' and surrounding whitespace from a release tag.
  static std::string normalize_version(const std::string& tag);

  /**
   * @brief Compare two normalised version strings.
   *
   * Dot-separated segments are compared numerically by their leading digits,
   * with a lexicographic tie-break on any non-numeric remainder (so
   * "1.0.10" > "1.0.9" and "1.0.5" > "1.0.5-rc1" is not attempted — equal
   * numeric parts fall back to string comparison of the remainder).
   *
   * @return <0 when a < b, 0 when equal, >0 when a > b.
   */
  static int compare_versions(const std::string& a, const std::string& b);

 private:
  const IHttpFetcher& fetcher_;
};

}  // namespace orc
