/*
 * File:        teletext_page_decoder.cpp
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     WST (System B) teletext magazine/page decoder producing
 *              Level 1 page snapshots and subtitle cues
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_page_decoder.h"

#include <algorithm>
#include <utility>

namespace orc {

namespace {

// ETSI EN 300 706 §7.1.2 / ITU-R BT.653 Table 1b §3.3: every packet opens with
// the two Hamming 8/4 magazine-and-row address bytes, so the display bytes of
// a row start after them on either service.
constexpr int kMragBytes = 2;

// ETSI EN 300 706 §9.3.1: a page header (X/0) spends its first ten bytes on
// addressing and control — the MRAG, the two page-number bytes, the four
// sub-code bytes and the two control-bit bytes. The header text follows, and
// is displayed from column 8: columns 0-7 carry the receiver's own page number
// and clock.
constexpr size_t kHeaderControlBytes = 10;
constexpr int kHeaderTextColumn = 8;

// Magazine address bit that marks a 525-line row-extension packet (see the
// TeletextPageDecoder class comment): the service uses magazines 8, 1, 2 and 3
// for its pages and addresses each one's extension packets to that magazine
// with this bit set. Read only when the service's packets are short, so a
// 625-line stream's magazines 4-7 are untouched.
constexpr int kExtensionMagazineFlag = 0x4;

// Rows one extension packet serves: its display bytes are that many equal
// groups, one per row, in ascending row order.
constexpr int kExtensionPacketRows = 4;

// Packet numbers the row-extension scheme uses: one per block of four display
// rows, which is six for the 24 rows of a page (see extension_first_row()).
bool is_extension_block_number(int packet_number) {
  return packet_number == 1 ||
         (packet_number >= kExtensionPacketRows &&
          packet_number <= TeletextPageSnapshot::kRows - kExtensionPacketRows &&
          packet_number % kExtensionPacketRows == 0);
}

// Consecutive block-numbered packets a magazine 4-7 must send, without a header
// and without a packet numbered anything else, before it is taken to carry row
// extensions rather than pages of its own. One page's worth: a magazine sending
// only these six numbers, over and over, is not transmitting pages.
constexpr int kExtensionEvidencePackets = 6;

// First display row an extension packet numbered |packet_number| serves. The
// number identifies a block of kExtensionPacketRows rows rather than naming a
// row, so it rounds down: the observed packets are numbered 1, 4, 8, 12, 16 and
// 20 and serve rows 0-3, 4-7, 8-11, 12-15, 16-19 and 20-23. The first block
// cannot be numbered 0 — that is the page header's packet number — which is why
// its packets carry 1.
int extension_first_row(int packet_number) {
  return (packet_number / kExtensionPacketRows) * kExtensionPacketRows;
}

// ETSI EN 300 706 §9.3.1.1: page number 0xFF marks time-filling / page
// terminating headers ("null page" convention, §7.2.2 with sub-code 3F7F);
// such headers terminate a transmission but never open a page.
constexpr int kTimeFillingPageNumber = 0xFF;

// Spacing-attribute codes of ETSI EN 300 706 §12.2 Table 26 (7-bit values).
constexpr uint8_t kAlphaColourBase = 0x00;   // 0/0-0/7 (0/0 no Level 1 action)
constexpr uint8_t kFlash = 0x08;             // 0/8
constexpr uint8_t kSteady = 0x09;            // 0/9
constexpr uint8_t kEndBox = 0x0A;            // 0/A
constexpr uint8_t kStartBox = 0x0B;          // 0/B
constexpr uint8_t kNormalSize = 0x0C;        // 0/C
constexpr uint8_t kDoubleHeight = 0x0D;      // 0/D
constexpr uint8_t kMosaicColourBase = 0x10;  // 1/0-1/7 (1/0 no Level 1 action)
constexpr uint8_t kConceal = 0x18;           // 1/8
constexpr uint8_t kContiguousMosaic = 0x19;  // 1/9
constexpr uint8_t kSeparatedMosaic = 0x1A;   // 1/A
constexpr uint8_t kBlackBackground = 0x1C;   // 1/C
constexpr uint8_t kNewBackground = 0x1D;     // 1/D
constexpr uint8_t kHoldMosaics = 0x1E;       // 1/E
constexpr uint8_t kReleaseMosaics = 0x1F;    // 1/F

int hex_digit_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Transmission magazine 0 is displayed as magazine 8 (ETSI EN 300 706 §3.1:
// page 800-8FF carries magazine address 0).
int displayed_magazine(int transmission_magazine) {
  return transmission_magazine == 0 ? 8 : transmission_magazine;
}

// The thirteen G0 positions a national option sub-set replaces
// (ETSI EN 300 706 §15.6.1 Table 35 NOTE 2 — the shaded positions).
constexpr std::array<uint8_t, 13> kNationalOptionPositions = {
    0x23, 0x24, 0x40, 0x5B, 0x5C, 0x5D, 0x5E,
    0x5F, 0x60, 0x7B, 0x7C, 0x7D, 0x7E};

// ETSI EN 300 706 §15.6.2 Table 36, the seven sub-sets a Level 1 page can
// reach through C12-C14, in TeletextNationalOption order. Columns follow
// kNationalOptionPositions.
//
// Two glyphs the standard draws rather than names take the nearest Unicode:
// English 6/0 is a horizontal line across the character rectangle (em dash
// here) and 7/C a double vertical line (U+2016). Note that the sub-sets which
// take 2/3 for a currency or accented character put the '#' the primary set
// holds there at 5/F instead, rather than dropping it.
constexpr std::array<std::array<char32_t, 13>, 7> kNationalOptionSubsets = {{
    // English
    {U'£', U'$', U'@', U'←', U'½', U'→', U'↑', U'#', U'—', U'¼', U'‖', U'¾',
     U'÷'},
    // German
    {U'#', U'$', U'§', U'Ä', U'Ö', U'Ü', U'^', U'_', U'°', U'ä', U'ö', U'ü',
     U'ß'},
    // Swedish/Finnish/Hungarian
    {U'#', U'¤', U'É', U'Ä', U'Ö', U'Å', U'Ü', U'_', U'é', U'ä', U'ö', U'å',
     U'ü'},
    // Italian
    {U'£', U'$', U'é', U'°', U'ç', U'→', U'↑', U'#', U'ù', U'à', U'ò', U'è',
     U'ì'},
    // French
    {U'é', U'ï', U'à', U'ë', U'ê', U'ù', U'î', U'#', U'è', U'â', U'ô', U'û',
     U'ç'},
    // Portuguese/Spanish
    {U'ç', U'$', U'¡', U'á', U'é', U'í', U'ó', U'ú', U'¿', U'ü', U'ñ', U'è',
     U'à'},
    // Czech/Slovak
    {U'#', U'ů', U'č', U'ť', U'ž', U'ý', U'í', U'ř', U'é', U'á', U'ě', U'ú',
     U'š'},
}};

// The three Cyrillic G0 primary sets, ETSI EN 300 706 §15.6.4 to §15.6.6
// Tables 38, 39 and 40, as the 96 code points of positions 2/0 to 7/F in
// ascending code order.
//
// Unlike the Latin set these reserve no positions for national option
// sub-sets: each is a complete alphabet in its own right, chosen by the
// character set *designation* rather than by the header's C12-C14 bits (§15.2
// Table 32, designation 0100). Every one of them therefore has to be written
// out in full — the letters are not an ASCII-compatible permutation of
// anything, and only positions 2/0-2/5, 2/7-2/9, 2/B-3/F agree with Latin.
//
// Common to all three (each table's NOTE 1 to NOTE 3):
//   - 2/0 is SPACE;
//   - 2/A is the asterisk, replaced by '@' only when the set is reached
//     through a packet X/26 column address triplet, which a Level 1 page has
//     no way to send;
//   - 7/F is a filled rectangle, as it is in the Latin set.

// Table 38 — Option 1, Serbian/Croatian. The Macedonian Ѓ/Ќ at 5/7 and 5/1
// come with it. The block at 7/F takes the position lowercase џ would have
// held, so this set has Џ and no џ; that is the table as printed, not an
// omission here.
constexpr std::array<char32_t, 96> kCyrillic1G0 = {
    /* 2/0 */ U' ', U'!', U'"', U'#', U'$', U'%', U'&', U'\'',
    /* 2/8 */ U'(', U')', U'*', U'+', U',', U'-', U'.', U'/',
    /* 3/0 */ U'0', U'1', U'2', U'3', U'4', U'5', U'6', U'7',
    /* 3/8 */ U'8', U'9', U':', U';', U'<', U'=', U'>', U'?',
    /* 4/0 */ U'Ч', U'А', U'Б', U'Ц', U'Д', U'Е', U'Ф', U'Г',
    /* 4/8 */ U'Х', U'И', U'Ј', U'К', U'Л', U'М', U'Н', U'О',
    /* 5/0 */ U'П', U'Ќ', U'Р', U'С', U'Т', U'У', U'В', U'Ѓ',
    /* 5/8 */ U'Љ', U'Њ', U'З', U'Ћ', U'Ж', U'Ђ', U'Ш', U'Џ',
    /* 6/0 */ U'ч', U'а', U'б', U'ц', U'д', U'е', U'ф', U'г',
    /* 6/8 */ U'х', U'и', U'ј', U'к', U'л', U'м', U'н', U'о',
    /* 7/0 */ U'п', U'ќ', U'р', U'с', U'т', U'у', U'в', U'ѓ',
    /* 7/8 */ U'љ', U'њ', U'з', U'ћ', U'ж', U'ђ', U'ш', U'■',
};

// Table 39 — Option 2, Russian/Bulgarian. The alphabet is laid out as KOI-7
// lays it out, except that Ъ and Ы are the other way round (5/9 and 5/F) and
// the block at 7/F displaces lowercase ы to 2/6, where the Latin set has '&'.
constexpr std::array<char32_t, 96> kCyrillic2G0 = {
    /* 2/0 */ U' ', U'!', U'"', U'#', U'$', U'%', U'ы', U'\'',
    /* 2/8 */ U'(', U')', U'*', U'+', U',', U'-', U'.', U'/',
    /* 3/0 */ U'0', U'1', U'2', U'3', U'4', U'5', U'6', U'7',
    /* 3/8 */ U'8', U'9', U':', U';', U'<', U'=', U'>', U'?',
    /* 4/0 */ U'Ю', U'А', U'Б', U'Ц', U'Д', U'Е', U'Ф', U'Г',
    /* 4/8 */ U'Х', U'И', U'Й', U'К', U'Л', U'М', U'Н', U'О',
    /* 5/0 */ U'П', U'Я', U'Р', U'С', U'Т', U'У', U'Ж', U'В',
    /* 5/8 */ U'Ь', U'Ъ', U'З', U'Ш', U'Э', U'Щ', U'Ч', U'Ы',
    /* 6/0 */ U'ю', U'а', U'б', U'ц', U'д', U'е', U'ф', U'г',
    /* 6/8 */ U'х', U'и', U'й', U'к', U'л', U'м', U'н', U'о',
    /* 7/0 */ U'п', U'я', U'р', U'с', U'т', U'у', U'ж', U'в',
    /* 7/8 */ U'ь', U'ъ', U'з', U'ш', U'э', U'щ', U'ч', U'■',
};

// Table 40 — Option 3, Ukrainian. Option 2's layout with the letters Ukrainian
// does not use replaced by the ones it does: І at 5/9 for Ъ, Є at 5/C for Э,
// Ї at 5/F for Ы, and ї at 2/6 for ы.
constexpr std::array<char32_t, 96> kCyrillic3G0 = {
    /* 2/0 */ U' ', U'!', U'"', U'#', U'$', U'%', U'ї', U'\'',
    /* 2/8 */ U'(', U')', U'*', U'+', U',', U'-', U'.', U'/',
    /* 3/0 */ U'0', U'1', U'2', U'3', U'4', U'5', U'6', U'7',
    /* 3/8 */ U'8', U'9', U':', U';', U'<', U'=', U'>', U'?',
    /* 4/0 */ U'Ю', U'А', U'Б', U'Ц', U'Д', U'Е', U'Ф', U'Г',
    /* 4/8 */ U'Х', U'И', U'Й', U'К', U'Л', U'М', U'Н', U'О',
    /* 5/0 */ U'П', U'Я', U'Р', U'С', U'Т', U'У', U'Ж', U'В',
    /* 5/8 */ U'Ь', U'І', U'З', U'Ш', U'Є', U'Щ', U'Ч', U'Ї',
    /* 6/0 */ U'ю', U'а', U'б', U'ц', U'д', U'е', U'ф', U'г',
    /* 6/8 */ U'х', U'и', U'й', U'к', U'л', U'м', U'н', U'о',
    /* 7/0 */ U'п', U'я', U'р', U'с', U'т', U'у', U'ж', U'в',
    /* 7/8 */ U'ь', U'і', U'з', U'ш', U'є', U'щ', U'ч', U'■',
};

// Lowest G0 code the tables above start at: everything below 2/0 is a spacing
// attribute rather than a character (§15.5).
constexpr uint8_t kFirstG0Code = 0x20;

const std::array<char32_t, 96>* cyrillic_table(TeletextG0Set g0_set) {
  switch (g0_set) {
    case TeletextG0Set::Cyrillic1:
      return &kCyrillic1G0;
    case TeletextG0Set::Cyrillic2:
      return &kCyrillic2G0;
    case TeletextG0Set::Cyrillic3:
      return &kCyrillic3G0;
    case TeletextG0Set::Latin:
      break;
  }
  return nullptr;
}

// The G0 set names the parameter surface and the project file use. They name
// the languages rather than the standard's option numbers, because "Cyrillic
// option 2" tells a user nothing about whether it is the one their recording
// needs.
struct G0SetName {
  TeletextG0Set set;
  const char* name;
};

constexpr std::array<G0SetName, 4> kG0SetNames = {{
    {TeletextG0Set::Latin, "Latin"},
    {TeletextG0Set::Cyrillic1, "Cyrillic (Serbian/Croatian)"},
    {TeletextG0Set::Cyrillic2, "Cyrillic (Russian/Bulgarian)"},
    {TeletextG0Set::Cyrillic3, "Cyrillic (Ukrainian)"},
}};

// ETSI EN 300 706 §15.2 Table 32: the G0 set a *Default G0 and G2 Character
// Set Designation and National Option Selection* value selects. |designation|
// is triplet 1 bits 14-11 and |national_option| bits 10-8.
//
// Only the Cyrillic rows are distinguished. Every other designation carries a
// Latin G0 set — including the four Latin rows of designation 0100 itself —
// and the national option sub-set of a Latin page keeps coming from the
// header's C12-C14 bits, as it does at Level 1 (§15.2). The Latin sub-sets
// that only some designations reach (Polish, Turkish, Estonian,
// Lettish/Lithuanian, Romanian) are not modelled; such a page displays in the
// designation-0000 sub-set its header bits name, which is what it did before.
TeletextG0Set g0_set_for_designation(int designation, int national_option) {
  if (designation == 0b0100) {
    switch (national_option) {
      case 0b000:
        return TeletextG0Set::Cyrillic1;
      case 0b100:
        return TeletextG0Set::Cyrillic2;
      case 0b101:
        return TeletextG0Set::Cyrillic3;
      default:
        break;
    }
  }
  return TeletextG0Set::Latin;
}

// Designation codes and packet numbers of the two packets that can carry a
// character set designation (§9.1 Table 1, §9.4.2, §9.5).
constexpr int kPacketX28 = 28;
constexpr int kPacketM29 = 29;
constexpr int kDesignationCodeZero = 0;

// X/28/0 carries Format 1 — the format Table 4 codes, and the only one with a
// character set designation in it — when triplet 1's Page Function bits say
// the page is a basic Level 1 teletext page (§9.4.2.1 Table 3, code 0000).
constexpr int kPageFunctionBasicLevel1 = 0;

// The last displayable row, X/24 — the FLOF label row of §9.6.1, which sits at
// the boundary between the display rows and the non-display packets X/25 to
// X/31 and so collects their mis-corrections (see accept_last_display_row()).
constexpr int kLastDisplayRow = TeletextPageSnapshot::kRows - 1;

// Whether a Hamming 8/4 byte arrived as a codeword rather than being corrected
// into one. §8.2 gives the code minimum distance 4: one error is corrected and
// two are detected, so a byte that needed correction is one channel error away
// from having been mis-corrected into a neighbouring codeword instead.
bool hamming84_uncorrected(uint8_t byte, int decoded) {
  return teletext_hamming84_encode(static_cast<uint8_t>(decoded)) == byte;
}

// Whether a packet addressed to the last display row may be believed.
//
// Row 24 is the one display address a corrected MRAG must not be trusted for.
// Its neighbours X/25 to X/31 are non-display packets that carry no odd parity
// and are transmitted continuously, and the MRAG is Hamming 8/4 — distance 4,
// so a two-bit burst is not detected but silently resolved to the neighbouring
// codeword. Row 24 is where those land.
//
// What makes it show is that almost no page uses row 24. A service transmits
// FLOF labels on a handful of pages and leaves the row empty on the rest
// (the reference SECAM capture: 7 pages of 89). Rows 1-23 are re-sent with
// every cycle of the page and the squasher votes an intruder down among five
// hundred copies; row 24 has nothing to vote with, so the single packet that
// arrives is the row, and the slicer's parity repair hands it over with no
// damage to report.
//
// The measurement that sets the rule, over that capture's 228 839 packets:
// rows X/1-23 arrive with both MRAG bytes uncorrected 78.4% of the time, and
// X/24 on the pages that really carry FLOF labels 82.8% — but X/24 on every
// other page only 29.8%. Genuine traffic sits at the baseline; the deficit is
// packets that could only ever reach row 24 by being corrected into it.
// Requiring both bytes as transmitted costs about one row 24 in six on the
// pages that have one — a row re-sent tens of times, so nothing is lost — and
// removes about seven in ten of the intruders.
bool accept_last_display_row(
    const std::array<uint8_t, kTeletextPacketBytes>& packet, int mrag_low,
    int mrag_high) {
  return hamming84_uncorrected(packet[0], mrag_low) &&
         hamming84_uncorrected(packet[1], mrag_high);
}

}  // namespace

bool teletext_odd_parity_valid(uint8_t byte) {
  // ETSI EN 300 706 §8.1: accept when D1..D7 ⊕ P = 1 (odd number of set
  // bits over the whole byte).
  int ones = 0;
  for (int bit = 0; bit < 8; ++bit) {
    ones += (byte >> bit) & 1;
  }
  return (ones % 2) == 1;
}

uint8_t teletext_odd_parity_encode(uint8_t value) {
  uint8_t byte = value & 0x7F;
  int ones = 0;
  for (int bit = 0; bit < 7; ++bit) {
    ones += (byte >> bit) & 1;
  }
  // ETSI EN 300 706 §8.1: P = 1 ⊕ D1 ⊕ ... ⊕ D7.
  if ((ones % 2) == 0) {
    byte |= 0x80;
  }
  return byte;
}

std::string to_string(TeletextG0Set g0_set) {
  for (const G0SetName& entry : kG0SetNames) {
    if (entry.set == g0_set) {
      return entry.name;
    }
  }
  return kG0SetNames.front().name;
}

std::optional<TeletextG0Set> teletext_g0_set_from_string(
    std::string_view name) {
  for (const G0SetName& entry : kG0SetNames) {
    if (name == entry.name) {
      return entry.set;
    }
  }
  return std::nullopt;
}

char32_t teletext_g0_to_unicode(uint8_t code, TeletextG0Set g0_set,
                                int national_option_subset) {
  const uint8_t c = static_cast<uint8_t>(code & 0x7F);
  // Codes 0/0-1/F are spacing attributes, not characters (§15.5).
  if (c < kFirstG0Code) {
    return U' ';
  }

  // A Cyrillic set defines all 96 of its positions, so it is a lookup and the
  // national option sub-set has no part in it.
  if (const std::array<char32_t, 96>* table = cyrillic_table(g0_set);
      table != nullptr) {
    return (*table)[static_cast<size_t>(c - kFirstG0Code)];
  }

  // §15.6.1 Table 35 NOTE 4: 7/F is a rectangle filling the character area.
  if (c == 0x7F) {
    return U'■';
  }
  const size_t subset =
      national_option_subset >= 0 &&
              static_cast<size_t>(national_option_subset) <
                  kNationalOptionSubsets.size()
          ? static_cast<size_t>(national_option_subset)
          : 0;  // Table 32 designates no sub-set for 1 1 1; render as English
  for (size_t i = 0; i < kNationalOptionPositions.size(); ++i) {
    if (kNationalOptionPositions[i] == c) {
      return kNationalOptionSubsets[subset][i];
    }
  }
  // Every remaining Table 35 position coincides with ASCII.
  return static_cast<char32_t>(c);
}

std::string teletext_g0_to_utf8(uint8_t code, TeletextG0Set g0_set,
                                int national_option_subset) {
  const char32_t cp =
      teletext_g0_to_unicode(code, g0_set, national_option_subset);
  std::string out;
  // Table 36 reaches U+2016 at most, so two continuation bytes suffice; the
  // three-byte branch is written out anyway rather than assuming that.
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

TeletextPageDecoder::TeletextPageDecoder() = default;

std::optional<std::pair<int, int>> TeletextPageDecoder::parse_page_number(
    std::string_view page) {
  if (page.size() != 3) {
    return std::nullopt;
  }
  const char magazine_char = page[0];
  if (magazine_char < '1' || magazine_char > '8') {
    return std::nullopt;
  }
  const int tens = hex_digit_value(page[1]);
  const int units = hex_digit_value(page[2]);
  if (tens < 0 || units < 0) {
    return std::nullopt;
  }
  return std::make_pair(magazine_char - '0', (tens << 4) | units);
}

void TeletextPageDecoder::set_page_callback(PageCallback callback) {
  page_callback_ = std::move(callback);
}

bool TeletextPageDecoder::set_subtitle_page(std::string_view page) {
  const auto parsed = parse_page_number(page);
  if (!parsed.has_value()) {
    return false;
  }
  subtitle_filter_ = parsed;
  return true;
}

void TeletextPageDecoder::process_packet(
    const std::array<uint8_t, kTeletextPacketBytes>& packet,
    int64_t field_index, int64_t source,
    const TeletextPacketConfidence* confidence, size_t packet_bytes) {
  last_row_attribution_.reset();
  last_row_number_ = 0;

  // Display bytes one packet of this service carries: 42 bytes less the MRAG
  // gives the 40 of EN 300 706 §9.3.2, 34 gives the 32 of ITU-R BT.653
  // Table 1b §3.4. The grid stays 40 columns either way: what a short packet
  // does not fill comes from the row-extension packets, or shows as spaces.
  head_columns_ = std::clamp(static_cast<int>(packet_bytes) - kMragBytes, 0,
                             TeletextPageSnapshot::kColumns);

  // MRAG: two Hamming 8/4 bytes carrying the 3-bit magazine and 5-bit packet
  // number (ETSI EN 300 706 §7.1.2). An uncorrectable MRAG byte means the
  // packet cannot be attributed; drop it.
  const int mrag_low = teletext_hamming84_decode(packet[0]);
  const int mrag_high = teletext_hamming84_decode(packet[1]);
  if (mrag_low < 0 || mrag_high < 0) {
    return;
  }
  const int magazine = mrag_low & 0x7;
  const int packet_number = ((mrag_low >> 3) & 0x1) | (mrag_high << 1);

  if (head_columns_ < TeletextPageSnapshot::kColumns &&
      (magazine & kExtensionMagazineFlag) != 0) {
    // A magazine 4-7 either carries pages of its own or carries the row
    // extensions of magazine M&3, and an X/0 header is what tells the two apart
    // (see the class comment). An extension carrier never sends one, so a
    // header here is both a page and the evidence that this magazine has them.
    const auto index = static_cast<size_t>(magazine);
    const bool settled =
        magazine_extension_evidence_[index] >= kExtensionEvidencePackets;
    // An X/0 says this magazine has pages of its own — but only while that is
    // still an open question, and only from a header good enough to open one.
    // A magazine already shown to carry extensions does not acquire pages, and
    // reading one mis-corrected address as though it had would disable its
    // extensions for the rest of the recording.
    if (!settled && packet_number == 0 &&
        teletext_hamming84_decode(packet[2]) >= 0 &&
        teletext_hamming84_decode(packet[3]) >= 0) {
      magazine_carries_pages_[index] = true;
    }
    if (!magazine_carries_pages_[index]) {
      if (!settled) {
        // Still deciding. A carrier sends nothing but block numbers; anything
        // else is a display row, so the evidence starts again — a page
        // magazine's rows include the block numbers, which is why one of those
        // alone proves nothing, and only an unbroken run of them settles it.
        //
        // Until it is settled the packets are discarded rather than guessed at:
        // a row extension written to the wrong page stays in the record, since
        // a squasher keeps every copy it is given, while one discarded here
        // comes round again with the page.
        if (is_extension_block_number(packet_number)) {
          ++magazine_extension_evidence_[index];
        } else {
          magazine_extension_evidence_[index] = 0;
        }
        return;
      }
      // Settled, and it stays settled. A magazine that has shown it carries no
      // pages does not acquire them, so a packet numbered anything but a block
      // is a mis-read address (the MRAG is Hamming 8/4, which mis-corrects on a
      // burst) — dropped, where before it restarted the evidence and cost the
      // six packets that followed it, which is a whole page's extensions.
      if (is_extension_block_number(packet_number)) {
        handle_extension_packet(magazine & ~kExtensionMagazineFlag,
                                packet_number, packet, field_index,
                                source == kAutoSource ? next_source_++ : source,
                                confidence);
      }
      return;
    }
  }

  if (packet_number == 0) {
    handle_header_packet(magazine, packet, field_index);
  } else if (packet_number >= 1 &&
             packet_number < TeletextPageSnapshot::kRows) {
    // X/1 to X/24: directly displayable rows (EN 300 706 §9.3.2). X/25
    // (key-word search labels) and X/26, X/27 and X/30-X/31 (enhancement,
    // editorial linking and independent data, §9.4-§9.8) are outside the
    // Level 1 grid and are ignored.
    if (packet_number == kLastDisplayRow &&
        !accept_last_display_row(packet, mrag_low, mrag_high)) {
      return;
    }
    handle_display_packet(magazine, packet_number, packet, field_index,
                          source == kAutoSource ? next_source_++ : source,
                          confidence);
  } else if (packet_number == kPacketX28 || packet_number == kPacketM29) {
    // Not display data, but the one thing outside the Level 1 grid that
    // changes how the grid is read: which G0 set its codes are in (§15.2).
    handle_character_set_designation(magazine, packet,
                                     packet_number == kPacketM29);
  }
}

void TeletextPageDecoder::handle_character_set_designation(
    int transmission_magazine,
    const std::array<uint8_t, kTeletextPacketBytes>& packet,
    bool magazine_wide) {
  // Byte 6 of the packet — index 2 here, the MRAG being bytes 4 and 5 — is the
  // designation code, Hamming 8/4 (§9.4.1 figure 11). Only designation 0
  // carries a character set designation in the bits below; X/28/1 to X/28/4
  // and M/29/1 to M/29/4 code other things at other positions, so anything
  // else is left alone rather than guessed at.
  const int designation_code = teletext_hamming84_decode(packet[2]);
  if (designation_code != kDesignationCodeZero) {
    return;
  }

  // Before believing anything the packet says, require the whole of it to be
  // what it claims: §9.4.1 codes all 39 bytes after the designation code as
  // thirteen Hamming 24/18 triplets, so on a genuine packet every one of them
  // decodes and on a damaged one the drop costs nothing — it is re-sent with
  // every cycle of the page.
  //
  // This is not pedantry but the noise gate. On a tape recovery the MRAG's
  // Hamming 8/4 mis-corrects bursts into valid addresses, and a long capture
  // yields hundreds of garbage packets numbered 28 or 29 (the reference SECAM
  // capture: 246 over 80 677 frames). A random triplet passes 24/18 about 39%
  // of the time, so gating on triplet 1 alone let roughly one in forty of
  // those through — and one fake M/29/0 re-designates a whole magazine for
  // the rest of the recording, which on the reference capture silently undid
  // the configured Cyrillic set. All thirteen triplets at once is a pass rate
  // of about 5 in a million, and none of the observed fakes decodes more than
  // seven.
  for (size_t byte = 3; byte + 2 < kTeletextPacketBytes; byte += 3) {
    if (teletext_hamming2418_decode(packet[byte], packet[byte + 1],
                                    packet[byte + 2]) < 0) {
      return;
    }
  }

  // Triplet 1 is bytes 7 to 9 (indices 3 to 5), Hamming 24/18. An
  // uncorrectable triplet is dropped: a mis-read designation would change the
  // alphabet of every page in the magazine, and the packet is re-sent with the
  // page.
  const int32_t triplet =
      teletext_hamming2418_decode(packet[3], packet[4], packet[5]);
  if (triplet < 0) {
    return;
  }

  // X/28/0 is Format 1 — the format that carries a designation — only when its
  // Page Function says a basic Level 1 page (§9.4.2.1 Table 3). M/29/0 carries
  // no page function at all (§9.1 Table 1: "the same functions apart from page
  // function and coding"), so its triplet 1 bits 1-7 are not read.
  if (!magazine_wide && (triplet & 0xF) != kPageFunctionBasicLevel1) {
    return;
  }

  // Triplet 1 bits 8-14: the 7-bit Table 32 value, four designation bits
  // (14-11) over three national option bits (10-8).
  const int value = (triplet >> 7) & 0x7F;
  const TeletextG0Set g0_set =
      g0_set_for_designation((value >> 3) & 0xF, value & 0x7);

  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  if (magazine_wide) {
    // M/29/0 sets the magazine's default. It does not reach back into a page
    // already open: that page's rows were addressed under the set it was
    // opened with, and §15.2 gives the page's own X/28/0 the higher priority
    // anyway.
    state.magazine_g0_set = g0_set;
    return;
  }
  // X/28/0 belongs to the page it was transmitted with, so it applies to the
  // open page immediately — it may well arrive after some of the page's rows,
  // and the page is only rendered when it terminates.
  state.g0_set = g0_set;
}

int TeletextPageDecoder::erase_epoch(const PageIdentity& identity) const {
  const auto it = erase_epochs_.find(identity);
  return it == erase_epochs_.end() ? 0 : it->second;
}

TeletextPageKey TeletextPageDecoder::page_key(int transmission_magazine) const {
  const MagazineState& state =
      magazines_[static_cast<size_t>(transmission_magazine)];
  const int magazine = displayed_magazine(transmission_magazine);
  return TeletextPageKey{
      magazine, state.page_number, state.subcode,
      erase_epoch({magazine, state.page_number, state.subcode})};
}

void TeletextPageDecoder::handle_header_packet(
    int transmission_magazine,
    const std::array<uint8_t, kTeletextPacketBytes>& packet,
    int64_t field_index) {
  // Page number: bytes 6-7 of the transmission packet = packet bytes 2-3
  // (EN 300 706 §9.3.1.1). Without a correctable page number the header
  // cannot be attributed to a page; drop it entirely.
  const int units = teletext_hamming84_decode(packet[2]);
  const int tens = teletext_hamming84_decode(packet[3]);
  if (units < 0 || tens < 0) {
    return;
  }
  const int page_number = (tens << 4) | units;

  // Sub-code and control bits (EN 300 706 §9.3.1.2, §9.3.1.3 Table 2).
  // Uncorrectable nibbles degrade to zero rather than dropping the header.
  const int s1_raw = teletext_hamming84_decode(packet[4]);
  const int s2_c4_raw = teletext_hamming84_decode(packet[5]);
  const int s3_raw = teletext_hamming84_decode(packet[6]);
  const int s4_c5_c6_raw = teletext_hamming84_decode(packet[7]);
  const int c7_c10_raw = teletext_hamming84_decode(packet[8]);
  const int c11_c14_raw = teletext_hamming84_decode(packet[9]);
  const int s1 = s1_raw < 0 ? 0 : s1_raw;
  const int s2_c4 = s2_c4_raw < 0 ? 0 : s2_c4_raw;
  const int s3 = s3_raw < 0 ? 0 : s3_raw;
  const int s4_c5_c6 = s4_c5_c6_raw < 0 ? 0 : s4_c5_c6_raw;
  const int c7_c10 = c7_c10_raw < 0 ? 0 : c7_c10_raw;
  const int c11_c14 = c11_c14_raw < 0 ? 0 : c11_c14_raw;

  const int subcode =
      s1 | ((s2_c4 & 0x7) << 4) | (s3 << 7) | ((s4_c5_c6 & 0x3) << 11);
  const bool erase_page = (s2_c4 & 0x8) != 0;         // C4
  const bool newsflash = (s4_c5_c6 & 0x4) != 0;       // C5
  const bool subtitle = (s4_c5_c6 & 0x8) != 0;        // C6
  const bool magazine_serial = (c11_c14 & 0x1) != 0;  // C11

  // A service may re-send a page's header while the page's rows are still
  // being transmitted (a rolling header keeps the on-screen clock live,
  // EN 300 706 §9.3.1.4). That header closes the assembly like any other,
  // but the transmission it closes has not finished — the same page simply
  // reopens and its remaining rows follow. Recognising this is what stops a
  // consumer counting one appearance of a page as several, and what lets it
  // tell a fragment from a finished page.
  const MagazineState& open_state =
      magazines_[static_cast<size_t>(transmission_magazine)];
  const bool same_page_continues =
      open_state.page_open && open_state.have_page && !erase_page &&
      open_state.page_number == page_number && open_state.subcode == subcode;

  // A page header terminates the page currently being transmitted: in serial
  // mode (C11 set) any magazine's page, in parallel mode only the page of
  // the same magazine (EN 300 706 §7.2.1).
  if (magazine_serial) {
    for (int m = 0; m < static_cast<int>(magazines_.size()); ++m) {
      terminate_page(m, m != transmission_magazine || !same_page_continues);
    }
  } else {
    terminate_page(transmission_magazine, !same_page_continues);
  }

  // Subtitle clear events act on header arrival: a header for the watched
  // page with C4 (erase) set, or with C6 no longer set, removes the display
  // (design §6; EN 300 706 §9.3.1.3 Table 2).
  if (subtitle_filter_.has_value() &&
      displayed_magazine(transmission_magazine) == subtitle_filter_->first &&
      page_number == subtitle_filter_->second && (erase_page || !subtitle)) {
    subtitle_clear_event(field_index);
  }

  // Time-filling / terminating headers never open a page (EN 300 706 §7.3).
  if (page_number == kTimeFillingPageNumber) {
    return;
  }

  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];

  // Retain stored rows only for a retransmission of the same page and
  // sub-code without C4 (erase); a different page, a different sub-code
  // (sub-page replacement), or an erase starts from a clean grid
  // (EN 300 706 §9.3.1.3 Table 2, C4).
  const bool retain_rows = state.have_page && !erase_page &&
                           state.page_number == page_number &&
                           state.subcode == subcode;
  if (!retain_rows) {
    state.rows = {};
  }

  state.page_open = true;
  state.have_page = true;
  state.page_number = page_number;
  state.subcode = subcode;
  state.erase_page = erase_page;

  // C4 replaces the page's content rather than updating it, so copies of its
  // rows recorded so far describe a page that no longer exists; combining them
  // with what follows would blend the old page into the new one. Advancing the
  // epoch moves the page to a fresh set of buckets, which separates the two
  // runs without discarding the first: a consumer replaying this stream feeds
  // the same packets in the same order, so it arrives at the same epoch at the
  // same point and can still ask about the rows that came before.
  if (erase_page) {
    ++erase_epochs_[{displayed_magazine(transmission_magazine), page_number,
                     subcode}];
  }
  state.newsflash = newsflash;
  state.subtitle = subtitle;
  state.suppress_header = (c7_c10 & 0x1) != 0;       // C7
  state.update_indicator = (c7_c10 & 0x2) != 0;      // C8
  state.interrupted_sequence = (c7_c10 & 0x4) != 0;  // C9
  state.inhibit_display = (c7_c10 & 0x8) != 0;       // C10
  state.magazine_serial = magazine_serial;           // C11
  // C12-C14. Byte 13 carries C11 in bit 2 and C12, C13, C14 in bits 4, 6 and 8
  // (§9.3.1.3 Table 2), which Hamming 8/4 delivers as D1-D4 in bits 0-3 — so
  // the three come out least-significant-first and have to be reversed to
  // index Table 32, which prints C12 as the most significant of them.
  state.national_option_subset = (((c11_c14 >> 1) & 0x1) << 2) |  // C12
                                 (((c11_c14 >> 2) & 0x1) << 1) |  // C13
                                 ((c11_c14 >> 3) & 0x1);          // C14

  // The G0 set the page opens in: the magazine's designation if an M/29/0 has
  // established one, otherwise the local Code of Practice (§15.2). A packet
  // X/28/0 for this page overrides it if one arrives before the page is
  // rendered — but only for this page, which is why it is taken afresh here
  // rather than left at whatever the previous page settled on. A rolling
  // header re-sent mid-page is the exception: it is the same transmission, so
  // an X/28/0 already received for it must survive.
  if (!same_page_continues) {
    state.g0_set = state.magazine_g0_set.value_or(default_g0_set_);
  }
  // The header that *opened* this transmission stamps it, so a rolling header
  // re-sent while the rows are still going out does not make the same
  // appearance of the page look like a series of new ones.
  if (!same_page_continues) {
    state.header_field_index = field_index;
  }
  state.last_field_index = field_index;

  // Header display bytes: the packet bytes after the ten addressing and
  // control ones carry odd-parity characters shown in row 0 from column 8
  // (EN 300 706 §9.3.1.4) — 32 of them on 625 lines, 24 on 525, the addressing
  // ahead of them being identical (ITU-R BT.653 Table 1b §3.3). Columns 0-7
  // are decoder-generated (page number, clock) and left as spaces here.
  RowData& header_row = state.rows[0];
  const bool header_extension_present = header_row.extension_present;
  const auto header_extension_columns = header_row.characters;
  const auto header_extension_parity = header_row.parity_error;
  header_row.present = true;
  header_row.characters.fill(0x20);
  header_row.parity_error.fill(false);
  // The header row's own extension columns survive the rewrite: they were sent
  // in a different packet and this one says nothing about them.
  if (header_extension_present) {
    header_row.extension_present = true;
    for (int column = head_columns_; column < TeletextPageSnapshot::kColumns;
         ++column) {
      const auto i = static_cast<size_t>(column);
      header_row.characters[i] = header_extension_columns[i];
      header_row.parity_error[i] = header_extension_parity[i];
    }
  }
  for (int column = kHeaderTextColumn; column < head_columns_; ++column) {
    const auto i = static_cast<size_t>(column - kHeaderTextColumn);
    const uint8_t byte = packet[kHeaderControlBytes + i];
    if (teletext_odd_parity_valid(byte)) {
      header_row.characters[static_cast<size_t>(column)] = byte & 0x7F;
    } else {
      header_row.characters[static_cast<size_t>(column)] = 0x20;
      header_row.parity_error[static_cast<size_t>(column)] = true;
    }
  }
}

void TeletextPageDecoder::handle_display_packet(
    int transmission_magazine, int row,
    const std::array<uint8_t, kTeletextPacketBytes>& packet,
    int64_t field_index, int64_t source,
    const TeletextPacketConfidence* confidence) {
  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  // Rows belong to the page whose transmission is in progress in this
  // magazine (EN 300 706 §7.2.1); orphan rows with no open page are dropped.
  if (!state.page_open) {
    return;
  }

  const TeletextPageKey key = page_key(transmission_magazine);
  last_row_attribution_ = key;
  last_row_number_ = row;

  RowData& row_data = state.rows[static_cast<size_t>(row)];
  row_data.present = true;
  for (size_t column = 0; column < static_cast<size_t>(head_columns_);
       ++column) {
    // The service's display bytes, 7 data bits + odd parity (EN 300 706
    // §9.3.2, §8.1).
    const uint8_t byte = packet[kMragBytes + column];
    if (teletext_odd_parity_valid(byte)) {
      row_data.characters[column] = byte & 0x7F;
      row_data.parity_error[column] = false;
    } else {
      row_data.characters[column] = 0x20;
      row_data.parity_error[column] = true;
    }
  }

  if (row_squasher_ != nullptr) {
    // The squasher's rows are the widest a service transmits; a service whose
    // packets carry fewer leaves the rest at zero, and the render below takes
    // those columns from the row-extension store instead, so the squasher needs
    // no notion of the width itself.
    TeletextRowBytes display{};
    std::copy(packet.begin() + kMragBytes,
              packet.begin() + kMragBytes + head_columns_, display.begin());
    // The row's own bytes are the packet bytes after the MRAG (§9.3.2), so its
    // confidences are the matching slice of the packet's.
    TeletextRowConfidence weights{};
    if (confidence != nullptr) {
      std::copy(confidence->begin() + kMragBytes,
                confidence->begin() + kMragBytes + head_columns_,
                weights.begin());
    }
    row_squasher_->add_row(key, row, display, source,
                           confidence != nullptr ? &weights : nullptr, 0,
                           static_cast<size_t>(head_columns_));
  }

  state.last_field_index = field_index;
}

void TeletextPageDecoder::handle_extension_packet(
    int transmission_magazine, int packet_number,
    const std::array<uint8_t, kTeletextPacketBytes>& packet,
    int64_t field_index, int64_t source,
    const TeletextPacketConfidence* confidence) {
  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  // Like a display row, an extension belongs to the page whose transmission is
  // in progress in the magazine it is addressed to (EN 300 706 §7.2.1).
  if (!state.page_open) {
    return;
  }

  const int extension_columns = TeletextPageSnapshot::kColumns - head_columns_;
  if (extension_columns <= 0) {
    return;
  }

  const TeletextPageKey key = page_key(transmission_magazine);
  const int first_row = extension_first_row(packet_number);

  for (int group = 0; group < kExtensionPacketRows; ++group) {
    const int row = first_row + group;
    if (row >= TeletextPageSnapshot::kRows) {
      break;
    }
    const size_t group_base = static_cast<size_t>(kMragBytes) +
                              static_cast<size_t>(group * extension_columns);

    RowData& row_data = state.rows[static_cast<size_t>(row)];
    for (int offset = 0; offset < extension_columns; ++offset) {
      const size_t column =
          static_cast<size_t>(head_columns_) + static_cast<size_t>(offset);
      const uint8_t byte = packet[group_base + static_cast<size_t>(offset)];
      // A byte that fails odd parity (EN 300 706 §8.1) is known to be corrupt,
      // so it never replaces a clean one already recovered for this column.
      // This is the decoder's own store, which is all a caller without a row
      // squasher has; with one attached the render takes these columns from its
      // vote across every copy instead.
      const bool clean = teletext_odd_parity_valid(byte);
      if (!clean && row_data.extension_present &&
          !row_data.parity_error[column]) {
        continue;
      }
      row_data.characters[column] = clean ? static_cast<uint8_t>(byte & 0x7F)
                                          : static_cast<uint8_t>(0x20);
      row_data.parity_error[column] = !clean;
    }
    row_data.extension_present = true;

    if (row_squasher_ == nullptr) {
      continue;
    }
    // The extension columns go into the squasher as a copy speaking for those
    // columns only, so repeated transmissions of them correct each other just
    // as the display packets' columns do. One packet contributes to four rows;
    // they are separate buckets, so the one source id serves all four.
    TeletextRowBytes display{};
    std::copy(packet.begin() + static_cast<std::ptrdiff_t>(group_base),
              packet.begin() + static_cast<std::ptrdiff_t>(group_base) +
                  extension_columns,
              display.begin() + head_columns_);
    TeletextRowConfidence weights{};
    if (confidence != nullptr) {
      std::copy(confidence->begin() + static_cast<std::ptrdiff_t>(group_base),
                confidence->begin() + static_cast<std::ptrdiff_t>(group_base) +
                    extension_columns,
                weights.begin() + head_columns_);
    }
    row_squasher_->add_row(key, row, display, source,
                           confidence != nullptr ? &weights : nullptr,
                           static_cast<size_t>(head_columns_),
                           static_cast<size_t>(extension_columns));
  }

  state.last_field_index = field_index;
}

void TeletextPageDecoder::terminate_page(int transmission_magazine,
                                         bool transmission_complete) {
  MagazineState& state = magazines_[static_cast<size_t>(transmission_magazine)];
  if (!state.page_open) {
    return;
  }
  state.page_open = false;

  // Rendering the snapshot is the expensive part of closing a page — with a
  // squasher attached it votes over every stored copy of all 24 rows — and a
  // header packet closes a page every few dozen packets for the whole of a
  // recording. Skip it when nothing will consume it: no page callback, and
  // the closed page is not the watched subtitle page (the only page
  // subtitle_page_completed() acts on).
  const bool watched_subtitle_page =
      subtitle_filter_.has_value() &&
      displayed_magazine(transmission_magazine) == subtitle_filter_->first &&
      state.page_number == subtitle_filter_->second;
  if (page_callback_ == nullptr && !watched_subtitle_page) {
    return;
  }

  TeletextPageSnapshot snapshot = render_snapshot(transmission_magazine, state);
  snapshot.transmission_complete = transmission_complete;
  if (page_callback_) {
    page_callback_(snapshot);
  }
  subtitle_page_completed(snapshot);
}

std::vector<TeletextPageSnapshot> TeletextPageDecoder::open_page_snapshots()
    const {
  std::vector<TeletextPageSnapshot> snapshots;
  for (int magazine = 0; magazine < static_cast<int>(magazines_.size());
       ++magazine) {
    const MagazineState& state = magazines_[static_cast<size_t>(magazine)];
    if (!state.page_open) {
      continue;
    }
    TeletextPageSnapshot snapshot = render_snapshot(magazine, state);
    snapshot.transmission_complete = false;
    snapshots.push_back(std::move(snapshot));
  }
  return snapshots;
}

TeletextPageSnapshot TeletextPageDecoder::render_snapshot(
    int transmission_magazine, const MagazineState& state) const {
  TeletextPageSnapshot snapshot;
  snapshot.magazine = displayed_magazine(transmission_magazine);
  snapshot.page_number = state.page_number;
  snapshot.subcode = state.subcode;
  snapshot.erase_page = state.erase_page;
  snapshot.newsflash = state.newsflash;
  snapshot.subtitle = state.subtitle;
  snapshot.suppress_header = state.suppress_header;
  snapshot.update_indicator = state.update_indicator;
  snapshot.interrupted_sequence = state.interrupted_sequence;
  snapshot.inhibit_display = state.inhibit_display;
  snapshot.magazine_serial = state.magazine_serial;
  snapshot.national_option_subset = state.national_option_subset;
  snapshot.g0_set = state.g0_set;
  snapshot.header_field_index = state.header_field_index;
  snapshot.last_field_index = state.last_field_index;
  snapshot.columns = columns_;
  // A short packet is the 525-line service, whose graphics use the codes a
  // 625-line one would blast alphanumerics through (see the field's comment).
  snapshot.mosaic_blast_through =
      head_columns_ == TeletextPageSnapshot::kColumns;

  // With a squasher attached, display rows come from the combined copies
  // rather than from the last one received: repeated transmissions correct
  // each other, and a row recovered during an earlier transmission is still
  // available when the current one was clipped (teletext_row_squasher.h).
  const TeletextPageKey key{
      snapshot.magazine, state.page_number, state.subcode,
      erase_epoch({snapshot.magazine, state.page_number, state.subcode})};

  for (int row = 0; row < TeletextPageSnapshot::kRows; ++row) {
    // The decoder's own store is the starting point, and the columns it never
    // wrote show as spaces: the zeros of a row never written would otherwise
    // read as spacing attributes rather than as the blank they stand for.
    RowData row_data = state.rows[static_cast<size_t>(row)];
    if (!row_data.present) {
      std::fill(row_data.characters.begin(),
                row_data.characters.begin() + head_columns_, 0x20);
      std::fill(row_data.parity_error.begin(),
                row_data.parity_error.begin() + head_columns_, false);
    }
    if (!row_data.extension_present) {
      std::fill(row_data.characters.begin() + head_columns_,
                row_data.characters.end(), 0x20);
      std::fill(row_data.parity_error.begin() + head_columns_,
                row_data.parity_error.end(), false);
    }

    // Where a squasher has copies, its vote replaces what the store holds:
    // repeated transmissions correct each other, and a row recovered during an
    // earlier transmission is still available when the current one was clipped.
    // It votes per column, so for a service that splits its rows the display
    // packet's columns and the extension packets' are each combined across
    // their own repeats — including on row 0, whose extension columns hold
    // still even though the header text beside them carries a live clock.
    int row_copies = 0;
    if (row_squasher_ != nullptr) {
      TeletextRowCoverage covered{};
      if (const auto squashed =
              row_squasher_->squashed_row(key, row, &covered)) {
        row_copies = static_cast<int>(row_squasher_->copy_count(key, row));
        for (int column = 0; column < columns_; ++column) {
          const auto i = static_cast<size_t>(column);
          if (!covered[i]) {
            continue;
          }
          if (column < head_columns_) {
            row_data.present = true;
          } else {
            row_data.extension_present = true;
          }
          const uint8_t byte = (*squashed)[i];
          if (teletext_odd_parity_valid(byte)) {
            row_data.characters[i] = byte & 0x7F;
            row_data.parity_error[i] = false;
          } else {
            row_data.characters[i] = 0x20;
            row_data.parity_error[i] = true;
          }
        }
      }
    }
    row_data.present = row_data.present || row_data.extension_present;
    auto& cells = snapshot.cells[static_cast<size_t>(row)];
    snapshot.row_received[static_cast<size_t>(row)] = row_data.present;
    // Without a squasher — or for a row it has no copies of, which is this
    // magazine's own store answering — the row rests on the one copy received.
    if (row_copies == 0 && row_data.present && row >= 1) {
      row_copies = 1;
    }
    snapshot.row_copies[static_cast<size_t>(row)] = row_copies;

    // Start-of-row default conditions (EN 300 706 §12.2 Table 26): white
    // alphanumeric foreground, black background, steady, unboxed, normal
    // size, contiguous mosaics, hold off.
    TeletextColour foreground = TeletextColour::White;
    TeletextColour background = TeletextColour::Black;
    bool mosaic = false;
    bool separated = false;
    bool hold = false;
    bool flash = false;
    bool conceal = false;
    bool boxed = false;
    bool double_height = false;
    uint8_t held_character = 0x20;
    bool held_separated = false;

    for (int column = 0; column < columns_; ++column) {
      const uint8_t code =
          row_data.present ? row_data.characters[static_cast<size_t>(column)]
                           : static_cast<uint8_t>(0x20);
      const bool parity_error =
          row_data.present &&
          row_data.parity_error[static_cast<size_t>(column)];
      TeletextPageCell& cell = cells[static_cast<size_t>(column)];

      if (code < 0x20 && !parity_error) {
        // Spacing attribute: "Set-At" codes act on this cell, "Set-After"
        // codes from the next cell (EN 300 706 §12.2 Table 26).
        switch (code) {
          case kSteady:
            flash = false;
            break;
          case kNormalSize:
            if (double_height) {
              held_character = 0x20;  // size change resets the held mosaic
              held_separated = false;
            }
            double_height = false;
            break;
          case kConceal:
            conceal = true;
            break;
          case kContiguousMosaic:
            separated = false;
            break;
          case kSeparatedMosaic:
            separated = true;
            break;
          case kBlackBackground:
            background = TeletextColour::Black;
            break;
          case kNewBackground:
            background = foreground;
            break;
          case kHoldMosaics:
            hold = true;
            break;
          default:
            break;
        }

        // The attribute cell displays as SPACE, or as the held mosaic
        // character in mosaics + Hold Mosaics mode (§12.2 1/E); the held
        // character keeps its original contiguous/separated form.
        const bool substitute = hold && mosaic && held_character != 0x20;
        cell.character = substitute ? held_character : 0x20;
        cell.held_mosaic = substitute;
        cell.mosaic = substitute;
        cell.separated_mosaic = substitute ? held_separated : separated;
        cell.foreground = foreground;
        cell.background = background;
        cell.flash = flash;
        cell.conceal = conceal;
        cell.boxed = boxed;
        cell.double_height = double_height;
        cell.parity_error = false;

        // "Set-After" actions.
        if (code >= kAlphaColourBase + 1 && code <= kAlphaColourBase + 7) {
          // 0/1-0/7 alpha colours (0/0 has no Level 1 response). Colour
          // codes cancel conceal (§12.2 1/8) and select the G0 set.
          foreground = static_cast<TeletextColour>(code);
          conceal = false;
          if (mosaic) {
            mosaic = false;
            held_character = 0x20;  // mode change resets the held mosaic
            held_separated = false;
          }
        } else if (code >= kMosaicColourBase + 1 &&
                   code <= kMosaicColourBase + 7) {
          // 1/1-1/7 mosaic colours (1/0 has no Level 1 response).
          foreground = static_cast<TeletextColour>(code - kMosaicColourBase);
          conceal = false;
          if (!mosaic) {
            mosaic = true;
            held_character = 0x20;
            held_separated = false;
          }
        } else if (code == kFlash) {
          flash = true;
        } else if (code == kEndBox) {
          boxed = false;
        } else if (code == kStartBox) {
          boxed = true;
        } else if (code == kDoubleHeight) {
          if (!double_height) {
            double_height = true;
            held_character = 0x20;
            held_separated = false;
          }
        } else if (code == kReleaseMosaics) {
          hold = false;
        }
        // 0/E double width, 0/F double size, 1/B ESC: no Level 1 response.
      } else {
        // Displayable character (or a parity-damaged byte rendered as a
        // flagged SPACE).
        cell.character = parity_error ? 0x20 : code;
        cell.parity_error = parity_error;
        // G1 codes 0x40-0x5F are alphanumeric capitals even in mosaics mode
        // (§12.2: mosaic blocks live in G1 columns 2, 3, 6 and 7).
        const bool mosaic_glyph = mosaic && (code & 0x20) != 0;
        cell.mosaic = !parity_error && mosaic_glyph;
        cell.separated_mosaic = cell.mosaic && separated;
        cell.held_mosaic = false;
        cell.foreground = foreground;
        cell.background = background;
        cell.flash = flash;
        cell.conceal = conceal;
        cell.boxed = boxed;
        cell.double_height = double_height;
        if (cell.mosaic) {
          // §12.2 1/E: the held mosaic is the most recent G1 mosaic
          // character with bit 6 set on this row.
          held_character = code;
          held_separated = separated;
        }
      }
    }
  }

  // Double-height post-pass (EN 300 706 §12.2 0/D): a row containing double
  // height characters consumes the row below — its transmitted data is
  // ignored and it displays only the origin row's background.
  std::array<bool, TeletextPageSnapshot::kRows> is_lower_row{};
  for (int row = 0; row + 1 < TeletextPageSnapshot::kRows; ++row) {
    if (is_lower_row[static_cast<size_t>(row)]) {
      continue;
    }
    const auto& origin_cells = snapshot.cells[static_cast<size_t>(row)];
    bool row_has_double_height = false;
    for (const auto& cell : origin_cells) {
      if (cell.double_height) {
        row_has_double_height = true;
        break;
      }
    }
    if (!row_has_double_height) {
      continue;
    }
    const size_t lower_row = static_cast<size_t>(row) + 1;
    is_lower_row[lower_row] = true;
    auto& lower_cells = snapshot.cells[lower_row];
    for (int column = 0; column < snapshot.columns; ++column) {
      TeletextPageCell lower;
      lower.background = origin_cells[static_cast<size_t>(column)].background;
      lower.double_height_lower = true;
      lower_cells[static_cast<size_t>(column)] = lower;
    }
  }

  return snapshot;
}

void TeletextPageDecoder::subtitle_page_completed(
    const TeletextPageSnapshot& snapshot) {
  if (!subtitle_filter_.has_value() ||
      snapshot.magazine != subtitle_filter_->first ||
      snapshot.page_number != subtitle_filter_->second) {
    return;
  }

  // Only C6-flagged transmissions display subtitle text (EN 300 706
  // §9.3.1.3 Table 2); a completion without C6 clears at most (the header
  // arrival already fired the clear event).
  if (!snapshot.subtitle) {
    subtitle_clear_event(snapshot.last_field_index);
    return;
  }

  const std::string text = extract_subtitle_text(snapshot);
  const int64_t field_index = snapshot.last_field_index;

  if (open_cue_.has_value()) {
    if (open_cue_->text == text) {
      return;  // unchanged retransmission: the cue stays on screen
    }
    subtitle_clear_event(field_index);
  }
  if (!text.empty()) {
    TeletextSubtitleCue cue;
    cue.start_field_index = field_index;
    cue.text = text;
    open_cue_ = std::move(cue);
  }
}

void TeletextPageDecoder::subtitle_clear_event(int64_t field_index) {
  if (!open_cue_.has_value()) {
    return;
  }
  open_cue_->end_field_index = field_index;
  // Degenerate zero-length cues (cleared in the same field they appeared)
  // are dropped.
  if (open_cue_->end_field_index > open_cue_->start_field_index) {
    subtitle_cues_.push_back(std::move(*open_cue_));
  }
  open_cue_.reset();
}

std::string TeletextPageDecoder::extract_subtitle_text(
    const TeletextPageSnapshot& snapshot) {
  // On C5/C6 pages only boxed regions are displayed (EN 300 706 §12.2
  // 0/A-0/B); respect that so stray unboxed bytes never leak into cues.
  const bool boxed_only = snapshot.newsflash || snapshot.subtitle;

  std::string text;
  for (int row = 1; row < TeletextPageSnapshot::kRows; ++row) {
    const auto& cells = snapshot.cells[static_cast<size_t>(row)];
    std::string row_text;
    bool last_was_space = true;  // collapses runs and trims the left edge
    for (int column = 0; column < snapshot.columns; ++column) {
      const TeletextPageCell& cell = cells[static_cast<size_t>(column)];
      // Displayable cells go out as UTF-8 through the page's own G0 set, so
      // the text reads as the page does: a UK service's 2/3 is "£", not the
      // "#" the transmitted code would be in ASCII (§15.6.2 Table 36).
      std::string glyph;
      if (!cell.mosaic && !cell.held_mosaic && !cell.conceal &&
          !cell.double_height_lower && !cell.parity_error &&
          (!boxed_only || cell.boxed) && cell.character > 0x20 &&
          cell.character < 0x7F) {
        glyph = teletext_g0_to_utf8(cell.character, snapshot.g0_set,
                                    snapshot.national_option_subset);
      }
      if (glyph.empty()) {
        if (!last_was_space) {
          row_text.push_back(' ');
        }
        last_was_space = true;
      } else {
        row_text += glyph;
        last_was_space = false;
      }
    }
    while (!row_text.empty() && row_text.back() == ' ') {
      row_text.pop_back();
    }
    if (!row_text.empty()) {
      if (!text.empty()) {
        text.push_back('\n');
      }
      text += row_text;
    }
  }
  return text;
}

void TeletextPageDecoder::finalize(int64_t end_field_index) {
  for (int magazine = 0; magazine < static_cast<int>(magazines_.size());
       ++magazine) {
    terminate_page(magazine);
  }
  subtitle_clear_event(end_field_index);
}

}  // namespace orc
