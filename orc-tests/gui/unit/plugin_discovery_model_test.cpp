/*
 * File:        plugin_discovery_model_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Model tests for plugin trust/enable separation and index browse
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "mocks/mock_project_presenter.h"
#include "pluginbrowsemodel.h"
#include "pluginmanagermodel.h"

namespace gui_unit_test {

using ::testing::_;
using ::testing::Return;
using ::testing::StrictMock;

namespace {

orc::presenters::PluginRegistryMutationResult ok() {
  orc::presenters::PluginRegistryMutationResult r;
  r.success = true;
  return r;
}

orc::presenters::PluginIndexEntryInfo makeEntry(const std::string& id,
                                                bool compatible) {
  orc::presenters::PluginIndexEntryInfo e;
  e.id = id;
  e.display_name = id + " display";
  e.description = "does " + id + " things";
  e.tags = {"video"};
  e.has_compatible_build = compatible;
  return e;
}

}  // namespace

// --- PluginManagerModel: enable and trust are independent -------------------

TEST(PluginManagerModelTest, SetEnabledDoesNotGrantTrust) {
  StrictMock<orc::presenters::test::MockProjectPresenter> mock;
  orc::PluginManagerModel model(mock);

  // Only setPluginEnabled is permitted; StrictMock fails the test if
  // setPluginTrusted (or anything else) is called during enable.
  EXPECT_CALL(mock, setPluginEnabled("plug", true)).WillOnce(Return(ok()));

  const auto result = model.setEnabled("plug", true);
  EXPECT_TRUE(result.success);
}

TEST(PluginManagerModelTest, SetTrustedDelegatesToPresenter) {
  StrictMock<orc::presenters::test::MockProjectPresenter> mock;
  orc::PluginManagerModel model(mock);

  EXPECT_CALL(mock, setPluginTrusted("plug", true)).WillOnce(Return(ok()));
  EXPECT_TRUE(model.setTrusted("plug", true).success);
}

TEST(PluginManagerModelTest, AddFromUrlForwardsTrustFlag) {
  StrictMock<orc::presenters::test::MockProjectPresenter> mock;
  orc::PluginManagerModel model(mock);

  EXPECT_CALL(mock, addPluginFromUrl("https://example.invalid/releases", false))
      .WillOnce(Return(ok()));
  EXPECT_TRUE(
      model.addFromUrl("https://example.invalid/releases", false).success);
}

TEST(PluginManagerModelTest, CheckUpdatesDelegatesToPresenter) {
  StrictMock<orc::presenters::test::MockProjectPresenter> mock;
  orc::PluginManagerModel model(mock);

  orc::presenters::PluginUpdateStatusInfo status;
  status.plugin_id = "plug";
  status.installed_version = "1.0.5";
  status.latest_version = "1.0.6";
  status.latest_tag = "v1.0.6";
  status.status = orc::presenters::PluginUpdateStatus::UpdateAvailable;
  EXPECT_CALL(mock, checkPluginUpdates())
      .WillOnce(
          Return(std::vector<orc::presenters::PluginUpdateStatusInfo>{status}));

  const auto results = model.checkUpdates();
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].plugin_id, "plug");
  EXPECT_EQ(results[0].status,
            orc::presenters::PluginUpdateStatus::UpdateAvailable);
  EXPECT_EQ(results[0].latest_version, "1.0.6");
}

TEST(PluginManagerModelTest, UpdateToLatestDoesNotGrantTrust) {
  StrictMock<orc::presenters::test::MockProjectPresenter> mock;
  orc::PluginManagerModel model(mock);

  // Only updatePluginToLatestRelease is permitted; StrictMock fails the test
  // if the model tried to grant trust as part of the update.
  EXPECT_CALL(mock, updatePluginToLatestRelease("plug")).WillOnce(Return(ok()));
  EXPECT_TRUE(model.updateToLatest("plug").success);
}

// --- PluginBrowseModel: fetch, offline, search, install ---------------------

TEST(PluginBrowseModelTest, RefreshPopulatesFromPresenter) {
  StrictMock<orc::presenters::test::MockProjectPresenter> mock;
  orc::PluginBrowseModel model(mock);

  orc::presenters::PluginIndexInfo info;
  info.available = true;
  info.entries = {makeEntry("acme.deint", true)};
  EXPECT_CALL(mock, fetchPluginIndex()).WillOnce(Return(info));

  model.refresh();
  EXPECT_TRUE(model.available());
  EXPECT_FALSE(model.offline());
  ASSERT_EQ(model.index().entries.size(), 1U);
}

TEST(PluginBrowseModelTest, OfflineCacheSetsStatus) {
  StrictMock<orc::presenters::test::MockProjectPresenter> mock;
  orc::PluginBrowseModel model(mock);

  orc::presenters::PluginIndexInfo info;
  info.available = true;
  info.offline = true;
  info.from_cache = true;
  info.entries = {makeEntry("acme.deint", true)};
  EXPECT_CALL(mock, fetchPluginIndex()).WillOnce(Return(info));

  model.refresh();
  EXPECT_TRUE(model.offline());
  EXPECT_TRUE(model.fromCache());
  EXPECT_NE(model.statusMessage().find("cached"), std::string::npos);
}

TEST(PluginBrowseModelTest, SearchFiltersCaseInsensitively) {
  StrictMock<orc::presenters::test::MockProjectPresenter> mock;
  orc::PluginBrowseModel model(mock);

  orc::presenters::PluginIndexInfo info;
  info.available = true;
  info.entries = {makeEntry("acme.deint", true), makeEntry("zeta.tool", true)};
  EXPECT_CALL(mock, fetchPluginIndex()).WillOnce(Return(info));

  model.refresh();
  EXPECT_EQ(model.search("ACME").size(), 1U);
  EXPECT_EQ(model.search("video").size(), 2U);
  EXPECT_EQ(model.search("").size(), 2U);
  EXPECT_EQ(model.search("nomatch").size(), 0U);
}

TEST(PluginBrowseModelTest, InstallAndTrustDelegate) {
  StrictMock<orc::presenters::test::MockProjectPresenter> mock;
  orc::PluginBrowseModel model(mock);

  EXPECT_CALL(mock, installPluginFromIndex("acme.deint"))
      .WillOnce(Return(ok()));
  EXPECT_CALL(mock, setPluginTrusted("acme.deint", true))
      .WillOnce(Return(ok()));

  EXPECT_TRUE(model.install("acme.deint").success);
  EXPECT_TRUE(model.trust("acme.deint").success);
}

}  // namespace gui_unit_test
