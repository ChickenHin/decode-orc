/*
 * File:        nabts_caption_publication_test.cpp
 * Module:      orc-core-tests
 * Purpose:     The caption service reaches the file and the host by the same
 *              reading (CEA-516 §7.3.10)
 *
 * The stage publishes the caption service twice: as a SubRip document beside
 * the packet stream, and as the caption track of the catalogue the host
 * browses. The host used to derive its own cues by calling nabts_caption_cues()
 * across the plugin boundary; it now reads the published track and nothing
 * else, so nothing but these tests holds the two renderings together.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "nabts_catalogue_view.h"
#include "nabts_sink_deps.h"
#include "vbi-services/nabts_page.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {
namespace {

using Repertoire = NabtsPrimitive::Repertoire;

NabtsCataloguedRecord caption_record(uint8_t version, uint64_t first,
                                     uint64_t last, const std::string& text) {
  NabtsCataloguedRecord record;
  record.channel = 0xA00;
  record.address_text = "000";
  record.record_type = 1;  // §5.2.2.3 non-cyclic presentation
  record.caption = true;
  record.version = version;
  record.first_seen_frame = first;
  record.last_seen_frame = last;
  record.times_seen = 1;
  record.times_intact = 1;
  record.data = {0x1B};
  double x = 0.0;
  for (const char letter : text) {
    NabtsPrimitive primitive;
    primitive.kind = NabtsPrimitiveKind::kCharacter;
    primitive.character = static_cast<uint8_t>(letter);
    primitive.repertoire = Repertoire::kPrimary;
    primitive.origin = NabtsPoint{x, 0.2};
    primitive.size = NabtsSize{1.0 / 40.0, 2.0 / 40.0};
    record.page.primitives.push_back(primitive);
    x += 1.0 / 40.0;
  }
  return record;
}

/// The catalogue's caption track, or nullptr when it published none.
const CataloguePayload* caption_track(const CatalogueDataset& catalogue) {
  for (size_t i = 0; i < catalogue.items.size(); ++i) {
    if (catalogue.items[i].values.size() > 1 &&
        catalogue.items[i].values[1] == "Captions") {
      return &catalogue.payloads[i];
    }
  }
  return nullptr;
}

/// One SubRip block as the document carries it: the timing line and the text.
struct SrtBlock {
  std::string timing;
  std::string text;
};

/// Split a SubRip document into its blocks. Deliberately a separate reading
/// from the one that wrote it, so a malformed document fails here rather than
/// being parsed back into whatever was meant.
std::vector<SrtBlock> parse_srt(const std::string& document) {
  std::vector<SrtBlock> blocks;
  std::vector<std::string> lines;
  std::string line;
  for (const char c : document) {
    if (c == '\n') {
      lines.push_back(line);
      line.clear();
    } else {
      line += c;
    }
  }
  if (!line.empty()) lines.push_back(line);

  size_t i = 0;
  while (i < lines.size()) {
    if (lines[i].empty()) {
      ++i;
      continue;
    }
    // Sequence number, timing, then the text up to the blank line.
    EXPECT_EQ(lines[i], std::to_string(blocks.size() + 1))
        << "cue " << blocks.size() + 1 << " is not numbered in sequence";
    SrtBlock block;
    if (i + 1 < lines.size()) block.timing = lines[i + 1];
    i += 2;
    while (i < lines.size() && !lines[i].empty()) {
      if (!block.text.empty()) block.text += '\n';
      block.text += lines[i];
      ++i;
    }
    blocks.push_back(std::move(block));
  }
  return blocks;
}

/// A SubRip timing line as the catalogue table writes the same extent: the
/// decimal separator of a millisecond is a comma in SubRip and a point
/// everywhere else, and the arrow is drawn with one dash less.
std::string as_table_time(std::string timing) {
  std::replace(timing.begin(), timing.end(), ',', '.');
  const size_t arrow = timing.find(" --> ");
  if (arrow != std::string::npos) {
    timing.replace(arrow, 5, " -> ");
  }
  return timing;
}

// ---------------------------------------------------------------------------

// The two renderings must describe the same service, cue for cue. A difference
// here is a caption a reader saw on screen and did not get in the file, or the
// reverse — which is the whole reason the reading is shared.
TEST(NabtsCaptionPublication, TheCatalogueTrackAndTheSubRipFileAgree) {
  NabtsAnalysisDataset dataset;
  dataset.records = {
      caption_record(1, 100, 104, "FIRST CAPTION"),
      caption_record(2, 250, 258, "SECOND CAPTION"),
      caption_record(3, 900, 912, "THIRD CAPTION"),
  };

  const std::vector<NabtsCaptionCue> cues = nabts_caption_cues(dataset.records);
  ASSERT_EQ(cues.size(), 3u);

  const std::vector<SrtBlock> blocks = parse_srt(nabts_caption_srt(cues));
  const CatalogueDataset catalogue = build_nabts_catalogue(dataset);
  const CataloguePayload* track = caption_track(catalogue);
  ASSERT_NE(track, nullptr) << "the catalogue published no caption track";
  ASSERT_EQ(track->kind, CataloguePayload::Kind::kTable);

  ASSERT_EQ(blocks.size(), cues.size());
  ASSERT_EQ(track->table.rows.size(), cues.size())
      << "the file and the screen disagree about how many captions there were";

  // The table's columns are frames, time and text, in that order.
  ASSERT_EQ(track->table.columns.size(), 3u);
  for (size_t i = 0; i < cues.size(); ++i) {
    const std::vector<std::string>& row = track->table.rows[i];
    ASSERT_EQ(row.size(), 3u);
    EXPECT_EQ(row[1], as_table_time(blocks[i].timing))
        << "cue " << i + 1 << " is on screen for a different span than the "
        << "file gives it";
    EXPECT_EQ(row[2], blocks[i].text)
        << "cue " << i + 1 << " reads differently in the file";
    // Both come from the same cue, so both must be that cue: the table numbers
    // frames from 1 where the dataset numbers them from 0.
    EXPECT_EQ(row[0], std::to_string(cues[i].start_frame + 1) + "-" +
                          std::to_string(cues[i].end_frame + 1));
  }
}

// A caption is often two lines. SubRip carries the break; the table shows the
// cue on one line so the list stays scannable, which is the one place the two
// are allowed to differ.
TEST(NabtsCaptionPublication, ATwoLineCaptionKeepsItsBreakInTheFileOnly) {
  NabtsAnalysisDataset dataset;
  NabtsCataloguedRecord record = caption_record(1, 100, 140, "TOP");
  // A second row of characters below the first is a second line of the caption.
  for (const char letter : std::string("BOTTOM")) {
    NabtsPrimitive primitive;
    primitive.kind = NabtsPrimitiveKind::kCharacter;
    primitive.character = static_cast<uint8_t>(letter);
    primitive.repertoire = Repertoire::kPrimary;
    primitive.origin = NabtsPoint{0.0, 0.6};
    primitive.size = NabtsSize{1.0 / 40.0, 2.0 / 40.0};
    record.page.primitives.push_back(primitive);
  }
  dataset.records = {record};

  const std::vector<NabtsCaptionCue> cues = nabts_caption_cues(dataset.records);
  ASSERT_EQ(cues.size(), 1u);
  ASSERT_NE(cues[0].text.find('\n'), std::string::npos)
      << "the fixture did not produce a two-line caption";

  const std::vector<SrtBlock> blocks = parse_srt(nabts_caption_srt(cues));
  ASSERT_EQ(blocks.size(), 1u);
  EXPECT_EQ(blocks[0].text, cues[0].text);

  const CatalogueDataset catalogue = build_nabts_catalogue(dataset);
  const CataloguePayload* track = caption_track(catalogue);
  ASSERT_NE(track, nullptr);
  ASSERT_EQ(track->table.rows.size(), 1u);
  std::string flattened = cues[0].text;
  std::replace(flattened.begin(), flattened.end(), '\n', ' ');
  EXPECT_EQ(track->table.rows[0][2], flattened);
}

// A recording that carried no captioning publishes neither, rather than an
// empty track on one side and nothing on the other.
TEST(NabtsCaptionPublication, NeitherIsPublishedWithoutACaptionService) {
  NabtsAnalysisDataset dataset;
  NabtsCataloguedRecord page = caption_record(1, 100, 104, "NOT A CAPTION");
  page.caption = false;
  dataset.records = {page};

  const std::vector<NabtsCaptionCue> cues = nabts_caption_cues(dataset.records);
  EXPECT_TRUE(cues.empty());
  EXPECT_TRUE(nabts_caption_srt(cues).empty());
  const CatalogueDataset catalogue = build_nabts_catalogue(dataset);
  EXPECT_EQ(caption_track(catalogue), nullptr);
}

}  // namespace
}  // namespace orc
