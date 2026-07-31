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
#include <plugin_ux_strings.h>

#include "mocks/mock_project_presenter.h"
#include "plugin_row_presentation.h"
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

// The dialog carries each row's selector, so an id-less entry is removed by
// the same string the CLI takes for it.
TEST(PluginManagerModelTest, RemoveEntryPassesTheRowSelectorThrough) {
  StrictMock<orc::presenters::test::MockProjectPresenter> mock;
  orc::PluginManagerModel model(mock);

  EXPECT_CALL(mock, removePluginEntry("path:/plugins/unnamed.so"))
      .WillOnce(Return(ok()));
  EXPECT_TRUE(model.removeEntry("path:/plugins/unnamed.so").success);
}

TEST(PluginManagerModelTest, MutationsAcceptSelectorsNotJustIds) {
  StrictMock<orc::presenters::test::MockProjectPresenter> mock;
  orc::PluginManagerModel model(mock);

  EXPECT_CALL(mock, setPluginEnabled("path:/plugins/unnamed.so", true))
      .WillOnce(Return(ok()));
  EXPECT_CALL(mock, setPluginTrusted("path:/plugins/unnamed.so", true))
      .WillOnce(Return(ok()));
  EXPECT_CALL(mock, updatePluginToLatestRelease("url:https://host/a.so"))
      .WillOnce(Return(ok()));

  EXPECT_TRUE(model.setEnabled("path:/plugins/unnamed.so", true).success);
  EXPECT_TRUE(model.setTrusted("path:/plugins/unnamed.so", true).success);
  EXPECT_TRUE(model.updateToLatest("url:https://host/a.so").success);
}

// --- Plugin Manager rows follow the presenter-computed load state -----------

TEST(PluginRowPresentationTest, TickFollowsLoadState) {
  using orc::presenters::PluginLoadState;

  EXPECT_TRUE(orc::makePluginRowPresentation(PluginLoadState::WillLoad, {})
                  .enabled_checked);
  EXPECT_TRUE(orc::makePluginRowPresentation(PluginLoadState::Core, {})
                  .enabled_checked);
  EXPECT_FALSE(orc::makePluginRowPresentation(PluginLoadState::Disabled, {})
                   .enabled_checked);
  EXPECT_FALSE(orc::makePluginRowPresentation(PluginLoadState::NotTrusted, {})
                   .enabled_checked);
  // The binary cannot load on this host, so "will load at the next launch" is
  // false however the enable and trust flags are set.
  EXPECT_FALSE(orc::makePluginRowPresentation(PluginLoadState::AbiMismatch, {})
                   .enabled_checked);
  EXPECT_FALSE(orc::makePluginRowPresentation(PluginLoadState::FileMissing, {})
                   .enabled_checked);
}

TEST(PluginRowPresentationTest, TickIsInertWhenTogglingCannotChangeTheOutcome) {
  using orc::presenters::PluginLoadState;

  EXPECT_TRUE(orc::makePluginRowPresentation(PluginLoadState::WillLoad, {})
                  .enabled_interactive);
  EXPECT_TRUE(orc::makePluginRowPresentation(PluginLoadState::Disabled, {})
                  .enabled_interactive);
  EXPECT_TRUE(orc::makePluginRowPresentation(PluginLoadState::NotTrusted, {})
                  .enabled_interactive);
  EXPECT_FALSE(orc::makePluginRowPresentation(PluginLoadState::Core, {})
                   .enabled_interactive);
  EXPECT_FALSE(orc::makePluginRowPresentation(PluginLoadState::AbiMismatch, {})
                   .enabled_interactive);
  EXPECT_FALSE(orc::makePluginRowPresentation(PluginLoadState::FileMissing, {})
                   .enabled_interactive);
}

TEST(PluginRowPresentationTest, OnlyAnUntrustedRowAsksForTrustWhenTicked) {
  using orc::presenters::PluginLoadState;

  EXPECT_TRUE(orc::makePluginRowPresentation(PluginLoadState::NotTrusted, {})
                  .tick_grants_trust);
  EXPECT_FALSE(orc::makePluginRowPresentation(PluginLoadState::Disabled, {})
                   .tick_grants_trust);
  EXPECT_FALSE(orc::makePluginRowPresentation(PluginLoadState::WillLoad, {})
                   .tick_grants_trust);
}

TEST(PluginRowPresentationTest, OnlyAnAbiMismatchWarnsOnTheVersionCell) {
  using orc::presenters::PluginLoadState;

  const auto mismatch = orc::makePluginRowPresentation(
      PluginLoadState::AbiMismatch, orc::plugin_ux::abiMismatchDetail(11, 12));
  EXPECT_TRUE(mismatch.warn_version);
  EXPECT_NE(mismatch.tooltip.find("11"), std::string::npos);
  EXPECT_NE(mismatch.tooltip.find("12"), std::string::npos);

  EXPECT_FALSE(orc::makePluginRowPresentation(PluginLoadState::NotTrusted, {})
                   .warn_version);
  EXPECT_TRUE(orc::makePluginRowPresentation(PluginLoadState::WillLoad, {})
                  .tooltip.empty());
}

// The Plugin Manager's Diagnostics section and 'orc-cli plugins doctor' read
// the same list through the same seam.
TEST(PluginManagerModelTest, DiagnosticsAndSearchPathsDelegateToPresenter) {
  StrictMock<orc::presenters::test::MockProjectPresenter> mock;
  orc::PluginManagerModel model(mock);

  orc::presenters::PluginDiagnosticInfo diagnostic;
  diagnostic.severity = orc::presenters::PluginDiagnosticSeverity::Error;
  diagnostic.message = "Failed to load";
  diagnostic.path = "/plugins/broken.so";

  EXPECT_CALL(mock, listPluginDiagnostics())
      .WillOnce(Return(
          std::vector<orc::presenters::PluginDiagnosticInfo>{diagnostic}));
  EXPECT_CALL(mock, listPluginSearchPaths())
      .WillOnce(Return(std::vector<std::string>{"/plugins"}));

  const auto diagnostics = model.diagnostics();
  ASSERT_EQ(diagnostics.size(), 1U);
  EXPECT_EQ(diagnostics[0].message, "Failed to load");
  EXPECT_EQ(model.searchPaths(), std::vector<std::string>{"/plugins"});
}

// --- PluginBrowseModel: fetch, offline, search, install ---------------------

TEST(PluginBrowseModelTest, RefreshPopulatesFromPresenter) {
  StrictMock<orc::presenters::test::MockProjectPresenter> mock;
  orc::PluginBrowseModel model(mock);

  orc::presenters::PluginIndexInfo info;
  info.available = true;
  info.entries = {makeEntry("acme.deint", true)};
  EXPECT_CALL(mock, fetchPluginIndex()).WillOnce(Return(info));
  EXPECT_CALL(mock, getPluginRegistry())
      .WillOnce(Return(orc::presenters::PluginRegistryInfo{}));

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
  EXPECT_CALL(mock, getPluginRegistry())
      .WillOnce(Return(orc::presenters::PluginRegistryInfo{}));

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
  EXPECT_CALL(mock, getPluginRegistry())
      .WillOnce(Return(orc::presenters::PluginRegistryInfo{}));

  model.refresh();
  EXPECT_EQ(model.search("ACME").size(), 1U);
  EXPECT_EQ(model.search("video").size(), 2U);
  EXPECT_EQ(model.search("").size(), 2U);
  EXPECT_EQ(model.search("nomatch").size(), 0U);
}

// An index entry that is already installed is matched to its registry entry,
// so the browse dialog can say which version is installed rather than only
// that one is.
TEST(PluginBrowseModelTest, InstalledEntryIsMatchedFromTheRegistry) {
  StrictMock<orc::presenters::test::MockProjectPresenter> mock;
  orc::PluginBrowseModel model(mock);

  orc::presenters::PluginIndexInfo info;
  info.available = true;
  auto entry = makeEntry("acme.deint", true);
  entry.version = "1.0.6";
  entry.already_installed = true;
  info.entries = {entry};

  orc::presenters::PluginRegistryInfo registry;
  orc::presenters::PluginRegistryEntryInfo installed;
  installed.selector = "acme.deint";
  installed.plugin_id = "acme.deint";
  installed.plugin_version = "1.0.5";
  registry.entries = {installed};

  EXPECT_CALL(mock, fetchPluginIndex()).WillOnce(Return(info));
  EXPECT_CALL(mock, getPluginRegistry()).WillOnce(Return(registry));

  model.refresh();
  const auto* found = model.installedEntry("acme.deint");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->plugin_version, "1.0.5");
  EXPECT_EQ(model.installedEntry("zeta.tool"), nullptr);
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
