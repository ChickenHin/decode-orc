/*
 * File:        colour_preview_conversion.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     Render-boundary conversion from colour carriers to PreviewImage.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/stage/preview/orc_preview_carriers.h>
#include <orc/stage/preview/orc_rendering.h>

namespace orc {

/**
 * @brief Convert a colour-domain carrier to display-target RGB888 preview.
 *
 * Conversion target is BT.709 primaries with sRGB transfer characteristics.
 * The source colorimetry is taken from carrier.colorimetry.
 */
PreviewImage render_preview_from_colour_carrier(
    const ColourFrameCarrier& carrier);

/**
 * @brief Re-order an interlaced (weaved) preview image into sequential fields.
 *
 * The weaved image has field 1 on even display rows (0-based) and field 2 on
 * odd rows.  This rearranges the rows in place so the field-1 lines form the
 * top block and the field-2 lines the bottom block, matching the
 * "sequential" layout offered by the signal-domain preview helpers.  Any
 * dropout regions are remapped to their new display rows.  Image dimensions
 * are unchanged; invalid images are left untouched.
 */
void reorder_preview_image_to_sequential_fields(PreviewImage& image);

}  // namespace orc
