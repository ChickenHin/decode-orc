/*
 * File:        observation_cache_retention_fs_test.cpp
 * Module:      orc-core functional tests
 * Purpose:     Filesystem half of the observation sidecar retention policy
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * Functional (not unit): listSidecars/removeSidecar/enforceSidecarRetention
 * exist to touch real files, which unit tests may not do (AGENTS.md §4.2). The
 * decision logic they drive is unit-tested separately in
 * observation_cache_retention_test.cpp.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "observation_cache_retention.h"

namespace orc {
namespace {

using namespace std::chrono_literals;

class SidecarRetentionFsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("orc-obs-gc-") + info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  // Create a file of @p bytes, aged @p age.
  std::string make_file(const std::string& name, std::chrono::hours age,
                        std::size_t bytes = 8) {
    const auto path = dir_ / name;
    {
      std::ofstream out(path, std::ios::binary);
      out << std::string(bytes, 'x');
    }
    std::filesystem::last_write_time(
        path, std::filesystem::file_time_type::clock::now() - age);
    return path.string();
  }

  std::string make_sidecar(const std::string& stem, std::chrono::hours age,
                           std::size_t bytes = 8) {
    return make_file(stem + kSidecarSuffix, age, bytes);
  }

  std::vector<std::string> remaining_names() const {
    std::vector<std::string> names;
    for (const auto& item : std::filesystem::directory_iterator(dir_)) {
      names.push_back(item.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
  }

  std::filesystem::path dir_;
};

TEST_F(SidecarRetentionFsTest, ListsOnlySidecarsAndReportsTheirAgeAndSize) {
  make_sidecar("a", 1h, 100);
  make_sidecar("b", 5h, 200);
  // Neither of these is a sidecar, so neither may be listed (nor, later,
  // deleted): the cache directory is not ours to sweep wholesale.
  make_file("notes.txt", 1h);
  make_file("something.sqlite", 1h);

  auto entries = listSidecars(dir_.string());
  ASSERT_EQ(entries.size(), 2u);
  std::sort(entries.begin(), entries.end(),
            [](const SidecarEntry& x, const SidecarEntry& y) {
              return x.path < y.path;
            });
  EXPECT_EQ(std::filesystem::path(entries[0].path).filename().string(),
            std::string("a") + kSidecarSuffix);
  EXPECT_EQ(entries[0].bytes, 100u);
  EXPECT_GT(entries[0].last_used, entries[1].last_used);  // "a" is newer
}

TEST_F(SidecarRetentionFsTest, CompanionActivityCountsAsUseOfTheSidecar) {
  // The database itself looks stale, but its write-ahead log was written
  // minutes ago — the sidecar is in use and must not read as abandoned.
  const auto path = make_sidecar("busy", 24h * 60);
  make_file(std::string("busy") + kSidecarSuffix + "-wal", 1h, 500);

  const auto entries = listSidecars(dir_.string());
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].path, path);
  EXPECT_EQ(entries[0].bytes, 8u + 500u);  // companion bytes counted too
  EXPECT_GT(entries[0].last_used, std::chrono::system_clock::now() - 24h * 2);
}

TEST_F(SidecarRetentionFsTest, RemoveTakesCompanionsAndReportsBytesReclaimed) {
  const auto path = make_sidecar("gone", 1h, 100);
  make_file(std::string("gone") + kSidecarSuffix + "-wal", 1h, 20);
  make_file(std::string("gone") + kSidecarSuffix + "-shm", 1h, 30);

  EXPECT_EQ(removeSidecar(path), 150u);
  EXPECT_TRUE(remaining_names().empty());
}

TEST_F(SidecarRetentionFsTest, TouchRenewsASidecarSoItReadsAsRecentlyUsed) {
  const auto path = make_sidecar("old", 24h * 60);
  touchSidecar(path);

  const auto entries = listSidecars(dir_.string());
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_GT(entries[0].last_used, std::chrono::system_clock::now() - 1h);
}

TEST_F(SidecarRetentionFsTest, EnforceAppliesBothLimitsAndSparesTheKeptPath) {
  const auto keep = make_sidecar("keep", 24h * 90);  // ancient, but in use
  make_sidecar("fresh1", 1h);
  make_sidecar("fresh2", 2h);
  make_sidecar("crowded_out", 3h);
  make_sidecar("expired", 24h * 40);
  make_file("notes.txt", 1h);

  // Room for the kept sidecar plus two others; anything past 30 days goes.
  const SidecarRetentionPolicy policy{/*max_entries=*/3, 24h * 30};
  EXPECT_EQ(enforceSidecarRetention(dir_.string(), policy, keep), 2u);

  EXPECT_EQ(remaining_names(), (std::vector<std::string>{
                                   std::string("fresh1") + kSidecarSuffix,
                                   std::string("fresh2") + kSidecarSuffix,
                                   std::string("keep") + kSidecarSuffix,
                                   "notes.txt",
                               }));
}

TEST_F(SidecarRetentionFsTest, EnforceOnAnAbsentDirectoryIsANoop) {
  const auto absent = (dir_ / "not-created-yet").string();
  EXPECT_EQ(enforceSidecarRetention(absent, SidecarRetentionPolicy{}, ""), 0u);
  EXPECT_TRUE(listSidecars(absent).empty());
}

TEST_F(SidecarRetentionFsTest, EnforceKeepsEverythingWhenLimitsAreDisabled) {
  make_sidecar("ancient", 24h * 3650);
  for (int i = 0; i < 20; ++i) {
    make_sidecar("s" + std::to_string(i), std::chrono::hours(i + 1));
  }

  const SidecarRetentionPolicy unlimited{/*max_entries=*/0, /*max_age=*/0s};
  EXPECT_EQ(enforceSidecarRetention(dir_.string(), unlimited, ""), 0u);
  EXPECT_EQ(remaining_names().size(), 21u);
}

}  // namespace
}  // namespace orc
