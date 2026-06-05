#!/usr/bin/env python3
"""Generate MSX VDP (TMS9918A Text Mode / Screen 0) pattern table from the
column-major 5x7 font table embedded in src/firmware/oled.c.

Source format (oled.c font5x7[96][5]):
    One entry per printable ASCII char (0x20..0x7F).
    Each entry is 5 bytes; each byte = one glyph column; bit r = row r pixel.

Output format (MSX pattern table row-major):
    96 glyphs * 8 bytes = 768 bytes.
    Each byte is one pixel row; bit 7 = leftmost pixel.
    Source column 0..4 are placed in bits 6..2 of the output byte (1-pixel
    left gap, 2-pixel right gap to fill the 6-wide Text-mode cell).
    Row 7 is always 0 (inter-line gap / descender space).

Usage:
    python3 gen_msx_font.py <oled.c path> <output font.bin path>
"""

import re
import sys
from pathlib import Path


GLYPH_RE = re.compile(
    r"\{\s*"
    r"0x([0-9A-Fa-f]{2})\s*,\s*"
    r"0x([0-9A-Fa-f]{2})\s*,\s*"
    r"0x([0-9A-Fa-f]{2})\s*,\s*"
    r"0x([0-9A-Fa-f]{2})\s*,\s*"
    r"0x([0-9A-Fa-f]{2})\s*"
    r"\}"
)


def parse_font5x7(source: str) -> list[list[int]]:
    start = source.find("font5x7")
    if start < 0:
        raise SystemExit("font5x7 table not found in oled.c")
    tail = source[start:]
    glyphs = [[int(b, 16) for b in m.groups()] for m in GLYPH_RE.finditer(tail)]
    if len(glyphs) < 96:
        raise SystemExit(
            f"expected at least 96 glyphs in font5x7, found {len(glyphs)}"
        )
    return glyphs[:96]


def convert_glyph(cols: list[int]) -> bytes:
    """5 column bytes -> 8 row bytes (MSX Text Mode cell)."""
    out = bytearray(8)
    for r in range(7):
        byte = 0
        for c in range(5):
            if (cols[c] >> r) & 1:
                byte |= 1 << (6 - c)
        out[r] = byte
    out[7] = 0
    return bytes(out)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    src_path = Path(argv[1])
    dst_path = Path(argv[2])

    glyphs = parse_font5x7(src_path.read_text())
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    with dst_path.open("wb") as f:
        for g in glyphs:
            f.write(convert_glyph(g))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
