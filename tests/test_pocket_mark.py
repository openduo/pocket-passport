#!/usr/bin/env python3
# Copyright 2026 openduo
# SPDX-License-Identifier: FSL-1.1-Apache-2.0
"""The committed mark images must be drawable in place by LVGL.

LVGL converts an image to an aligned copy per draw when its row stride misses
LV_DRAW_BUF_STRIDE_ALIGN (lv_image_decoder.c); the copy of a 132 px frame
needs ~38 KB, more than the LVGL pool at the time (20 KB
CONFIG_LV_MEM_SIZE_KILOBYTES in sdkconfig.defaults), and the failed
LV_ASSERT_MALLOC spins the LVGL task
forever (screen and keys freeze on the pairing screen). The generator pads
every row (tools/pocket/gen_mark.py); this test keeps the committed images
honest: stride aligned, data size matching, RGB565 row payload intact.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MARK_C = ROOT / "assets/images/mark/pocket_mark.c"
STRIDE_ALIGN = 32  # LVGL default LV_DRAW_BUF_STRIDE_ALIGN

DSC = re.compile(
    r"static const lv_image_dsc_t (\w+) = \{\s*\.header\.magic[^,]*,\s*\.header\.cf = (\w+),"
    r"\s*\.header\.w = (\d+),\s*\.header\.h = (\d+),\s*\.header\.stride = (\d+),"
    r"\s*\.data_size = sizeof\((\w+)\)")
MAPS = re.compile(r"static const uint8_t (\w+)\[\] = \{(.*?)\};", re.S)


def main() -> int:
    src = MARK_C.read_text(encoding="utf-8")
    maps = {name: len(re.findall(r"0x[0-9a-fA-F]{2}", body)) for name, body in MAPS.findall(src)}
    dscs = DSC.findall(src)
    assert maps and dscs, "no mark images found"
    for name, cf, w, h, stride, map_name in dscs:
        w, h, stride = int(w), int(h), int(stride)
        assert cf == "LV_COLOR_FORMAT_RGB565", f"{name}: {cf}"
        assert stride % STRIDE_ALIGN == 0, f"{name}: stride {stride} misses {STRIDE_ALIGN}-byte alignment"
        assert stride >= w * 2, f"{name}: stride {stride} < row payload {w * 2}"
        assert maps[map_name] == stride * h, (
            f"{name}: data {maps[map_name]} B != stride*h {stride * h} B")
        print(f"{name}: {w}x{h} RGB565 stride {stride} ({maps[map_name]} B) OK")
    print(f"pocket mark image tests: PASS ({len(dscs)} images)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
