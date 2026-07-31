/*
 * File:        preview_frame_layout.h
 * Module:      orc-core
 * Purpose:     Row layout mapping between preview images and field lines
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>
#include <orc/stage/preview/orc_rendering.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace orc {

/**
 * @brief How a frame preview arranges its image rows
 *
 * A frame preview carries both fields, but stages render them in one of two
 * row orders. Both report the same PreviewOutputType, so the preview option
 * id is what distinguishes them.
 */
enum class PreviewFrameLayout {
  Weaved,          ///< Rows alternate between field 1 and field 2
  FieldSequential  ///< Field 1 block above field 2 block
};

/**
 * @brief Determine the frame layout a preview option renders
 *
 * The signal-domain preview helpers expose the sequential layout as
 * "sequential_clamped"/"sequential_raw" (optionally with a "_y"/"_c"/"_yc"
 * channel suffix), and the colour-carrier path as
 * "phase2_colour_carrier_sequential". Everything else is weaved.
 */
PreviewFrameLayout preview_frame_layout_for_option(
    const std::string& option_id);

/**
 * @brief Field line counts of the frame being previewed
 *
 * Field 1 is always the top spatial field in the VFR domain. The two counts
 * differ for PAL (313/312) and are equal for the 525-line systems.
 */
struct PreviewFieldGeometry {
  size_t field1_lines = 0;
  size_t field2_lines = 0;
};

/**
 * @brief Map a preview image row to the field line it displays
 *
 * @param output_type The output type being displayed
 * @param layout Row layout of frame outputs (ignored for other output types)
 * @param output_index The displayed field/frame index (0-based)
 * @param image_y The row in the preview image
 * @param geometry Field line counts of the displayed frame
 * @return Field index and line within that field, or is_valid=false when the
 *         row falls outside the displayed fields
 */
ImageToFieldMappingResult map_preview_row_to_field(
    PreviewOutputType output_type, PreviewFrameLayout layout,
    uint64_t output_index, int image_y, const PreviewFieldGeometry& geometry);

/**
 * @brief Map a field line back to the preview image row that displays it
 *
 * The inverse of map_preview_row_to_field; used to place the line-scope
 * cross-hairs on the row whose samples the scope is showing.
 *
 * @param output_type The output type being displayed
 * @param layout Row layout of frame outputs (ignored for other output types)
 * @param output_index The displayed field/frame index (0-based)
 * @param field_index The field to locate
 * @param field_line The line within that field
 * @param geometry Field line counts of the displayed frame
 * @return Image row, or is_valid=false when the field is not on screen
 */
FieldToImageMappingResult map_field_to_preview_row(
    PreviewOutputType output_type, PreviewFrameLayout layout,
    uint64_t output_index, uint64_t field_index, int field_line,
    const PreviewFieldGeometry& geometry);

}  // namespace orc
