"""Generate main/font5x7.h from the glyph art below.

Run it after editing a glyph:

    python tools/make_font.py

The point of this file is that a font is *data*, and data you cannot read is
data you cannot check. Written directly as C hex, a wrong bit in a glyph is
invisible until it is on the screen and you are wondering why the 8 looks
odd. Written as art, it is wrong in a way you can see here. The generated
header is committed too, so building the firmware never requires Python.

Encoding, which is the ordinary one for small bitmap fonts: five bytes per
glyph, one per column, left to right. Within a byte, bit 0 is the top row and
bit 6 the bottom -- so a column is read downward. Column-major suits the draw
code, which walks a row at a time across all five columns.

The table covers ASCII 32..90 (space through 'Z') as one contiguous block, so
a lookup is a subtraction with no search. Characters in that range without art
below get the "unknown" box, which is deliberately conspicuous: a missing
glyph should look like a missing glyph, not like a space. Lowercase is mapped
to uppercase by the draw code rather than doubling the table -- see gc9a01.c.
"""

import os

WIDTH, HEIGHT = 5, 7
FIRST, LAST = 32, 90            # ' ' .. 'Z'

# A glyph is seven strings of exactly five characters. '#' is on, anything
# else is off, so '.' can be used for readability where blanks get confusing.
UNKNOWN = [
    "#####",
    "#   #",
    "#   #",
    "#   #",
    "#   #",
    "#   #",
    "#####",
]

GLYPHS = {
    " ": ["     "] * 7,
    "0": [" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "],
    "1": ["  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "],
    "2": [" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"],
    "3": ["#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### "],
    "4": ["   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "],
    "5": ["#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "],
    "6": ["  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### "],
    "7": ["#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "],
    "8": [" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "],
    "9": [" ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  "],
    "%": ["##  #", "##  #", "   # ", "  #  ", " #   ", "#  ##", "#  ##"],
    ":": ["     ", "  #  ", "  #  ", "     ", "  #  ", "  #  ", "     "],
    "-": ["     ", "     ", "     ", "#####", "     ", "     ", "     "],
    ".": ["     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "],
    "!": ["  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  "],
    "?": [" ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  "],
    "A": [" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"],
    "B": ["#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "],
    "C": [" ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "],
    "D": ["#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "],
    "E": ["#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"],
    "F": ["#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "],
    "G": [" ### ", "#   #", "#    ", "#  ##", "#   #", "#   #", " ### "],
    "H": ["#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"],
    "I": [" ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "],
    "J": ["    #", "    #", "    #", "    #", "#   #", "#   #", " ### "],
    "K": ["#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"],
    "L": ["#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"],
    "M": ["#   #", "## ##", "# # #", "#   #", "#   #", "#   #", "#   #"],
    "N": ["#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"],
    "O": [" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "],
    "P": ["#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "],
    "Q": [" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"],
    "R": ["#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"],
    "S": [" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "],
    "T": ["#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "],
    "U": ["#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "],
    "V": ["#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "],
    "W": ["#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"],
    "X": ["#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"],
    "Y": ["#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "],
    "Z": ["#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"],
}


def check(name, rows):
    """A malformed glyph must fail here, not silently ship a wrong shape."""
    if len(rows) != HEIGHT:
        raise SystemExit(f"glyph {name!r}: {len(rows)} rows, expected {HEIGHT}")
    for i, row in enumerate(rows):
        if len(row) != WIDTH:
            raise SystemExit(
                f"glyph {name!r} row {i}: {len(row)} chars, expected {WIDTH}"
            )


def to_columns(rows):
    """Art -> five column bytes, bit 0 = top row."""
    cols = []
    for x in range(WIDTH):
        byte = 0
        for y in range(HEIGHT):
            if rows[y][x] == "#":
                byte |= 1 << y
        cols.append(byte)
    return cols


def main():
    check("<unknown>", UNKNOWN)
    for name, rows in GLYPHS.items():
        check(name, rows)

    here = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.join(here, "..", "main", "font5x7.h")

    lines = [
        "/* GENERATED by tools/make_font.py -- do not edit by hand.",
        " *",
        " * Edit the glyph art in that script and re-run it instead; the art is",
        " * readable and checkable, and five columns of hex are neither.",
        " *",
        " * Five bytes per glyph, one per column, left to right. Bit 0 is the top",
        " * row, bit 6 the bottom. Covers ASCII 32..90 as one contiguous block, so",
        " * a lookup is `c - 32` with no search; characters in range with no art",
        " * defined get a conspicuous box rather than a blank.",
        " */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define FONT5X7_FIRST {FIRST}",
        f"#define FONT5X7_LAST  {LAST}",
        f"#define FONT5X7_W     {WIDTH}",
        f"#define FONT5X7_H     {HEIGHT}",
        "",
        f"static const uint8_t font5x7[{LAST - FIRST + 1}][{WIDTH}] = {{",
    ]

    defined = 0
    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        rows = GLYPHS.get(ch)
        if rows is None:
            rows, label = UNKNOWN, "(undefined)"
        else:
            defined += 1
            label = "space" if ch == " " else ch
        cols = ", ".join(f"0x{b:02X}" for b in to_columns(rows))
        lines.append(f"    {{ {cols} }},   /* {code:3d}  {label} */")

    lines.append("};")
    lines.append("")
    lines.append("/* For characters outside the table entirely -- anything above 'Z', and")
    lines.append(" * every byte of a multi-byte UTF-8 sequence. The draw code needs somewhere")
    lines.append(" * to point that is not a wrong glyph and not a blank. */")
    unknown_cols = ", ".join(f"0x{b:02X}" for b in to_columns(UNKNOWN))
    lines.append(f"static const uint8_t font5x7_unknown[{WIDTH}] = {{ {unknown_cols} }};")
    lines.append("")

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    total = LAST - FIRST + 1
    print(f"wrote {os.path.normpath(out_path)}")
    print(f"  {total} entries, {defined} with real art, "
          f"{total - defined} using the box")
    print(f"  {total * WIDTH} bytes of flash")

    # Preview, so a broken glyph is visible in the terminal rather than only
    # on the panel.
    print("\npreview:")
    for group in ("0123456789", "ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "%:-.!?"):
        art = [GLYPHS[c] for c in group]
        for y in range(HEIGHT):
            # Plain ASCII on purpose: a Windows console running a legacy
            # codepage cannot encode block-drawing characters, and a preview
            # that crashes the generator is worse than a plain one.
            print("  " + "  ".join(g[y] for g in art))
        print()


if __name__ == "__main__":
    main()
