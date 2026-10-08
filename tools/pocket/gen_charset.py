#!/usr/bin/env python3
# Copyright 2026 openduo
# SPDX-License-Identifier: FSL-1.1-Apache-2.0
"""Write the pocket UI character inventory, one code-point range per line.

Inventory: printable ASCII, every character of GB2312 (6763 hanzi plus its
symbols, punctuation, Latin, Greek, Cyrillic and kana rows), the common
punctuation blocks Chinese replies use outside GB2312, and the replacement
glyph U+25A1 used for anything uncovered (see main/pocket_text.h).
Deterministic: derived from Python's gb2312 codec, no external list.
"""
from __future__ import annotations

import sys
from pathlib import Path

EXTRA_RANGES = [
    (0x20, 0x7E),      # printable ASCII
    (0xA0, 0xFF),      # Latin-1 punctuation and letters (e.g. ° · é)
    (0x2010, 0x2027),  # dashes, quotes, ellipsis
    (0x2030, 0x203B),  # per mille, primes, reference mark
    (0x20AC, 0x20AC),  # euro sign
    (0x2190, 0x2193),  # arrows
    (0x3000, 0x3029),  # CJK symbols and punctuation, up to the Hangzhou numerals
    (0x3030, 0x3030),  # wavy dash; 302A-302F tone marks and 3031-3035 vertical
    (0x3036, 0x303F),  # repeat marks are left out: they triple the line height
    (0xFF01, 0xFF5E),  # full-width ASCII forms
    (0xFFE0, 0xFFE5),  # full-width currency signs
    (0x25A1, 0x25A1),  # replacement glyph
]


def gb2312_codepoints() -> set[int]:
    cps: set[int] = set()
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes((hi, lo)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            cps.add(ord(ch))
    return cps


def inventory() -> list[int]:
    cps = gb2312_codepoints()
    for lo, hi in EXTRA_RANGES:
        cps.update(range(lo, hi + 1))
    cps.discard(0xA0)  # no-break space renders as space; keep the set printable
    return sorted(cps)


def ranges(cps: list[int]) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []
    for cp in cps:
        if out and out[-1][1] == cp - 1:
            out[-1] = (out[-1][0], cp)
        else:
            out.append((cp, cp))
    return out


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: gen_charset.py <output.txt>", file=sys.stderr)
        return 2
    cps = inventory()
    lines = [f"0x{a:04X}-0x{b:04X}" if a != b else f"0x{a:04X}" for a, b in ranges(cps)]
    Path(sys.argv[1]).write_text(
        f"# pocket CJK font inventory: {len(cps)} code points, {len(lines)} ranges\n"
        + "\n".join(lines) + "\n", encoding="utf-8")
    print(f"{len(cps)} code points in {len(lines)} ranges")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
