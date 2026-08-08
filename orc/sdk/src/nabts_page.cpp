/*
 * File:        nabts_page.cpp
 * Module:      decode-orc Plugin SDK (support)
 * Purpose:     The NAPLPS default colour map
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/support/nabts_page.h>

#include <cmath>

namespace orc {

namespace {

// Bits per gun, which Table D1 item 5(4) fixes at three — "sixteen simultaneous
// colours out of a set of 512 obtained by allocating three bits each to G R &
// B".
constexpr int kGunBits = 3;
constexpr int kGunMax = (1 << kGunBits) - 1;  // 7

// X3.110 §5.3.2.5.2 places the three primaries equidistant around a hue circle
// with blue at 0 degrees, red at 120 and green at 240.
constexpr double kBlueAngle = 0.0;
constexpr double kRedAngle = 120.0;
constexpr double kGreenAngle = 240.0;

/// Angular distance between |a| and |b| the short way round the circle.
double angular_distance(double a, double b) {
  double delta = std::fabs(a - b);
  if (delta > 180.0) {
    delta = 360.0 - delta;
  }
  return delta;
}

/**
 * @brief The hue at |angle| degrees, per the algorithm of X3.110 §5.3.2.5.2
 *
 * The closest primary to the angle is set full on, the furthest full off, and
 * the second closest gets the angular distance from the closest primary divided
 * by 60 degrees — which is the half-separation of two primaries, so the
 * fraction runs from 0 at a pure primary to 1 midway between two.
 *
 * The value is then "normalized by multiplying it by the maximum color value
 * which can be stored for that primary", i.e. by 7 with three bits, "and then
 * rounded to three places". Verified against T.101 Table II-3: this reproduces
 * all eight of its hue entries exactly.
 */
NabtsColour hue_at(double angle) {
  struct Primary {
    double angle;
    uint8_t* component;
  };

  NabtsColour colour;
  Primary primaries[3] = {
      {kGreenAngle, &colour.green},
      {kRedAngle, &colour.red},
      {kBlueAngle, &colour.blue},
  };

  // Sort by distance from the hue: closest, second, furthest. Three elements,
  // so a couple of swaps rather than a sort.
  double distances[3];
  for (int i = 0; i < 3; ++i) {
    distances[i] = angular_distance(angle, primaries[i].angle);
  }
  for (int i = 0; i < 2; ++i) {
    for (int j = i + 1; j < 3; ++j) {
      if (distances[j] < distances[i]) {
        std::swap(distances[i], distances[j]);
        std::swap(primaries[i], primaries[j]);
      }
    }
  }

  // P1 full on, P3 off, P2 by the fraction above.
  *primaries[0].component = static_cast<uint8_t>(kGunMax);
  *primaries[2].component = 0;
  const double fraction = distances[0] / 60.0;
  *primaries[1].component = static_cast<uint8_t>(
      std::lround(fraction * static_cast<double>(kGunMax)));
  return colour;
}

}  // namespace

void nabts_default_colour_map(NabtsColour (&map)[kNabtsColourMapEntries]) {
  // §5.3.2.5.2: "The first half of the default color map is used to store a
  // complete, uniformly spaced grey scale", G = R = B, black through white.
  constexpr size_t kGreys = kNabtsColourMapEntries / 2;
  for (size_t i = 0; i < kGreys; ++i) {
    const uint8_t level = static_cast<uint8_t>(i);
    map[i] = NabtsColour{level, level, level, false};
  }

  // "The second half ... a full range of hues equally spaced around the
  // perimeter of the hue circle", starting at 0 degrees — blue — and proceeding
  // counterclockwise, which is the direction of increasing angle given where
  // §5.3.2.5.2 places the primaries.
  constexpr size_t kHues = kNabtsColourMapEntries - kGreys;
  for (size_t i = 0; i < kHues; ++i) {
    const double angle =
        360.0 * static_cast<double>(i) / static_cast<double>(kHues);
    map[kGreys + i] = hue_at(angle);
  }
}

}  // namespace orc
