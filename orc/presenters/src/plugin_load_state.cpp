/*
 * File:        plugin_load_state.cpp
 * Module:      orc-presenters
 * Purpose:     Derive whether a registered plugin will load, and why not
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "plugin_load_state.h"

#include <plugin_ux_strings.h>

namespace orc::presenters {

namespace {

/// Artifact source of an entry whose binary is fetched from a release; its
/// `path` is a download cache location, so an absent file means "not fetched
/// yet", not "broken entry".
constexpr const char* kRemoteArtifactSource = "github_release_asset";

}  // namespace

PluginLoadState computePluginLoadState(const PluginRegistryEntryInfo& entry,
                                       std::string* detail) {
  auto set_detail = [detail](std::string text) {
    if (detail != nullptr) {
      *detail = std::move(text);
    }
  };

  if (entry.is_core_plugin) {
    set_detail(plugin_ux::kLoadStateCoreDetail);
    return PluginLoadState::Core;
  }

  if (!entry.abi_compatible) {
    set_detail(plugin_ux::abiMismatchDetail(entry.required_host_abi,
                                            entry.host_abi_version));
    return PluginLoadState::AbiMismatch;
  }

  // Only local entries can be judged by the filesystem: a remote entry's path
  // is populated after the first successful download.
  const bool is_remote = entry.artifact_source == kRemoteArtifactSource;
  if (!is_remote && !entry.path.empty() && !entry.path_exists) {
    set_detail(plugin_ux::fileMissingDetail(entry.path));
    return PluginLoadState::FileMissing;
  }

  if (entry.trust_state != kPluginTrustStateTrusted) {
    set_detail(plugin_ux::kLoadStateNotTrustedDetail);
    return PluginLoadState::NotTrusted;
  }

  if (!entry.enabled) {
    set_detail(plugin_ux::kLoadStateDisabledDetail);
    return PluginLoadState::Disabled;
  }

  set_detail(plugin_ux::kLoadStateWillLoadDetail);
  return PluginLoadState::WillLoad;
}

const char* pluginLoadStateLabel(PluginLoadState state) {
  switch (state) {
    case PluginLoadState::WillLoad:
      return plugin_ux::kLoadStateWillLoadLabel;
    case PluginLoadState::Disabled:
      return plugin_ux::kLoadStateDisabledLabel;
    case PluginLoadState::NotTrusted:
      return plugin_ux::kLoadStateNotTrustedLabel;
    case PluginLoadState::AbiMismatch:
      return plugin_ux::kLoadStateAbiMismatchLabel;
    case PluginLoadState::FileMissing:
      return plugin_ux::kLoadStateFileMissingLabel;
    case PluginLoadState::Core:
      return plugin_ux::kLoadStateCoreLabel;
  }
  return plugin_ux::kLoadStateNotTrustedLabel;
}

const char* pluginLoadStateId(PluginLoadState state) {
  switch (state) {
    case PluginLoadState::WillLoad:
      return "will_load";
    case PluginLoadState::Disabled:
      return "disabled";
    case PluginLoadState::NotTrusted:
      return "not_trusted";
    case PluginLoadState::AbiMismatch:
      return "abi_mismatch";
    case PluginLoadState::FileMissing:
      return "file_missing";
    case PluginLoadState::Core:
      return "core";
  }
  return "not_trusted";
}

std::string pluginLoadStateSummary(PluginLoadState state,
                                   const std::string& detail) {
  std::string summary = pluginLoadStateLabel(state);
  if (!detail.empty()) {
    summary += plugin_ux::kLabelDetailSeparator;
    summary += detail;
  }
  return summary;
}

}  // namespace orc::presenters
