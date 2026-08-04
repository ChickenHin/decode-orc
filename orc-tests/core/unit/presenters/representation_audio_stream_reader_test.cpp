/*
 * File:        representation_audio_stream_reader_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the presenter audio access surface: pair
 *              enumeration, reader creation, carrier→float conversion and
 *              SMPTE 272M frame ↔ stream-position mapping
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/presenters/src/representation_audio_stream_reader.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/audio/audio_sample_feed.h>

#include <memory>

#include "../mocks/mock_video_frame_representation.h"

namespace orc_unit_test {

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;

namespace {

// SMPTE 272M-1994 §14.3 Table 1: PAL is a constant 1920 stereo pairs per frame.
constexpr uint32_t kPalPairsPerFrame = 1920;

std::optional<orc::SourceParameters> params_for(orc::VideoSystem system) {
  orc::SourceParameters params{};
  params.system = system;
  return params;
}

orc::AudioChannelPairDescriptor descriptor(std::string name,
                                           orc::AudioOrigin origin) {
  orc::AudioChannelPairDescriptor desc;
  desc.name = std::move(name);
  desc.origin = origin;
  return desc;
}

// A representation carrying |pair_count| pairs over frames [0, last_frame].
std::shared_ptr<NiceMock<MockVideoFrameRepresentation>> make_representation(
    orc::VideoSystem system, size_t pair_count, orc::FrameID last_frame = 99) {
  auto repr = std::make_shared<NiceMock<MockVideoFrameRepresentation>>();
  ON_CALL(*repr, get_video_parameters())
      .WillByDefault(Return(params_for(system)));
  ON_CALL(*repr, audio_channel_pair_count()).WillByDefault(Return(pair_count));
  ON_CALL(*repr, frame_range())
      .WillByDefault(Return(orc::FrameIDRange{0, last_frame}));
  return repr;
}

// Ramp of distinct carrier values so conversion and ordering are both visible:
// sample s of frame f is (f + 1) * 1000 + s.
std::vector<int32_t> ramp_samples(orc::FrameID frame, uint32_t pairs) {
  std::vector<int32_t> out(static_cast<size_t>(pairs) * 2);
  for (size_t s = 0; s < out.size(); ++s) {
    out[s] = static_cast<int32_t>((frame + 1) * 1000 + s);
  }
  return out;
}

}  // namespace

// === Pair enumeration =====================================================

TEST(RepresentationAudioPairEnumerationTest,
     EnumeratesEveryPairWithIndexNameAndOrigin) {
  auto repr = make_representation(orc::VideoSystem::PAL, 2);
  ON_CALL(*repr, get_audio_channel_pair_descriptor(0))
      .WillByDefault(
          Return(descriptor("Analogue", orc::AudioOrigin::ANALOGUE)));
  ON_CALL(*repr, get_audio_channel_pair_descriptor(1))
      .WillByDefault(
          Return(descriptor("EFM digital audio", orc::AudioOrigin::EFM)));

  const auto pairs = orc::presenters::enumerate_audio_channel_pairs(*repr);

  ASSERT_EQ(pairs.size(), 2u);
  EXPECT_EQ(pairs[0].index, 0u);
  EXPECT_EQ(pairs[0].name, "Analogue");
  EXPECT_EQ(pairs[0].origin, "analogue");
  EXPECT_EQ(pairs[1].index, 1u);
  EXPECT_EQ(pairs[1].name, "EFM digital audio");
  EXPECT_EQ(pairs[1].origin, "efm");
}

TEST(RepresentationAudioPairEnumerationTest,
     ReturnsEmptyList_WhenRepresentationHasNoAudio) {
  // The normal case for most projects — audio is optional end to end.
  auto repr = make_representation(orc::VideoSystem::PAL, 0);

  EXPECT_TRUE(orc::presenters::enumerate_audio_channel_pairs(*repr).empty());
}

TEST(RepresentationAudioPairEnumerationTest,
     ReturnsEmptyList_WhenVideoSystemIsUnknown) {
  // Without a video system the SMPTE 272M cadence is undefined, so nominally
  // present pairs cannot be addressed by frame and are reported as absent.
  auto repr = make_representation(orc::VideoSystem::Unknown, 2);

  EXPECT_TRUE(orc::presenters::enumerate_audio_channel_pairs(*repr).empty());
}

TEST(RepresentationAudioPairEnumerationTest,
     ReturnsEmptyList_WhenVideoParametersAreAbsent) {
  auto repr = std::make_shared<NiceMock<MockVideoFrameRepresentation>>();
  ON_CALL(*repr, get_video_parameters()).WillByDefault(Return(std::nullopt));
  ON_CALL(*repr, audio_channel_pair_count()).WillByDefault(Return(1));

  EXPECT_TRUE(orc::presenters::enumerate_audio_channel_pairs(*repr).empty());
}

TEST(RepresentationAudioPairEnumerationTest,
     ReportsUnknownOrigin_WhenDescriptorIsAbsent) {
  auto repr = make_representation(orc::VideoSystem::NTSC, 1);
  ON_CALL(*repr, get_audio_channel_pair_descriptor(0))
      .WillByDefault(Return(std::nullopt));

  const auto pairs = orc::presenters::enumerate_audio_channel_pairs(*repr);

  ASSERT_EQ(pairs.size(), 1u);
  EXPECT_TRUE(pairs[0].name.empty());
  EXPECT_EQ(pairs[0].origin, "unknown");
}

// === Reader creation ======================================================

TEST(RepresentationAudioStreamReaderCreationTest, CreatesReader_ForUsablePair) {
  auto repr = make_representation(orc::VideoSystem::PAL, 1);

  EXPECT_NE(orc::presenters::make_audio_stream_reader(repr, 0), nullptr);
}

TEST(RepresentationAudioStreamReaderCreationTest,
     ReturnsNull_WhenPairIsOutOfRange) {
  auto repr = make_representation(orc::VideoSystem::PAL, 1);

  EXPECT_EQ(orc::presenters::make_audio_stream_reader(repr, 1), nullptr);
}

TEST(RepresentationAudioStreamReaderCreationTest,
     ReturnsNull_WhenVideoSystemIsUnknown) {
  auto repr = make_representation(orc::VideoSystem::Unknown, 1);

  EXPECT_EQ(orc::presenters::make_audio_stream_reader(repr, 0), nullptr);
}

TEST(RepresentationAudioStreamReaderCreationTest,
     ReturnsNull_WhenRepresentationIsNull) {
  EXPECT_EQ(orc::presenters::make_audio_stream_reader(nullptr, 0), nullptr);
}

TEST(RepresentationAudioStreamReaderCreationTest,
     KeepsRepresentationAliveForTheReadersLifetime) {
  auto repr = make_representation(orc::VideoSystem::PAL, 1);
  std::weak_ptr<const orc::VideoFrameRepresentation> observer = repr;

  auto reader = orc::presenters::make_audio_stream_reader(repr, 0);
  repr.reset();

  // The playback session must not read through a freed representation.
  EXPECT_FALSE(observer.expired());
  reader.reset();
  EXPECT_TRUE(observer.expired());
}

// === Frame ↔ stream-position mapping ======================================

TEST(RepresentationAudioStreamReaderMappingTest, PalUsesConstant1920Stride) {
  auto repr = make_representation(orc::VideoSystem::PAL, 1);
  auto reader = orc::presenters::make_audio_stream_reader(repr, 0);
  ASSERT_NE(reader, nullptr);

  EXPECT_EQ(reader->pairPositionForFrame(0), 0u);
  EXPECT_EQ(reader->pairPositionForFrame(1), kPalPairsPerFrame);
  EXPECT_EQ(reader->pairPositionForFrame(100), 100u * kPalPairsPerFrame);

  EXPECT_EQ(reader->frameForPairPosition(0), 0u);
  EXPECT_EQ(reader->frameForPairPosition(kPalPairsPerFrame - 1), 0u);
  EXPECT_EQ(reader->frameForPairPosition(kPalPairsPerFrame), 1u);
}

TEST(RepresentationAudioStreamReaderMappingTest,
     NtscFollowsTheFiveFrameCadence) {
  auto repr = make_representation(orc::VideoSystem::NTSC, 1);
  auto reader = orc::presenters::make_audio_stream_reader(repr, 0);
  ASSERT_NE(reader, nullptr);

  // SMPTE 272M-1994 §14.3 Table 1: 1602/1601/1602/1601/1602 per frame gives
  // cumulative in-sequence offsets 0, 1602, 3203, 4805, 6406, 8008.
  const uint64_t expected[] = {0, 1602, 3203, 4805, 6406, 8008};
  for (uint64_t frame = 0; frame < 6; ++frame) {
    EXPECT_EQ(reader->pairPositionForFrame(frame), expected[frame])
        << "frame " << frame;
  }

  // No drift accumulates: five frames per sequence, exactly 8008 pairs.
  EXPECT_EQ(reader->pairPositionForFrame(5 * 100), 8008u * 100u);
}

TEST(RepresentationAudioStreamReaderMappingTest,
     NtscPositionMapsBackToItsOwnFrame) {
  auto repr = make_representation(orc::VideoSystem::NTSC, 1);
  auto reader = orc::presenters::make_audio_stream_reader(repr, 0);
  ASSERT_NE(reader, nullptr);

  // Round-trip every frame of two full audio-frame sequences plus the last
  // position inside each frame — the boundary the naive pos*5/8008 estimate
  // gets wrong.
  for (orc::FrameID frame = 0; frame < 10; ++frame) {
    const uint64_t start = reader->pairPositionForFrame(frame);
    const uint64_t last = reader->pairPositionForFrame(frame + 1) - 1;
    EXPECT_EQ(reader->frameForPairPosition(start), frame) << "start " << frame;
    EXPECT_EQ(reader->frameForPairPosition(last), frame) << "last " << frame;
  }
}

TEST(RepresentationAudioStreamReaderMappingTest,
     FrameRangeMirrorsTheRepresentation) {
  auto repr = make_representation(orc::VideoSystem::PAL, 1, /*last_frame=*/41);
  auto reader = orc::presenters::make_audio_stream_reader(repr, 0);
  ASSERT_NE(reader, nullptr);

  EXPECT_EQ(reader->frameRange().first, 0u);
  EXPECT_EQ(reader->frameRange().last, 41u);
  EXPECT_EQ(reader->frameRange().count(), 42u);
}

// === Sample reads =========================================================

TEST(RepresentationAudioStreamReaderReadTest,
     ConvertsCarrierToFloatAtFullScale) {
  auto repr = make_representation(orc::VideoSystem::PAL, 1);
  // One PAL frame of samples whose first four values probe the carrier extremes
  // (SMPTE 272M-1994 §1.3: −8388608 … 8388607 in int32_t).
  std::vector<int32_t> block(kPalPairsPerFrame * 2, 0);
  block[0] = 0;
  block[1] = 8388607;
  block[2] = -8388608;
  block[3] = 4194304;
  ON_CALL(*repr, get_audio_samples(0, 0)).WillByDefault(Return(block));

  auto reader = orc::presenters::make_audio_stream_reader(repr, 0);
  ASSERT_NE(reader, nullptr);

  const auto samples = reader->readFrames(0, 1);

  ASSERT_EQ(samples.size(), kPalPairsPerFrame * 2u);
  EXPECT_FLOAT_EQ(samples[0], 0.0f);
  EXPECT_FLOAT_EQ(samples[1], 8388607.0f / 8388608.0f);
  EXPECT_FLOAT_EQ(samples[2], -1.0f);
  EXPECT_FLOAT_EQ(samples[3], 0.5f);
}

TEST(RepresentationAudioStreamReaderReadTest,
     ConcatenatesFramesInTemporalOrder) {
  auto repr = make_representation(orc::VideoSystem::PAL, 1);
  ON_CALL(*repr, get_audio_samples(0, _))
      .WillByDefault(Invoke([](size_t, orc::FrameID frame) {
        return ramp_samples(frame, kPalPairsPerFrame);
      }));

  auto reader = orc::presenters::make_audio_stream_reader(repr, 0);
  ASSERT_NE(reader, nullptr);

  const auto samples = reader->readFrames(3, 2);

  ASSERT_EQ(samples.size(), kPalPairsPerFrame * 2u * 2u);
  // First sample of frame 3, then first sample of frame 4.
  EXPECT_FLOAT_EQ(samples[0], orc::audio_carrier_to_float(4000));
  EXPECT_FLOAT_EQ(samples[kPalPairsPerFrame * 2],
                  orc::audio_carrier_to_float(5000));
}

TEST(RepresentationAudioStreamReaderReadTest,
     SizesNtscReadsFromTheCadenceNotAConstantStride) {
  auto repr = make_representation(orc::VideoSystem::NTSC, 1);
  ON_CALL(*repr, get_audio_samples(0, _))
      .WillByDefault(Invoke([](size_t, orc::FrameID frame) {
        return ramp_samples(
            frame, orc::audio_pairs_in_frame(frame, orc::VideoSystem::NTSC));
      }));

  auto reader = orc::presenters::make_audio_stream_reader(repr, 0);
  ASSERT_NE(reader, nullptr);

  // Frames 0..4 are one complete audio frame sequence: 8008 pairs total.
  EXPECT_EQ(reader->readFrames(0, 5).size(), 8008u * 2u);
  // Frames 1..2 are 1601 + 1602 pairs — not 2 × any constant.
  EXPECT_EQ(reader->readFrames(1, 2).size(), (1601u + 1602u) * 2u);
}

TEST(RepresentationAudioStreamReaderReadTest,
     SubstitutesCadenceExactSilence_WhenAFrameHasNoSamples) {
  auto repr = make_representation(orc::VideoSystem::PAL, 1);
  ON_CALL(*repr, get_audio_samples(0, 0))
      .WillByDefault(Return(std::vector<int32_t>(kPalPairsPerFrame * 2, 7)));
  // A legal response: e.g. a CVBS placeholder pair with nothing for this frame.
  ON_CALL(*repr, get_audio_samples(0, 1))
      .WillByDefault(Return(std::vector<int32_t>{}));

  auto reader = orc::presenters::make_audio_stream_reader(repr, 0);
  ASSERT_NE(reader, nullptr);

  const auto samples = reader->readFrames(0, 2);

  // The gap must not shorten the buffer, or the audio clock would slip against
  // the frame timeline.
  ASSERT_EQ(samples.size(), kPalPairsPerFrame * 2u * 2u);
  EXPECT_FLOAT_EQ(samples[0], orc::audio_carrier_to_float(7));
  EXPECT_FLOAT_EQ(samples[kPalPairsPerFrame * 2], 0.0f);
  EXPECT_FLOAT_EQ(samples.back(), 0.0f);
}

TEST(RepresentationAudioStreamReaderReadTest,
     PadsShortBlocksAndTruncatesLongOnes) {
  auto repr = make_representation(orc::VideoSystem::PAL, 1);
  ON_CALL(*repr, get_audio_samples(0, 0))
      .WillByDefault(Return(std::vector<int32_t>{11, 22}));  // one pair only
  ON_CALL(*repr, get_audio_samples(0, 1))
      .WillByDefault(
          Return(std::vector<int32_t>(kPalPairsPerFrame * 4, 33)));  // too long

  auto reader = orc::presenters::make_audio_stream_reader(repr, 0);
  ASSERT_NE(reader, nullptr);

  const auto samples = reader->readFrames(0, 2);

  ASSERT_EQ(samples.size(), kPalPairsPerFrame * 2u * 2u);
  EXPECT_FLOAT_EQ(samples[0], orc::audio_carrier_to_float(11));
  EXPECT_FLOAT_EQ(samples[1], orc::audio_carrier_to_float(22));
  EXPECT_FLOAT_EQ(samples[2], 0.0f);  // short block padded with silence
  EXPECT_FLOAT_EQ(samples[kPalPairsPerFrame * 2],
                  orc::audio_carrier_to_float(33));
  EXPECT_FLOAT_EQ(samples.back(), orc::audio_carrier_to_float(33));
}

TEST(RepresentationAudioStreamReaderReadTest, ClampsReadsToTheFrameRange) {
  auto repr = make_representation(orc::VideoSystem::PAL, 1, /*last_frame=*/4);
  ON_CALL(*repr, get_audio_samples(0, _))
      .WillByDefault(Invoke([](size_t, orc::FrameID frame) {
        return ramp_samples(frame, kPalPairsPerFrame);
      }));

  auto reader = orc::presenters::make_audio_stream_reader(repr, 0);
  ASSERT_NE(reader, nullptr);

  // Overruns the end: frames 3 and 4 only.
  EXPECT_EQ(reader->readFrames(3, 10).size(), kPalPairsPerFrame * 2u * 2u);
  // Entirely past the end.
  EXPECT_TRUE(reader->readFrames(5, 4).empty());
  // Zero-length request.
  EXPECT_TRUE(reader->readFrames(0, 0).empty());
}

TEST(RepresentationAudioStreamReaderReadTest,
     ReadsTheRequestedPairOnly_WhenSeveralArePresent) {
  auto repr = make_representation(orc::VideoSystem::PAL, 3);
  EXPECT_CALL(*repr, get_audio_samples(2, 0))
      .WillOnce(Return(std::vector<int32_t>(kPalPairsPerFrame * 2, 5)));

  auto reader = orc::presenters::make_audio_stream_reader(repr, 2);
  ASSERT_NE(reader, nullptr);

  const auto samples = reader->readFrames(0, 1);

  ASSERT_FALSE(samples.empty());
  EXPECT_FLOAT_EQ(samples[0], orc::audio_carrier_to_float(5));
}

// === Priming ==============================================================

TEST(RepresentationAudioStreamReaderPrimeTest,
     ForwardsProgressToTheRepresentation) {
  auto repr = make_representation(orc::VideoSystem::PAL, 1);
  uint64_t seen_done = 0;
  uint64_t seen_total = 0;
  std::string seen_message;
  EXPECT_CALL(*repr, prime_audio_decode(_))
      .WillOnce(Invoke([](const orc::AudioDecodeProgressFn& progress) {
        // Producers report through the callback they were handed; EFM decode is
        // the one that reports real progress.
        if (progress) {
          progress(3, 10, "Decoding EFM audio");
        }
      }));

  auto reader = orc::presenters::make_audio_stream_reader(repr, 0);
  ASSERT_NE(reader, nullptr);

  reader->prime([&](uint64_t done, uint64_t total, const std::string& message) {
    seen_done = done;
    seen_total = total;
    seen_message = message;
  });

  EXPECT_EQ(seen_done, 3u);
  EXPECT_EQ(seen_total, 10u);
  EXPECT_EQ(seen_message, "Decoding EFM audio");
}

}  // namespace orc_unit_test
