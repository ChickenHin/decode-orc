/*
 * File:        nabts_catalogue_view.cpp
 * Module:      nabts_sink stage plugin
 * Purpose:     The record catalogue as an SDK CatalogueDataset the host can
 *              browse
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_catalogue_view.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace orc {

namespace {

// Bits per gun in the transmitted colour (X3.110 §5.3.1, Table D1 item 5(4)),
// and the full-scale value that gives.
constexpr int kGunBits = 3;
constexpr int kGunMax = (1 << kGunBits) - 1;  // 7

// Unit-space slack for deciding whether two character origins are a step apart.
// Table D1 item 8 puts the nominal resolution at 256 x 200, so one pixel is
// about 1/256; this is three orders of magnitude below that and exists only to
// absorb the arithmetic of resolving a relative coordinate.
constexpr double kOriginEpsilon = 1e-6;

// The widest step that still counts as the next character field along the path.
// X3.110 §5.3.2.3.4 Table 8's inter-character spacings are 1, 1.25 and 1.5
// character fields, and proportional spacing is at least one; two fields is
// clear of all of them and well short of a jump to another line.
constexpr double kMaxStepFields = 2.0;

// SMPTE 170M: 525-line NTSC scans 59.94 fields per second, so a frame is
// 1001/30000 of a second. Caption cue extents are frame counts, and this is
// what turns one into a time a reader recognises.
constexpr double kNtscFramesPerSecond = 30000.0 / 1001.0;

/// A three-bit gun value as an 8-bit channel, full scale to full scale.
uint8_t to_channel(uint8_t gun) {
  return static_cast<uint8_t>((std::min<int>(gun, kGunMax) * 255) / kGunMax);
}

CatalogueColour to_colour(const NabtsColour& colour) {
  CatalogueColour out;
  out.red = to_channel(colour.red);
  out.green = to_channel(colour.green);
  out.blue = to_channel(colour.blue);
  out.transparent = colour.transparent;
  return out;
}

CataloguePoint to_point(const NabtsPoint& point) {
  return CataloguePoint{point.x, point.y};
}

CatalogueSize to_size(const NabtsSize& size) {
  return CatalogueSize{size.dx, size.dy};
}

/**
 * @brief One six-bit numeric colour specification as a colour (X3.110 Fig. 12)
 *
 * The six payload bits are two three-tuples in the order green, red, blue, so
 * each gun gets two bits taken one per tuple, most significant first. §5.3.2.5
 * zero-extends a value with fewer bits than the map holds, which for two bits
 * into three is a single shift.
 */
NabtsColour colour_from_numeric(uint8_t payload) {
  const auto gun = [payload](int offset) {
    const uint32_t high = (payload >> (3 + offset)) & 0x1u;
    const uint32_t low = (payload >> offset) & 0x1u;
    return static_cast<uint8_t>(((high << 1) | low) << (kGunBits - 2));
  };
  NabtsColour colour;
  colour.green = gun(2);
  colour.red = gun(1);
  colour.blue = gun(0);
  return colour;
}

/// One entry of an incremental colour run, resolved (§5.3.3.6.3): a value in
/// colour mode 0, a colour-map address in modes 1 and 2.
CatalogueColour resolve_incremental(uint8_t entry, NabtsColourMode mode,
                                    const NabtsPageSnapshot& snapshot) {
  if (mode == NabtsColourMode::kDirect) {
    return to_colour(colour_from_numeric(entry));
  }
  const size_t address = static_cast<size_t>(entry) % kNabtsColourMapEntries;
  return to_colour(snapshot.colour_map[address]);
}

CatalogueDrawKind to_draw_kind(NabtsPrimitiveKind kind) {
  switch (kind) {
    case NabtsPrimitiveKind::kPoint:
      return CatalogueDrawKind::kPoint;
    case NabtsPrimitiveKind::kLine:
      return CatalogueDrawKind::kLine;
    case NabtsPrimitiveKind::kArc:
      return CatalogueDrawKind::kArc;
    case NabtsPrimitiveKind::kRectangle:
      return CatalogueDrawKind::kRectangle;
    case NabtsPrimitiveKind::kPolygon:
      return CatalogueDrawKind::kPolygon;
    case NabtsPrimitiveKind::kIncrementalPoints:
      return CatalogueDrawKind::kColourRun;
    case NabtsPrimitiveKind::kCharacter:
      break;
  }
  // Split by repertoire at the call site; never reached with a character.
  return CatalogueDrawKind::kText;
}

int rotation_degrees(NabtsCharRotation rotation) {
  switch (rotation) {
    case NabtsCharRotation::kNone:
      return 0;
    case NabtsCharRotation::k90:
      return 90;
    case NabtsCharRotation::k180:
      return 180;
    case NabtsCharRotation::k270:
      return 270;
  }
  return 0;
}

/// Everything an operation carries that is not geometry: two operations that
/// agree on all of it can be drawn by one call.
void copy_attributes(const NabtsPrimitive& from, CatalogueDrawOp& to) {
  to.filled = from.filled;
  // X3.110 §5.3.2.4.3: a highlighted figure is filled as usual and outlined in
  // nominal black, or in the background colour where there is one.
  to.outlined = from.highlighted;
  to.pen_size = to_size(from.logical_pel);
  to.line_style = static_cast<CatalogueLineStyle>(from.line_texture);
  to.fill_pattern = static_cast<CatalogueFillPattern>(from.texture_pattern);
  to.fill_mask_size = to_size(from.texture_mask_size);
  to.colour = to_colour(from.colour);
  to.has_background =
      from.colour_mode == NabtsColourMode::kMappedWithBackground;
  to.background = to_colour(from.background);
  to.blinking = from.blinking;
  to.rotation_degrees = rotation_degrees(from.rotation);
  to.reverse_video = from.reverse_video;
  to.underlined = from.underlined;
}

/// Whether |candidate| could be drawn as part of a run already carrying the
/// attributes of |run|.
bool attributes_match(const CatalogueDrawOp& run,
                      const CatalogueDrawOp& candidate) {
  return run.colour == candidate.colour &&
         run.has_background == candidate.has_background &&
         run.background == candidate.background &&
         run.blinking == candidate.blinking &&
         run.rotation_degrees == candidate.rotation_degrees &&
         run.reverse_video == candidate.reverse_video &&
         run.underlined == candidate.underlined &&
         std::fabs(run.size.dx - candidate.size.dx) < kOriginEpsilon &&
         std::fabs(run.size.dy - candidate.size.dy) < kOriginEpsilon;
}

/**
 * @brief The state of a text run being built up
 *
 * NAPLPS emits one primitive per character, because that is what the record
 * transmits; a renderer would rather have the word. Coalescing needs both
 * halves of "the same run": the attributes have to match, and the character has
 * to sit where the cursor would have left it — one character field along a
 * single axis, in the same direction as the run's earlier steps.
 */
class TextRun {
 public:
  bool open() const { return open_; }

  /// Start a run at |primitive|, discarding whatever was there.
  void start(const NabtsPrimitive& primitive, std::string glyph) {
    op_ = CatalogueDrawOp{};
    op_.kind = CatalogueDrawKind::kText;
    copy_attributes(primitive, op_);
    op_.origin = to_point(primitive.origin);
    op_.size = to_size(primitive.size);
    op_.advance = nominal_advance(primitive);
    op_.points.push_back(op_.origin);
    op_.text = std::move(glyph);
    op_.character_count = 1;
    last_origin_ = op_.origin;
    have_step_ = false;
    open_ = true;
  }

  /// Extend the run with |primitive| if it belongs to it; false if it does not.
  bool extend(const NabtsPrimitive& primitive, const std::string& glyph) {
    if (!open_) {
      return false;
    }
    CatalogueDrawOp candidate;
    copy_attributes(primitive, candidate);
    candidate.size = to_size(primitive.size);
    if (!attributes_match(op_, candidate)) {
      return false;
    }

    const double dx = primitive.origin.x - last_origin_.x;
    const double dy = primitive.origin.y - last_origin_.y;
    if (!step_is_along_the_path(dx, dy)) {
      return false;
    }

    op_.text += glyph;
    ++op_.character_count;
    // The measured step beats the nominal one: it carries whatever
    // inter-character spacing (§5.3.2.3.4) the record asked for.
    op_.advance = CatalogueSize{dx, dy};
    last_origin_ = to_point(primitive.origin);
    step_x_ = dx;
    step_y_ = dy;
    have_step_ = true;
    return true;
  }

  /// Compose |mark| onto the character just added, which is what a non-spacing
  /// mark at the same origin is (X3.110 §7.2).
  void compose(const std::string& mark) { op_.text += mark; }

  CatalogueDrawOp take() {
    open_ = false;
    return std::move(op_);
  }

 private:
  /// The step the character path would take with no extra spacing, which is all
  /// a run of one character says about its direction (§5.3.2.3.3 Table 7).
  static CatalogueSize nominal_advance(const NabtsPrimitive& primitive) {
    const double width = std::fabs(primitive.size.dx);
    const double height = std::fabs(primitive.size.dy);
    switch (primitive.path) {
      case NabtsCharPath::kRight:
        return CatalogueSize{width, 0.0};
      case NabtsCharPath::kLeft:
        return CatalogueSize{-width, 0.0};
      case NabtsCharPath::kUp:
        return CatalogueSize{0.0, height};
      case NabtsCharPath::kDown:
        return CatalogueSize{0.0, -height};
    }
    return CatalogueSize{width, 0.0};
  }

  /// One character field along a single axis, in the run's established
  /// direction. Two fields is the ceiling — see kMaxStepFields.
  bool step_is_along_the_path(double dx, double dy) const {
    const double width = std::fabs(op_.size.dx);
    const double height = std::fabs(op_.size.dy);
    const bool horizontal =
        std::fabs(dy) < kOriginEpsilon && std::fabs(dx) > kOriginEpsilon &&
        std::fabs(dx) <= width * kMaxStepFields + kOriginEpsilon;
    const bool vertical =
        std::fabs(dx) < kOriginEpsilon && std::fabs(dy) > kOriginEpsilon &&
        std::fabs(dy) <= height * kMaxStepFields + kOriginEpsilon;
    if (!horizontal && !vertical) {
      return false;
    }
    if (!have_step_) {
      return true;  // the second character establishes the direction
    }
    // A record that turned a corner mid-word started a new run, whatever the
    // attributes say.
    return std::fabs(dx - step_x_) < kOriginEpsilon &&
           std::fabs(dy - step_y_) < kOriginEpsilon;
  }

  CatalogueDrawOp op_;
  CataloguePoint last_origin_;
  double step_x_ = 0.0;
  double step_y_ = 0.0;
  bool have_step_ = false;
  bool open_ = false;
};

/// CEA-516 §5.2.2: record types 0, 1 and 3 carry presentation data, which §6.1
/// makes NAPLPS; type 2 carries application data. The reserved types 4 to 15
/// say nothing about their data, so nothing is assumed of them.
bool type_is_presentation(uint8_t type) {
  return type == 0 || type == 1 || type == 3;
}

/// CEA-516 §5.2.2's record types. 4 to 15 are reserved, and reporting the
/// number is more use than inventing a name for it.
std::string record_type_name(uint8_t type) {
  switch (type) {
    case 0:
      return "Cyclic presentation";
    case 1:
      return "Non-cyclic presentation";
    case 2:
      return "Application";
    case 3:
      return "Priority presentation";
    default:
      break;
  }
  return "Reserved (" + std::to_string(static_cast<int>(type)) + ")";
}

/// Short form of a record type for the list, which has no room for the full
/// name.
std::string short_type_name(uint8_t type) {
  switch (type) {
    case 0:
      return "Cyclic";
    case 1:
      return "Non-cyclic";
    case 2:
      return "Application";
    case 3:
      return "Priority";
    default:
      break;
  }
  return "Type " + std::to_string(static_cast<int>(type));
}

std::string to_upper(std::string text) {
  for (char& character : text) {
    if (character >= 'a' && character <= 'z') {
      character = static_cast<char>(character - 'a' + 'A');
    }
  }
  return text;
}

/// §5.2.1: channel, record address and version together are the identity, so
/// all three are shown — two records at one address in different versions are
/// different records.
std::string record_identity(const NabtsCataloguedRecord& record) {
  char version[8];
  std::snprintf(version, sizeof(version), "%X", record.version & 0xF);
  return to_upper(record.channel_text) + " v" + version;
}

std::string frame_range(uint64_t first, uint64_t last) {
  // Frame numbers are 1-based in the UI; the dataset carries them 0-based.
  const uint64_t from = first + 1;
  const uint64_t to = last + 1;
  if (from == to) {
    return std::to_string(from);
  }
  return std::to_string(from) + "-" + std::to_string(to);
}

/// Zero-padded decimal, for the fixed-width fields of a timecode.
std::string padded(int64_t value, size_t width) {
  std::string text = std::to_string(value);
  while (text.size() < width) {
    text.insert(text.begin(), '0');
  }
  return text;
}

/// A frame index as a wall-clock position, HH:MM:SS.mmm.
std::string frame_time(uint64_t frame) {
  const double seconds_total =
      static_cast<double>(frame) / kNtscFramesPerSecond;
  const auto millis =
      static_cast<int64_t>(std::llround(seconds_total * 1000.0));
  return padded(millis / 3600000, 2) + ":" + padded((millis / 60000) % 60, 2) +
         ":" + padded((millis / 1000) % 60, 2) + "." + padded(millis % 1000, 3);
}

std::string plural(uint64_t count, const char* singular, const char* many) {
  return std::to_string(count) + " " + (count == 1 ? singular : many);
}

std::string join(const std::vector<std::string>& parts,
                 const std::string& separator) {
  if (parts.empty()) {
    return {};
  }
  std::string text = parts.front();
  for (size_t i = 1; i < parts.size(); ++i) {
    text += separator + parts[i];
  }
  return text;
}

/// The classification flags a record declared (§5.2.7.2), as a list.
std::vector<std::string> classification_flags(
    const NabtsCataloguedRecord& record) {
  std::vector<std::string> flags;
  if (record.caption) flags.emplace_back("caption");
  if (record.cyclic_marker) flags.emplace_back("cyclic marker");
  if (record.priority) flags.emplace_back("priority");
  if (record.alarm) flags.emplace_back("alarm");
  if (record.update) flags.emplace_back("update");
  if (record.support_record) flags.emplace_back("support record");
  if (record.index) flags.emplace_back("index");
  if (record.more) flags.emplace_back("more");
  return flags;
}

std::string record_condition(const NabtsCataloguedRecord& record,
                             bool presentation) {
  std::vector<std::string> parts;
  if (!record.complete) {
    // §5.2.6: a message missing a link renders short, and there is no way to
    // tell that from a record the service meant to be short.
    parts.emplace_back("incomplete");
  } else if (record.times_intact == 0) {
    parts.emplace_back("no undamaged copy");
  } else {
    parts.emplace_back("complete");
  }
  if (record.records_in_message > 1) {
    parts.push_back(
        plural(record.records_in_message, "linked record", "linked records"));
  }
  parts.push_back(plural(record.data.size(), "byte", "bytes"));

  if (presentation) {
    const auto& diagnostics = record.page.diagnostics;
    if (diagnostics.truncated_pdis > 0) {
      parts.push_back(plural(diagnostics.truncated_pdis,
                             "truncated instruction",
                             "truncated instructions"));
    }
    if (diagnostics.unresolved_macros > 0) {
      parts.push_back(plural(diagnostics.unresolved_macros, "unresolved macro",
                             "unresolved macros"));
    }
    if (diagnostics.storage_refusals > 0) {
      parts.push_back(
          plural(diagnostics.storage_refusals, "definition", "definitions") +
          " over the storage budget");
    }
  }
  return join(parts, ", ");
}

/// An application record's descriptors as a listing (§7.2.2).
std::string function_listing(const NabtsCataloguedRecord& record) {
  if (record.functions.empty()) {
    return "This application record carried no function descriptors.";
  }
  std::vector<std::string> lines;
  lines.push_back(plural(record.functions.size(), "function descriptor",
                         "function descriptors") +
                  " (CEA-516 §7.2.2):");
  lines.emplace_back();
  for (const auto& function : record.functions) {
    const std::string kind = function.control ? "control" : "information";
    // §7.2.3.1: a descriptor with no arguments asks for that function's
    // initial state back.
    const std::string arguments = function.arguments.empty()
                                      ? "(restore initial state)"
                                      : function.arguments;
    lines.push_back(function.code + "  [" + kind + "]  " + arguments);
  }
  return join(lines, "\n");
}

std::string run_headline(const NabtsRecoverySummary& summary) {
  if (summary.frames_analysed == 0 && summary.packets_recovered == 0) {
    return {};
  }
  std::vector<std::string> parts;
  parts.push_back(std::to_string(summary.packets_recovered) +
                  " packets recovered from " +
                  std::to_string(summary.fields_with_data) + " fields over " +
                  std::to_string(summary.frames_analysed) + " frames");
  parts.push_back(std::to_string(summary.groups_completed) +
                  " data groups complete, " +
                  std::to_string(summary.groups_incomplete) + " incomplete");
  parts.push_back(std::to_string(summary.messages_complete) +
                  " records complete, " +
                  std::to_string(summary.messages_partial) + " partial");
  if (summary.packets_prefix_rejected > 0) {
    // §3.2.2: a packet whose Hamming prefix will not decode cannot even be
    // filed under a channel, so it is lost before any of the above.
    parts.push_back(std::to_string(summary.packets_prefix_rejected) +
                    " packets refused on their prefix");
  }
  if (summary.blocks_corrected > 0 || summary.blocks_damaged > 0) {
    parts.push_back(std::to_string(summary.blocks_corrected) +
                    " blocks repaired, " +
                    std::to_string(summary.blocks_damaged) + " beyond repair");
  }
  if (summary.lost_packets_estimate > 0) {
    parts.push_back("about " + std::to_string(summary.lost_packets_estimate) +
                    " packets lost");
  }
  if (summary.records_truncated) {
    parts.push_back("record list truncated at the catalogue limit");
  }
  return join(parts, "; ");
}

}  // namespace

CatalogueDisplayList nabts_page_display_list(
    const NabtsPageSnapshot& snapshot) {
  CatalogueDisplayList out;
  out.aspect_height = kNabtsDisplayAreaHeight;

  out.palette.reserve(kNabtsColourMapEntries);
  for (size_t i = 0; i < kNabtsColourMapEntries; ++i) {
    out.palette.push_back(to_colour(snapshot.colour_map[i]));
  }

  // The glyph list is indexed by the operations, so it is built first and the
  // code position a character was defined at maps to its place in the list.
  std::vector<int> glyph_slot(1u << 8, -1);
  for (const auto& glyph : snapshot.drcs) {
    if (!glyph.defined()) {
      continue;  // §5.6: a character never defined is displayed as SPACE
    }
    CatalogueBitmap bitmap;
    bitmap.width = glyph.width;
    bitmap.height = glyph.height;
    bitmap.elements = glyph.elements;
    glyph_slot[glyph.code] = static_cast<int>(out.glyphs.size());
    out.glyphs.push_back(std::move(bitmap));
  }

  out.fill_masks.reserve(kNabtsTextureMaskCount);
  for (size_t i = 0; i < kNabtsTextureMaskCount; ++i) {
    const NabtsTextureMask& mask = snapshot.texture_masks[i];
    CatalogueBitmap bitmap;
    bitmap.width = mask.width;
    bitmap.height = mask.height;
    bitmap.elements = mask.elements;
    out.fill_masks.push_back(std::move(bitmap));
  }

  TextRun run;
  // A non-spacing mark arrives before the letter it modifies and shares its
  // origin, so it is held until that letter arrives and composed onto it —
  // Unicode's order, which is the reverse of the transmission's.
  std::string pending_marks;

  const auto flush_run = [&] {
    if (run.open()) {
      out.ops.push_back(run.take());
    }
  };

  for (const NabtsPrimitive& primitive : snapshot.primitives) {
    if (primitive.kind != NabtsPrimitiveKind::kCharacter) {
      flush_run();
      CatalogueDrawOp op;
      op.kind = to_draw_kind(primitive.kind);
      copy_attributes(primitive, op);
      op.points.reserve(primitive.points.size());
      for (const NabtsPoint& point : primitive.points) {
        op.points.push_back(to_point(point));
      }
      op.origin = to_point(primitive.origin);
      op.size = to_size(primitive.size);
      if (op.kind == CatalogueDrawKind::kColourRun) {
        op.colour_run.reserve(primitive.incremental_colours.size());
        for (const uint8_t entry : primitive.incremental_colours) {
          op.colour_run.push_back(
              resolve_incremental(entry, primitive.colour_mode, snapshot));
        }
      }
      out.ops.push_back(std::move(op));
      continue;
    }

    switch (primitive.repertoire) {
      case NabtsPrimitive::Repertoire::kMosaic: {
        flush_run();
        CatalogueDrawOp op;
        op.kind = CatalogueDrawKind::kMosaic;
        copy_attributes(primitive, op);
        op.origin = to_point(primitive.origin);
        op.size = to_size(primitive.size);
        op.points.push_back(op.origin);
        // §5.4: the positions §5.4 does not assign "shall be displayed as
        // SPACE", which is a mosaic with nothing lit.
        op.mosaic_pattern = nabts_is_mosaic_code(primitive.character)
                                ? nabts_mosaic_sixels(primitive.character)
                                : 0;
        // §6.2.7.15: underline mode is what puts mosaics into separated mode.
        op.mosaic_separated = primitive.underlined;
        op.underlined = false;
        out.ops.push_back(std::move(op));
        break;
      }

      case NabtsPrimitive::Repertoire::kDrcs: {
        flush_run();
        CatalogueDrawOp op;
        op.kind = CatalogueDrawKind::kGlyph;
        copy_attributes(primitive, op);
        op.origin = to_point(primitive.origin);
        op.size = to_size(primitive.size);
        op.points.push_back(op.origin);
        op.glyph_index = glyph_slot[primitive.character];
        out.ops.push_back(std::move(op));
        break;
      }

      case NabtsPrimitive::Repertoire::kSupplementary:
        if (nabts_supplementary_is_nonspacing(primitive.character)) {
          const std::string mark = nabts_character_to_utf8(
              primitive.character, primitive.repertoire);
          if (run.open()) {
            // The mark modifies the character the run just took.
            pending_marks += mark;
          } else {
            // A mark with no letter behind it — a record that ended mid
            // composition. Kept rather than dropped: it is what arrived.
            run.start(primitive, mark);
          }
          continue;
        }
        [[fallthrough]];

      case NabtsPrimitive::Repertoire::kPrimary: {
        std::string glyph =
            nabts_character_to_utf8(primitive.character, primitive.repertoire);
        glyph += pending_marks;
        pending_marks.clear();
        if (!run.extend(primitive, glyph)) {
          flush_run();
          run.start(primitive, std::move(glyph));
        }
        break;
      }
    }
  }

  if (!pending_marks.empty() && run.open()) {
    run.compose(pending_marks);
  }
  flush_run();

  return out;
}

CatalogueDataset build_nabts_catalogue(const NabtsAnalysisDataset& data) {
  CatalogueDataset out;

  out.schema.columns = {
      CatalogueColumn{"address", "Address", false},
      CatalogueColumn{"type", "Type", false},
      CatalogueColumn{"seen", "Seen", true},
      CatalogueColumn{"frames", "Frames", true},
  };
  out.schema.item_noun = "Record";
  out.schema.highlight_label = "Show display area";
  out.schema.empty_message = "No NABTS records were recovered";

  for (const auto& record : data.records) {
    const bool presentation = type_is_presentation(record.record_type);
    const std::string identity = record_identity(record);

    CatalogueItem item;
    item.id = identity;
    item.find_key = identity;
    item.values = {
        identity,
        short_type_name(record.record_type),
        std::to_string(record.times_seen),
        frame_range(record.first_seen_frame, record.last_seen_frame),
    };

    std::vector<std::string> tips;
    if (!record.reserved_purpose.empty()) {
      // §7.1.5 reserves a handful of channel/address pairings; a reader has no
      // way of knowing which without being told.
      tips.push_back("CEA-516 §7.1.5 reserves this address for: " +
                     record.reserved_purpose);
    }
    const auto flags = classification_flags(record);
    if (!flags.empty()) {
      tips.push_back("Classification flags (§5.2.7.2): " + join(flags, ", "));
    }
    tips.push_back(std::to_string(record.times_intact) + " of " +
                   std::to_string(record.times_seen) +
                   " copies arrived undamaged");
    item.tooltip = join(tips, "\n\n");

    CataloguePayload payload;
    payload.headline =
        "Record " + identity + " (" + record_type_name(record.record_type) +
        ") seen " + plural(record.times_seen, "time", "times") + ", frames " +
        frame_range(record.first_seen_frame, record.last_seen_frame);

    if (presentation) {
      payload.kind = CataloguePayload::Kind::kDisplayList;
      payload.display_list = nabts_page_display_list(record.page);
      // A caption or an index page is usually read rather than looked at, so
      // the text goes beside the drawing. It comes from the snapshot rather
      // than from the display list: reading a record and drawing it want
      // different things out of the same characters, and the sink stage needs
      // the same reading for its caption export.
      payload.companion_text = nabts_page_text(record.page);
    } else {
      payload.kind = CataloguePayload::Kind::kText;
      payload.document.text = function_listing(record);
      payload.document.monospace = true;
    }
    payload.condition = record_condition(record, presentation);

    out.items.push_back(std::move(item));
    out.payloads.push_back(std::move(payload));
  }

  // The caption service as one further item. §7.3.10 carries captioning as
  // records with the Caption Flag of §5.2.7.3 set, each new caption a new
  // version of the same address, and reading them one record at a time tells a
  // viewer nothing about the service.
  const auto cues = nabts_caption_cues(data.records);
  if (!cues.empty()) {
    CatalogueItem item;
    // Outside the identity space records occupy: a record id is always
    // "<channel>/<address> v<n>", so a leading marker byte cannot collide.
    item.id = std::string(1, '\x01') + "captions";
    item.values = {
        "Caption track", "Captions", std::to_string(cues.size()),
        frame_range(cues.front().start_frame, cues.back().end_frame)};
    item.tooltip =
        "These records were transmitted with the Caption Flag set (CEA-516 "
        "§5.2.7.3), which is what makes a record part of the captioning "
        "service of §7.3.10. Selecting this reads them in order.";

    CataloguePayload payload;
    payload.kind = CataloguePayload::Kind::kTable;
    payload.table.columns = {
        CatalogueColumn{"frames", "Frames", true},
        CatalogueColumn{"time", "Time", false},
        CatalogueColumn{"text", "Caption", false},
    };
    std::vector<std::string> channels;
    for (const auto& cue : cues) {
      char channel[8];
      std::snprintf(channel, sizeof(channel), "%03X", cue.channel & 0xFFF);
      const std::string label =
          std::string(channel) + "/" + to_upper(cue.address_text);
      if (std::find(channels.begin(), channels.end(), label) ==
          channels.end()) {
        channels.push_back(label);
      }
      // A cue is often two lines; the table shows it on one so the list stays
      // scannable.
      std::string text = cue.text;
      std::replace(text.begin(), text.end(), '\n', ' ');
      payload.table.rows.push_back(
          {frame_range(cue.start_frame, cue.end_frame),
           frame_time(cue.start_frame) + " -> " + frame_time(cue.end_frame),
           std::move(text)});
    }
    payload.headline = plural(cues.size(), "caption", "captions") + " on " +
                       join(channels, ", ");

    out.items.push_back(std::move(item));
    out.payloads.push_back(std::move(payload));

    // Which channel carries them, because §7.3.10 makes A00 the entry point
    // but the captions themselves are wherever the Caption Flag is set.
    out.summary.notices.push_back(plural(cues.size(), "caption", "captions") +
                                  " on " + join(channels, ", "));
  }

  out.summary.headline = run_headline(data.summary);
  return out;
}

}  // namespace orc
