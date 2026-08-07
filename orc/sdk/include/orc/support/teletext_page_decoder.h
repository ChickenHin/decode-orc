/*
 * File:        teletext_page_decoder.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     WST (System B) teletext magazine/page decoder producing Level 1
 *              page snapshots and subtitle cues
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_PAGE_DECODER_H
#define ORC_TELETEXT_PAGE_DECODER_H

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "teletext_row_squasher.h"
#include "teletext_slicer.h"

namespace orc {

// Check a display byte for odd parity.
// ETSI EN 300 706 §8.1: bit 8 is the parity bit, bits 1-7 carry the data;
// the byte is accepted when it contains an odd number of '1' bits.
bool teletext_odd_parity_valid(uint8_t byte);

// Encode a 7-bit value as an odd-parity protected byte (ETSI EN 300 706
// §8.1). Only the low 7 bits of |value| are used.
uint8_t teletext_odd_parity_encode(uint8_t value);

// Level 1 display colours in spacing-attribute code order.
// ETSI EN 300 706 §12.2 Table 26: alpha colour codes 0/0-0/7 and mosaic
// colour codes 1/0-1/7 select black through white in this order.
enum class TeletextColour : uint8_t {
  Black = 0,
  Red = 1,
  Green = 2,
  Yellow = 3,
  Blue = 4,
  Magenta = 5,
  Cyan = 6,
  White = 7,
};

// One rendered character cell of a Level 1 page.
struct TeletextPageCell {
  // 7-bit transmitted code (odd parity removed). For alphanumeric cells this
  // indexes the current G0 set; for mosaic cells the G1 set. Cells occupied
  // by a spacing attribute hold 0x20 (SPACE), or the held-mosaic character
  // when |held_mosaic| is set (EN 300 706 §12.2 code 1/E).
  uint8_t character = 0x20;
  TeletextColour foreground = TeletextColour::White;
  TeletextColour background = TeletextColour::Black;
  // G1 mosaic set selected (EN 300 706 §12.2 codes 1/1-1/7). Character codes
  // 0x40-0x5F remain alphanumeric capitals even in mosaic mode.
  bool mosaic = false;
  // Separated (bordered) rather than contiguous mosaic blocks (§12.2 1/A).
  bool separated_mosaic = false;
  // Cell is a spacing attribute displayed as the held mosaic character
  // (§12.2 1/E); |separated_mosaic| then reflects the held character's
  // original mode.
  bool held_mosaic = false;
  // Origin (upper) cell of a double-height pair (§12.2 0/D).
  bool double_height = false;
  // Lower cell of a double-height pair: no foreground data, background
  // copied from the origin row (§12.2 0/D).
  bool double_height_lower = false;
  bool flash = false;    // §12.2 0/8 (static or ignored by renderers)
  bool conceal = false;  // §12.2 1/8: display as SPACE until revealed
  bool boxed = false;    // inside a Start Box/End Box region (§12.2 0/A-0/B)
  // The transmitted byte failed odd parity (EN 300 706 §8.1); |character| is
  // replaced with SPACE and the cell flagged so renderers can mark it.
  bool parity_error = false;
};

// A completed Level 1 page: the 25-row grid (row 0 is the header row) plus
// the page address and header control bits of ETSI EN 300 706 §9.3.1.
struct TeletextPageSnapshot {
  static constexpr int kRows = 25;     // header row 0 + display rows 1-24
  static constexpr int kColumns = 40;  // EN 300 706 §9.3.2: 40 display bytes

  // Display columns to draw. kColumns on both services: the Level 1 display is
  // a 40-column grid whatever the packet length, and a row the service left
  // short simply shows spaces to the right of what it sent — which is what a
  // receiver puts on screen. Carried in the snapshot rather than read from
  // kColumns by consumers so a service that displays fewer can say so.
  int columns = kColumns;

  // Whether character codes 4/0-5/F keep their alphanumeric meaning while
  // mosaic graphics are selected — "blast-through", ETSI EN 300 706 §15.7.1
  // Table 47 NOTE 1 — so that capitals can be written into a graphic without
  // leaving mosaic mode. True on 625 lines.
  //
  // The 525-line recordings say otherwise: their page graphics run codes from
  // that range in among the mosaic ones (the service logo alternates 6/0 and
  // 7/E with 5/7 and 5/F, identically in every copy of the row), and read as
  // mosaics those are the block patterns the drawing needs — 5/F being a solid
  // block. Rendered as capitals they put stray letters through the artwork.
  // Nothing in ITU-R BT.653 settles it: §5.2.2 describes mosaic coding without
  // giving a code table, so this is what the material shows rather than what a
  // standard states.
  //
  // A renderer reads this to decide whether a cell in mosaic mode holding such
  // a code is a character or a block; nothing else about the page depends on
  // it.
  bool mosaic_blast_through = true;

  // Displayed magazine number 1-8. Transmission magazine 0 is displayed as
  // magazine 8 (EN 300 706 §3.1 "page number" convention: page 100 = 1/00).
  int magazine = 8;
  // Two-digit hexadecimal page number 0x00-0xFF (EN 300 706 §9.3.1.1).
  int page_number = 0;
  // 13-bit page sub-code S1-S4 (EN 300 706 §9.3.1.2).
  int subcode = 0;

  // Page header control bits (EN 300 706 §9.3.1.3 Table 2).
  bool erase_page = false;            // C4
  bool newsflash = false;             // C5
  bool subtitle = false;              // C6
  bool suppress_header = false;       // C7
  bool update_indicator = false;      // C8
  bool interrupted_sequence = false;  // C9
  bool inhibit_display = false;       // C10
  bool magazine_serial = false;       // C11
  int national_option_subset = 0;     // C12-C14 (EN 300 706 §15.2)

  // Field indices (as passed to process_packet()) of the header packet that
  // *opened* this transmission and of the last packet that contributed to
  // the page. A header re-sent while the page's own rows are still being
  // transmitted does not restamp the first of these, so every snapshot of
  // one appearance of a page shares a header_field_index and a consumer can
  // use it to tell appearances apart.
  int64_t header_field_index = 0;
  int64_t last_field_index = 0;

  // Whether a packet was received for each row of this page (row 0 = the
  // X/0 header). A row with no packet displays as spaces on a black
  // background, which is indistinguishable from a transmitted blank row —
  // so recovery gaps can only be reported from this flag, never inferred
  // from the cells.
  std::array<bool, kRows> row_received{};

  // Copies of each display row that were combined to produce it: 0 where no
  // packet was received, 1 where the row rests on a single copy, more where
  // repeated transmissions corrected each other (see teletext_row_squasher.h).
  // Index 0 is always 0 — header rows carry a live clock and are not squashed.
  //
  // This is the page's confidence in its own rows. One copy is not a fault,
  // but it is unchecked: the row is shown exactly as it was received, and a
  // burst long enough to carry a row's address onto another codeword (Hamming
  // 8/4 corrects one bit and detects two, EN 300 706 §8.2 — a longer burst can
  // still land on a valid address) puts that row in the wrong place with
  // nothing to contradict it. A second copy is what turns that into a vote.
  std::array<int, kRows> row_copies{};

  // Whether the page's transmission had finished when this snapshot was
  // taken. False means more rows of *this* transmission were still to come:
  // either the header was repeated part-way through the page (a rolling
  // header, EN 300 706 §9.3.1.4 — the service re-sends X/0 while the rows
  // continue to arrive), or a consumer peeked at the page in progress with
  // open_page_snapshots(). A partial snapshot is not damaged data; it is a
  // page that has not all arrived yet, and the two look identical on screen,
  // so only this flag distinguishes them.
  bool transmission_complete = true;

  std::array<std::array<TeletextPageCell, kColumns>, kRows> cells{};
};

// One subtitle cue recovered from a C6-flagged page. Times are expressed as
// the field indices passed to process_packet(); consumers convert to seconds
// via the field rate (50 fields/s for 625-line PAL).
struct TeletextSubtitleCue {
  int64_t start_field_index = 0;
  int64_t end_field_index = 0;
  // Plain text, rows separated by '\n', Level 1 attributes dropped.
  std::string text;
};

/**
 * @brief WST teletext magazine/page decoder (Level 1).
 *
 * Consumes T42 packets (MRAG + data bytes, transmission coding) in strictly
 * temporal order, applies Hamming 8/4 and odd-parity decoding (ETSI EN 300 706
 * §8.1-8.2), and assembles pages in both serial and parallel magazine
 * transmission modes (§7.2, §7.3, control bit C11).
 *
 * Both packet lengths ITU-R BT.653 defines for System B are handled: 42 bytes
 * on 625 lines and 34 on 525 (Table 1b). Everything the decoder reads by
 * position — the MRAG, the page number, the sub-code and the control bits —
 * sits at the same byte offsets in both, so only the number of display bytes a
 * packet carries changes: 40 and 32 (see process_packet()).
 *
 * Pages are 40 columns wide on both, because a 525-line service sends the
 * remaining 8 columns of its rows in separate *row-extension* packets. This is
 * not in BT.653 — the standard describes the 32-byte data block and stops — but
 * it is what the surviving 525-line WST recordings carry, and it is the only
 * way a 34-byte packet can deliver the 40-column page the standard's own
 * addressing, header layout and display model assume.
 *
 * An extension packet is addressed to magazine M|4. Its 32 display bytes are
 * four groups of 8 carrying columns 32-39 of four consecutive rows, and its
 * packet number identifies that block of four rather than naming a row: it
 * rounds down to a multiple of four, so packets numbered 1, 4, 8, 12, 16 and 20
 * complete rows 0-3, 4-7, 8-11, 12-15, 16-19 and 20-23. The first block's
 * packets carry 1 rather than 0 because 0 is the page header's own packet
 * number. Row 0 is therefore extended like any other: on the reference
 * recordings its columns 32-39 carry the service name, which is where a
 * 40-column receiver photograph of the same service shows it.
 *
 * Magazine M|4 is only borrowed where the service is not using it for pages.
 * The reference recordings carry pages in magazines 8, 1, 2, 3 *and* 4 (test
 * pages 400-403), with extensions only in 5 and 6 — so which of 4 to 7 are
 * extension carriers has to be read from the stream rather than assumed. The
 * signal is exact: a magazine carrying pages opens every one of them with an
 * X/0 header (§7.2.1), and an extension carrier, having no pages, never sends
 * one, and sends nothing but the six block numbers. A magazine 4-7 is therefore
 * taken to carry pages the moment an X/0 arrives in it, and to be an extension
 * carrier once it has sent a page's worth of block-numbered packets and nothing
 * else. Its packets are discarded until then, because an extension applied to a
 * page it was never meant for cannot be taken back — a squasher keeps every
 * copy — while ones discarded here come round again with the next cycle.
 *
 * A row that gets no extension shows spaces there, as it would on a receiver.
 *
 * Completed pages are delivered as Level 1 snapshots through the page
 * callback when the page transmission is terminated by the next page header
 * (§7.2.1) or by finalize(). When a subtitle page filter is set, subtitle
 * cues are additionally emitted per the C5/C6 conventions (§9.3.1.3): page
 * arrival displays the text, a header for the page with C4 (erase) set or C6
 * clear removes it.
 *
 * Error handling degrades gracefully: packets whose MRAG is uncorrectable
 * are dropped, headers whose page number is uncorrectable are dropped,
 * uncorrectable control nibbles fall back to zero, and display bytes failing
 * odd parity render as flagged SPACE cells.
 *
 * This component is deliberately stateful (page assembly spans many fields)
 * and therefore lives outside the stateless teletext observer.
 *
 * Thread safety: none; confine an instance to one thread.
 */
class TeletextPageDecoder {
 public:
  using PageCallback = std::function<void(const TeletextPageSnapshot&)>;

  TeletextPageDecoder();

  // Parse a page number string in the conventional magazine + two-hex-digit
  // form (e.g. "888", "100", "1F0"). Returns {displayed magazine 1-8, page
  // number 0x00-0xFF}, or std::nullopt when malformed.
  static std::optional<std::pair<int, int>> parse_page_number(
      std::string_view page);

  // Invoked for every completed page snapshot, in temporal order.
  void set_page_callback(PageCallback callback);

  // Enable subtitle cue emission for one page ("888"-style string, see
  // parse_page_number()). Returns false and leaves the filter unset when the
  // string is malformed.
  bool set_subtitle_page(std::string_view page);

  /**
   * @brief Attach a squasher so repeated copies of a row correct each other
   *
   * With a squasher attached, every displayable row packet is recorded into
   * it under the page identity the packet was attributed to, and rendered
   * pages are built from the squashed rows rather than from the last copy
   * received. Because the squasher outlives any one decoder, this also lets
   * a page be assembled from several partial transmissions — a page whose
   * transmission was clipped still renders from rows recovered earlier.
   *
   * A header with C4 (erase page) set advances the page's erase_epoch rather
   * than deleting the copies recorded before it: the earlier run stays
   * addressable for a consumer replaying the same stream, while this decoder
   * — which always keys on the current epoch — sees a clean page, exactly as
   * ETSI EN 300 706 §9.3.1.3 Table 2 requires.
   *
   * The squasher is not owned and must outlive the decoder. Pass nullptr to
   * detach. See teletext_row_squasher.h for the technique and its origin.
   */
  void set_row_squasher(TeletextRowSquasher* squasher) {
    row_squasher_ = squasher;
  }

  // Feed one T42 packet. |field_index| is the packet's temporal position in
  // fields; it must be monotonically non-decreasing across calls.
  //
  // |source| identifies this copy for a attached squasher: re-feeding the
  // same recovered line (as a sliding-window previewer does on every window
  // rebuild) must reuse its source so the copy is replaced rather than
  // counted again. The default derives a unique id per call, which is what a
  // one-pass consumer wants.
  //
  // |confidence| is how sure the recovery chain was of each byte, passed on to
  // an attached squasher so its vote can be weighted by it (see
  // teletext_row_squasher.h). nullptr — the default — means the caller cannot
  // say, and the copy votes at full weight.
  //
  // |packet_bytes| is how many of |packet| the service transmitted:
  // kTeletextPacketBytes on 625 lines, kTeletext525PacketBytes on 525 (the
  // byte_count of TeletextObservedPacket, or the packet_bytes of
  // TeletextLineResult). It sets how many display bytes a packet carries for
  // every page assembled from here on — a recording carries one service
  // throughout, and rows are rendered long after the packet that brought them,
  // so this has to be decoder state rather than something each stored row
  // carries. A short packet also enables row-extension decoding (see above).
  void process_packet(const std::array<uint8_t, kTeletextPacketBytes>& packet,
                      int64_t field_index, int64_t source = kAutoSource,
                      const TeletextPacketConfidence* confidence = nullptr,
                      size_t packet_bytes = kTeletextPacketBytes);

  /// Sentinel for process_packet()'s |source|: allocate a fresh copy id.
  static constexpr int64_t kAutoSource = -1;

  /**
   * @brief Page identity the last process_packet() call was attributed to
   *
   * Set for displayable row packets (X/1 to X/24) that belonged to an open
   * page, cleared otherwise. Lets a consumer rewriting a packet stream ask
   * the squasher for the corrected form of the row it just fed in.
   */
  const std::optional<TeletextPageKey>& last_row_attribution() const {
    return last_row_attribution_;
  }
  /// Display row the last packet carried, valid when the above is set
  int last_row_number() const { return last_row_number_; }

  // Flush open page assemblies and close any open subtitle cue at
  // |end_field_index|.
  void finalize(int64_t end_field_index);

  /**
   * @brief Snapshot every page whose transmission is currently in progress
   *
   * Renders the pages that are open right now without terminating them, so
   * decoding can carry on with the packets that follow. This is what lets a
   * consumer feeding the decoder incrementally show a page as it arrives:
   * finalize() would answer the same question, but it closes the pages, and
   * rows arriving afterwards would then be dropped as orphans.
   *
   * The returned snapshots all carry transmission_complete == false.
   */
  std::vector<TeletextPageSnapshot> open_page_snapshots() const;

  // Subtitle cues emitted so far (closed cues only; an open cue is closed by
  // finalize() or by the page's clear/replace events).
  const std::vector<TeletextSubtitleCue>& subtitle_cues() const {
    return subtitle_cues_;
  }

 private:
  // Raw stored bytes of one page row (7-bit codes, parity removed).
  struct RowData {
    // A packet carrying this row's own display bytes was received: columns 0
    // to head_columns_ are what it brought.
    bool present = false;
    // A row-extension packet covering this row was received: columns
    // head_columns_ to columns_ are what it brought. Tracked apart from
    // |present| because the two arrive in different packets and either can go
    // missing on a tape.
    bool extension_present = false;
    std::array<uint8_t, TeletextPageSnapshot::kColumns> characters{};
    std::array<bool, TeletextPageSnapshot::kColumns> parity_error{};
  };

  // Assembly state for one magazine. Row data is retained after a page is
  // emitted so a retransmission of the same page without C4 (erase) updates
  // rows incrementally (EN 300 706 §9.3.1.3 Table 2, C4).
  struct MagazineState {
    bool page_open = false;
    bool have_page = false;  // rows/identity below are meaningful
    int page_number = 0xFF;
    int subcode = 0;
    bool erase_page = false;
    bool newsflash = false;
    bool subtitle = false;
    bool suppress_header = false;
    bool update_indicator = false;
    bool interrupted_sequence = false;
    bool inhibit_display = false;
    bool magazine_serial = false;
    int national_option_subset = 0;
    int64_t header_field_index = 0;
    int64_t last_field_index = 0;
    std::array<RowData, TeletextPageSnapshot::kRows> rows{};
  };

  void handle_header_packet(
      int transmission_magazine,
      const std::array<uint8_t, kTeletextPacketBytes>& packet,
      int64_t field_index);
  void handle_display_packet(
      int transmission_magazine, int row,
      const std::array<uint8_t, kTeletextPacketBytes>& packet,
      int64_t field_index, int64_t source,
      const TeletextPacketConfidence* confidence);
  // A 525-line row-extension packet: |packet_number| identifies the block of
  // four rows whose columns head_columns_ to kColumns it carries (see the class
  // comment).
  void handle_extension_packet(
      int transmission_magazine, int packet_number,
      const std::array<uint8_t, kTeletextPacketBytes>& packet,
      int64_t field_index, int64_t source,
      const TeletextPacketConfidence* confidence);

  // A sub-page's identity without the erase epoch, which is what the epoch
  // counter below is keyed on: {displayed magazine, page number, sub-code}.
  using PageIdentity = std::array<int, 3>;

  // Identity of the page currently open in |magazine|, for squasher keying.
  TeletextPageKey page_key(int transmission_magazine) const;

  // Display columns of the page grid (see TeletextPageSnapshot::columns).
  int columns_ = TeletextPageSnapshot::kColumns;

  // Display bytes one packet of the service carries: the packet length passed
  // to process_packet() less the MRAG. Equal to columns_ on 625 lines; 32 of
  // the 40 on 525, the rest arriving in row-extension packets.
  int head_columns_ = TeletextPageSnapshot::kColumns;

  // Magazines seen to carry pages of their own, which for 4 to 7 is what says
  // they are not row-extension carriers (see the class comment). Indices 0 to 3
  // are unused: a service's own magazines are never read as carriers.
  std::array<bool, 8> magazine_carries_pages_{};

  // Consecutive block-numbered packets seen from magazines 4 to 7 while it is
  // still unsettled whether they carry pages, counted to the threshold that
  // decides it and reset by any packet numbered otherwise.
  std::array<int, 8> magazine_extension_evidence_{};

  // Erase epoch of |identity| (0 until its first C4 header).
  int erase_epoch(const PageIdentity& identity) const;

  // Emit the open page of |magazine| (if any) through the callback and the
  // subtitle machinery, then mark it closed (row data retained).
  //
  // |transmission_complete| is false when the page is being closed only
  // because its own header was re-sent mid-transmission: the rows keep
  // coming, so what is emitted is a fragment and is flagged as one.
  void terminate_page(int transmission_magazine,
                      bool transmission_complete = true);

  TeletextPageSnapshot render_snapshot(int transmission_magazine,
                                       const MagazineState& state) const;

  // Subtitle cue lifecycle (design §6: arrival = display, erase/C6-clear =
  // clear, changed text = replace).
  void subtitle_page_completed(const TeletextPageSnapshot& snapshot);
  void subtitle_clear_event(int64_t field_index);
  static std::string extract_subtitle_text(
      const TeletextPageSnapshot& snapshot);

  std::array<MagazineState, 8> magazines_{};

  // How many C4 (erase page) headers each sub-page has been given, which is
  // the erase_epoch of its TeletextPageKey. Kept per sub-page rather than per
  // magazine so erasing one page does not orphan the copies of another page
  // carried in the same magazine. One int per distinct sub-page seen.
  std::map<PageIdentity, int> erase_epochs_;

  // Not owned; see set_row_squasher().
  TeletextRowSquasher* row_squasher_ = nullptr;
  int64_t next_source_ = 0;
  std::optional<TeletextPageKey> last_row_attribution_;
  int last_row_number_ = 0;

  PageCallback page_callback_;

  // Subtitle filter (displayed magazine 1-8 + page number) and cue state.
  std::optional<std::pair<int, int>> subtitle_filter_;
  std::vector<TeletextSubtitleCue> subtitle_cues_;
  std::optional<TeletextSubtitleCue> open_cue_;
};

}  // namespace orc

#endif  // ORC_TELETEXT_PAGE_DECODER_H
