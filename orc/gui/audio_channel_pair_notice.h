/*
 * File:        audio_channel_pair_notice.h
 * Module:      orc-gui
 * Purpose:     Pure helper that notes when an audio stage's input carries no
 *              audio channel pairs (Tier 1 / gui-logic testable)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <string>

namespace orc::gui {

// Note shown in an audio stage's parameter dialog when the node's input
// carries |pair_count| audio channel pairs. Empty unless the input carries
// none: with no pairs to choose from, the channel-pair dropdown falls back to
// the stage's full container-slot list, so every selection the user can make
// is one the input cannot satisfy. The stages pass their input through
// unchanged in that case, and this explains why nothing happens.
inline std::string audioChannelPairNotice(std::size_t pair_count) {
  if (pair_count > 0) {
    return {};
  }
  return "The input to this node carries no audio channel pairs, so this "
         "stage will pass its input through unchanged. Add audio to the "
         "pipeline (for example with an Audio Import stage) before mapping or "
         "aligning channel pairs.";
}

// Note shown on the preview dialogue's audio selector when the viewed node
// carries no audio channel pairs. Separate from audioChannelPairNotice()
// because nothing is being passed through here: the preview simply has no
// audio to offer, and the user needs to know that is a property of the
// pipeline rather than a broken control.
inline std::string audioChannelPairPreviewNotice() {
  return "This stage's output carries no audio channel pairs, so there is "
         "nothing to play. Add audio to the pipeline (for example with an "
         "Audio Import stage) or view a stage downstream of one that carries "
         "audio.";
}

// Appends audioChannelPairNotice() to a stage description, separated by a
// blank line. Returns |description| unchanged when there is nothing to note.
inline std::string withAudioChannelPairNotice(const std::string& description,
                                              std::size_t pair_count) {
  const std::string note = audioChannelPairNotice(pair_count);
  if (note.empty()) {
    return description;
  }
  if (description.empty()) {
    return note;
  }
  return description + "\n\n" + note;
}

}  // namespace orc::gui
