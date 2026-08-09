/*
 * File:        curl_http_fetcher.cpp
 * Module:      orc-core
 * Purpose:     libcurl-backed IHttpFetcher used for plugin-index discovery
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "include/curl_http_fetcher.h"

#include <curl/curl.h>

namespace orc {
namespace {

size_t write_string_callback(void* contents, size_t size, size_t nmemb,
                             std::string* output) {
  const size_t real_size = size * nmemb;
  if (!output) {
    return 0;
  }
  output->append(static_cast<const char*>(contents), real_size);
  return real_size;
}

}  // namespace

HttpFetchResult CurlHttpFetcher::fetch(const std::string& url) const {
  HttpFetchResult result;

  CURL* curl = curl_easy_init();
  if (!curl) {
    result.error_message = "Failed to initialize libcurl";
    return result;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   "decode-orc/1.0 (+https://github.com/decode-orc/decode-orc)");

  const CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    result.body.clear();
    result.error_message =
        std::string("Failed to fetch URL: ") + curl_easy_strerror(res);
    curl_easy_cleanup(curl);
    return result;
  }

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status_code);
  curl_easy_cleanup(curl);

  if (result.status_code < 200 || result.status_code >= 300) {
    result.body.clear();
    result.error_message = "HTTP error " + std::to_string(result.status_code) +
                           " while fetching URL";
    return result;
  }

  result.success = true;
  return result;
}

}  // namespace orc
