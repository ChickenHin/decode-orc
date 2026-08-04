/*
 * File:        audio_output.cpp
 * Module:      orc-gui
 * Purpose:     Platform audio output selection for preview playback
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "audio_output.h"

#ifdef ORC_GUI_AUDIO_PLAYBACK
#include "qt_audio_output.h"
#endif

namespace orc::gui {

std::unique_ptr<IAudioOutput> createSystemAudioOutput() {
#ifdef ORC_GUI_AUDIO_PLAYBACK
  return std::make_unique<QtAudioOutput>();
#else
  // Built without an audio backend: the preview dialogue keeps its legacy
  // timer-paced video-only playback and never opens a device.
  return nullptr;
#endif
}

}  // namespace orc::gui
