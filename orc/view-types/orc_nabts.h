/*
 * File:        orc_nabts.h
 * Module:      orc-view-types
 * Purpose:     NABTS record and presentation view models for MVP architecture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace orc::presenters {

/**
 * @file
 * @brief What the NABTS viewer draws and lists
 *
 * A NABTS presentation record is NAPLPS (CEA-516 §6.1, ANSI X3.110-1983) — a
 * drawing program rather than a character grid — so this is a display list
 * rather than a cell array. Everything in it is resolved: colours are RGB, code
 * positions are Unicode or sixels, and geometry is in the unit Cartesian space
 * of X3.110 §5.3.1 with y running upwards from the bottom left. A renderer
 * walks the list front to back and needs no state of its own.
 *
 * Unit space rather than pixels because the record has no pixel size: X3.110
 * Appendix B asks for 12 bits of internal precision at a 256-pixel resolution,
 * which double gives outright and a fixed raster would throw away.
 */

/// The unit screen's y extent Table D1 item 10 guarantees is visible; the strip
/// above it is border a receiver need not show.
constexpr double kNabtsDisplayAreaHeightView = 0.78125;

/// One colour, resolved out of the three-bits-per-gun GRB of X3.110 §5.3.1 into
/// the 8-bit channels a renderer wants.
struct NabtsColourView {
  uint8_t red = 0;
  uint8_t green = 0;
  uint8_t blue = 0;
  /// X3.110 §5.3.2.5's transparent colour, which shows the program video
  /// through. A renderer with nothing behind draws it as black — but a caption
  /// is exactly the case where something *is* behind, so it is carried rather
  /// than resolved away.
  bool transparent = false;

  bool operator==(const NabtsColourView& other) const {
    return red == other.red && green == other.green && blue == other.blue &&
           transparent == other.transparent;
  }
};

/// Colour-map entries Table D1 item 5(4) requires.
constexpr size_t kNabtsColourMapEntriesView = 16;
/// Programmable texture masks A to D (X3.110 §6.2.4).
constexpr size_t kNabtsTextureMaskCountView = 4;

/// A point in unit space.
struct NabtsPointView {
  double x = 0.0;
  double y = 0.0;
};

/// A size in unit space, which may be negative — X3.110 §5.3.2.3.9 lets a
/// character field be, and the sign is the direction the glyph is drawn in.
struct NabtsSizeView {
  double dx = 0.0;
  double dy = 0.0;
};

/// What a display-list entry draws. The geometric kinds are X3.110 §5.3.3's
/// primitives; the three character kinds split NabtsPrimitiveKind::kCharacter
/// by what a renderer has to do with it, which is different in each case.
enum class NabtsPrimitiveKindView : uint8_t {
  kPoint,              ///< §5.3.3.1, sized by the logical pel
  kLine,               ///< §5.3.3.2
  kArc,                ///< §5.3.3.3: three points is an arc, more a spline
  kRectangle,          ///< §5.3.3.4
  kPolygon,            ///< §5.3.3.5
  kIncrementalPoints,  ///< §5.3.3.6.3, a raster run of colours
  /// A run of characters from the primary or supplementary set, coalesced into
  /// one entry: @ref text is UTF-8, laid out from @ref origin along the
  /// character path with @ref size per character.
  kText,
  /// One 2x3 block mosaic (§5.4); @ref mosaic_pattern holds the six elements.
  kMosaic,
  /// One DRCS character (§5.6); @ref drcs_index indexes NabtsPageView::drcs.
  kDrcs,
};

/// X3.110 §5.3.2.4.2 Table 12.
enum class NabtsLineTextureView : uint8_t {
  kSolid = 0,
  kDotted = 1,
  kDashed = 2,
  kDottedDashed = 3,
};

/// X3.110 §5.3.2.4.4 Table 13; kMaskA to kMaskD are the programmable masks
/// carried in NabtsPageView::texture_masks.
enum class NabtsTexturePatternView : uint8_t {
  kSolid = 0,
  kVerticalHatch = 1,
  kHorizontalHatch = 2,
  kCrossHatch = 3,
  kMaskA = 4,
  kMaskB = 5,
  kMaskC = 6,
  kMaskD = 7,
};

/**
 * @brief One drawing operation with the state that was in force when it ran
 *
 * Self-contained, so the list can be drawn, re-drawn, clipped or scaled without
 * re-running anything. The fields a kind does not use keep their defaults.
 */
struct NabtsPrimitiveView {
  NabtsPrimitiveKindView kind = NabtsPrimitiveKindView::kPoint;

  /// Control points in unit space, already resolved from whatever mix of
  /// absolute and relative coordinates the record used.
  std::vector<NabtsPointView> points;
  /// Origin of a rectangle or a character run; the first of @ref points
  /// otherwise.
  NabtsPointView origin;
  /// Extent of a rectangle, or the character field of a character run.
  NabtsSizeView size;

  /// Filled rather than outlined (the filled forms of ARC, RECTANGLE and
  /// POLYGON, and INCREMENTAL POLYGON).
  bool filled = false;
  /// X3.110 §5.3.2.4.3: a filled figure outlined in nominal black, or in the
  /// background colour where there is one.
  bool highlighted = false;

  /// The logical pel in force (§5.3.2.2.6), which is what gives a line its
  /// width and a separated mosaic its gap.
  NabtsSizeView logical_pel;
  NabtsLineTextureView line_texture = NabtsLineTextureView::kSolid;
  NabtsTexturePatternView texture_pattern = NabtsTexturePatternView::kSolid;
  /// Step-and-repeat size for a programmable mask (§5.3.2.4.5).
  NabtsSizeView texture_mask_size;

  /// Drawing colour, already resolved through the colour map where the
  /// record's colour mode said to.
  NabtsColourView colour;
  /// Background colour. Only colour mode 2 has one — see @ref has_background,
  /// which is how a renderer tells "black background" from "no background".
  NabtsColourView background;
  bool has_background = false;
  /// X3.110 §6.2.8.1: part of a blink process, alternating between @ref colour
  /// and nominal black (or the background where there is one).
  bool blinking = false;

  /// kIncrementalPoints only: one colour per point, in raster order within the
  /// active field.
  std::vector<NabtsColourView> incremental_colours;

  // ---- Character kinds ----------------------------------------------------

  /// kText only: the run as UTF-8, one character per character field along the
  /// path. Non-spacing marks (X3.110 Tables 26 and 27) are transmitted before
  /// the letter they modify and are re-ordered here into Unicode's order, so
  /// this is directly displayable.
  std::string text;
  /// kText only: the step from one character field origin to the next, in unit
  /// space. X3.110 §5.3.2.3.3's character path plus §5.3.2.3.4's
  /// inter-character spacing decide it; a renderer laying the run out needs
  /// both and can recover neither from the origin alone. On a run of one
  /// character it is the nominal step the path would have taken.
  NabtsSizeView advance;
  /// kText only: how many character fields the run occupies, which is the
  /// count of *base* characters — a composed accent occupies one field, not
  /// two, so this cannot be recovered from @ref text alone.
  int character_count = 0;
  /// kMosaic only: the six sub-elements, bit 0 top-left, 1 top-right,
  /// 2 middle-left, 3 middle-right, 4 bottom-left, 5 bottom-right.
  uint8_t mosaic_pattern = 0;
  /// kMosaic only: separated (X3.110 §5.4) rather than contiguous, so each
  /// element is shrunk by the logical pel and left-and-bottom justified.
  bool mosaic_separated = false;
  /// kDrcs only: index into NabtsPageView::drcs, or -1 for a character the
  /// record used but never defined (§5.6 shows one as SPACE).
  int drcs_index = -1;

  /// Counterclockwise rotation of the character field about its origin
  /// (§5.3.2.3.2 Table 6): 0, 90, 180 or 270 degrees.
  int rotation_degrees = 0;
  /// Reverse video (§6.2.7.4): the field is filled and the character shape left
  /// undrawn.
  bool reverse_video = false;
  /// Underline mode (§6.2.7.15).
  bool underlined = false;
};

/// A DRCS character's resolved bitmap (X3.110 §5.6, §6.2.3). Row 0 is the
/// bottom of the buffer, matching unit space.
struct NabtsDrcsGlyphView {
  uint8_t code = 0;  ///< Code position it was defined at, 2/0 to 7/15
  int width = 0;
  int height = 0;
  /// Row-major, @ref width * @ref height entries, true for an element that is
  /// on.
  std::vector<bool> elements;

  bool defined() const {
    return width > 0 && height > 0 &&
           elements.size() ==
               static_cast<size_t>(width) * static_cast<size_t>(height);
  }
};

/// One programmable texture mask (X3.110 §6.2.4), same storage convention.
struct NabtsTextureMaskView {
  int width = 0;
  int height = 0;
  std::vector<bool> elements;

  bool defined() const {
    return width > 0 && height > 0 &&
           elements.size() ==
               static_cast<size_t>(width) * static_cast<size_t>(height);
  }
};

/**
 * @brief What the interpreter could not do with the record it was given
 *
 * A NAPLPS record recovered off air has lost packets in it, and a display list
 * that came up short looks exactly like one the service meant to be short. This
 * is the only thing that tells them apart.
 */
struct NabtsPageRecoveryView {
  /// Bytes executed, which on a truncated record is fewer than it carried.
  uint64_t bytes_read = 0;
  /// Escape sequences naming a set that is not implemented, or a null set.
  uint64_t unknown_designations = 0;
  /// Controls recognised but with no effect on a display list — the
  /// transmission and device controls of X3.110 §6.1.4 and §6.1.5, and the
  /// interactive controls Table D1 marks not applicable to teletext.
  uint64_t ignored_controls = 0;
  /// PDI sequences whose operands ran out before the opcode had what it needed.
  uint64_t truncated_pdis = 0;
  /// Coordinates outside the unit screen, counted and clamped.
  uint64_t out_of_range_coordinates = 0;
  /// Macro invocations that could not run: undefined, or nested too deep.
  uint64_t unresolved_macros = 0;
  /// Definitions refused because the 3072-byte shared macro and DRCS budget of
  /// CEA-516 §8.6.1 was full, and how much of it the record ended up using.
  uint64_t storage_refusals = 0;
  uint64_t storage_used = 0;

  /// True when the record ran to its end with nothing unexplained. Says
  /// nothing about whether every byte of it arrived — that is the record's
  /// own completeness, not the interpreter's.
  bool clean() const {
    return truncated_pdis == 0 && unresolved_macros == 0 &&
           storage_refusals == 0 && unknown_designations == 0;
  }
};

/**
 * @brief One presentation record run into something drawable
 *
 * The list is in execution order, which is also back-to-front paint order:
 * NAPLPS has no z-ordering, so a later primitive covers an earlier one.
 */
struct NabtsPageView {
  std::vector<NabtsPrimitiveView> primitives;

  /// The y extent a receiver shows, as a fraction of the unit screen.
  double display_area_height = kNabtsDisplayAreaHeightView;

  /// The colour map as it stood at the end of the record. Carried even though
  /// the primitives already have their colours resolved, because §5.3.2.5 makes
  /// a map write retroactive and a renderer showing the map is showing what was
  /// on screen.
  std::array<NabtsColourView, kNabtsColourMapEntriesView> colour_map{};

  std::vector<NabtsDrcsGlyphView> drcs;
  std::array<NabtsTextureMaskView, kNabtsTextureMaskCountView> texture_masks{};

  /// The record's text content in reading order — top row first, left to right
  /// within a row — with a newline between rows. NAPLPS has no rows, so the
  /// rows are the presenter's reading of the character origins; the display
  /// list is what a renderer should draw.
  std::string text;

  NabtsPageRecoveryView recovery;

  /// Whether the record drew anything at all. A record that only defined macros
  /// or DRCS is legitimately empty.
  bool empty() const { return primitives.empty(); }
};

/// One application function descriptor, ready to list (CEA-516 §7.2.2).
struct NabtsRecordFunctionView {
  /// Function code in the "2/0" column/row notation the standard uses.
  std::string code;
  /// Control data (column 2) rather than information (column 3).
  bool control = false;
  /// Arguments as printable text, unprintable bytes shown as their hexadecimal
  /// value. Empty for a descriptor that §7.2.3.1 makes a request to restore
  /// that function's initial state.
  std::string arguments;
};

/**
 * @brief One teletext record the analysed range carried
 *
 * A *message* in the standard's terms (CEA-516 §5.2.6) — one unlinked record,
 * or a linked series joined — because that is the unit a receiver presents.
 * Catalogued rather than kept per transmission: a cyclic service brings the
 * same record round throughout a recording.
 */
struct NabtsCatalogueRecordView {
  /// Data channel, i.e. the packet address of §3.2.3.
  uint16_t channel = 0;
  /// Record address in the nine-digit long form §5.2.5 makes equivalent to the
  /// short one, so the two forms of an address compare equal.
  uint64_t address = 0;
  /// The address as transmitted: three hexadecimal digits, or nine when the
  /// record carried an address extension.
  std::string address_text;
  /// Channel and address together, as a reader would cite them.
  std::string channel_text;

  /// RT (§5.2.2), and its name where the standard gives one.
  uint8_t record_type = 0;
  std::string record_type_name;
  /// True for record types 0, 1 and 3, whose data is NAPLPS (§6.1) and so has a
  /// @ref page; false for type 2, whose data is @ref functions.
  bool presentation = false;

  /// Version from classification flag byte Y16 (§5.2.7.2), 0 where the record
  /// carried no classification sequence.
  uint8_t version = 0;

  // Classification flags worth listing (§5.2.7.2). All false where the record
  // carried no classification sequence, which §5.2.7.2 makes the correct
  // reading of an absent flag byte.
  bool caption = false;
  bool cyclic_marker = false;
  bool priority = false;
  bool alarm = false;
  bool update = false;
  bool support_record = false;
  bool index = false;
  bool more = false;

  /// What §7.1.5 reserves this channel and address for, or empty.
  std::string reserved_purpose;

  /// Frames carrying the first and the most recent copy (0-based; the view adds
  /// one where it displays them).
  uint64_t first_seen_frame = 0;
  uint64_t last_seen_frame = 0;
  /// Copies counted over the analysed range, and how many of those arrived
  /// whole and undamaged.
  uint64_t times_seen = 0;
  uint64_t times_intact = 0;

  /// Records in the linked series (§5.2.6); 1 for an unlinked record.
  uint32_t records_in_message = 0;
  /// The best copy had every link of its series and every packet of every
  /// group. A false here is why a presentation record may render short.
  bool complete = false;
  /// Bytes of record data the best copy carried.
  uint64_t data_bytes = 0;

  /// Function descriptors, for an application record; empty otherwise.
  std::vector<NabtsRecordFunctionView> functions;
  /// The presentation code run into a display list, for a presentation record;
  /// empty otherwise.
  NabtsPageView page;
};

/**
 * @brief One caption, with the frames it was on screen for
 *
 * CEA-516 §7.3.10 carries captioning as non-cyclic presentation records on data
 * channel A00, each new caption a new version of the same record address. The
 * cue's text is its record's, and its extent runs to the next caption — a
 * receiver replaces the caption on screen rather than being told to erase it.
 */
struct NabtsCaptionCueView {
  uint64_t start_frame = 0;
  uint64_t end_frame = 0;
  uint16_t channel = 0;
  std::string address_text;
  uint8_t version = 0;
  std::string text;
};

/**
 * @brief How the NABTS recovery went over the analysed range
 *
 * Aggregate counts only; the per-line and per-group detail lives in the stage's
 * own report.
 */
struct NabtsRecoverySummaryView {
  uint64_t frames_analysed = 0;
  uint64_t fields_with_data = 0;
  uint64_t packets_recovered = 0;
  /// Packets whose Hamming 8/4 prefix did not decode (§3.2.2), and so could not
  /// even be filed under a channel.
  uint64_t packets_prefix_rejected = 0;
  /// Packet slots that came back empty on a line the recording has been seen to
  /// carry data on — an estimate of what was lost.
  uint64_t lost_packets_estimate = 0;
  /// Data blocks the suffix product code repaired, and blocks whose suffix
  /// check failed and could not be repaired (§3.4).
  uint64_t blocks_corrected = 0;
  uint64_t blocks_damaged = 0;
  /// Data groups that arrived whole, and groups that ended without every packet
  /// they promised (§4.2.5).
  uint64_t groups_completed = 0;
  uint64_t groups_incomplete = 0;
  /// Messages assembled whole, and messages missing at least one link.
  uint64_t messages_complete = 0;
  uint64_t messages_partial = 0;
  /// True when the stage's record cap dropped records, so the catalogue is not
  /// everything the range carried.
  bool records_truncated = false;
};

/**
 * @brief Everything the NABTS viewer shows for one triggered node
 *
 * The whole analysed range, not a window around the previewer: the stage
 * decodes the range once per trigger and the viewer reads the result.
 */
struct NabtsAnalysisView {
  /// Ascending by {channel, address, version}, which is the order §7.3 steps a
  /// service in.
  std::vector<NabtsCatalogueRecordView> records;
  /// The caption service's cues, ascending by start frame. Empty on a recording
  /// that carried no captioning.
  std::vector<NabtsCaptionCueView> captions;
  NabtsRecoverySummaryView summary;
};

}  // namespace orc::presenters
