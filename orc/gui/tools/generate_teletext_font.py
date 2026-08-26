#!/usr/bin/env python3
#
# File:        generate_teletext_font.py
# Module:      orc-gui
# Purpose:     Generate teletext_font.cpp from the upstream Bedstead glyph
#              bitmaps
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# The character generator a teletext page is drawn from is compiled into the
# GUI as a table. Generating it rather than transcribing it is what lets a
# reader check the table against the source it came from:
#
#   curl -O https://raw.githubusercontent.com/glxxyz/bedstead/e3938b7af387db6fedcc74de6772e7b78567b666/bedstead.c
#   orc/gui/tools/generate_teletext_font.py bedstead.c > orc/gui/teletext_font.cpp
#   clang-format -i orc/gui/teletext_font.cpp
#
# Regenerating from the same upstream revision reproduces the committed file
# byte for byte, so a diff is the check.
#
# Provenance
# ----------
# Bedstead <http://bjh21.me.uk/bedstead/> reconstructs the typeface of the
# Mullard SAA5050 series of teletext character generators — the chip that drew
# a page on a British receiver, and on the BBC Micro in Mode 7. Its bedstead.c
# carries the 5 by 9 bitmaps of every member of the family the author knew of
# (SAA5050 to SAA5057, which is where the Cyrillic alphabet comes from), taken
# from the July 1982 datasheet code tables and checked against a real SAA5050.
#
# The bitmaps and the code around them were dedicated to the public domain
# under CC0 1.0 by Ben Harris, Simon Tatham and Marnanel Thurman, which is
# compatible with this project's GPL-3.0-or-later. Upstream's own note on the
# typeface itself is worth repeating: copyright in it is still owned by
# Mullard's corporate successors, but under section 55 of the Copyright
# Designs and Patents Act 1988 that copyright is no longer infringed by the
# production or use of articles designed for producing material in it.
#
# The revision pinned above is the one linked from decode-orc issue #255,
# glxxyz/bedstead ("Teletext50"), which mirrors bjh21's Bedstead with changes
# to how the outlines are emitted. Only the bitmaps are taken here, and those
# are the mirror's unchanged.
#
# What is taken and what is not
# -----------------------------
# Every non-mosaic glyph upstream defines, keyed by Unicode code point. The
# repertoire a page needs is not fixed — the G0 set is designated by the
# transmission and the standard has more sets than this decoder reads today —
# so carrying the whole face costs a few kilobytes and means a set that is
# added later is drawn in the right typeface without a second visit here.
#
# Mosaic glyphs are skipped. A mosaic cell carries a six-bit pattern rather
# than a character (ETSI EN 300 706 clause 15.7 G1 set), and the viewer paints
# its sub-elements as rectangles scaled to the cell, which is exact at any
# size and needs no bitmap.
#
# Upstream's alternates — a second bitmap for a code point some language draws
# differently — are entered with no code point of their own, so taking the
# first entry per code point takes the default upstream chose.

import re
import sys

# The 5 by 9 matrix and the 6 by 10 character rectangle it sits in, as
# upstream's XSIZE/YSIZE. Where the matrix sits in the rectangle is not in the
# bitmaps and is stated in teletext_font.h instead: upstream's own placement,
# read off its getpix() against the rectangle its domosaic() fills, is the
# rightmost five columns and the bottom nine rows.
GLYPH_COLUMNS = 5
GLYPH_ROWS = 9

# Letters the decoder's G0 tables need that the SAA5050 family never held.
#
# ETSI EN 300 706 clause 15.6.4 Table 38 (Cyrillic G0 option 1,
# Serbian/Croatian) puts the Macedonian GJE and KJE at 5/7, 5/1, 7/7 and 7/1.
# The SAA5057 is a Russian/Bulgarian generator and has neither, so upstream has
# no bitmap for them and a page in that set would drop out of the typeface for
# four letters.
#
# Both letters are their base letter with an acute above, and the face already
# draws a dozen acute-accented letters (Ó, Ć, É, Ú and the rest). Every one of
# them is built the same way: the acute occupies rows 0 and 1, and the letter
# is set below it in the five rows 2 to 6. These four follow that rule, so
# nothing here is invented beyond applying it — the capitals take the five-row
# form of their base letter, as Ú takes a five-row U, and the lowercase take
# theirs unchanged, because a lowercase letter already sits in rows 2 to 6.
#
# One consequence is worth stating rather than working around: Ѓ comes out
# identical to ѓ, because Г and г have the same shape and only differ in
# height, so accenting the capital compresses it onto the lowercase. Ќ and ќ
# stay distinct, because К and к do not have the same shape.
ACUTE = [0o02, 0o04]
ADDITIONS = {
    # CYRILLIC CAPITAL LETTER GJE, CYRILLIC SMALL LETTER GJE
    0x0403: (ACUTE + [0o37, 0o20, 0o20, 0o20, 0o20, 0o00, 0o00], "uni0403"),
    0x0453: (ACUTE + [0o37, 0o20, 0o20, 0o20, 0o20, 0o00, 0o00], "uni0453"),
    # CYRILLIC CAPITAL LETTER KJE, CYRILLIC SMALL LETTER KJE
    0x040C: (ACUTE + [0o21, 0o22, 0o30, 0o22, 0o21, 0o00, 0o00], "uni040C"),
    0x045C: (ACUTE + [0o21, 0o22, 0o34, 0o22, 0o21, 0o00, 0o00], "uni045C"),
}


def read_glyph_table(path):
    """Return [(code point, [row bits, top first], upstream name)] from
    bedstead.c, in the order upstream lists them."""
    with open(path, "r", encoding="utf-8") as handle:
        source = handle.read()

    opening = "} const glyphs[] = {"
    start = source.index(opening) + len(opening)
    end = source.index("\n};", start)
    body = source[start:end]

    # Armenian is behind a preprocessor conditional upstream never defines, so
    # the built font does not have it and neither does this table.
    body = re.sub(r"#ifdef ARMENIAN\b.*?#endif", "", body, flags=re.S)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    # Preprocessor lines, continuations and all, which in this array are the
    # mosaic-emitting macros. Their invocations are left behind and fall
    # outside every brace, so the split below never sees them.
    body = re.sub(r"^[ \t]*#(?:[^\n\\]|\\[\s\S])*", "", body, flags=re.M)

    entries = []
    depth = 0
    current = ""
    for character in body:
        if character == "{":
            depth += 1
        if depth > 0:
            current += character
        if character == "}":
            depth -= 1
            if depth == 0:
                entries.append(current)
                current = ""

    glyphs = []
    for entry in entries:
        if "ALIAS(" in entry:
            continue
        match = re.match(r"\{\{([^}]*)\}\s*,\s*(.*)\}$", entry, flags=re.S)
        if match is None:
            raise SystemExit("unrecognised glyph entry: %r" % entry[:120])
        data, rest = match.group(1), match.group(2).strip()
        # A mosaic is a pattern, not a character; see the note at the top.
        if "MOS" in rest or "SEP" in rest:
            continue
        macro = re.match(r"U\(([0-9A-Fa-f]+)\)", rest)
        if macro is not None:
            code = int(macro.group(1), 16)
            name = "uni" + macro.group(1)
        else:
            fields = re.match(r'(-?\w+)\s*,\s*"([^"]*)"', rest)
            if fields is None:
                raise SystemExit("unrecognised glyph entry: %r" % entry[:120])
            value = fields.group(1)
            code = int(value, 16) if value.startswith("0x") else int(value)
            name = fields.group(2)
        # An alternate bitmap for a code point that already has one; upstream
        # gives it no code point of its own.
        if code < 0:
            continue
        rows = [int(field.strip(), 8) for field in data.split(",") if field.strip()]
        if len(rows) > GLYPH_ROWS:
            raise SystemExit("glyph U+%04X has %d rows" % (code, len(rows)))
        rows += [0] * (GLYPH_ROWS - len(rows))
        if any(row >= (1 << GLYPH_COLUMNS) for row in rows):
            raise SystemExit("glyph U+%04X is wider than %d columns"
                             % (code, GLYPH_COLUMNS))
        glyphs.append((code, rows, name))
    return glyphs


def emit(glyphs, out):
    out.write("""/*
 * File:        teletext_font.cpp
 * Module:      orc-gui
 * Purpose:     The SAA5050 character generator's bitmaps, keyed by Unicode
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

// Generated by orc/gui/tools/generate_teletext_font.py from the Bedstead
// glyph bitmaps (CC0 1.0; see teletext_font.h and the generator for the
// provenance). Do not edit by hand: regenerate instead.

#include "teletext_font.h"

#include <algorithm>

namespace {

struct TeletextFontEntry {
  char32_t code;
  TeletextGlyph glyph;
};

// Ascending by code point, which is what the lookup binary-searches. Each
// entry is a code point and its matrix, rows top down.
constexpr TeletextFontEntry kEntries[] = {
""")
    for code, rows, _name in glyphs:
        packed = ", ".join("0x%02X" % row for row in rows)
        out.write("    {0x%04X, {{%s}}},\n" % (code, packed))
    out.write("""};

}  // namespace

const TeletextGlyph* teletext_font_glyph(char32_t code) {
  const auto* const end = std::end(kEntries);
  const auto* const found =
      std::lower_bound(std::begin(kEntries), end, code,
                       [](const TeletextFontEntry& entry, char32_t wanted) {
                         return entry.code < wanted;
                       });
  if (found == end || found->code != code) {
    return nullptr;
  }
  return &found->glyph;
}
""")


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: generate_teletext_font.py <bedstead.c>\n")
        return 2

    glyphs = read_glyph_table(argv[1])

    chosen = {}
    for code, rows, name in glyphs:
        # First entry wins: upstream lists the default before its alternates.
        chosen.setdefault(code, (rows, name))
    for code, (rows, name) in ADDITIONS.items():
        if code in chosen:
            raise SystemExit("U+%04X is upstream's now; drop the addition"
                             % code)
        chosen[code] = (rows, name)

    emit([(code, rows, name) for code, (rows, name) in sorted(chosen.items())],
         sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
