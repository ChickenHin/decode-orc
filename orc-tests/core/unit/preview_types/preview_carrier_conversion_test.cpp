/*
 * File:        preview_carrier_conversion_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for Phase 2 colour carrier conversion.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/preview/orc_preview_carriers.h>
#include <orc/support/colour_preview_conversion.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace orc_unit_test {

TEST(ColourFrameCarrierTest, InvalidWhenPlaneSizesDoNot_MatchDimensions) {
  orc::ColourFrameCarrier carrier{};
  carrier.width = 2;
  carrier.height = 2;
  carrier.y_plane = {1.0, 2.0, 3.0};
  carrier.u_plane = {0.0, 0.0, 0.0};
  carrier.v_plane = {0.0, 0.0, 0.0};

  EXPECT_FALSE(carrier.is_valid());
}

TEST(ColourFrameCarrierTest, ValidWhenAllPlanes_MatchDimensions) {
  orc::ColourFrameCarrier carrier{};
  carrier.width = 2;
  carrier.height = 2;
  carrier.cvbs_blanking = 0.0;
  carrier.cvbs_white = 1000.0;
  carrier.y_plane = {200.0, 300.0, 400.0, 500.0};
  carrier.u_plane = {0.0, 0.0, 0.0, 0.0};
  carrier.v_plane = {0.0, 0.0, 0.0, 0.0};

  EXPECT_TRUE(carrier.is_valid());
}

TEST(ColourCarrierConversionTest, Produces_ValidRgbImage) {
  // Y values in the carrier's own domain [cvbs_blanking=0, cvbs_white=65535].
  orc::ColourFrameCarrier carrier{};
  carrier.width = 2;
  carrier.height = 1;
  carrier.cvbs_blanking = 0.0;
  carrier.cvbs_white = 65535.0;
  carrier.colorimetry = orc::ColorimetricMetadata::default_ntsc();
  carrier.y_plane = {20000.0, 46000.0};  // dim and bright, both in TBC domain
  carrier.u_plane = {0.0, 0.0};
  carrier.v_plane = {0.0, 0.0};

  const auto image = orc::render_preview_from_colour_carrier(carrier);
  ASSERT_TRUE(image.is_valid());
  ASSERT_EQ(image.width, 2u);
  ASSERT_EQ(image.height, 1u);

  // Neutral chroma should yield grayscale output.
  EXPECT_EQ(image.rgb_data[0], image.rgb_data[1]);
  EXPECT_EQ(image.rgb_data[1], image.rgb_data[2]);
  EXPECT_EQ(image.rgb_data[3], image.rgb_data[4]);
  EXPECT_EQ(image.rgb_data[4], image.rgb_data[5]);

  // Higher luma should produce a brighter output pixel.
  EXPECT_LT(image.rgb_data[0], image.rgb_data[3]);
}

TEST(ColourCarrierConversionTest, MatrixSelection_AffectsRgbResult) {
  // Y/U/V in carrier domain [cvbs_blanking=0, cvbs_white=65535]; chroma
  // large enough for the small matrix coefficient difference to survive
  // 8-bit quantization.
  orc::ColourFrameCarrier ntsc{};
  ntsc.width = 1;
  ntsc.height = 1;
  ntsc.cvbs_blanking = 0.0;
  ntsc.cvbs_white = 65535.0;
  ntsc.colorimetry = orc::ColorimetricMetadata::default_ntsc();
  ntsc.y_plane = {30000.0};
  ntsc.u_plane = {12000.0};
  ntsc.v_plane = {5000.0};

  orc::ColourFrameCarrier fcc = ntsc;
  fcc.colorimetry.matrix_coefficients =
      orc::ColorimetricMatrixCoefficients::NTSC1953_FCC;

  const auto ntsc_image = orc::render_preview_from_colour_carrier(ntsc);
  const auto fcc_image = orc::render_preview_from_colour_carrier(fcc);

  ASSERT_TRUE(ntsc_image.is_valid());
  ASSERT_TRUE(fcc_image.is_valid());

  const bool differs = ntsc_image.rgb_data[0] != fcc_image.rgb_data[0] ||
                       ntsc_image.rgb_data[1] != fcc_image.rgb_data[1] ||
                       ntsc_image.rgb_data[2] != fcc_image.rgb_data[2];

  EXPECT_TRUE(differs);
}

// =============================================================================
// ColourFrameCarrier — additional validity edge cases
// =============================================================================

TEST(ColourFrameCarrierTest, EqualBlackAndWhiteLevels_IsNotValid) {
  // If the signal range is zero the conversion denominator would be zero —
  // the carrier is considered invalid.
  orc::ColourFrameCarrier carrier{};
  carrier.width = 1;
  carrier.height = 1;
  carrier.cvbs_blanking = 500.0;
  carrier.cvbs_white = 500.0;  // equal to black
  carrier.y_plane = {500.0};
  carrier.u_plane = {0.0};
  carrier.v_plane = {0.0};

  EXPECT_FALSE(carrier.is_valid());
}

TEST(ColourFrameCarrierTest, InvertedBlackAndWhiteLevels_IsNotValid) {
  orc::ColourFrameCarrier carrier{};
  carrier.width = 1;
  carrier.height = 1;
  carrier.cvbs_blanking = 1000.0;
  carrier.cvbs_white = 0.0;  // white < black
  carrier.y_plane = {500.0};
  carrier.u_plane = {0.0};
  carrier.v_plane = {0.0};

  EXPECT_FALSE(carrier.is_valid());
}

// =============================================================================
// render_preview_from_colour_carrier — transfer characteristic paths
// =============================================================================

TEST(ColourCarrierConversionTest, InvalidCarrier_ReturnsInvalidImage) {
  // render_preview_from_colour_carrier() should guard against an invalid
  // carrier and return an empty/invalid PreviewImage rather than crashing.
  orc::ColourFrameCarrier carrier{};
  carrier.width = 0;  // zero dimension → invalid
  carrier.height = 0;

  const auto image = orc::render_preview_from_colour_carrier(carrier);
  EXPECT_FALSE(image.is_valid());
}

TEST(ColourCarrierConversionTest, Gamma28Transfer_ProducesValidRgbImage) {
  // PAL recording using BT.601-625 matrix with 2.8 gamma transfer.
  // Y values in carrier domain [cvbs_blanking=0, cvbs_white=65535].
  orc::ColourFrameCarrier carrier{};
  carrier.width = 2;
  carrier.height = 1;
  carrier.cvbs_blanking = 0.0;
  carrier.cvbs_white = 65535.0;
  carrier.colorimetry = orc::ColorimetricMetadata::default_pal();
  carrier.y_plane = {20000.0, 46000.0};  // dim and bright, both in TBC domain
  carrier.u_plane = {0.0, 0.0};
  carrier.v_plane = {0.0, 0.0};

  const auto image = orc::render_preview_from_colour_carrier(carrier);
  ASSERT_TRUE(image.is_valid());
  ASSERT_EQ(image.width, 2u);
  ASSERT_EQ(image.height, 1u);

  // Neutral chroma (U=V=0) → grayscale regardless of transfer curve.
  EXPECT_EQ(image.rgb_data[0], image.rgb_data[1]);
  EXPECT_EQ(image.rgb_data[1], image.rgb_data[2]);

  // Higher luma should still produce brighter output.
  EXPECT_LT(image.rgb_data[0], image.rgb_data[3]);
}

TEST(ColourCarrierConversionTest,
     Bt709Transfer_ProducesDistinctOutputFromGamma22) {
  // BT.709 OETF is piecewise-linear near black, unlike pure power-law gamma,
  // so the resulting display values should differ for the same input.
  // Y in carrier domain [cvbs_blanking=0, cvbs_white=65535]; neutral chroma.
  orc::ColourFrameCarrier gamma22{};
  gamma22.width = 1;
  gamma22.height = 1;
  gamma22.cvbs_blanking = 0.0;
  gamma22.cvbs_white = 65535.0;
  gamma22.colorimetry = orc::ColorimetricMetadata::default_ntsc();
  gamma22.colorimetry.transfer_characteristics =
      orc::ColorimetricTransferCharacteristics::Gamma22;
  gamma22.y_plane = {25000.0};
  gamma22.u_plane = {0.0};
  gamma22.v_plane = {0.0};

  orc::ColourFrameCarrier bt709 = gamma22;
  bt709.colorimetry.transfer_characteristics =
      orc::ColorimetricTransferCharacteristics::BT709;

  const auto gamma22_image = orc::render_preview_from_colour_carrier(gamma22);
  const auto bt709_image = orc::render_preview_from_colour_carrier(bt709);

  ASSERT_TRUE(gamma22_image.is_valid());
  ASSERT_TRUE(bt709_image.is_valid());

  // Transfer curves differ in their linearisation behaviour; output should
  // differ.
  const bool pixel_differs =
      gamma22_image.rgb_data[0] != bt709_image.rgb_data[0];

  EXPECT_TRUE(pixel_differs);
}

TEST(ColourCarrierConversionTest, Bt1886Transfer_ProducesValidImage) {
  // BT.1886 (effective 2.4 gamma) should compile and produce valid RGB.
  orc::ColourFrameCarrier carrier{};
  carrier.width = 1;
  carrier.height = 1;
  carrier.cvbs_blanking = 0.0;
  carrier.cvbs_white = 1000.0;
  carrier.colorimetry = orc::ColorimetricMetadata::default_ntsc();
  carrier.colorimetry.transfer_characteristics =
      orc::ColorimetricTransferCharacteristics::BT1886;
  carrier.y_plane = {500.0};
  carrier.u_plane = {0.0};
  carrier.v_plane = {0.0};

  const auto image = orc::render_preview_from_colour_carrier(carrier);
  ASSERT_TRUE(image.is_valid());
  ASSERT_EQ(image.width, 1u);
  ASSERT_EQ(image.height, 1u);
}

// =============================================================================
// Transfer-function lookup tables
//
// The conversion evaluates the transfer decode and the sRGB encode through an
// interpolated table rather than std::pow per component. These tests pin the
// table's output against the transfer formulae computed directly, so a change
// to the table resolution or the interpolation cannot silently shift the
// rendered image.
// =============================================================================

namespace {

// The reference conversion, written straight from the specifications rather
// than from the implementation: ITU-R BT.470/BT.709 opto-electronic transfer
// decode to linear light, then the IEC 61966-2-1 (sRGB) encode.
double referenceDecodeToLinear(
    double value, orc::ColorimetricTransferCharacteristics transfer) {
  value = std::clamp(value, 0.0, 1.0);

  switch (transfer) {
    case orc::ColorimetricTransferCharacteristics::Gamma28:
      return std::pow(value, 2.8);
    case orc::ColorimetricTransferCharacteristics::BT709:
      // ITU-R BT.709-6 Section 1.2: linear below 0.081, power law above.
      if (value < 0.081) {
        return value / 4.5;
      }
      return std::pow((value + 0.099) / 1.099, 1.0 / 0.45);
    case orc::ColorimetricTransferCharacteristics::BT1886:
    case orc::ColorimetricTransferCharacteristics::BT1886App1:
      return std::pow(value, 2.4);
    case orc::ColorimetricTransferCharacteristics::Gamma22:
    case orc::ColorimetricTransferCharacteristics::Unspecified:
    default:
      return std::pow(value, 2.2);
  }
}

// IEC 61966-2-1: sRGB opto-electronic transfer function.
double referenceEncodeToSrgb(double linear) {
  linear = std::clamp(linear, 0.0, 1.0);
  if (linear <= 0.0031308) {
    return 12.92 * linear;
  }
  return 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

uint8_t referenceCode(double non_linear,
                      orc::ColorimetricTransferCharacteristics transfer) {
  const double srgb =
      referenceEncodeToSrgb(referenceDecodeToLinear(non_linear, transfer));
  return static_cast<uint8_t>(std::clamp(srgb, 0.0, 1.0) * 255.0 + 0.5);
}

// A single-row carrier whose luma sweeps [0, 1] of the black→white range with
// neutral chroma, so every output byte is the transfer function of a known
// non-linear input.
orc::ColourFrameCarrier makeLumaRamp(
    uint32_t samples, orc::ColorimetricTransferCharacteristics transfer) {
  constexpr double kWhite = 65535.0;

  orc::ColourFrameCarrier carrier{};
  carrier.width = samples;
  carrier.height = 1;
  carrier.cvbs_blanking = 0.0;
  carrier.cvbs_black = 0.0;
  carrier.cvbs_white = kWhite;
  carrier.colorimetry = orc::ColorimetricMetadata::default_ntsc();
  carrier.colorimetry.transfer_characteristics = transfer;

  carrier.y_plane.resize(samples);
  carrier.u_plane.assign(samples, 0.0);
  carrier.v_plane.assign(samples, 0.0);
  for (uint32_t i = 0; i < samples; ++i) {
    carrier.y_plane[i] =
        kWhite * static_cast<double>(i) / static_cast<double>(samples - 1);
  }
  return carrier;
}

}  // namespace

TEST(ColourCarrierConversionTest, TransferTable_MatchesDirectEvaluation) {
  // Every supported characteristic, swept densely enough to cross every table
  // interval many times over.
  const orc::ColorimetricTransferCharacteristics transfers[] = {
      orc::ColorimetricTransferCharacteristics::Unspecified,
      orc::ColorimetricTransferCharacteristics::Gamma22,
      orc::ColorimetricTransferCharacteristics::Gamma28,
      orc::ColorimetricTransferCharacteristics::BT709,
      orc::ColorimetricTransferCharacteristics::BT1886,
      orc::ColorimetricTransferCharacteristics::BT1886App1,
  };
  constexpr uint32_t kSamples = 20000;

  for (const auto transfer : transfers) {
    const auto carrier = makeLumaRamp(kSamples, transfer);
    const auto image = orc::render_preview_from_colour_carrier(carrier);
    ASSERT_TRUE(image.is_valid());

    size_t off_by_one = 0;
    for (uint32_t i = 0; i < kSamples; ++i) {
      const double non_linear =
          static_cast<double>(i) / static_cast<double>(kSamples - 1);
      const int expected = referenceCode(non_linear, transfer);
      const int actual = image.rgb_data[static_cast<size_t>(i) * 3];

      // Never more than one 8-bit code away from the directly evaluated
      // transfer function, for any input.
      ASSERT_LE(std::abs(actual - expected), 1)
          << "transfer=" << static_cast<int>(transfer) << " sample=" << i
          << " expected=" << expected << " actual=" << actual;
      if (actual != expected) {
        ++off_by_one;
      }
    }

    // Only inputs landing within the table's reconstruction error of a
    // rounding boundary may differ at all; that is a small fraction of a
    // percent, not a systematic shift.
    EXPECT_LT(off_by_one, kSamples / 100)
        << "transfer=" << static_cast<int>(transfer) << " differed on "
        << off_by_one << " of " << kSamples << " samples";
  }
}

TEST(ColourCarrierConversionTest, TransferTable_IsExactAtTheRangeEndpoints) {
  // Black and white must not drift: the top endpoint in particular is the one
  // input that falls outside every interpolation interval.
  const auto carrier =
      makeLumaRamp(2, orc::ColorimetricTransferCharacteristics::Gamma22);
  const auto image = orc::render_preview_from_colour_carrier(carrier);
  ASSERT_TRUE(image.is_valid());

  EXPECT_EQ(image.rgb_data[0], 0);
  EXPECT_EQ(image.rgb_data[1], 0);
  EXPECT_EQ(image.rgb_data[2], 0);
  EXPECT_EQ(image.rgb_data[3], 255);
  EXPECT_EQ(image.rgb_data[4], 255);
  EXPECT_EQ(image.rgb_data[5], 255);
}

TEST(ColourCarrierConversionTest, TransferTable_StaysMonotonicAcrossTheRamp) {
  // A table lookup must not introduce a local dip; banding in a preview ramp
  // is exactly what a badly interpolated table looks like.
  const auto carrier =
      makeLumaRamp(4096, orc::ColorimetricTransferCharacteristics::BT709);
  const auto image = orc::render_preview_from_colour_carrier(carrier);
  ASSERT_TRUE(image.is_valid());

  for (uint32_t i = 1; i < carrier.width; ++i) {
    EXPECT_LE(image.rgb_data[static_cast<size_t>(i - 1) * 3],
              image.rgb_data[static_cast<size_t>(i) * 3])
        << "output fell at sample " << i;
  }
}

namespace {

// Build a width×height image where every pixel of row N has value N.
orc::PreviewImage makeRowLabelledImage(uint32_t width, uint32_t height) {
  orc::PreviewImage image{};
  image.width = width;
  image.height = height;
  image.rgb_data.resize(static_cast<size_t>(width) * height * 3);
  for (uint32_t row = 0; row < height; ++row) {
    for (uint32_t i = 0; i < width * 3; ++i) {
      image.rgb_data[static_cast<size_t>(row) * width * 3 + i] =
          static_cast<uint8_t>(row);
    }
  }
  return image;
}

uint8_t rowValue(const orc::PreviewImage& image, uint32_t row) {
  return image.rgb_data[static_cast<size_t>(row) * image.width * 3];
}

}  // namespace

TEST(SequentialFieldReorderTest, EvenHeight_StacksField1AboveField2) {
  auto image = makeRowLabelledImage(2, 4);

  orc::reorder_preview_image_to_sequential_fields(image);

  ASSERT_TRUE(image.is_valid());
  // Weaved rows 0,2 (field 1) then 1,3 (field 2).
  EXPECT_EQ(rowValue(image, 0), 0);
  EXPECT_EQ(rowValue(image, 1), 2);
  EXPECT_EQ(rowValue(image, 2), 1);
  EXPECT_EQ(rowValue(image, 3), 3);
}

TEST(SequentialFieldReorderTest, OddHeight_GivesField1TheExtraLine) {
  auto image = makeRowLabelledImage(3, 5);

  orc::reorder_preview_image_to_sequential_fields(image);

  ASSERT_TRUE(image.is_valid());
  // Field 1 = weaved rows 0,2,4; field 2 = rows 1,3.
  EXPECT_EQ(rowValue(image, 0), 0);
  EXPECT_EQ(rowValue(image, 1), 2);
  EXPECT_EQ(rowValue(image, 2), 4);
  EXPECT_EQ(rowValue(image, 3), 1);
  EXPECT_EQ(rowValue(image, 4), 3);
}

TEST(SequentialFieldReorderTest, DropoutRegions_AreRemappedToNewRows) {
  auto image = makeRowLabelledImage(2, 4);
  orc::DropoutRegion even_row{};
  even_row.line = 2;
  orc::DropoutRegion odd_row{};
  odd_row.line = 3;
  image.dropout_regions = {even_row, odd_row};

  orc::reorder_preview_image_to_sequential_fields(image);

  // field1_rows = 2: weaved row 2 → 1; weaved row 3 → 2 + 1 = 3.
  EXPECT_EQ(image.dropout_regions[0].line, 1u);
  EXPECT_EQ(image.dropout_regions[1].line, 3u);
}

TEST(SequentialFieldReorderTest, InvalidOrSingleRowImage_IsUnchanged) {
  orc::PreviewImage invalid{};
  orc::reorder_preview_image_to_sequential_fields(invalid);
  EXPECT_TRUE(invalid.rgb_data.empty());

  auto single = makeRowLabelledImage(4, 1);
  const auto before = single.rgb_data;
  orc::reorder_preview_image_to_sequential_fields(single);
  EXPECT_EQ(single.rgb_data, before);
}

}  // namespace orc_unit_test
