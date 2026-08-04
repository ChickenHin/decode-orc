/*
 * File:        preview_dialog_audio_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Audio selector, volume wiring and audio-clocked playback tests
 *              for the preview dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/audio/audio_channel_pair.h>  // audio_pairs_in_frame
#include <orc/stage/common_types.h>              // VideoSystem

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QThread>
#include <chrono>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "mocks/fake_audio_output.h"
#include "mocks/fake_audio_stream_reader.h"
#include "previewdialog.h"

namespace gui_unit_test {

namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-widget-test";
  static char platform_opt[] = "-platform";
  static char platform_val[] = "offscreen";
  static char* argv[] = {app_name, platform_opt, platform_val, nullptr};
  static QApplication* app = [] {
    auto* created_app = new QApplication(argc, argv);
    created_app->setQuitOnLastWindowClosed(false);
    return created_app;
  }();
  return *app;
}

constexpr int kFirstIndex = 0;
constexpr int kLastIndex = 99;

// Priming runs on its own thread and the chase runs off the dialogue's
// playback timer, so the test drives the event loop until the condition holds
// rather than assuming either has completed.
bool waitFor(const std::function<bool()>& condition, int timeout_ms = 2000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    if (condition()) {
      return true;
    }
    QThread::msleep(2);
  }
  return condition();
}

std::vector<orc::AudioPairView> twoPairs() {
  return {orc::AudioPairView{0, "Analogue", "analogue"},
          orc::AudioPairView{1, "EFM digital audio", "efm"}};
}

// Installs a device-free output and returns a borrowed pointer to it.
orc::gui::test::FakeAudioOutput* installFakeOutput(PreviewDialog& dialog) {
  auto output = std::make_unique<orc::gui::test::FakeAudioOutput>();
  auto* borrowed = output.get();
  dialog.setAudioOutput(std::move(output));
  return borrowed;
}

std::shared_ptr<orc::gui::test::FakeAudioStreamReader> palReader() {
  return std::make_shared<orc::gui::test::FakeAudioStreamReader>(
      orc::FrameIDRange{static_cast<orc::FrameID>(kFirstIndex),
                        static_cast<orc::FrameID>(kLastIndex)},
      orc::VideoSystem::PAL);
}

// Play with the given pair selected and run through reader delivery and
// priming until the audio clock is driving the preview.
bool startAudioPlayback(PreviewDialog& dialog, int combo_index) {
  dialog.audioPairCombo()->setCurrentIndex(combo_index);
  dialog.playPauseButton()->click();
  dialog.setAudioStreamReader(palReader());
  return waitFor([&dialog]() { return dialog.isAudioPlaybackActive(); });
}

}  // namespace

// --- Selector population and enable states ---------------------------------

TEST(PreviewDialogAudio, NoPairs_DisablesSelectorAndVolume) {
  ensureApplication();
  PreviewDialog dialog;

  dialog.setAudioChannelPairs({});

  EXPECT_EQ(dialog.audioPairCombo()->count(), 1);
  EXPECT_EQ(dialog.audioPairCombo()->currentText(), QString("Mute/None"));
  EXPECT_FALSE(dialog.audioPairCombo()->isEnabled());
  EXPECT_FALSE(dialog.audioVolumeSlider()->isEnabled());
  // The advisory text explains that the absence is a property of the pipeline.
  EXPECT_FALSE(dialog.audioPairCombo()->toolTip().isEmpty());
}

TEST(PreviewDialogAudio, PairsAvailable_PopulateSelectorAndDefaultToNoAudio) {
  ensureApplication();
  PreviewDialog dialog;

  dialog.setAudioChannelPairs(twoPairs());

  ASSERT_EQ(dialog.audioPairCombo()->count(), 3);
  EXPECT_EQ(dialog.audioPairCombo()->itemText(0), QString("Mute/None"));
  EXPECT_EQ(dialog.audioPairCombo()->itemText(1), QString("0: Analogue"));
  EXPECT_EQ(dialog.audioPairCombo()->itemText(2),
            QString("1: EFM digital audio"));
  EXPECT_TRUE(dialog.audioPairCombo()->isEnabled());

  // Pair indices are node-specific, so nothing is selected until the user says
  // so — and volume stays disabled with "Mute/None" selected.
  EXPECT_EQ(dialog.audioPairCombo()->currentIndex(), 0);
  EXPECT_FALSE(dialog.selectedAudioPair().has_value());
  EXPECT_FALSE(dialog.audioVolumeSlider()->isEnabled());
}

TEST(PreviewDialogAudio, SelectingAPair_EnablesVolumeWhilePaused) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.setAudioChannelPairs(twoPairs());

  dialog.audioPairCombo()->setCurrentIndex(2);

  ASSERT_TRUE(dialog.selectedAudioPair().has_value());
  EXPECT_EQ(*dialog.selectedAudioPair(), 1u);
  EXPECT_TRUE(dialog.audioVolumeSlider()->isEnabled());
}

TEST(PreviewDialogAudio, NodeChange_ClearsSelectorBackToNoAudio) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.setCurrentNodeId(orc::NodeID(1));
  dialog.setAudioChannelPairs(twoPairs());
  dialog.audioPairCombo()->setCurrentIndex(1);

  dialog.setCurrentNodeId(orc::NodeID(2));

  EXPECT_EQ(dialog.audioPairCombo()->count(), 1);
  EXPECT_FALSE(dialog.selectedAudioPair().has_value());
  EXPECT_FALSE(dialog.audioPairCombo()->isEnabled());
}

TEST(PreviewDialogAudio, NodeChange_ReselectsTheSamePairAtTheNewNode) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.setCurrentNodeId(orc::NodeID(1));
  dialog.setAudioChannelPairs(twoPairs());
  dialog.audioPairCombo()->setCurrentIndex(2);  // EFM digital audio

  dialog.setCurrentNodeId(orc::NodeID(2));
  dialog.setAudioChannelPairs(twoPairs());

  // The pair the user chose carries through the pipeline, so stepping to the
  // next stage keeps playing it rather than reverting to "Mute/None".
  EXPECT_EQ(dialog.audioPairCombo()->currentIndex(), 2);
  ASSERT_TRUE(dialog.selectedAudioPair().has_value());
  EXPECT_EQ(*dialog.selectedAudioPair(), 1u);
  EXPECT_TRUE(dialog.audioVolumeSlider()->isEnabled());
}

TEST(PreviewDialogAudio, NodeChange_MatchesTheDescriptorNotThePairIndex) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.setCurrentNodeId(orc::NodeID(1));
  dialog.setAudioChannelPairs(twoPairs());
  dialog.audioPairCombo()->setCurrentIndex(2);  // EFM digital audio, index 1

  // A node that dropped the analogue pair renumbers what is left.
  dialog.setCurrentNodeId(orc::NodeID(2));
  dialog.setAudioChannelPairs(
      {orc::AudioPairView{0, "EFM digital audio", "efm"}});

  EXPECT_EQ(dialog.audioPairCombo()->currentIndex(), 1);
  ASSERT_TRUE(dialog.selectedAudioPair().has_value());
  EXPECT_EQ(*dialog.selectedAudioPair(), 0u);
}

TEST(PreviewDialogAudio, NodeWithoutThePair_FallsBackButKeepsTheChoice) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.setCurrentNodeId(orc::NodeID(1));
  dialog.setAudioChannelPairs(twoPairs());
  dialog.audioPairCombo()->setCurrentIndex(2);  // EFM digital audio

  dialog.setCurrentNodeId(orc::NodeID(2));
  dialog.setAudioChannelPairs({orc::AudioPairView{0, "Analogue", "analogue"}});

  EXPECT_FALSE(dialog.selectedAudioPair().has_value());

  // Stepping on to a node that does carry the pair restores it: passing a
  // stage without audio must not lose the selection.
  dialog.setCurrentNodeId(orc::NodeID(3));
  dialog.setAudioChannelPairs(twoPairs());

  ASSERT_TRUE(dialog.selectedAudioPair().has_value());
  EXPECT_EQ(*dialog.selectedAudioPair(), 1u);
}

TEST(PreviewDialogAudio, ChoosingNoAudio_IsRememberedAcrossNodes) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.setCurrentNodeId(orc::NodeID(1));
  dialog.setAudioChannelPairs(twoPairs());
  dialog.audioPairCombo()->setCurrentIndex(1);
  dialog.audioPairCombo()->setCurrentIndex(0);  // Back to "Mute/None"

  dialog.setCurrentNodeId(orc::NodeID(2));
  dialog.setAudioChannelPairs(twoPairs());

  EXPECT_EQ(dialog.audioPairCombo()->currentIndex(), 0);
  EXPECT_FALSE(dialog.selectedAudioPair().has_value());
}

// --- Volume wiring ---------------------------------------------------------

TEST(PreviewDialogAudio, VolumeSlider_ForwardsSquaredTaperToTheDevice) {
  ensureApplication();
  PreviewDialog dialog;
  auto* output = installFakeOutput(dialog);
  dialog.setAudioChannelPairs(twoPairs());
  dialog.audioPairCombo()->setCurrentIndex(1);

  dialog.audioVolumeSlider()->setValue(50);

  // A linear slider tracks perceived loudness better as an amplitude square.
  EXPECT_DOUBLE_EQ(output->volume(), 0.25);
  EXPECT_DOUBLE_EQ(dialog.audioController()->volume(), 0.5);
}

// --- Play button paths -----------------------------------------------------

TEST(PreviewDialogAudio, PlayWithNoAudioSelected_KeepsTheVideoOnlyPath) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  auto* output = installFakeOutput(dialog);
  dialog.setAudioChannelPairs(twoPairs());  // Available but not selected

  QSignalSpy reader_requested(&dialog,
                              &PreviewDialog::audioStreamReaderRequested);
  dialog.playPauseButton()->click();

  EXPECT_EQ(dialog.playPauseButton()->text(), QString::fromUtf8("⏸"));
  EXPECT_FALSE(dialog.isAudioPlaybackActive());
  EXPECT_EQ(reader_requested.count(), 0);
  EXPECT_FALSE(output->isStarted());  // No device is opened

  dialog.stopPlayback();
}

TEST(PreviewDialogAudio, PlayWithPairSelected_RequestsAReaderBeforeStarting) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  auto* output = installFakeOutput(dialog);
  dialog.setAudioChannelPairs(twoPairs());
  dialog.audioPairCombo()->setCurrentIndex(2);

  QSignalSpy reader_requested(&dialog,
                              &PreviewDialog::audioStreamReaderRequested);
  dialog.playPauseButton()->click();

  ASSERT_EQ(reader_requested.count(), 1);
  EXPECT_EQ(reader_requested.at(0).at(0).toULongLong(), 1u);
  // The transport shows playback intent, but nothing runs until the reader
  // arrives and its deferred decode has been forced.
  EXPECT_EQ(dialog.playPauseButton()->text(), QString::fromUtf8("⏸"));
  EXPECT_FALSE(dialog.isAudioPlaybackActive());
  EXPECT_FALSE(output->isStarted());

  dialog.stopPlayback();
}

TEST(PreviewDialogAudio, ReaderDelivered_StartsAudioClockedPlayback) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  auto* output = installFakeOutput(dialog);
  dialog.setAudioChannelPairs(twoPairs());

  ASSERT_TRUE(startAudioPlayback(dialog, 1));

  EXPECT_TRUE(output->isStarted());
  EXPECT_EQ(output->requestedSampleRateHz(), orc::kAudioSampleRateHz);
  EXPECT_EQ(output->requestedChannels(), 2u);
  EXPECT_EQ(dialog.playPauseButton()->text(), QString::fromUtf8("⏸"));

  dialog.stopPlayback();
  EXPECT_FALSE(dialog.isAudioPlaybackActive());
  EXPECT_FALSE(output->isStarted());
}

TEST(PreviewDialogAudio, NullReader_FallsBackToVideoOnlyPlayback) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  auto* output = installFakeOutput(dialog);
  dialog.setAudioChannelPairs(twoPairs());
  dialog.audioPairCombo()->setCurrentIndex(1);

  dialog.playPauseButton()->click();
  dialog.setAudioStreamReader(nullptr);

  EXPECT_FALSE(dialog.isAudioPlaybackActive());
  EXPECT_FALSE(output->isStarted());
  // Playback continues — just without sound.
  EXPECT_EQ(dialog.playPauseButton()->text(), QString::fromUtf8("⏸"));

  dialog.stopPlayback();
}

TEST(PreviewDialogAudio, AudioClock_DrivesThePreviewPositionAndSkipsFrames) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  auto* output = installFakeOutput(dialog);
  dialog.setAudioChannelPairs(twoPairs());
  ASSERT_TRUE(startAudioPlayback(dialog, 1));
  ASSERT_EQ(dialog.currentIndex(), 0);

  // Consume three PAL frames' worth of audio in one go — what a device does
  // while a heavy DAG is still rendering frame 1.
  const uint64_t three_frames =
      orc::audio_pairs_in_frame(0, orc::VideoSystem::PAL) +
      orc::audio_pairs_in_frame(1, orc::VideoSystem::PAL) +
      orc::audio_pairs_in_frame(2, orc::VideoSystem::PAL);
  output->playPairs(three_frames);

  // The chase jumps straight to the frame the audio has reached; the frames
  // in between are simply never shown.
  EXPECT_TRUE(waitFor([&dialog]() { return dialog.currentIndex() == 3; }));

  dialog.stopPlayback();
}

// --- Invalidation ----------------------------------------------------------

TEST(PreviewDialogAudio, InvalidateAudioSource_StopsPlaybackAndDropsTheReader) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  auto* output = installFakeOutput(dialog);
  dialog.setAudioChannelPairs(twoPairs());
  ASSERT_TRUE(startAudioPlayback(dialog, 1));

  dialog.invalidateAudioSource();

  EXPECT_FALSE(dialog.isAudioPlaybackActive());
  EXPECT_FALSE(output->isStarted());
  EXPECT_EQ(dialog.playPauseButton()->text(), QString::fromUtf8("▶"));
  EXPECT_FALSE(dialog.audioController()->hasReader());

  // The dropped reader must be re-requested rather than silently reused.
  QSignalSpy reader_requested(&dialog,
                              &PreviewDialog::audioStreamReaderRequested);
  dialog.playPauseButton()->click();
  EXPECT_EQ(reader_requested.count(), 1);

  dialog.stopPlayback();
}

TEST(PreviewDialogAudio, ReselectingThePair_RequestsAReaderForTheNewPair) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  auto* output = installFakeOutput(dialog);
  dialog.setAudioChannelPairs(twoPairs());
  ASSERT_TRUE(startAudioPlayback(dialog, 1));

  QSignalSpy reader_requested(&dialog,
                              &PreviewDialog::audioStreamReaderRequested);
  dialog.audioPairCombo()->setCurrentIndex(2);

  // The old session is gone, but playback intent survives: the new pair's
  // reader is asked for without the user pressing Play again.
  EXPECT_FALSE(dialog.isAudioPlaybackActive());
  EXPECT_FALSE(output->isStarted());
  EXPECT_FALSE(dialog.audioController()->hasReader());
  ASSERT_EQ(reader_requested.count(), 1);
  EXPECT_EQ(reader_requested.at(0).at(0).toULongLong(), 1u);
  EXPECT_EQ(dialog.playPauseButton()->text(), QString::fromUtf8("⏸"));

  dialog.stopPlayback();
}

TEST(PreviewDialogAudio, ReselectingThePair_ResumesOnTheNewPairOncePrimed) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  auto* output = installFakeOutput(dialog);
  dialog.setAudioChannelPairs(twoPairs());
  ASSERT_TRUE(startAudioPlayback(dialog, 1));

  // Move off frame 0 so the resume position is observable.
  output->playPairs(orc::audio_pairs_in_frame(0, orc::VideoSystem::PAL) +
                    orc::audio_pairs_in_frame(1, orc::VideoSystem::PAL));
  ASSERT_TRUE(waitFor([&dialog]() { return dialog.currentIndex() == 2; }));

  dialog.audioPairCombo()->setCurrentIndex(2);
  dialog.setAudioStreamReader(palReader());
  ASSERT_TRUE(waitFor([&dialog]() { return dialog.isAudioPlaybackActive(); }));

  EXPECT_TRUE(output->isStarted());
  EXPECT_EQ(dialog.playPauseButton()->text(), QString::fromUtf8("⏸"));
  // Switching pair compares the same moment, so the position is kept.
  EXPECT_EQ(dialog.currentIndex(), 2);

  dialog.stopPlayback();
}

TEST(PreviewDialogAudio, ReselectingNoAudioWhilePlaying_KeepsTheVideoRunning) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  auto* output = installFakeOutput(dialog);
  dialog.setAudioChannelPairs(twoPairs());
  ASSERT_TRUE(startAudioPlayback(dialog, 1));

  QSignalSpy reader_requested(&dialog,
                              &PreviewDialog::audioStreamReaderRequested);
  dialog.audioPairCombo()->setCurrentIndex(0);  // "Mute/None"

  EXPECT_FALSE(dialog.isAudioPlaybackActive());
  EXPECT_FALSE(output->isStarted());
  EXPECT_EQ(reader_requested.count(), 0);
  // Playback continues — just without sound.
  EXPECT_EQ(dialog.playPauseButton()->text(), QString::fromUtf8("⏸"));

  dialog.stopPlayback();
}

TEST(PreviewDialogAudio, NodeChangeDuringPlayback_StopsPlayback) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  dialog.setCurrentNodeId(orc::NodeID(1));
  auto* output = installFakeOutput(dialog);
  dialog.setAudioChannelPairs(twoPairs());
  ASSERT_TRUE(startAudioPlayback(dialog, 1));

  dialog.setCurrentNodeId(orc::NodeID(2));

  EXPECT_FALSE(dialog.isAudioPlaybackActive());
  EXPECT_FALSE(output->isStarted());
  EXPECT_EQ(dialog.playPauseButton()->text(), QString::fromUtf8("▶"));
}

TEST(PreviewDialogAudio, ClosingTheDialog_StopsAudioPlayback) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  auto* output = installFakeOutput(dialog);
  dialog.setAudioChannelPairs(twoPairs());
  ASSERT_TRUE(startAudioPlayback(dialog, 1));

  dialog.close();

  EXPECT_FALSE(dialog.isAudioPlaybackActive());
  EXPECT_FALSE(output->isStarted());
}

// --- Seeking ---------------------------------------------------------------

TEST(PreviewDialogAudio, UserSeekDuringPlayback_RebasesTheAudioClock) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  auto* output = installFakeOutput(dialog);
  dialog.setAudioChannelPairs(twoPairs());
  ASSERT_TRUE(startAudioPlayback(dialog, 1));
  const int starts_before_seek = output->startCalls();

  dialog.navigateToIndex(40);

  // The device is restarted from the new position, so the clock never has to
  // carry a correction term across the seek.
  EXPECT_EQ(dialog.audioController()->startFrame(), 40u);
  EXPECT_GT(output->startCalls(), starts_before_seek);
  EXPECT_TRUE(dialog.isAudioPlaybackActive());
  EXPECT_EQ(dialog.currentIndex(), 40);

  dialog.stopPlayback();
}

TEST(PreviewDialogAudio, ScrubbingSilencesTheDeviceUntilThePositionSettles) {
  ensureApplication();
  PreviewDialog dialog;
  dialog.previewSlider()->setRange(kFirstIndex, kLastIndex);
  auto* output = installFakeOutput(dialog);
  dialog.setAudioChannelPairs(twoPairs());
  ASSERT_TRUE(startAudioPlayback(dialog, 1));

  // A drag emits a position per mouse move. Rebasing the device on each one
  // would thrash it, so it goes quiet for the duration of the scrub.
  dialog.navigateToIndexDebounced(10);
  dialog.navigateToIndexDebounced(20);
  dialog.navigateToIndexDebounced(30);
  EXPECT_FALSE(output->isStarted());
  EXPECT_EQ(output->startCalls(), 1);
  // Playback intent survives the scrub — this is a seek, not a stop.
  EXPECT_TRUE(dialog.isAudioPlaybackActive());
  EXPECT_EQ(dialog.playPauseButton()->text(), QString::fromUtf8("⏸"));

  // Once the debounce settles the clock restarts at the settled position.
  EXPECT_TRUE(waitFor([output]() { return output->isStarted(); }));
  EXPECT_EQ(output->startCalls(), 2);
  EXPECT_EQ(dialog.audioController()->startFrame(), 30u);

  dialog.stopPlayback();
}

}  // namespace gui_unit_test
