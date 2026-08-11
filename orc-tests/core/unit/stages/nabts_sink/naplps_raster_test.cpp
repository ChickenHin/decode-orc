/*
 * File:        naplps_raster_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the pel-accurate NAPLPS raster core
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_raster.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "naplps_font.h"

namespace orc {
namespace {

// A grid small enough to reason about cell by cell. 32 * 0,78125 = 25, so a
// cell is square in unit space exactly as the shipped grids' are, and one cell
// is 1/32 of the unit screen along both axes — which makes a unit coordinate
// and a cell coordinate the same number over 32.
constexpr NaplpsRenderGrid kTestGrid{32, 25};
constexpr double kCell = 1.0 / 32.0;

/// A unit-screen point at cell-space |column|, |row|.
NabtsPoint at(double column, double row) {
  return NabtsPoint{column * kCell, row * kCell};
}

/// A pel |columns| by |rows| cells.
NabtsSize pel(double columns, double rows) {
  return NabtsSize{columns * kCell, rows * kCell};
}

NaplpsInk white() {
  NaplpsInk ink;
  ink.colour = kNabtsNominalWhite;
  return ink;
}

/// The cells painted in a row, as a string of '#' and '.', for golden patterns
/// that read as pictures rather than as coordinates.
std::string row_pattern(const NaplpsCellSurface& surface, int row,
                        int first_column, int last_column) {
  std::string out;
  for (int column = first_column; column <= last_column; ++column) {
    out.push_back(surface.at(column, row).painted ? '#' : '.');
  }
  return out;
}

/// The cells painted in a column, bottom row first.
std::string column_pattern(const NaplpsCellSurface& surface, int column,
                           int first_row, int last_row) {
  std::string out;
  for (int row = first_row; row <= last_row; ++row) {
    out.push_back(surface.at(column, row).painted ? '#' : '.');
  }
  return out;
}

// ---------------------------------------------------------------------------
// The pel brush
// ---------------------------------------------------------------------------

// §5.3.2.2.6: "the logical pel, therefore, will always map to at least one and
// possibly many display pixels". The default pel is dimensionless, and a point
// drawn with it still lights the pixel the drawing point falls in.
TEST(NaplpsRasterTest, ADimensionlessPelStillPaintsOnePixel) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stamp_pel(at(3.5, 7.5), NabtsSize{0.0, 0.0}, white());

  EXPECT_EQ(surface.painted_count(), 1u);
  EXPECT_TRUE(surface.at(3, 7).painted);
}

// "All of those pixels that lie under any portion of the logical pel": a pel
// three cells wide by two high, laid on a cell boundary, is exactly six pixels.
TEST(NaplpsRasterTest, APelPaintsEveryPixelItCovers) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stamp_pel(at(4.0, 6.0), pel(3.0, 2.0), white());

  EXPECT_EQ(surface.painted_count(), 6u);
  EXPECT_EQ(row_pattern(surface, 6, 3, 8), ".###..");
  EXPECT_EQ(row_pattern(surface, 7, 3, 8), ".###..");
  EXPECT_FALSE(surface.at(4, 8).painted);
}

// The pel is a brush, and §5.3.2.2.6 sizes it without reference to where it
// happens to be: one of a given size is the same number of pixels wherever it
// is put down. A brush that grew a pixel whenever it straddled a boundary would
// draw a line whose weight varied along its own length.
TEST(NaplpsRasterTest, APelIsTheSameSizeWhereverItFallsOnTheGrid) {
  for (const double offset : {0.0, 0.25, 0.5, 0.75}) {
    NaplpsCellSurface surface(kTestGrid);
    NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

    raster.stamp_pel(at(4.0 + offset, 6.0 + offset), pel(1.0, 1.0), white());

    EXPECT_EQ(surface.painted_count(), 1u) << "at an offset of " << offset;
    EXPECT_EQ(row_pattern(surface, 6, 3, 7), ".#...")
        << "at an offset of " << offset;
  }
}

// And the size it is given is the size it comes out: a two-cell pel is two
// cells, not the three a boundary-straddling brush would cover.
TEST(NaplpsRasterTest, APelIsAsManyPixelsAsItIsWide) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stamp_pel(at(4.5, 6.5), pel(2.0, 2.0), white());

  EXPECT_EQ(surface.painted_count(), 4u);
  EXPECT_EQ(row_pattern(surface, 6, 3, 8), ".##...");
  EXPECT_EQ(row_pattern(surface, 7, 3, 8), ".##...");
}

// §5.3.2.2.6 puts the drawing point at the pel's lower left when both
// dimensions are positive and at its other three corners for the other three
// sign combinations, so a negative width reaches back from the drawing point
// rather than forward.
TEST(NaplpsRasterTest, ThePelSignDecidesWhichCornerTheDrawingPointIs) {
  NaplpsCellSurface positive(kTestGrid);
  NaplpsRasteriser(positive, NaplpsGridMapping{kTestGrid})
      .stamp_pel(at(10.0, 4.0), pel(4.0, 2.0), white());
  EXPECT_EQ(row_pattern(positive, 4, 5, 14), ".....####.");

  NaplpsCellSurface negative_width(kTestGrid);
  NaplpsRasteriser(negative_width, NaplpsGridMapping{kTestGrid})
      .stamp_pel(at(10.0, 4.0), pel(-4.0, 2.0), white());
  EXPECT_EQ(row_pattern(negative_width, 4, 5, 14), ".####.....");

  NaplpsCellSurface negative_height(kTestGrid);
  NaplpsRasteriser(negative_height, NaplpsGridMapping{kTestGrid})
      .stamp_pel(at(10.0, 4.0), pel(4.0, -2.0), white());
  EXPECT_EQ(column_pattern(negative_height, 10, 1, 5), ".##..");
}

// "A LINE is a locus of points following a straight line algorithm ... The
// physical picture elements through which the infinitely small locus point
// passes would be drawn." With no pel to widen it, a diagonal is one cell wide.
TEST(NaplpsRasterTest, ADimensionlessPelDrawsAOneCellWideLocus) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stroke_path({at(0.5, 0.5), at(10.5, 10.5)}, NabtsSize{0.0, 0.0},
                     NabtsLineTexture::kSolid, white());

  // Eleven cells, one per step of the diagonal, and nothing beside them.
  EXPECT_EQ(surface.painted_count(), 11u);
  for (int i = 0; i <= 10; ++i) {
    EXPECT_TRUE(surface.at(i, i).painted) << "cell " << i << " of the locus";
  }
}

// The pel is a brush that "turns on additional pixels as it traverses its
// geometric path and generates the effect of line width". A horizontal line
// swept with a pel four wide and two high is a band two cells tall, running a
// pel's width past the end point because the pel is anchored at its left edge.
TEST(NaplpsRasterTest, ThePelGivesALineItsWidth) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stroke_path({at(2.0, 4.0), at(12.0, 4.0)}, pel(4.0, 2.0),
                     NabtsLineTexture::kSolid, white());

  EXPECT_EQ(row_pattern(surface, 4, 0, 17), "..##############..");
  EXPECT_EQ(row_pattern(surface, 5, 0, 17), "..##############..");
  EXPECT_EQ(row_pattern(surface, 3, 0, 17), "..................");
  EXPECT_EQ(row_pattern(surface, 6, 0, 17), "..................");
}

// A coordinate outside the display area is not drawn. Table D1 item 10 makes
// the area the part a receiver guarantees is visible, and the surface is that
// area alone.
TEST(NaplpsRasterTest, DrawingOffTheSurfaceIsDropped) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stamp_pel(at(-5.0, 4.0), pel(2.0, 2.0), white());
  raster.stamp_pel(at(4.0, 200.0), pel(2.0, 2.0), white());

  EXPECT_EQ(surface.painted_count(), 0u);
}

// ---------------------------------------------------------------------------
// Line textures (§5.3.2.4.2)
// ---------------------------------------------------------------------------

// "The size of the dot is set equal to the size of the logical pel. For
// horizontal lines, the inter-dot spacing is the width of the logical pel." A
// pel two cells wide therefore gives two cells of dot and two of gap.
TEST(NaplpsRasterTest, ADottedLineAlternatesOnePelOnAndOnePelOff) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stroke_path({at(0.0, 4.0), at(16.0, 4.0)}, pel(2.0, 1.0),
                     NabtsLineTexture::kDotted, white());

  EXPECT_EQ(row_pattern(surface, 4, 0, 15), "##..##..##..##..");
}

// "The width (length) of the dash and the inter-dash spacing are equal to three
// times the width of the logical pel."
TEST(NaplpsRasterTest, ADashedLineIsThreePelsOnAndThreeOff) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stroke_path({at(0.0, 4.0), at(24.0, 4.0)}, pel(2.0, 1.0),
                     NabtsLineTexture::kDashed, white());

  EXPECT_EQ(row_pattern(surface, 4, 0, 23), "######......######......");
}

// Dash of three pels, gap of one, dot of one, gap of one — "the inter-dot-dash
// spacing is equivalent to the inter-dot spacing".
TEST(NaplpsRasterTest, ADottedDashedLineRunsDashGapDotGap) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stroke_path({at(0.0, 4.0), at(24.0, 4.0)}, pel(2.0, 1.0),
                     NabtsLineTexture::kDottedDashed, white());

  EXPECT_EQ(row_pattern(surface, 4, 0, 23), "######..##..######..##..");
}

// For a vertical line the spacings are the pel's height rather than its width,
// so the same texture reads down the line at the other dimension.
TEST(NaplpsRasterTest, AVerticalLineIsTexturedByThePelHeight) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stroke_path({at(6.0, 0.0), at(6.0, 16.0)}, pel(1.0, 2.0),
                     NabtsLineTexture::kDotted, white());

  EXPECT_EQ(column_pattern(surface, 6, 0, 15), "##..##..##..##..");
}

// §5.3.2.4.2's closing note: "If logical pel size dx = 0, all nonvertical lines
// are solid. If logical pel size dy = 0, all nonhorizontal lines are solid."
// With no extent along the line there is no dot to space, so the texture has
// nothing to work with and the line comes out solid.
TEST(NaplpsRasterTest, ATextureWithNoPelAlongTheLineDrawsSolid) {
  NaplpsCellSurface horizontal(kTestGrid);
  NaplpsRasteriser(horizontal, NaplpsGridMapping{kTestGrid})
      .stroke_path({at(0.0, 4.0), at(12.0, 4.0)}, pel(0.0, 2.0),
                   NabtsLineTexture::kDashed, white());
  EXPECT_EQ(row_pattern(horizontal, 4, 0, 11), "############");

  // A diagonal is nonvertical too, so a pel of no width leaves it solid.
  NaplpsCellSurface diagonal(kTestGrid);
  NaplpsRasteriser(diagonal, NaplpsGridMapping{kTestGrid})
      .stroke_path({at(0.5, 0.5), at(10.5, 10.5)}, pel(0.0, 2.0),
                   NabtsLineTexture::kDotted, white());
  for (int i = 0; i <= 10; ++i) {
    EXPECT_TRUE(diagonal.at(i, i).painted) << "cell " << i << " of a solid run";
  }
}

// "All end points of lines and arcs and all vertices ... must be plotted
// regardless of the line texture used." The end point here falls in a gap of
// the pattern and is drawn anyway.
TEST(NaplpsRasterTest, EndPointsArePlottedThroughAGapInTheTexture) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  // The line stops two cells into the gap that follows the first dot.
  raster.stroke_path({at(0.0, 4.0), at(3.0, 4.0)}, pel(1.0, 1.0),
                     NabtsLineTexture::kDotted, white());

  EXPECT_TRUE(surface.at(0, 4).painted) << "the start point";
  EXPECT_TRUE(surface.at(3, 4).painted) << "the end point, mid-gap";
}

// ---------------------------------------------------------------------------
// Fills, hatching and highlight
// ---------------------------------------------------------------------------

/// The four corners of a rectangle as a closed path.
std::vector<NabtsPoint> rectangle(double column, double row, double columns,
                                  double rows) {
  return {at(column, row), at(column + columns, row),
          at(column + columns, row + rows), at(column, row + rows)};
}

// A line is drawn by sweeping the pel along it, and §5.3.2.2.6 sizes the pel
// without reference to the direction of the sweep, so a line comes out the
// pel's weight whichever way it runs. Measured across the grid, that is the
// count of cells in a cross-section taken at right angles to the axis the
// sweep is stepping along: one cell for a one-cell pel, running horizontally,
// vertically or at any slope between.
TEST(NaplpsRasterTest, ALineIsThePelsWeightWhicheverWayItRuns) {
  struct Direction {
    const char* name;
    int run;
    int rise;
  };
  const Direction directions[] = {{"horizontal", 16, 0}, {"vertical", 0, 16},
                                  {"shallow", 16, 5},    {"steep", 5, 16},
                                  {"diagonal", 16, 16},  {"backwards", -16, 7}};

  for (const Direction& direction : directions) {
    // Started wherever the line fits on the grid, which for one running
    // leftwards is the far side.
    const int start_column = direction.run < 0 ? 24 : 8;
    NaplpsCellSurface surface(kTestGrid);
    NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
    raster.stroke_path({at(start_column, 3.0),
                        at(start_column + direction.run, 3.0 + direction.rise)},
                       pel(1.0, 1.0), NabtsLineTexture::kSolid, white());

    // Across whichever axis the sweep steps along, so that the section is taken
    // across the line rather than along it.
    const bool steps_along_columns =
        std::abs(direction.run) >= std::abs(direction.rise);
    const int sections = steps_along_columns ? std::abs(direction.run)
                                             : std::abs(direction.rise);
    for (int section = 1; section < sections; ++section) {
      int painted = 0;
      for (int across = 0;
           across < (steps_along_columns ? kTestGrid.height : kTestGrid.width);
           ++across) {
        const int column =
            steps_along_columns
                ? start_column + (direction.run < 0 ? -section : section)
                : across;
        const int row = steps_along_columns ? across : 3 + section;
        painted += surface.at(column, row).painted ? 1 : 0;
      }
      EXPECT_EQ(painted, 1) << "a one-cell pel drawn " << direction.name
                            << ", across section " << section;
    }
  }
}

// And the weight follows the pel: state it two cells and the line is two cells
// across, not the three a brush measured by what it covered would give.
TEST(NaplpsRasterTest, ALineIsAsManyCellsAcrossAsThePelIsWide) {
  NaplpsCellSurface horizontal(kTestGrid);
  NaplpsRasteriser(horizontal, NaplpsGridMapping{kTestGrid})
      .stroke_path({at(4.5, 6.5), at(16.5, 6.5)}, pel(2.0, 2.0),
                   NabtsLineTexture::kSolid, white());
  EXPECT_EQ(column_pattern(horizontal, 10, 4, 9), "..##..");

  NaplpsCellSurface vertical(kTestGrid);
  NaplpsRasteriser(vertical, NaplpsGridMapping{kTestGrid})
      .stroke_path({at(6.5, 4.5), at(6.5, 16.5)}, pel(2.0, 2.0),
                   NabtsLineTexture::kSolid, white());
  EXPECT_EQ(row_pattern(vertical, 10, 4, 9), "..##..");
}

// An outlined figure is its outline alone: the pel sweeps the edges and the
// inside is left showing whatever was under it.
TEST(NaplpsRasterTest, AnOutlinedRectangleIsHollow) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stroke_path(rectangle(4.0, 4.0, 6.0, 5.0), NabtsSize{0.0, 0.0},
                     NabtsLineTexture::kSolid, white(), /*closed=*/true);

  EXPECT_EQ(row_pattern(surface, 4, 3, 11), ".#######.");
  EXPECT_EQ(row_pattern(surface, 6, 3, 11), ".#.....#.");
  EXPECT_EQ(row_pattern(surface, 9, 3, 11), ".#######.");
}

// §5.3.3.4.1 fills "the area enclosed by the outline (including the region of
// the outline traced by the logical pel)", so a filled rectangle is the
// outlined one solid: the same cells @ref AnOutlinedRectangleIsHollow leaves
// round the edge, with the middle filled in too.
TEST(NaplpsRasterTest, AFilledRectangleIsTheOutlinedOneSolid) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.fill_path(rectangle(4.0, 4.0, 6.0, 5.0), NabtsSize{0.0, 0.0},
                   NabtsTexturePattern::kSolid, NabtsSize{0.0, 0.0}, nullptr,
                   white());

  EXPECT_EQ(row_pattern(surface, 4, 3, 11), ".#######.");
  EXPECT_EQ(row_pattern(surface, 6, 3, 11), ".#######.");
  EXPECT_EQ(row_pattern(surface, 9, 3, 11), ".#######.");
  EXPECT_EQ(surface.painted_count(), 42u);
}

// The traced outline is part of the filled region, not a line over it: a path
// enclosing no area at all still draws, because the pel gives it its weight.
// This is what a service relies on when it draws a letterform as a stroke.
TEST(NaplpsRasterTest, AFilledFigureIncludesTheOutlineThePelTraces) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  // Out and back along one line: zero enclosed area.
  raster.fill_path({at(6.0, 4.0), at(6.0, 12.0), at(6.0, 4.0)}, pel(2.0, 1.0),
                   NabtsTexturePattern::kSolid, NabtsSize{0.0, 0.0}, nullptr,
                   white());

  EXPECT_GT(surface.painted_count(), 0u)
      << "the pel traced no outline, so the figure drew nothing at all";
  EXPECT_EQ(row_pattern(surface, 8, 4, 9), "..##..");
}

// And a dimensionless pel traces a one-pixel outline rather than none, because
// §5.3.2.2.6 maps the pel to "at least one" pixel however small it is stated.
// The same path stroked with the same pel draws the same line, which is what a
// fill and the outline a service draws over it depend on.
TEST(NaplpsRasterTest, ADimensionlessPelTracesAFillsOutlineOnePixelWide) {
  const std::vector<NabtsPoint> out_and_back = {at(6.0, 4.0), at(6.0, 12.0),
                                                at(6.0, 4.0)};

  NaplpsCellSurface filled(kTestGrid);
  NaplpsRasteriser(filled, NaplpsGridMapping{kTestGrid})
      .fill_path(out_and_back, NabtsSize{0.0, 0.0}, NabtsTexturePattern::kSolid,
                 NabtsSize{0.0, 0.0}, nullptr, white());

  NaplpsCellSurface stroked(kTestGrid);
  NaplpsRasteriser(stroked, NaplpsGridMapping{kTestGrid})
      .stroke_path(out_and_back, NabtsSize{0.0, 0.0}, NabtsLineTexture::kSolid,
                   white(), /*closed=*/true);

  EXPECT_EQ(filled.painted_count(), 9u);
  EXPECT_EQ(column_pattern(filled, 6, 4, 12), "#########");
  EXPECT_EQ(filled.painted_count(), stroked.painted_count());
}

// §5.3.2.4.4: "The width and spacing of hatching lines in the vertical hatching
// pattern are equal to the width of the logical pel." A pel two cells wide
// gives two cells of line and two of gap.
TEST(NaplpsRasterTest, VerticalHatchingIsSpacedByThePelWidth) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.fill_path(rectangle(0.0, 4.0, 16.0, 4.0), pel(2.0, 2.0),
                   NabtsTexturePattern::kVerticalHatch, NabtsSize{0.0, 0.0},
                   nullptr, white());

  EXPECT_EQ(row_pattern(surface, 5, 0, 15), "##..##..##..##..");
  EXPECT_EQ(row_pattern(surface, 6, 0, 15), "##..##..##..##..");
}

// The hatch is registered against the unit screen's origin rather than the
// figure's, which is what §5.3.2.4.4 means by "registration of the patterns
// shall be maintained across figures if the logical pel size is the same". Two
// figures at different offsets therefore hatch in step.
TEST(NaplpsRasterTest, HatchingKeepsRegistrationAcrossFigures) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.fill_path(rectangle(0.0, 4.0, 8.0, 2.0), pel(2.0, 2.0),
                   NabtsTexturePattern::kVerticalHatch, NabtsSize{0.0, 0.0},
                   nullptr, white());
  // A second figure starting at an odd column: were the pattern anchored to the
  // figure it would start a line here and fall out of step with the first.
  raster.fill_path(rectangle(9.0, 8.0, 7.0, 2.0), pel(2.0, 2.0),
                   NabtsTexturePattern::kVerticalHatch, NabtsSize{0.0, 0.0},
                   nullptr, white());

  // Every painted cell of both figures falls in an even-numbered band of two
  // columns, counted from the unit screen's origin — which is what being in
  // register means, and is not something either figure's own extent could give.
  int painted = 0;
  for (int row = 0; row < kTestGrid.height; ++row) {
    for (int column = 0; column < kTestGrid.width; ++column) {
      if (!surface.at(column, row).painted) {
        continue;
      }
      ++painted;
      EXPECT_EQ((column / 2) % 2, 0)
          << "cell " << column << "," << row << " is in a gap of the hatch";
    }
  }
  EXPECT_GT(painted, 0);
  // Both figures drew: the second one's rows are above the first one's.
  EXPECT_TRUE(surface.at(0, 4).painted);
  EXPECT_TRUE(surface.at(12, 8).painted);
}

// §5.3.2.4.3: a highlighted figure is filled as usual and its outline drawn
// solid in nominal black (colour modes 0 and 1), so the outline reads as a
// different colour from the fill rather than as more of it.
TEST(NaplpsRasterTest, AHighlightOutlinesTheFillInItsOwnColour) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  const std::vector<NabtsPoint> path = rectangle(4.0, 4.0, 6.0, 5.0);
  raster.fill_path(path, NabtsSize{0.0, 0.0}, NabtsTexturePattern::kSolid,
                   NabtsSize{0.0, 0.0}, nullptr, white());

  NaplpsInk black;
  black.colour = kNabtsNominalBlack;
  raster.highlight_path(path, NabtsSize{0.0, 0.0}, black);

  EXPECT_EQ(surface.at(4, 4).colour.red, kNabtsNominalBlack.red)
      << "the outline should be the highlight colour";
  EXPECT_EQ(surface.at(6, 6).colour.red, kNabtsNominalWhite.red)
      << "the inside should still be the fill colour";
}

// ---------------------------------------------------------------------------
// Arcs, circles and splines (§5.3.3.3)
// ---------------------------------------------------------------------------

// "If the end point is omitted, it is taken to be coincident with the start
// point and a circle is drawn", the intermediate point being diametrically
// opposite. Drawn with no pel it is a closed ring one cell thick.
TEST(NaplpsRasterTest, CoincidentEndPointsDrawACircle) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  // A circle of radius 5 cells centred on cell (11, 11).
  const std::vector<NabtsPoint> outline =
      raster.arc_polyline({at(6.0, 11.0), at(16.0, 11.0), at(6.0, 11.0)});
  raster.stroke_path(outline, NabtsSize{0.0, 0.0}, NabtsLineTexture::kSolid,
                     white());

  // The ring passes through the four extremes of the circle and leaves the
  // middle untouched.
  EXPECT_TRUE(surface.at(6, 11).painted);
  EXPECT_TRUE(surface.at(11, 6).painted);
  EXPECT_TRUE(surface.at(11, 16).painted);
  EXPECT_FALSE(surface.at(11, 11).painted) << "a stroked circle is hollow";
}

// "If the three drawing points are colinear, a line is drawn from the start
// point to the end point" — an arc of unbounded radius is not attempted.
TEST(NaplpsRasterTest, ColinearControlPointsDrawALine) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  const std::vector<NabtsPoint> outline =
      raster.arc_polyline({at(2.5, 4.5), at(6.5, 4.5), at(10.5, 4.5)});
  raster.stroke_path(outline, NabtsSize{0.0, 0.0}, NabtsLineTexture::kSolid,
                     white());

  EXPECT_EQ(row_pattern(surface, 4, 0, 13), "..#########...");
}

// The arc runs the way round that passes through the intermediate point, which
// is the only thing that distinguishes the two arcs joining its ends.
TEST(NaplpsRasterTest, AnArcRunsThroughItsIntermediatePoint) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  // Ends level at cell row 11, intermediate point below them: the arc dips.
  const std::vector<NabtsPoint> outline =
      raster.arc_polyline({at(6.0, 11.0), at(11.0, 6.0), at(16.0, 11.0)});
  raster.stroke_path(outline, NabtsSize{0.0, 0.0}, NabtsLineTexture::kSolid,
                     white());

  EXPECT_TRUE(surface.at(11, 6).painted) << "the arc should pass below";
  EXPECT_FALSE(surface.at(11, 16).painted)
      << "the arc took the long way round instead";
}

// §5.3.3.3: "The minimum implementation of the spline shall be a series of
// lines connecting the start, intermediate, and end points of the spline", and
// the smooth form is reserved for future standardization.
TEST(NaplpsRasterTest, MoreThanThreePointsDrawAsAPolyline) {
  const std::vector<NabtsPoint> control = {at(2.0, 2.0), at(6.0, 10.0),
                                           at(12.0, 4.0), at(18.0, 12.0)};
  const std::vector<NabtsPoint> line =
      naplps_arc_polyline(control, 0.2 * kCell);
  EXPECT_EQ(line.size(), control.size());
  for (size_t i = 0; i < control.size(); ++i) {
    EXPECT_DOUBLE_EQ(line[i].x, control[i].x);
    EXPECT_DOUBLE_EQ(line[i].y, control[i].y);
  }
}

// A filled arc encloses the area between the curve and the chord joining its
// ends (§5.3.3.3), so the middle of that region is painted.
TEST(NaplpsRasterTest, AFilledArcEnclosesTheAreaUpToItsChord) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  const std::vector<NabtsPoint> outline =
      raster.arc_polyline({at(6.0, 12.0), at(11.0, 6.0), at(16.0, 12.0)});
  raster.fill_path(outline, NabtsSize{0.0, 0.0}, NabtsTexturePattern::kSolid,
                   NabtsSize{0.0, 0.0}, nullptr, white());

  EXPECT_TRUE(surface.at(11, 9).painted)
      << "between the curve and its chord should be filled";
  EXPECT_FALSE(surface.at(11, 16).painted)
      << "outside the arc should be left alone";
}

// ---------------------------------------------------------------------------
// Incremental colour runs (§5.3.3.6.3)
// ---------------------------------------------------------------------------

/// |count| inks, alternating so a raster's layout is readable from the result.
std::vector<NaplpsInk> alternating(size_t count) {
  std::vector<NaplpsInk> out;
  for (size_t i = 0; i < count; ++i) {
    NaplpsInk ink;
    ink.colour = (i % 2 == 0) ? kNabtsNominalWhite : kNabtsNominalBlack;
    out.push_back(ink);
  }
  return out;
}

// The colours are laid raster-sequentially across the field, one logical pel
// apiece, wrapping at the field's edge — so the pel is the raster cell and the
// receiver's resolution shows in this primitive most directly.
TEST(NaplpsRasterTest, AColourRunRastersOnePelPerEntryAcrossTheField) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  // A field eight cells wide with a pel two cells across wraps every four
  // entries.
  raster.deposit_colour_run(at(4.0, 8.0), NabtsSize{8.0 * kCell, 4.0 * kCell},
                            pel(2.0, 2.0), alternating(8));

  // The first row of the raster sits at the top of the field, the second below
  // it: a raster runs top down while unit y runs up.
  EXPECT_EQ(row_pattern(surface, 10, 3, 12), ".########.");
  EXPECT_EQ(row_pattern(surface, 8, 3, 12), ".########.");
  EXPECT_EQ(surface.at(4, 10).colour.red, kNabtsNominalWhite.red);
  EXPECT_EQ(surface.at(6, 10).colour.red, kNabtsNominalBlack.red);
  // Entry 4 begins the second row, back at the field's left edge.
  EXPECT_EQ(surface.at(4, 8).colour.red, kNabtsNominalWhite.red);
}

// A dimensionless pel still has a pixel to fill, so the raster falls back to
// one cell of the grid per entry — which is what makes the same run finer on a
// finer receiver.
TEST(NaplpsRasterTest, AColourRunWithNoPelRastersOneCellPerEntry) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.deposit_colour_run(at(4.0, 8.0), NabtsSize{4.0 * kCell, 2.0 * kCell},
                            NabtsSize{0.0, 0.0}, alternating(4));

  EXPECT_EQ(row_pattern(surface, 9, 3, 9), ".####..");
  EXPECT_EQ(surface.painted_count(), 4u);
}

// ---------------------------------------------------------------------------
// Characters, mosaics and downloadable glyphs
// ---------------------------------------------------------------------------

/// A character primitive of |code| in |repertoire|, its field |columns| by
/// |rows| cells with its origin at the given cell position.
NabtsPrimitive character_at(uint8_t code, NabtsPrimitive::Repertoire repertoire,
                            double column, double row, double columns,
                            double rows) {
  NabtsPrimitive primitive;
  primitive.kind = NabtsPrimitiveKind::kCharacter;
  primitive.character = code;
  primitive.repertoire = repertoire;
  primitive.origin = at(column, row);
  primitive.size = NabtsSize{columns * kCell, rows * kCell};
  return primitive;
}

// A character is deposited as the pattern a receiver stored for it, so what
// lands on the grid is a glyph rather than a box: some cells of the field come
// on and some do not.
TEST(NaplpsRasterTest, ACharacterDepositsItsStoredPattern) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.deposit_character(
      character_at('H', NabtsPrimitive::Repertoire::kPrimary, 4, 4, 6, 10), {},
      white(), nullptr);

  const size_t painted = surface.painted_count();
  EXPECT_GT(painted, 0u) << "the character drew nothing at all";
  EXPECT_LT(painted, 60u) << "the character filled its whole field";
}

// The default character field of §5.3.2.3.9, deposited at a place that is a
// whole number of cells from the origin on all three grids: unit x 16/256 and
// y 100/256 are columns 16, 32 and 48 and rows 100, 200 and 300.
NabtsPrimitive default_field_character(uint8_t code) {
  NabtsPrimitive primitive;
  primitive.kind = NabtsPrimitiveKind::kCharacter;
  primitive.character = code;
  primitive.repertoire = NabtsPrimitive::Repertoire::kPrimary;
  primitive.origin = NabtsPoint{16.0 / 256.0, 100.0 / 256.0};
  primitive.size = NabtsSize{1.0 / 40.0, 5.0 / 128.0};
  return primitive;
}

/// Deposit |code| in the default character field on |grid|.
void deposit_default_field(NaplpsCellSurface& surface, uint8_t code) {
  NaplpsRasteriser(surface, NaplpsGridMapping{surface.grid()})
      .deposit_character(default_field_character(code), {}, white(), nullptr);
}

/// Whether the reference receiver's 6 by 10 face would light the cell
/// |column|, |row| of a |columns| by |rows| field measured from the field's
/// lower left — the pattern a receiver holding only that one face deposits,
/// by the same nearest-neighbour rule the deposit uses.
bool reference_face_lights(char32_t code, int columns, int rows, int column,
                           int row) {
  const NaplpsFontFace& face = naplps_font_face(0);
  const uint16_t* pattern = naplps_face_pattern(face, code);
  if (pattern == nullptr) {
    return false;
  }
  const int from_top = rows - 1 - row;
  const int source_row =
      std::min(face.height - 1, face.height * from_top / rows);
  const int source_column =
      std::min(face.width - 1, face.width * column / columns);
  return (pattern[source_row] & (1u << (face.width - 1 - source_column))) != 0u;
}

/// How many cells of the field the deposit and the reference face disagree on.
int cells_differing_from_the_reference_face(const NaplpsRenderGrid& grid,
                                            uint8_t code) {
  NaplpsCellSurface surface(grid);
  deposit_default_field(surface, code);

  const int scale = grid.width / kNaplpsGridReference.width;
  const NaplpsGridMapping mapping{grid};
  const int columns =
      static_cast<int>(std::lround(mapping.columns_across(1.0 / 40.0)));
  const int rows = static_cast<int>(std::lround(mapping.rows_up(5.0 / 128.0)));

  int differing = 0;
  for (int row = 0; row < rows; ++row) {
    for (int column = 0; column < columns; ++column) {
      const bool drawn =
          surface.at(16 * scale + column, 100 * scale + row).painted;
      if (drawn != reference_face_lights(code, columns, rows, column, row)) {
        ++differing;
      }
    }
  }
  return differing;
}

// §5.1 constrains the character patterns only "for a given display
// resolution", and a receiver with more pixels held a generator with finer
// ones. So a finer grid has to deposit a different pattern rather than the
// reference receiver's one sampled up: with one face for every receiver the
// three grids draw the same shape over more cells, which is what this
// disagreement count would find at zero.
TEST(NaplpsRasterTest, AFinerReceiverDrawsAFinerLetterform) {
  EXPECT_EQ(cells_differing_from_the_reference_face(kNaplpsGridReference, 'R'),
            0)
      << "the reference receiver drew something other than its own face";
  EXPECT_GT(cells_differing_from_the_reference_face(kNaplpsGridTwice, 'R'), 0)
      << "the 512 by 400 receiver drew the reference face sampled up";
  EXPECT_GT(cells_differing_from_the_reference_face(kNaplpsGridThrice, 'R'), 0)
      << "the 768 by 600 receiver drew the reference face sampled up";
}

// Whichever face a receiver draws from, the character stays inside the field
// the page gave it: a finer pattern is not licence to spill into the character
// beside it.
TEST(NaplpsRasterTest, ACharacterStaysInsideItsFieldAtEveryResolution) {
  for (const NaplpsRenderGrid& grid :
       {kNaplpsGridReference, kNaplpsGridTwice, kNaplpsGridThrice}) {
    const int scale = grid.width / kNaplpsGridReference.width;
    NaplpsCellSurface surface(grid);
    deposit_default_field(surface, 'R');

    const NaplpsGridMapping mapping{grid};
    const int first_column = 16 * scale;
    const int first_row = 100 * scale;
    const int columns =
        static_cast<int>(std::lround(mapping.columns_across(1.0 / 40.0)));
    const int rows =
        static_cast<int>(std::lround(mapping.rows_up(5.0 / 128.0)));

    for (int row = 0; row < grid.height; ++row) {
      for (int column = 0; column < grid.width; ++column) {
        if (!surface.at(column, row).painted) {
          continue;
        }
        EXPECT_GE(column, first_column) << grid.width;
        EXPECT_LT(column, first_column + columns) << grid.width;
        EXPECT_GE(row, first_row) << grid.width;
        EXPECT_LT(row, first_row + rows) << grid.width;
      }
    }
  }
}

// The reference receiver is the era-accurate one, and its generator is the
// 6 by 10 alone: at its own default field the cells it deposits are that face's
// pattern, one element to one pixel, with nothing scaled or resampled.
TEST(NaplpsRasterTest, TheReferenceReceiverDepositsItsFacePatternExactly) {
  NaplpsCellSurface surface(kNaplpsGridReference);
  deposit_default_field(surface, 'R');

  const NaplpsFontFace& face =
      naplps_font_face_for_field(kNaplpsGridReference, 6, 10);
  ASSERT_EQ(std::string(face.name), "misc-fixed 6x10");
  const uint16_t* pattern = naplps_face_pattern(face, U'R');
  ASSERT_NE(pattern, nullptr);

  for (int row = 0; row < face.height; ++row) {
    for (int column = 0; column < face.width; ++column) {
      // The pattern's rows run top down while the field's run up.
      const bool stored =
          (pattern[row] & (1u << (face.width - 1 - column))) != 0u;
      EXPECT_EQ(surface.at(16 + column, 100 + face.height - 1 - row).painted,
                stored)
          << "row " << row << " column " << column;
    }
  }
}

// §5.3.2.3.9's character size is continuously variable, and a receiver meets it
// by depositing the same stored pattern over more pixels — so a field twice the
// size shows the same shape, larger, rather than a different one.
TEST(NaplpsRasterTest, ADoubleSizeFieldScalesTheSamePattern) {
  NaplpsCellSurface normal(kTestGrid);
  NaplpsRasteriser(normal, NaplpsGridMapping{kTestGrid})
      .deposit_character(
          character_at('H', NabtsPrimitive::Repertoire::kPrimary, 2, 2, 6, 10),
          {}, white(), nullptr);

  NaplpsCellSurface doubled(kTestGrid);
  NaplpsRasteriser(doubled, NaplpsGridMapping{kTestGrid})
      .deposit_character(
          character_at('H', NabtsPrimitive::Repertoire::kPrimary, 2, 2, 12, 20),
          {}, white(), nullptr);

  // Four times the area for twice the field in each direction, near enough:
  // nearest-neighbour puts each stored element under a 2 by 2 block.
  EXPECT_NEAR(static_cast<double>(doubled.painted_count()),
              4.0 * static_cast<double>(normal.painted_count()),
              0.35 * 4.0 * static_cast<double>(normal.painted_count()));
}

// §6.2.7.4: reverse video fills the character field and leaves the shape
// undrawn, so the cells that were on and off swap over.
TEST(NaplpsRasterTest, ReverseVideoFillsTheFieldAndLeavesTheShape) {
  NabtsPrimitive normal =
      character_at('H', NabtsPrimitive::Repertoire::kPrimary, 4, 4, 6, 10);
  NabtsPrimitive reversed = normal;
  reversed.reverse_video = true;

  NaplpsInk ground;
  ground.colour = kNabtsNominalBlack;

  NaplpsCellSurface plain(kTestGrid);
  NaplpsRasteriser(plain, NaplpsGridMapping{kTestGrid})
      .deposit_character(normal, {}, white(), &ground);

  NaplpsCellSurface inverted(kTestGrid);
  NaplpsRasteriser(inverted, NaplpsGridMapping{kTestGrid})
      .deposit_character(reversed, {}, white(), &ground);

  // Both cover the same field; a cell white in one is black in the other.
  EXPECT_EQ(plain.painted_count(), inverted.painted_count());
  int swapped = 0;
  for (int row = 4; row < 14; ++row) {
    for (int column = 4; column < 10; ++column) {
      const bool lit_plain = plain.at(column, row).colour == kNabtsNominalWhite;
      const bool lit_inverted =
          inverted.at(column, row).colour == kNabtsNominalWhite;
      if (lit_plain != lit_inverted) {
        ++swapped;
      }
    }
  }
  EXPECT_GT(swapped, 0) << "reverse video drew the same thing as normal video";
}

// §5.3.2.3.1 rotates the character field counter-clockwise about its origin, so
// a field six wide by ten tall covers ten cells across and six up at 90
// degrees.
TEST(NaplpsRasterTest, RotationTurnsTheCharacterField) {
  NabtsPrimitive rotated =
      character_at('H', NabtsPrimitive::Repertoire::kPrimary, 8, 4, 6, 10);
  rotated.rotation = NabtsCharRotation::k90;

  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .deposit_character(rotated, {}, white(), nullptr);

  int min_column = kTestGrid.width;
  int max_column = -1;
  int min_row = kTestGrid.height;
  int max_row = -1;
  for (int row = 0; row < kTestGrid.height; ++row) {
    for (int column = 0; column < kTestGrid.width; ++column) {
      if (!surface.at(column, row).painted) {
        continue;
      }
      min_column = std::min(min_column, column);
      max_column = std::max(max_column, column);
      min_row = std::min(min_row, row);
      max_row = std::max(max_row, row);
    }
  }
  ASSERT_GE(max_column, 0) << "the rotated character drew nothing";
  // Wider than tall, which the upright field is not.
  EXPECT_GT(max_column - min_column, max_row - min_row);
}

// §5.4's mosaics are 2 by 3 blocks of the character field, bit 0 top-left
// through bit 5 bottom-right.
TEST(NaplpsRasterTest, AMosaicFillsTheSubElementsItsCodeNames) {
  // Figure 62 puts sixel bits 0 to 4 straight through from the code's low
  // bits, so code 2/1 lights bit 0 alone: the top-left block of the six.
  NabtsPrimitive mosaic =
      character_at(0x21, NabtsPrimitive::Repertoire::kMosaic, 4, 4, 4, 6);

  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .deposit_character(mosaic, {}, white(), nullptr);

  // The field is 4 by 6 cells, so each block is 2 by 2 and the top-left one
  // sits at the field's top-left corner.
  EXPECT_EQ(row_pattern(surface, 8, 3, 9), ".##....");
  EXPECT_EQ(row_pattern(surface, 9, 3, 9), ".##....");
  EXPECT_EQ(row_pattern(surface, 7, 3, 9), ".......");
}

// §5.3.2.2.6 lists separated mosaics among the things the logical pel sizes: a
// separated element is shrunk by the pel, which is what opens the gap that
// makes the blocks read as separate.
TEST(NaplpsRasterTest, SeparatedMosaicsAreInsetByThePel) {
  NabtsPrimitive mosaic =
      character_at(0x21, NabtsPrimitive::Repertoire::kMosaic, 4, 4, 4, 6);
  mosaic.logical_pel = pel(1.0, 1.0);
  // §6.2.7.15: underline mode is what puts mosaics into separated mode.
  mosaic.underlined = true;

  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .deposit_character(mosaic, {}, white(), nullptr);

  // One cell shrunk off the right and the top of the 2 by 2 block.
  EXPECT_EQ(row_pattern(surface, 8, 3, 9), ".#.....");
  EXPECT_EQ(row_pattern(surface, 9, 3, 9), ".......");
}

// §5.6: a downloadable character is displayed from the buffer its definition
// filled, and one that was never defined is displayed as SPACE.
TEST(NaplpsRasterTest, ADrcsCharacterDepositsItsDefinedBuffer) {
  NabtsDrcsCharacter glyph;
  glyph.code = 0x21;
  glyph.width = 2;
  glyph.height = 2;
  // Row 0 is the buffer's bottom: light the bottom-left and top-right.
  glyph.elements = {true, false, false, true};

  NabtsPrimitive primitive =
      character_at(0x21, NabtsPrimitive::Repertoire::kDrcs, 4, 4, 2, 2);

  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .deposit_character(primitive, {glyph}, white(), nullptr);

  EXPECT_TRUE(surface.at(4, 4).painted) << "bottom-left of the buffer";
  EXPECT_TRUE(surface.at(5, 5).painted) << "top-right of the buffer";
  EXPECT_FALSE(surface.at(5, 4).painted);
  EXPECT_FALSE(surface.at(4, 5).painted);

  NaplpsCellSurface undefined(kTestGrid);
  NaplpsRasteriser(undefined, NaplpsGridMapping{kTestGrid})
      .deposit_character(primitive, {}, white(), nullptr);
  EXPECT_EQ(undefined.painted_count(), 0u)
      << "a character never defined is displayed as SPACE";
}

// ---------------------------------------------------------------------------
// Run merging
// ---------------------------------------------------------------------------

// Cells of one colour along a row become one run, so a page of flat colour
// costs a run per row rather than one per pixel.
// A filled figure is its enclosed area *and* the region its outline traces
// (§5.3.3.5.1), and the two are worked out differently: the area by asking
// whether a cell's centre is inside, the outline by sweeping the pel along the
// edge. On a diagonal edge those two rules can each miss the same cell — the
// centre falls outside, and the pel, anchored outside the edge by its sign
// (§5.3.2.2.6), never reaches it — which opens a one-cell gap running down the
// diagonal between a figure and its own outline.
//
// The letterforms of the reference ExtraVision service are drawn exactly this
// way: an octagon with a 45-degree corner, filled, with a one-cell pel.
TEST(NaplpsRasterTest, AFilledFigureIsSealedAlongADiagonalEdge) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  // Corners cut at 45 degrees, the way a letterform's are.
  const std::vector<NabtsPoint> letterform = {
      at(8.0, 4.0),   at(20.0, 4.0), at(24.0, 8.0), at(24.0, 16.0),
      at(20.0, 20.0), at(8.0, 20.0), at(4.0, 16.0), at(4.0, 8.0)};
  raster.fill_path(letterform, pel(1.0, 1.0), NabtsTexturePattern::kSolid,
                   NabtsSize{}, nullptr, white());

  // No unpainted cell anywhere inside the figure: a gap shows up as an
  // unpainted cell with painted cells on both sides of it along a row.
  for (int row = 0; row < kTestGrid.height; ++row) {
    for (int column = 1; column + 1 < kTestGrid.width; ++column) {
      if (surface.at(column, row).painted) {
        continue;
      }
      bool painted_left = false;
      for (int x = column - 1; x >= 0 && !painted_left; --x) {
        painted_left = surface.at(x, row).painted;
      }
      bool painted_right = false;
      for (int x = column + 1; x < kTestGrid.width && !painted_right; ++x) {
        painted_right = surface.at(x, row).painted;
      }
      EXPECT_FALSE(painted_left && painted_right)
          << "cell " << column << "," << row << " is a hole in the fill:\n  "
          << row_pattern(surface, row, 0, kTestGrid.width - 1);
    }
  }
}

// §5.3.3.5.1's enclosed area is the odd-even one, so a frame traced as a single
// path — round the outside, in through a slit, and round the inside — is a
// frame and not a slab. Its own outline cannot tell the two apart, the slit
// being closed to anything working inward from the edge, so what the path
// encloses has to have a say.
TEST(NaplpsRasterTest, AFilledFrameKeepsItsHole) {
  const std::vector<NabtsPoint> frame = {
      at(2.0, 2.0),  at(30.0, 2.0), at(30.0, 22.0), at(2.0, 22.0),
      at(2.0, 9.0),  at(8.0, 9.0),  at(8.0, 16.0),  at(24.0, 16.0),
      at(24.0, 8.0), at(8.0, 8.0),  at(8.0, 9.0),   at(2.0, 9.0)};

  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .fill_path(frame, pel(1.0, 1.0), NabtsTexturePattern::kSolid, NabtsSize{},
                 nullptr, white());

  EXPECT_TRUE(surface.at(4, 12).painted) << "the frame's left bar";
  EXPECT_TRUE(surface.at(28, 12).painted) << "the frame's right bar";
  EXPECT_FALSE(surface.at(16, 12).painted) << "the hole in the middle";
}

// A figure drawn right up to the edge of the display area still has an outside,
// even though the receiver shows none of it. Deciding otherwise — taking the
// edge of the screen for the edge of the figure — let a page's leafy border
// claim the whole screen and paint out everything beneath it.
TEST(NaplpsRasterTest, AFigureDrawnToTheScreenEdgeDoesNotClaimTheScreen) {
  const double across = kTestGrid.width;
  const double up = kTestGrid.height;
  // A band round three sides of the screen, open at the top.
  const std::vector<NabtsPoint> border = {
      at(0.0, 0.0),          at(across, 0.0),
      at(across, up),        at(across - 4.0, up),
      at(across - 4.0, 4.0), at(4.0, 4.0),
      at(4.0, up),           at(0.0, up)};

  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .fill_path(border, pel(1.0, 1.0), NabtsTexturePattern::kSolid,
                 NabtsSize{}, nullptr, white());

  EXPECT_TRUE(surface.at(1, 12).painted)
      << "the band up the left of the screen";
  EXPECT_TRUE(surface.at(16, 1).painted) << "the band along the bottom";
  EXPECT_FALSE(surface.at(16, 15).painted) << "the opening in the middle";
}

// The outline of a filled figure and the same path stroked are the same thing:
// §5.3.3.5.1 puts "the region of the outline traced by the logical pel" inside
// the fill, and §5.3.2.2.6 defines that region once, for every drawing
// operation alike. A service draws a map by filling each region and then
// outlining it, so where the two disagree the fill shows past the outline meant
// to cover it — which is what a coloured fringe outside a black coastline is.
TEST(NaplpsRasterTest, AFilledFigureNeverShowsPastItsOwnOutline) {
  const std::vector<NabtsPoint> coastline = {
      at(8.0, 6.0),   at(20.0, 6.0),  at(30.0, 12.0), at(31.0, 14.0),
      at(31.0, 20.0), at(22.0, 23.0), at(12.0, 22.0), at(5.0, 15.0)};

  NaplpsCellSurface filled(kTestGrid);
  NaplpsRasteriser fill_raster(filled, NaplpsGridMapping{kTestGrid});
  fill_raster.fill_path(coastline, pel(1.0, 1.0), NabtsTexturePattern::kSolid,
                        NabtsSize{}, nullptr, white());

  NaplpsCellSurface stroked(kTestGrid);
  NaplpsRasteriser stroke_raster(stroked, NaplpsGridMapping{kTestGrid});
  stroke_raster.stroke_path(coastline, pel(1.0, 1.0), NabtsLineTexture::kSolid,
                            white(), /*closed=*/true);

  // A filled cell the outline does not cover must be one the outline encloses:
  // it shares no edge with the ground, so it cannot be seen from outside the
  // figure. Edges rather than corners, because the outline is a locus stepped
  // one cell at a time and a diagonal step of it meets the ground corner to
  // corner at every tread of the staircase — which is what a line drawn on a
  // grid looks like, not a gap in it.
  constexpr int kSides[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
  for (int row = 0; row < kTestGrid.height; ++row) {
    for (int column = 0; column < kTestGrid.width; ++column) {
      if (!filled.at(column, row).painted || stroked.at(column, row).painted) {
        continue;
      }
      for (const auto& side : kSides) {
        const int x = column + side[0];
        const int y = row + side[1];
        if (!filled.contains(x, y)) {
          continue;
        }
        EXPECT_TRUE(filled.at(x, y).painted || stroked.at(x, y).painted)
            << "the fill shows past its outline at " << column << "," << row
            << ", which is open to " << x << "," << y;
      }
    }
  }
}

TEST(NaplpsRasterTest, IdenticalCellsAlongARowMergeIntoOneRun) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .stroke_path({at(2.0, 4.0), at(12.0, 4.0)}, NabtsSize{0.0, 0.0},
                   NabtsLineTexture::kSolid, white());

  const std::vector<NaplpsCellRun> runs = naplps_merge_runs(surface);
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_EQ(runs[0].row, 4);
  EXPECT_EQ(runs[0].column, 2);
  EXPECT_EQ(runs[0].columns, 11);
}

// An unpainted cell breaks a run and produces none of its own: a page does not
// cover the screen, and emitting its ground would paint over whatever a
// renderer puts behind it.
TEST(NaplpsRasterTest, UnpaintedCellsBreakRunsAndEmitNothing) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  raster.stamp_pel(at(2.5, 4.5), NabtsSize{0.0, 0.0}, white());
  raster.stamp_pel(at(6.5, 4.5), NabtsSize{0.0, 0.0}, white());

  const std::vector<NaplpsCellRun> runs = naplps_merge_runs(surface);
  ASSERT_EQ(runs.size(), 2u);
  EXPECT_EQ(runs[0].column, 2);
  EXPECT_EQ(runs[0].columns, 1);
  EXPECT_EQ(runs[1].column, 6);
}

// A blink process is defined over a colour map entry (§5.3.2.7.2), so two cells
// of the same colour drawn from different entries can blink apart and cannot
// share a run.
TEST(NaplpsRasterTest, CellsThatBlinkApartDoNotShareARun) {
  NaplpsCellSurface surface(kTestGrid);

  NaplpsInk steady = white();
  steady.colour_map_address = 3;
  NaplpsInk blinking = white();
  blinking.colour_map_address = 4;
  blinking.blinking = true;
  blinking.blink_to = kNabtsNominalBlack;

  surface.deposit(4, 6, steady);
  surface.deposit(5, 6, blinking);

  const std::vector<NaplpsCellRun> runs = naplps_merge_runs(surface);
  ASSERT_EQ(runs.size(), 2u);
  EXPECT_FALSE(runs[0].cell.blinking);
  EXPECT_TRUE(runs[1].cell.blinking);
}

}  // namespace
}  // namespace orc
