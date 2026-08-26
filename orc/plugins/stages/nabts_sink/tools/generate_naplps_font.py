#!/usr/bin/env python3
#
# File:        generate_naplps_font.py
# Module:      nabts_sink stage plugin
# Purpose:     Generate naplps_font.cpp from the upstream X11 misc-fixed BDF
#              sources
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# The faces a NAPLPS receiver draws its built-in characters from are compiled
# into the plugin as tables. Generating them rather than transcribing them is
# what lets a reader check the tables against the face they came from:
#
#   nix build --no-link --print-out-paths nixpkgs#font-misc-misc.src
#   tar xf /nix/store/...-font-misc-misc-1.1.3.tar.xz
#   orc/plugins/stages/nabts_sink/tools/generate_naplps_font.py \
#       font-misc-misc-1.1.3 > orc/plugins/stages/nabts_sink/naplps_font.cpp
#   clang-format -i orc/plugins/stages/nabts_sink/naplps_font.cpp
#
# Regenerating from the same upstream release reproduces the committed file
# byte for byte, so a diff is the check.

import sys
import os

# The faces the plugin carries, ascending. Every one is a member of the X11
# "misc-fixed" family, which is public domain, so the whole set shares one
# provenance and one house style: a receiver drawing a page at a finer grid
# picks a finer member rather than a different typeface.
FACES = [
    ("6x10", "misc-fixed 6x10"),
    ("9x15", "misc-fixed 9x15"),
    ("10x20", "misc-fixed 10x20"),
]

# ISO 8859-1's printable positions, which cover the primary set of X3.110 §7.1
# and the accented letters its supplementary set (§7.2) composes. Every face
# must carry all of them: a character legible on one receiver may not vanish on
# another.
REPERTOIRE = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100))

ASCII_NAMES = {
    0x22: '"', 0x5C: "\\\\", 0x27: "'",
}

# The one place the embedded tables depart from upstream.
#
# The misc-fixed 6x10 draws a full stop as a five-pixel diamond straddling the
# baseline, and builds the colon and semicolon out of the same mark. That is a
# terminal font's device for keeping a full stop visible in a cell only ten
# rows tall, and it works at the size a terminal is read at; on a NAPLPS page,
# where a line of leader dots is a common device and the page is looked at
# rather than read close up, it reads as a row of crosses. The finer faces have
# no such trouble and use a plain square block, so these three glyphs are
# replaced by the same block scaled to the 6x10 cell: two pixels square, on the
# baseline at rows 6 and 7, centred in the six-pixel advance.
#
# Rows run top down; within a row bit 5 is the leftmost column. The face's
# baseline is row 7 (FONT_ASCENT 8), which the diamond dipped below.
SUBSTITUTIONS = {
    ("6x10", 0x2E): [0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                     0x0C, 0x0C, 0x00, 0x00],  # full stop
    ("6x10", 0x3A): [0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00,
                     0x0C, 0x0C, 0x00, 0x00],  # colon
    ("6x10", 0x3B): [0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00,
                     0x0C, 0x08, 0x10, 0x00],  # semicolon, keeping its tail
}


def read_bdf(path):
    """Return (width, height, {code: [row bits, top first]}) for a BDF face."""
    with open(path, "r", errors="replace") as handle:
        lines = handle.read().splitlines()

    bounding = None
    glyphs = {}
    index = 0
    while index < len(lines):
        line = lines[index]
        if line.startswith("FONTBOUNDINGBOX"):
            bounding = [int(value) for value in line.split()[1:]]
        elif line.startswith("STARTCHAR"):
            code = None
            box = None
            bitmap = []
            while index < len(lines) and not lines[index].startswith("ENDCHAR"):
                entry = lines[index]
                if entry.startswith("ENCODING"):
                    code = int(entry.split()[1])
                elif entry.startswith("BBX"):
                    box = [int(value) for value in entry.split()[1:]]
                elif entry.startswith("BITMAP"):
                    index += 1
                    while index < len(lines) and not lines[index].startswith(
                        "ENDCHAR"
                    ):
                        bitmap.append(lines[index].strip())
                        index += 1
                    break
                index += 1
            if code is not None and code >= 0:
                glyphs[code] = (box, bitmap)
        index += 1

    if bounding is None:
        raise SystemExit(f"{path}: no FONTBOUNDINGBOX")
    width, height = bounding[0], bounding[1]

    patterns = {}
    for code, (box, bitmap) in glyphs.items():
        if box[:2] != [width, height]:
            # Every misc-fixed glyph fills its cell, which is what lets a
            # pattern be stored as rows alone with no per-glyph placement.
            continue
        if len(bitmap) != height:
            raise SystemExit(f"{path}: U+{code:04X} has {len(bitmap)} rows")
        rows = []
        for hex_row in bitmap:
            bits = int(hex_row, 16)
            rows.append(bits >> (4 * len(hex_row) - width))
        patterns[code] = rows
    return width, height, patterns


def label(code):
    """The trailing comment for a code point."""
    if 0x21 <= code <= 0x7E:
        glyph = ASCII_NAMES.get(code, chr(code))
        return f"// U+{code:04X} '{glyph}'"
    return f"// U+{code:04X}"


def emit(directory, out):
    faces = []
    for stem, name in FACES:
        path = os.path.join(directory, f"{stem}.bdf")
        width, height, patterns = read_bdf(path)
        if f"{width}x{height}" != stem:
            raise SystemExit(f"{path}: cell {width}x{height} is not {stem}")
        if width > 16:
            raise SystemExit(f"{path}: cell wider than a pattern row holds")
        for (face_stem, code), rows in SUBSTITUTIONS.items():
            if face_stem != stem:
                continue
            if len(rows) != height:
                raise SystemExit(f"{path}: substitute for U+{code:04X} is "
                                 f"{len(rows)} rows, not {height}")
            patterns[code] = rows
        missing = [code for code in REPERTOIRE if code not in patterns]
        if missing:
            raise SystemExit(
                f"{path}: no pattern for " + ", ".join(f"U+{c:04X}" for c in missing)
            )
        faces.append((stem, name, width, height, patterns))

    digits = max(len(f"{(1 << width) - 1:X}") for _, _, width, _, _ in faces)

    out.write(
        """/*
 * File:        naplps_font.cpp
 * Module:      nabts_sink stage plugin
 * Purpose:     Fixed bitmap faces for depositing NAPLPS characters into a
 *              receiver's pixel grid
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

// Generated by tools/generate_naplps_font.py from the X11 misc-fixed BDF
// sources; see naplps_font.h for the provenance of each face. Edit the
// generator, not this file.

#include "naplps_font.h"

#include <algorithm>
#include <iterator>

namespace orc {

namespace {

// The repertoire every face carries, ascending so a lookup can bisect.
constexpr char32_t kCodes[] = {
"""
    )
    for code in REPERTOIRE:
        out.write(f"    0x{code:04X},\n")
    out.write("};\n\nconstexpr size_t kCodeCount = std::size(kCodes);\n")

    # The tables are laid out here rather than by clang-format, which puts one
    # value per line once a comment appears among the elements — a pattern is
    # only readable as a block of rows.
    per_line = 10
    for stem, name, width, height, patterns in faces:
        out.write(
            f"\n// The {name} face, one pattern per entry of kCodes and "
            f"{height} rows\n// per pattern.\n// clang-format off\n"
            f"constexpr uint16_t kRows{stem}[] = {{\n"
        )
        for code in REPERTOIRE:
            out.write(f"    {label(code)}\n")
            values = [f"0x{value:0{digits}X}" for value in patterns[code]]
            for start in range(0, len(values), per_line):
                out.write("    " + ", ".join(values[start:start + per_line]) + ",\n")
        out.write("};\n// clang-format on\n")

    out.write("\nconstexpr NaplpsFontFace kFaces[] = {\n")
    for stem, name, width, height, _ in faces:
        out.write(
            f'    {{"{name}", {width}, {height}, kCodes, kCodeCount, '
            f"kRows{stem}}},\n"
        )
    out.write("};\n\n}  // namespace\n")

    out.write(
        """
size_t naplps_font_face_count() { return std::size(kFaces); }

const NaplpsFontFace& naplps_font_face(size_t index) {
  return kFaces[std::min(index, std::size(kFaces) - 1)];
}

const uint16_t* naplps_face_pattern(const NaplpsFontFace& face,
                                    char32_t code) {
  const char32_t* const end = face.codes + face.count;
  const char32_t* const found = std::lower_bound(face.codes, end, code);
  if (found == end || *found != code) {
    return nullptr;
  }
  return face.rows + static_cast<size_t>(found - face.codes) *
                         static_cast<size_t>(face.height);
}

}  // namespace orc
"""
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: generate_naplps_font.py <font-misc-misc-source-dir>")
    emit(sys.argv[1], sys.stdout)


if __name__ == "__main__":
    main()
