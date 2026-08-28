/*
 * File:        preview_data_type_resolution.h
 * Module:      orc-gui
 * Purpose:     Resolve which video data type the preview is showing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_PREVIEW_PREVIEW_DATA_TYPE_RESOLUTION_H
#define ORC_GUI_PREVIEW_PREVIEW_DATA_TYPE_RESOLUTION_H

#include <orc/stage/preview/orc_preview_types.h>

#include <algorithm>
#include <vector>

namespace orc::gui {

// True for the colour-domain (decoder output) data types.  The distinction
// decides which views apply to a stage and which vectorscope acquisition its
// output calls for.
inline bool isColourDomainDataType(orc::VideoDataType data_type) {
  return data_type == orc::VideoDataType::ColourNTSC ||
         data_type == orc::VideoDataType::ColourPAL;
}

// ============================================================================
// resolvePreviewDataType
// ============================================================================
// Reconcile the data type implied by the selected preview output type with the
// types the stage declares it can preview.
//
// The output type alone is ambiguous: a source and a chroma-decoding sink both
// offer an interlaced "Frame" mode, but only the sink produces colour.  Taking
// the implied type at face value would report a signal-domain stage as
// colour-domain, and every view registered for its real type would then be
// filtered out — which is why the scope used to be reachable only on the
// decoding sink.
//
// |candidate| is the type the output type implies; |stage_types| is the
// stage's own declaration, in its preference order.  The candidate wins when
// the stage declares it; otherwise the stage's first declared type does, since
// stages list the domain they principally produce first.  An empty
// declaration (a stage with no preview capability) leaves the candidate alone.
inline orc::VideoDataType resolvePreviewDataType(
    orc::VideoDataType candidate,
    const std::vector<orc::VideoDataType>& stage_types) {
  if (stage_types.empty()) {
    return candidate;
  }

  if (std::find(stage_types.begin(), stage_types.end(), candidate) !=
      stage_types.end()) {
    return candidate;
  }

  return stage_types.front();
}

}  // namespace orc::gui

#endif  // ORC_GUI_PREVIEW_PREVIEW_DATA_TYPE_RESOLUTION_H
