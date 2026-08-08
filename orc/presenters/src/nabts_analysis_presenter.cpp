/*
 * File:        nabts_analysis_presenter.cpp
 * Module:      orc-presenters
 * Purpose:     NABTS analysis presenter implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../include/nabts_analysis_presenter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace orc::presenters {

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

/// A three-bit gun value as an 8-bit channel, full scale to full scale.
uint8_t to_channel(uint8_t gun) {
  return static_cast<uint8_t>((std::min<int>(gun, kGunMax) * 255) / kGunMax);
}

NabtsColourView to_colour_view(const NabtsColour& colour) {
  NabtsColourView view;
  view.red = to_channel(colour.red);
  view.green = to_channel(colour.green);
  view.blue = to_channel(colour.blue);
  view.transparent = colour.transparent;
  return view;
}

NabtsPointView to_point_view(const NabtsPoint& point) {
  return NabtsPointView{point.x, point.y};
}

NabtsSizeView to_size_view(const NabtsSize& size) {
  return NabtsSizeView{size.dx, size.dy};
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
NabtsColourView resolve_incremental(uint8_t entry, NabtsColourMode mode,
                                    const NabtsPageSnapshot& snapshot) {
  if (mode == NabtsColourMode::kDirect) {
    return to_colour_view(colour_from_numeric(entry));
  }
  const size_t address = static_cast<size_t>(entry) % kNabtsColourMapEntries;
  return to_colour_view(snapshot.colour_map[address]);
}

NabtsPrimitiveKindView to_kind_view(NabtsPrimitiveKind kind) {
  switch (kind) {
    case NabtsPrimitiveKind::kPoint:
      return NabtsPrimitiveKindView::kPoint;
    case NabtsPrimitiveKind::kLine:
      return NabtsPrimitiveKindView::kLine;
    case NabtsPrimitiveKind::kArc:
      return NabtsPrimitiveKindView::kArc;
    case NabtsPrimitiveKind::kRectangle:
      return NabtsPrimitiveKindView::kRectangle;
    case NabtsPrimitiveKind::kPolygon:
      return NabtsPrimitiveKindView::kPolygon;
    case NabtsPrimitiveKind::kIncrementalPoints:
      return NabtsPrimitiveKindView::kIncrementalPoints;
    case NabtsPrimitiveKind::kCharacter:
      break;
  }
  // Split by repertoire at the call site; never reached with a character.
  return NabtsPrimitiveKindView::kText;
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

/// Everything a primitive carries that is not geometry: two primitives that
/// agree on all of it can be drawn by one call.
void copy_attributes(const NabtsPrimitive& from, NabtsPrimitiveView& to) {
  to.filled = from.filled;
  to.highlighted = from.highlighted;
  to.logical_pel = to_size_view(from.logical_pel);
  to.line_texture = static_cast<NabtsLineTextureView>(from.line_texture);
  to.texture_pattern =
      static_cast<NabtsTexturePatternView>(from.texture_pattern);
  to.texture_mask_size = to_size_view(from.texture_mask_size);
  to.colour = to_colour_view(from.colour);
  to.has_background =
      from.colour_mode == NabtsColourMode::kMappedWithBackground;
  to.background = to_colour_view(from.background);
  to.blinking = from.blinking;
  to.rotation_degrees = rotation_degrees(from.rotation);
  to.reverse_video = from.reverse_video;
  to.underlined = from.underlined;
}

/// Whether |candidate| could be drawn as part of a run already carrying the
/// attributes of |run|.
bool attributes_match(const NabtsPrimitiveView& run,
                      const NabtsPrimitiveView& candidate) {
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
  NabtsPrimitiveView& view() { return view_; }

  /// Start a run at |primitive|, discarding whatever was there.
  void start(const NabtsPrimitive& primitive, std::string glyph) {
    view_ = NabtsPrimitiveView{};
    view_.kind = NabtsPrimitiveKindView::kText;
    copy_attributes(primitive, view_);
    view_.origin = to_point_view(primitive.origin);
    view_.size = to_size_view(primitive.size);
    view_.advance = nominal_advance(primitive);
    view_.points.push_back(view_.origin);
    view_.text = std::move(glyph);
    view_.character_count = 1;
    last_origin_ = view_.origin;
    have_step_ = false;
    open_ = true;
  }

  /// Extend the run with |primitive| if it belongs to it; false if it does not.
  bool extend(const NabtsPrimitive& primitive, const std::string& glyph) {
    if (!open_) {
      return false;
    }
    NabtsPrimitiveView candidate;
    copy_attributes(primitive, candidate);
    candidate.size = to_size_view(primitive.size);
    if (!attributes_match(view_, candidate)) {
      return false;
    }

    const double dx = primitive.origin.x - last_origin_.x;
    const double dy = primitive.origin.y - last_origin_.y;
    if (!step_is_along_the_path(dx, dy)) {
      return false;
    }

    view_.text += glyph;
    ++view_.character_count;
    // The measured step beats the nominal one: it carries whatever
    // inter-character spacing (§5.3.2.3.4) the record asked for.
    view_.advance = NabtsSizeView{dx, dy};
    last_origin_ = to_point_view(primitive.origin);
    step_x_ = dx;
    step_y_ = dy;
    have_step_ = true;
    return true;
  }

  /// Compose |mark| onto the character just added, which is what a non-spacing
  /// mark at the same origin is (X3.110 §7.2).
  void compose(const std::string& mark) { view_.text += mark; }

  NabtsPrimitiveView take() {
    open_ = false;
    return std::move(view_);
  }

 private:
  /// The step the character path would take with no extra spacing, which is all
  /// a run of one character says about its direction (§5.3.2.3.3 Table 7).
  static NabtsSizeView nominal_advance(const NabtsPrimitive& primitive) {
    const double width = std::fabs(primitive.size.dx);
    const double height = std::fabs(primitive.size.dy);
    switch (primitive.path) {
      case NabtsCharPath::kRight:
        return NabtsSizeView{width, 0.0};
      case NabtsCharPath::kLeft:
        return NabtsSizeView{-width, 0.0};
      case NabtsCharPath::kUp:
        return NabtsSizeView{0.0, height};
      case NabtsCharPath::kDown:
        return NabtsSizeView{0.0, -height};
    }
    return NabtsSizeView{width, 0.0};
  }

  /// One character field along a single axis, in the run's established
  /// direction. Two fields is the ceiling — see kMaxStepFields.
  bool step_is_along_the_path(double dx, double dy) const {
    const double width = std::fabs(view_.size.dx);
    const double height = std::fabs(view_.size.dy);
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

  NabtsPrimitiveView view_;
  NabtsPointView last_origin_;
  double step_x_ = 0.0;
  double step_y_ = 0.0;
  bool have_step_ = false;
  bool open_ = false;
};

NabtsPageRecoveryView to_recovery_view(const NabtsDecodeDiagnostics& source) {
  NabtsPageRecoveryView view;
  view.bytes_read = source.bytes_read;
  view.unknown_designations = source.unknown_designations;
  view.ignored_controls = source.ignored_controls;
  view.truncated_pdis = source.truncated_pdis;
  view.out_of_range_coordinates = source.out_of_range_coordinates;
  view.unresolved_macros = source.unresolved_macros;
  view.storage_refusals = source.storage_refusals;
  view.storage_used = source.storage_used;
  return view;
}

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

}  // namespace

NabtsPageView NabtsAnalysisPresenter::makePageView(
    const NabtsPageSnapshot& snapshot) {
  static_assert(kNabtsColourMapEntriesView == kNabtsColourMapEntries);
  static_assert(kNabtsTextureMaskCountView == kNabtsTextureMaskCount);

  NabtsPageView view;
  view.display_area_height = kNabtsDisplayAreaHeight;
  view.recovery = to_recovery_view(snapshot.diagnostics);

  for (size_t i = 0; i < kNabtsColourMapEntriesView; ++i) {
    view.colour_map[i] = to_colour_view(snapshot.colour_map[i]);
  }

  // The DRCS list is indexed by the primitives, so it is built first and the
  // code position a character was defined at maps to its place in the list.
  std::vector<int> drcs_slot(1u << 8, -1);
  for (const auto& glyph : snapshot.drcs) {
    if (!glyph.defined()) {
      continue;  // §5.6: a character never defined is displayed as SPACE
    }
    NabtsDrcsGlyphView out;
    out.code = glyph.code;
    out.width = glyph.width;
    out.height = glyph.height;
    out.elements = glyph.elements;
    drcs_slot[glyph.code] = static_cast<int>(view.drcs.size());
    view.drcs.push_back(std::move(out));
  }

  for (size_t i = 0; i < kNabtsTextureMaskCountView; ++i) {
    const NabtsTextureMask& mask = snapshot.texture_masks[i];
    view.texture_masks[i].width = mask.width;
    view.texture_masks[i].height = mask.height;
    view.texture_masks[i].elements = mask.elements;
  }

  TextRun run;
  // A non-spacing mark arrives before the letter it modifies and shares its
  // origin, so it is held until that letter arrives and composed onto it —
  // Unicode's order, which is the reverse of the transmission's.
  std::string pending_marks;

  const auto flush_run = [&] {
    if (run.open()) {
      view.primitives.push_back(run.take());
    }
  };

  for (const NabtsPrimitive& primitive : snapshot.primitives) {
    if (primitive.kind != NabtsPrimitiveKind::kCharacter) {
      flush_run();
      NabtsPrimitiveView out;
      out.kind = to_kind_view(primitive.kind);
      copy_attributes(primitive, out);
      out.points.reserve(primitive.points.size());
      for (const NabtsPoint& point : primitive.points) {
        out.points.push_back(to_point_view(point));
      }
      out.origin = to_point_view(primitive.origin);
      out.size = to_size_view(primitive.size);
      if (out.kind == NabtsPrimitiveKindView::kIncrementalPoints) {
        out.incremental_colours.reserve(primitive.incremental_colours.size());
        for (const uint8_t entry : primitive.incremental_colours) {
          out.incremental_colours.push_back(
              resolve_incremental(entry, primitive.colour_mode, snapshot));
        }
      }
      view.primitives.push_back(std::move(out));
      continue;
    }

    switch (primitive.repertoire) {
      case NabtsPrimitive::Repertoire::kMosaic: {
        flush_run();
        NabtsPrimitiveView out;
        out.kind = NabtsPrimitiveKindView::kMosaic;
        copy_attributes(primitive, out);
        out.origin = to_point_view(primitive.origin);
        out.size = to_size_view(primitive.size);
        out.points.push_back(out.origin);
        // §5.4: the positions §5.4 does not assign "shall be displayed as
        // SPACE", which is a mosaic with nothing lit.
        out.mosaic_pattern = nabts_is_mosaic_code(primitive.character)
                                 ? nabts_mosaic_sixels(primitive.character)
                                 : 0;
        // §6.2.7.15: underline mode is what puts mosaics into separated mode.
        out.mosaic_separated = primitive.underlined;
        out.underlined = false;
        view.primitives.push_back(std::move(out));
        break;
      }

      case NabtsPrimitive::Repertoire::kDrcs: {
        flush_run();
        NabtsPrimitiveView out;
        out.kind = NabtsPrimitiveKindView::kDrcs;
        copy_attributes(primitive, out);
        out.origin = to_point_view(primitive.origin);
        out.size = to_size_view(primitive.size);
        out.points.push_back(out.origin);
        out.drcs_index = drcs_slot[primitive.character];
        view.primitives.push_back(std::move(out));
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

  // The record's text comes from the snapshot rather than from the display
  // list built above: reading a record and drawing it want different things out
  // of the same characters, and the sink stage needs the same reading for its
  // caption export.
  view.text = nabts_page_text(snapshot);
  return view;
}

std::vector<NabtsCaptionCueView> NabtsAnalysisPresenter::makeCaptionCues(
    const std::vector<NabtsCataloguedRecord>& records) {
  std::vector<NabtsCaptionCueView> out;
  for (const NabtsCaptionCue& cue : nabts_caption_cues(records)) {
    NabtsCaptionCueView view;
    view.start_frame = cue.start_frame;
    view.end_frame = cue.end_frame;
    view.channel = cue.channel;
    view.address_text = cue.address_text;
    view.version = cue.version;
    view.text = cue.text;
    out.push_back(std::move(view));
  }
  return out;
}

NabtsAnalysisView NabtsAnalysisPresenter::makeAnalysisView(
    const NabtsAnalysisDataset& dataset) {
  NabtsAnalysisView view;

  view.records.reserve(dataset.records.size());
  for (const auto& catalogued : dataset.records) {
    NabtsCatalogueRecordView entry;
    entry.channel = catalogued.channel;
    entry.address = catalogued.address;
    entry.address_text = catalogued.address_text;
    entry.channel_text = catalogued.channel_text;
    entry.record_type = catalogued.record_type;
    entry.record_type_name = record_type_name(catalogued.record_type);
    entry.presentation = type_is_presentation(catalogued.record_type);
    entry.version = catalogued.version;
    entry.caption = catalogued.caption;
    entry.cyclic_marker = catalogued.cyclic_marker;
    entry.priority = catalogued.priority;
    entry.alarm = catalogued.alarm;
    entry.update = catalogued.update;
    entry.support_record = catalogued.support_record;
    entry.index = catalogued.index;
    entry.more = catalogued.more;
    entry.reserved_purpose = catalogued.reserved_purpose;
    entry.first_seen_frame = catalogued.first_seen_frame;
    entry.last_seen_frame = catalogued.last_seen_frame;
    entry.times_seen = catalogued.times_seen;
    entry.times_intact = catalogued.times_intact;
    entry.records_in_message = catalogued.records_in_message;
    entry.complete = catalogued.complete;
    entry.data_bytes = catalogued.data.size();

    entry.functions.reserve(catalogued.functions.size());
    for (const auto& function : catalogued.functions) {
      NabtsRecordFunctionView out;
      out.code = function.code;
      out.control = function.control;
      out.arguments = function.arguments;
      entry.functions.push_back(std::move(out));
    }

    if (entry.presentation) {
      entry.page = makePageView(catalogued.page);
    }

    view.records.push_back(std::move(entry));
  }

  view.captions = makeCaptionCues(dataset.records);

  const auto& summary = dataset.summary;
  view.summary.frames_analysed = summary.frames_analysed;
  view.summary.fields_with_data = summary.fields_with_data;
  view.summary.packets_recovered = summary.packets_recovered;
  view.summary.packets_prefix_rejected = summary.packets_prefix_rejected;
  view.summary.lost_packets_estimate = summary.lost_packets_estimate;
  view.summary.blocks_corrected = summary.blocks_corrected;
  view.summary.blocks_damaged = summary.blocks_damaged;
  view.summary.groups_completed = summary.groups_completed;
  view.summary.groups_incomplete = summary.groups_incomplete;
  view.summary.messages_complete = summary.messages_complete;
  view.summary.messages_partial = summary.messages_partial;
  view.summary.records_truncated = summary.records_truncated;

  return view;
}

}  // namespace orc::presenters
