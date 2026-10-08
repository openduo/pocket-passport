#!/usr/bin/env python3
# Copyright 2026 openduo
# SPDX-License-Identifier: FSL-1.1-Apache-2.0
"""Glyph coverage of the committed pocket font against its inventory and the UI copy.

Runs tools/pocket/check_glyphs.py on the generated font source: every UI
string in main/pocket_strings.h must be covered (strict), the inventory gaps
must be exactly the code points absent from Noto Sans SC, and a sample of
reply-like text exercises punctuation and mixed scripts.
"""
from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FONT_C = ROOT / "assets/fonts/pocket_cjk_20.c"
CHECK = ROOT / "tools/pocket/check_glyphs.py"
# Absent from the Noto Sans SC 2.004 source, so absent from the font.
KNOWN_GAPS = {"U+2017", "U+201B", "U+201F", "U+2023", "U+2024", "U+2031", "U+2034", "U+2036",
              "U+2037", "U+2038"}
SAMPLE = (
    "好的，明天下午三点提醒你给妈妈打电话。“周末”有空吗？——天气 23°C，"
    "降水 10%；地址：朝阳区 No.5（A 座）…… iPhone 15 Pro、Wi-Fi 6E、¥199、€9、×÷±\n"
)


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(CHECK), "--font-c", str(FONT_C), *args],
                          capture_output=True, text=True)


def main() -> int:
    r = run("--charset", str(ROOT / "assets/fonts/pocket_cjk_charset.txt"))
    assert r.returncode == 0, r.stderr
    gaps = set(re.findall(r"U\+[0-9A-F]{4,6}", r.stdout.split("without a glyph", 1)[-1]))
    assert gaps == KNOWN_GAPS, r.stdout

    r = run("--strict", "--text", str(ROOT / "main/pocket_strings.h"))
    assert r.returncode == 0, r.stdout + r.stderr

    with tempfile.TemporaryDirectory() as d:
        sample = Path(d) / "sample.txt"
        sample.write_text(SAMPLE, encoding="utf-8")
        r = run("--strict", "--text", str(sample))
        assert r.returncode == 0, r.stdout + r.stderr
        # Negative case: an emoji and a rare hanzi are reported, not passed.
        rare = Path(d) / "rare.txt"
        rare.write_text("\U0001F600龘", encoding="utf-8")
        r = run("--strict", "--text", str(rare))
        assert r.returncode == 1 and "U+9F98" in r.stdout and "U+1F600" in r.stdout, r.stdout
    print("pocket glyph coverage tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
