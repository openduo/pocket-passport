#!/usr/bin/env python3
# Copyright 2026 openduo
# SPDX-License-Identifier: FSL-1.1-Apache-2.0
"""Glyph coverage check for the generated LVGL font.

Reads the glyph list lv_font_conv writes into the font source (one
`/* U+XXXX "c" */` comment per bitmap) and checks code points against it:

  --charset FILE   every code point of the inventory must be present
  --text FILE...   every printable code point of UTF-8 text files is either
                   present or reported (the firmware shows U+25A1 for it)
  --strict         exit 1 when any --text code point is missing

A built-in negative case (U+9F98, outside the inventory) must be reported as
missing, so the check cannot pass unconditionally.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

GLYPH_RE = re.compile(r"/\* U\+([0-9A-F]{4,6}) ")
NEGATIVE = 0x9F98


def font_glyphs(path: Path) -> set[int]:
    return {int(m, 16) for m in GLYPH_RE.findall(path.read_text(encoding="utf-8"))}


def charset(path: Path) -> set[int]:
    cps: set[int] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        a, _, b = line.partition("-")
        cps.update(range(int(a, 16), int(b or a, 16) + 1))
    return cps


def text_codepoints(path: Path) -> set[int]:
    text = path.read_text(encoding="utf-8")
    return {ord(c) for c in text if ord(c) >= 0x20 and not (0x7F <= ord(c) < 0xA0)}


def fmt(cps: set[int], limit: int = 40) -> str:
    items = [f"U+{cp:04X} {chr(cp)!r}" for cp in sorted(cps)[:limit]]
    more = f" ... (+{len(cps) - limit})" if len(cps) > limit else ""
    return ", ".join(items) + more


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--font-c", required=True, type=Path)
    ap.add_argument("--charset", type=Path)
    ap.add_argument("--text", type=Path, nargs="*", default=[])
    ap.add_argument("--source-font", type=Path, help="explain gaps with the source cmap")
    ap.add_argument("--strict", action="store_true")
    args = ap.parse_args()

    glyphs = font_glyphs(args.font_c)
    if not glyphs:
        print(f"ERROR: no glyph comments found in {args.font_c}", file=sys.stderr)
        return 1
    print(f"{args.font_c.name}: {len(glyphs)} glyphs")
    if NEGATIVE in glyphs:
        print(f"ERROR: negative case U+{NEGATIVE:04X} unexpectedly present", file=sys.stderr)
        return 1
    status = 0

    if args.charset:
        want = charset(args.charset)
        missing = want - glyphs
        if missing:
            note = ""
            if args.source_font:
                from fontTools.ttLib import TTFont  # optional dependency
                cmap = TTFont(args.source_font).getBestCmap()
                absent = {cp for cp in missing if cp not in cmap}
                note = f" ({len(absent)} absent from the source font)"
                if absent == missing:
                    note += "; inventory otherwise complete"
            print(f"inventory: {len(want)} code points, {len(missing)} without a glyph{note}: "
                  f"{fmt(missing)}")
        else:
            print(f"inventory: all {len(want)} code points present")

    for path in args.text:
        cps = text_codepoints(path)
        missing = cps - glyphs
        if missing:
            print(f"{path}: {len(cps)} code points, {len(missing)} shown as U+25A1: {fmt(missing)}")
            if args.strict:
                status = 1
        else:
            print(f"{path}: all {len(cps)} code points covered")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
