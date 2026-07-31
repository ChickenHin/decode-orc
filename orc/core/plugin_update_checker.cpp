/*
 * File:        plugin_update_checker.cpp
 * Module:      orc-core
 * Purpose:     Check installed remote plugins against their latest release
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "include/plugin_update_checker.h"

#include <cctype>
#include <regex>

namespace orc {

namespace {

// Minimal JSON string unescape for values extracted by regex (GitHub tag
// names may contain escaped slashes).
std::string unescape_json_string(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\\' && i + 1 < value.size()) {
      out.push_back(value[i + 1]);
      ++i;
    } else {
      out.push_back(value[i]);
    }
  }
  return out;
}

// Split one dot-separated version segment into its leading numeric value and
// the non-numeric remainder ("10-rc1" -> {10, "-rc1"}).
struct VersionSegment {
  long long number = 0;  // NOLINT(google-runtime-int): local parse value
  bool has_number = false;
  std::string remainder;
};

VersionSegment parse_segment(const std::string& segment) {
  VersionSegment result;
  size_t pos = 0;
  while (pos < segment.size() &&
         (std::isdigit(static_cast<unsigned char>(segment[pos])) != 0)) {
    ++pos;
  }
  if (pos > 0) {
    result.has_number = true;
    result.number = std::stoll(segment.substr(0, pos));
  }
  result.remainder = segment.substr(pos);
  return result;
}

}  // namespace

std::optional<std::string> PluginUpdateChecker::github_latest_release_api_url(
    const std::string& source_repo_url) {
  const std::regex repo_regex(
      R"(^https?://(?:www\.)?github\.com/([^/]+)/([^/?#]+?)(?:\.git)?/?(?:[/?#].*)?$)");
  std::smatch repo_match;
  if (!std::regex_match(source_repo_url, repo_match, repo_regex)) {
    return std::nullopt;
  }
  return "https://api.github.com/repos/" + repo_match[1].str() + "/" +
         repo_match[2].str() + "/releases/latest";
}

std::string PluginUpdateChecker::parse_latest_release_tag(
    const std::string& release_json) {
  const std::regex tag_name_regex(R"json("tag_name"\s*:\s*"([^"]+)")json");
  std::smatch tag_match;
  if (!std::regex_search(release_json, tag_match, tag_name_regex)) {
    return std::string();
  }
  return unescape_json_string(tag_match[1].str());
}

std::string PluginUpdateChecker::normalize_version(const std::string& tag) {
  size_t begin = 0;
  size_t end = tag.size();
  while (begin < end &&
         (std::isspace(static_cast<unsigned char>(tag[begin])) != 0)) {
    ++begin;
  }
  while (end > begin &&
         (std::isspace(static_cast<unsigned char>(tag[end - 1])) != 0)) {
    --end;
  }
  if (begin < end && (tag[begin] == 'v' || tag[begin] == 'V') &&
      begin + 1 < end &&
      (std::isdigit(static_cast<unsigned char>(tag[begin + 1])) != 0)) {
    ++begin;
  }
  return tag.substr(begin, end - begin);
}

int PluginUpdateChecker::compare_versions(const std::string& a,
                                          const std::string& b) {
  size_t pos_a = 0;
  size_t pos_b = 0;
  while (pos_a < a.size() || pos_b < b.size()) {
    const size_t dot_a = a.find('.', pos_a);
    const size_t dot_b = b.find('.', pos_b);
    const std::string seg_a =
        pos_a < a.size()
            ? a.substr(pos_a,
                       (dot_a == std::string::npos ? a.size() : dot_a) - pos_a)
            : std::string();
    const std::string seg_b =
        pos_b < b.size()
            ? b.substr(pos_b,
                       (dot_b == std::string::npos ? b.size() : dot_b) - pos_b)
            : std::string();

    const VersionSegment parsed_a = parse_segment(seg_a);
    const VersionSegment parsed_b = parse_segment(seg_b);

    // Missing segments compare as 0 ("1.0" == "1.0.0").
    const auto num_a = parsed_a.has_number ? parsed_a.number : 0;
    const auto num_b = parsed_b.has_number ? parsed_b.number : 0;
    if (num_a != num_b) {
      return num_a < num_b ? -1 : 1;
    }
    if (parsed_a.remainder != parsed_b.remainder) {
      return parsed_a.remainder < parsed_b.remainder ? -1 : 1;
    }

    pos_a = (dot_a == std::string::npos) ? a.size() : dot_a + 1;
    pos_b = (dot_b == std::string::npos) ? b.size() : dot_b + 1;
  }
  return 0;
}

PluginUpdateChecker::Result PluginUpdateChecker::check(
    const std::string& source_repo_url,
    const std::string& installed_version) const {
  Result result;

  const auto api_url = github_latest_release_api_url(source_repo_url);
  if (!api_url) {
    result.status = Status::NotApplicable;
    result.message = source_repo_url.empty()
                         ? "No source repository recorded"
                         : "Source repository is not a GitHub URL";
    return result;
  }

  const HttpFetchResult fetched = fetcher_.fetch(*api_url);
  if (!fetched.success) {
    result.status = Status::Unreachable;
    result.message = fetched.error_message.empty()
                         ? "Failed to fetch release information"
                         : fetched.error_message;
    return result;
  }

  result.latest_tag = parse_latest_release_tag(fetched.body);
  if (result.latest_tag.empty()) {
    result.status = Status::Unreachable;
    result.message = "No release tag found in the repository's latest release";
    return result;
  }
  result.latest_version = normalize_version(result.latest_tag);

  const std::string installed = normalize_version(installed_version);
  if (installed.empty()) {
    result.status = Status::Unknown;
    return result;
  }

  result.status = compare_versions(installed, result.latest_version) < 0
                      ? Status::UpdateAvailable
                      : Status::UpToDate;
  return result;
}

}  // namespace orc
