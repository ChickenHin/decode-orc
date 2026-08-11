/*
 * File:        nabts_raster_view.h
 * Module:      nabts_sink stage plugin
 * Purpose:     A NAPLPS page deposited into a receiver's pixel grid and
 *              emitted as a scalable display list
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_SINK_NABTS_RASTER_VIEW_H
#define ORC_NABTS_SINK_NABTS_RASTER_VIEW_H

#include <orc/stage/tooling/catalogue_results.h>

#include "naplps_render_grid.h"
#include "vbi-services/nabts_page.h"

namespace orc {

/**
 * @brief Draw @p snapshot into a @p grid receiver and append it to @p list
 *
 * This is the pixel-mode reading of a page: rather than handing a renderer the
 * geometry and letting it decide what a stroke of no stated width looks like,
 * the page is deposited into the receiver's frame buffer exactly as X3.110
 * describes and the resulting pixels are what get emitted.
 *
 * The pixels are emitted as run-length merged rectangles in unit space, not as
 * a bitmap, so a renderer can draw a receiver's rectangular pixels at any size
 * with their edges intact. Every run is a filled rectangle with no pen, which
 * is a shape every existing renderer of a display list already draws — the
 * pixel modes need no new operation kind.
 *
 * @p list keeps whatever palette, aspect and nominal grid the caller has
 * already set; only the operations are appended.
 */
void naplps_emit_raster_page(const NabtsPageSnapshot& snapshot,
                             NaplpsRenderGrid grid, CatalogueDisplayList& list);

}  // namespace orc

#endif  // ORC_NABTS_SINK_NABTS_RASTER_VIEW_H
