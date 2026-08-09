/*
 * File:        observation_cache_retention_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     Eviction policy for the per-source observation sidecar cache
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * Unit (not functional): selectSidecarsToEvict() is pure — it is handed a
 * listing and a "now", and touches neither the filesystem nor the clock.
 */

#include "observation_cache_retention.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace orc {
namespace {

using namespace std::chrono_literals;

// A fixed reference point, so no test reads the system clock.
constexpr std::chrono::system_clock::time_point kNow{24h * 10000};

SidecarEntry entry(const std::string& path, std::chrono::hours age) {
  SidecarEntry e;
  e.path = path;
  e.last_used = kNow - age;
  return e;
}

std::vector<std::string> evict(std::vector<SidecarEntry> entries,
                               const SidecarRetentionPolicy& policy,
                               const std::string& keep = {}) {
  return selectSidecarsToEvict(std::move(entries), policy, keep, kNow);
}

TEST(SidecarRetention, KeepsEverythingWithinBothLimits) {
  const SidecarRetentionPolicy policy{/*max_entries=*/10, 24h * 30};
  EXPECT_TRUE(evict({entry("a", 1h), entry("b", 24h * 29)}, policy).empty());
}

TEST(SidecarRetention, EvictsSidecarsOlderThanTheAgeLimit) {
  const SidecarRetentionPolicy policy{/*max_entries=*/0, 24h * 30};
  const auto removed =
      evict({entry("fresh", 24h * 29), entry("stale", 24h * 31)}, policy);
  EXPECT_EQ(removed, std::vector<std::string>{"stale"});
}

TEST(SidecarRetention, AgeLimitIsExclusiveAtTheBoundary) {
  const SidecarRetentionPolicy policy{/*max_entries=*/0, 24h * 30};
  // Exactly at the limit is still within it; a second past is not.
  EXPECT_TRUE(evict({entry("boundary", 24h * 30)}, policy).empty());
  EXPECT_EQ(evict({entry("past", 24h * 30 + 1h)}, policy).size(), 1u);
}

TEST(SidecarRetention, EvictsLeastRecentlyUsedBeyondTheEntryLimit) {
  const SidecarRetentionPolicy policy{/*max_entries=*/3, /*max_age=*/0s};
  const auto removed =
      evict({entry("newest", 1h), entry("second", 2h), entry("third", 3h),
             entry("fourth", 4h), entry("fifth", 5h)},
            policy);
  // Oldest first, so a pass that fails part way still sheds the least useful.
  EXPECT_EQ(removed, (std::vector<std::string>{"fifth", "fourth"}));
}

TEST(SidecarRetention, TheSidecarAboutToBeOpenedIsNeverEvicted) {
  // "keep" is both the oldest entry and past the age limit: neither rule may
  // touch it, because the caller is about to open it.
  const SidecarRetentionPolicy policy{/*max_entries=*/2, 24h * 30};
  const auto removed =
      evict({entry("keep", 24h * 90), entry("a", 1h), entry("b", 2h)}, policy,
            "keep");
  EXPECT_EQ(removed, std::vector<std::string>{"b"});
}

TEST(SidecarRetention, TheKeptSidecarOccupiesOneOfTheAllowedSlots) {
  // With room for two and one reserved for the sidecar being opened, only the
  // single most recent existing sidecar survives.
  const SidecarRetentionPolicy policy{/*max_entries=*/2, /*max_age=*/0s};
  const auto removed =
      evict({entry("a", 1h), entry("b", 2h)}, policy, "brand-new");
  EXPECT_EQ(removed, std::vector<std::string>{"b"});
}

TEST(SidecarRetention, ZeroLimitsDisableTheirRule) {
  // No limits at all: nothing is ever evicted, however old or numerous.
  const SidecarRetentionPolicy policy{/*max_entries=*/0, /*max_age=*/0s};
  EXPECT_TRUE(evict({entry("ancient", 24h * 3650), entry("a", 1h),
                     entry("b", 2h), entry("c", 3h)},
                    policy)
                  .empty());
}

TEST(SidecarRetention, EitherRuleAloneIsEnoughToEvict) {
  const SidecarRetentionPolicy policy{/*max_entries=*/2, 24h * 30};
  const auto removed =
      evict({entry("recent_but_crowded_out", 3h), entry("a", 1h),
             entry("b", 2h), entry("old_but_within_count", 24h * 31)},
            policy);
  // "old_but_within_count" fails the age rule; the third-newest fails the
  // count rule. Both go, oldest first.
  EXPECT_EQ(removed, (std::vector<std::string>{"old_but_within_count",
                                               "recent_but_crowded_out"}));
}

TEST(SidecarRetention, EqualTimestampsResolveDeterministicallyByPath) {
  const SidecarRetentionPolicy policy{/*max_entries=*/1, /*max_age=*/0s};
  const std::vector<SidecarEntry> listing{entry("b", 5h), entry("a", 5h)};
  // Same input in either order must yield the same decision.
  const auto forward = evict(listing, policy);
  const auto reversed = evict(
      std::vector<SidecarEntry>(listing.rbegin(), listing.rend()), policy);
  EXPECT_EQ(forward, std::vector<std::string>{"b"});
  EXPECT_EQ(forward, reversed);
}

TEST(SidecarRetention, EmptyListingIsHandled) {
  EXPECT_TRUE(evict({}, SidecarRetentionPolicy{}, "keep").empty());
}

TEST(SidecarRetention, DefaultPolicyIsTenEntriesAndThirtyDays) {
  const SidecarRetentionPolicy policy;
  EXPECT_EQ(policy.max_entries, 10u);
  EXPECT_EQ(policy.max_age, std::chrono::seconds(24h * 30));
}

}  // namespace
}  // namespace orc
