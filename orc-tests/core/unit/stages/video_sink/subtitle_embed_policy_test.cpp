/*
 * File:        subtitle_embed_policy_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the video sink subtitle source selection
 *              (closed captions vs teletext) for the mov_text stream
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/plugins/stages/sinks/common/subtitle_embed_policy.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace orc_unit_test {

namespace {

using orc::select_subtitle_embed_source;
using orc::SubtitleEmbedRequest;
using orc::SubtitleEmbedSource;

// A viable closed caption request: MP4 container, observation context present.
SubtitleEmbedRequest cc_request() {
  SubtitleEmbedRequest request;
  request.embed_closed_captions = true;
  request.container_format = "mp4";
  request.video_system_is_pal = true;
  request.has_observation_context = true;
  return request;
}

// A viable teletext request: PAL, MP4 container, observation context present.
SubtitleEmbedRequest teletext_request() {
  SubtitleEmbedRequest request;
  request.embed_teletext_subtitles = true;
  request.container_format = "mp4";
  request.video_system_is_pal = true;
  request.has_observation_context = true;
  return request;
}

}  // namespace

// Regression for issue #236: closed caption embedding used to be gated on a
// VideoFrameRepresentation being handed to the backend, which only happened
// when audio embedding was enabled. The selection depends on no such input.
TEST(SubtitleEmbedPolicyTest,
     ClosedCaptions_AreSelected_WithoutAudioEmbedding) {
  const auto decision = select_subtitle_embed_source(cc_request());
  EXPECT_EQ(decision.source, SubtitleEmbedSource::kClosedCaptions);
  EXPECT_TRUE(decision.closed_caption_reason.empty());
}

TEST(SubtitleEmbedPolicyTest, NothingRequested_SelectsNoSource) {
  SubtitleEmbedRequest request;
  request.container_format = "mp4";
  request.has_observation_context = true;

  const auto decision = select_subtitle_embed_source(request);
  EXPECT_EQ(decision.source, SubtitleEmbedSource::kNone);
  EXPECT_TRUE(decision.closed_caption_reason.empty());
  EXPECT_TRUE(decision.teletext_reason.empty());
}

TEST(SubtitleEmbedPolicyTest, ClosedCaptions_AreSelected_InMovContainer) {
  auto request = cc_request();
  request.container_format = "mov";

  EXPECT_EQ(select_subtitle_embed_source(request).source,
            SubtitleEmbedSource::kClosedCaptions);
}

TEST(SubtitleEmbedPolicyTest, ClosedCaptions_AreDropped_InNonMovTextContainer) {
  auto request = cc_request();
  request.container_format = "mkv";

  const auto decision = select_subtitle_embed_source(request);
  EXPECT_EQ(decision.source, SubtitleEmbedSource::kNone);
  EXPECT_THAT(decision.closed_caption_reason, testing::HasSubstr("MP4/MOV"));
}

TEST(SubtitleEmbedPolicyTest, ClosedCaptions_AreDropped_WithoutObservations) {
  auto request = cc_request();
  request.has_observation_context = false;

  const auto decision = select_subtitle_embed_source(request);
  EXPECT_EQ(decision.source, SubtitleEmbedSource::kNone);
  EXPECT_THAT(decision.closed_caption_reason,
              testing::HasSubstr("observation context"));
}

TEST(SubtitleEmbedPolicyTest, ClosedCaptions_AreSelected_ForNtsc) {
  auto request = cc_request();
  request.video_system_is_pal = false;

  EXPECT_EQ(select_subtitle_embed_source(request).source,
            SubtitleEmbedSource::kClosedCaptions);
}

TEST(SubtitleEmbedPolicyTest, Teletext_IsSelected_WithoutAudioEmbedding) {
  const auto decision = select_subtitle_embed_source(teletext_request());
  EXPECT_EQ(decision.source, SubtitleEmbedSource::kTeletext);
  EXPECT_TRUE(decision.teletext_reason.empty());
}

TEST(SubtitleEmbedPolicyTest, Teletext_IsDropped_ForNonPalSystems) {
  auto request = teletext_request();
  request.video_system_is_pal = false;

  const auto decision = select_subtitle_embed_source(request);
  EXPECT_EQ(decision.source, SubtitleEmbedSource::kNone);
  EXPECT_THAT(decision.teletext_reason, testing::HasSubstr("PAL"));
}

TEST(SubtitleEmbedPolicyTest, Teletext_IsDropped_InNonMovTextContainer) {
  auto request = teletext_request();
  request.container_format = "mxf";

  const auto decision = select_subtitle_embed_source(request);
  EXPECT_EQ(decision.source, SubtitleEmbedSource::kNone);
  EXPECT_THAT(decision.teletext_reason, testing::HasSubstr("MP4/MOV"));
}

TEST(SubtitleEmbedPolicyTest, Teletext_IsDropped_WithoutObservations) {
  auto request = teletext_request();
  request.has_observation_context = false;

  const auto decision = select_subtitle_embed_source(request);
  EXPECT_EQ(decision.source, SubtitleEmbedSource::kNone);
  EXPECT_THAT(decision.teletext_reason,
              testing::HasSubstr("observation context"));
}

TEST(SubtitleEmbedPolicyTest, ClosedCaptions_WinTheStream_WhenBothRequested) {
  auto request = cc_request();
  request.embed_teletext_subtitles = true;

  const auto decision = select_subtitle_embed_source(request);
  EXPECT_EQ(decision.source, SubtitleEmbedSource::kClosedCaptions);
  EXPECT_TRUE(decision.closed_caption_reason.empty());
  EXPECT_THAT(decision.teletext_reason,
              testing::HasSubstr("already occupy the subtitle stream"));
}

TEST(SubtitleEmbedPolicyTest, BothRequested_InMkv_DropsBothWithReasons) {
  auto request = cc_request();
  request.embed_teletext_subtitles = true;
  request.container_format = "mkv";

  const auto decision = select_subtitle_embed_source(request);
  EXPECT_EQ(decision.source, SubtitleEmbedSource::kNone);
  EXPECT_THAT(decision.closed_caption_reason, testing::HasSubstr("MP4/MOV"));
  EXPECT_THAT(decision.teletext_reason, testing::HasSubstr("MP4/MOV"));
}

}  // namespace orc_unit_test
