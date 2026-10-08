#!/usr/bin/env python3
# Copyright 2026 openduo
# SPDX-License-Identifier: FSL-1.1-Apache-2.0
"""Render host-side mockups of every pocket screen at 240x320.

Design review aid, not the firmware renderer: the layout constants below
mirror main/pocket_ui.c, the copy comes from main/pocket_strings.h, avatars
from tools/pocket/gen_avatars.py and text from the pinned Noto Sans SC at the
firmware's 20 px size. Writes docs/pocket/mockups/<screen>.png (2x) and
overview.png in Simplified Chinese, and the same set in English under
docs/pocket/mockups/en/. Run gen_fonts.sh and gen_avatars.py first.
"""
from __future__ import annotations

import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parents[2]
FONT = REPO / "tools/pocket/.cache/NotoSansSC-Regular-2.004.otf"
AVATARS = REPO / "tools/pocket/.cache/avatars"
W, H, RADIUS, SCALE = 240, 320, 30, 2

# Colours: dark theme tokens of the DuoDuo web app (openduo/ambient,
# packages/channel-ambient/web/tokens/colors.css).
BG = "#0e0e0d"
SURFACE = "#141413"
BRAND = "#1fd9de"
BRAND_TINT = "#0c2b2e"
KICKER = "#6ec5cb"
STRONG = "#f5f4ed"
PRIMARY = "#dddcd5"
BODY = "#b4b3ac"
MUTED = "#8d8c86"
FAINT = "#6f6e69"
HAIRLINE = "#282826"
ATTENTION = "#d9a066"
ATTENTION_TINT = "#2a2015"


def strings(lang: str) -> dict[str, str]:
    """UI copy of one language, keyed by the id without the _EN suffix."""
    text = (REPO / "main/pocket_strings.h").read_text(encoding="utf-8")
    pairs = re.findall(r'#define POCKET_STR_(\w+)\s+"([^"]*)"', text)
    if lang == "en":
        return {k[:-3]: v for k, v in pairs if k.endswith("_EN")}
    return {k: v for k, v in pairs if not k.endswith("_EN")}


# Sample brain text (shown as sent, never translated): a transcript and reply
# in the language a user of that UI would speak.
SAMPLES = {
    "zh": ("明天下午三点提醒我给妈妈打电话，顺便问问她周末有没有空",
           "好的，明天下午三点我会提醒你给妈妈打电话。她上周说过周末想去公园走走，"
           "你可以顺便问问她周六上午方不方便，天气预报说那天晴，适合出门。"),
    "en": ("Remind me to call Mom tomorrow at three and ask if she's free this weekend",
           "Sure, I'll remind you to call your mom tomorrow at 3 pm. She mentioned last week "
           "that she'd like a walk in the park this weekend, so you could ask whether Saturday "
           "morning works. The forecast says it will be sunny."),
}
OUTS = {"zh": REPO / "docs/pocket/mockups", "en": REPO / "docs/pocket/mockups/en"}
S = strings("zh")
TRANSCRIPT, REPLY = SAMPLES["zh"]


def font(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(FONT), size)


F20, F14, F44 = font(20), font(14), font(44)
LINE = 26  # pocket_cjk_20 line height


BREAK_CHARS = " ,.;:-_)}"  # CONFIG_LV_TXT_BREAK_CHARS


def lv_word_char(ch: str) -> bool:
    """LVGL's lv_text_is_a_word: CJK characters are words on their own."""
    c = ord(ch)
    return (0x4E00 <= c <= 0x9FFF or 0xFF01 <= c <= 0xFF5E or 0x3000 <= c <= 0x303F
            or 0x2E80 <= c <= 0x2FDF or 0x31C0 <= c <= 0x31EF or 0x3400 <= c <= 0x4DBF)


def words(text: str) -> list[str]:
    """Split as LVGL does: a CJK character alone, else up to a break char."""
    out, cur = [], ""
    for ch in text:
        if ch == "\n" or lv_word_char(ch):
            if cur:
                out.append(cur)
            out.append(ch)
            cur = ""
            continue
        cur += ch
        if ch in BREAK_CHARS:
            out.append(cur)
            cur = ""
    if cur:
        out.append(cur)
    return out


def wrap(d: ImageDraw.ImageDraw, text: str, f, width: int) -> list[str]:
    """Line breaks as LVGL's LV_LABEL_LONG_WRAP makes them: whole words packed
    greedily; a word wider than the line is cut at a character."""
    lines, cur = [], ""
    for w in words(text):
        if w == "\n":
            lines.append(cur)
            cur = ""
            continue
        if d.textlength((cur + w).rstrip(" "), font=f) <= width:
            cur += w
            continue
        if cur:
            lines.append(cur.rstrip(" "))
            cur = ""
            w = w.lstrip(" ")
        for ch in w:  # a word longer than the line
            if d.textlength(cur + ch, font=f) > width and cur:
                lines.append(cur)
                cur = ""
            cur += ch
    lines.append(cur.rstrip(" "))
    return lines


def text_block(d, text, y, color, f=F20, width=200, center=True, x=20, max_lines=None):
    lines = wrap(d, text, f, width)
    if max_lines and len(lines) > max_lines:
        lines = lines[:max_lines]
        lines[-1] = lines[-1][:-1] + "…"
    for i, line in enumerate(lines):
        lx = (W - d.textlength(line, font=f)) / 2 if center else x
        d.text((lx, y + i * LINE), line, font=f, fill=color)
    return y + len(lines) * LINE


def status_bar(d, linked=True, battery=87):
    d.ellipse((28, 15, 36, 23), fill=BRAND if linked else FAINT)
    t = f"{battery}%"
    d.text((W - 28 - d.textlength(t, font=F14), 10), t, font=F14, fill=MUTED)


def avatar(img, state, size="large", xy=None):
    a = Image.open(AVATARS / f"{state}-{176 if size == 'large' else 64}.png").convert("RGB")
    if xy is None:
        xy = ((W - a.width) // 2, 40)
    img.paste(a, xy)


def hint(d, text):
    text_block(d, text, 282, FAINT)


def state_screen(img, d, state, title, sub=None, hint_text=None, title_color=STRONG,
                 sub_color=MUTED, linked=True):
    status_bar(d, linked)
    avatar(img, state)
    y = text_block(d, title, 176, title_color)
    if sub:
        text_block(d, sub, y + 4, sub_color, max_lines=2)
    if hint_text:
        hint(d, hint_text)


def scr_pairing(img, d):
    state_screen(img, d, "received", S["PAIR_TITLE"], S["PAIR_SUB"], linked=False)
    t = "DuoDuo Pocket 3F2A"
    d.text(((W - d.textlength(t, font=F14)) / 2, 284), t, font=F14, fill=FAINT)


def scr_pair_confirm(img, d):
    status_bar(d, False)
    text_block(d, S["PAIR_CODE_TITLE"], 64, MUTED)
    code = "482 917"
    d.text(((W - d.textlength(code, font=F44)) / 2, 104), code, font=F44, fill=BRAND)
    text_block(d, S["PAIR_CODE_SUB"], 182, STRONG, max_lines=2)
    hint(d, S["PAIR_CODE_HINT"])


def scr_offline(img, d):
    state_screen(img, d, "offline", S["OFFLINE_TITLE"], S["OFFLINE_SUB"], linked=False)


def scr_offline_refused(img, d):
    state_screen(img, d, "offline", S["OFFLINE_TITLE"], S["OFFLINE_REFUSED"],
                 title_color=ATTENTION, sub_color=ATTENTION, linked=False)


def scr_home(img, d):
    state_screen(img, d, "received", S["HOME_TITLE"], S["HOME_SUB"], S["HINT_SETTINGS"])


def scr_recording(img, d):
    status_bar(d)
    avatar(img, "listening")
    text_block(d, S["REC_TITLE"], 176, STRONG)
    clock = "0:07"
    d.text(((W - d.textlength(clock, font=F20)) / 2, 206), clock, font=F20, fill=BRAND)
    d.rounded_rectangle((40, 244, 200, 250), radius=3, fill=SURFACE)
    d.rounded_rectangle((40, 244, 40 + 96, 250), radius=3, fill=BRAND)
    hint(d, S["REC_HINT"])


def scr_sending(img, d):
    state_screen(img, d, "heard", S["SENDING_TITLE"])


def header(img, d, state, title, color=STRONG):
    status_bar(d)
    avatar(img, state, "small", (20, 36))
    d.text((94, 46), title, font=F20, fill=color)
    d.line((20, 88, 220, 88), fill=HAIRLINE)


def working(state, title, lit):
    """Working state: phase label, three dots with one lit."""
    def draw(img, d):
        header(img, d, state, S[title])
        x = 94 + d.textlength(S[title], font=F20) + 10
        for i in range(3):
            fill = BRAND if i == lit else "#134b4c"  # brand at LV_OPA_30 over BG
            d.ellipse((x + i * 12, 58, x + i * 12 + 6, 64), fill=fill)
        d.text((20, 98), S["YOU_SAID"], font=F20, fill=KICKER)
        text_block(d, TRANSCRIPT, 126, BODY, center=False, max_lines=6)
    return draw


def reply_body(d, scroll_lines=0):
    lines = wrap(d, REPLY, F20, 196)
    visible = lines[scroll_lines:scroll_lines + 7]
    for i, line in enumerate(visible):
        d.text((20, 96 + i * LINE + 2), line, font=F20, fill=PRIMARY)
    total, shown = len(lines), 7
    if total > shown:
        track_top, track_h = 96, 186
        bar_h = max(16, track_h * shown // total)
        top = track_top + (track_h - bar_h) * scroll_lines // max(1, total - shown)
        d.rounded_rectangle((226, top, 229, top + bar_h), radius=2, fill="#2a6c6f")


def scr_reply(img, d):
    header(img, d, "reply", S["REPLY_NAME"])
    reply_body(d)


def scr_reply_streaming(img, d):
    header(img, d, "generating", S["REPLY_STREAMING"], KICKER)
    reply_body(d)


def scr_reply_scrolled_banner(img, d):
    header(img, d, "reply", S["REPLY_NAME"])
    reply_body(d, scroll_lines=3)
    d.rounded_rectangle((36, 284, 204, 312), radius=8, fill=ATTENTION_TINT)
    t = S["BANNER_SERVER"]
    d.text(((W - d.textlength(t, font=F20)) / 2, 285), t, font=F20, fill=ATTENTION)


def notice(state, title, sub):
    def draw(img, d):
        state_screen(img, d, state, S[title], S[sub], S["NOTICE_HINT"])
    return draw


def scr_settings(img, d):
    status_bar(d)
    d.text((24, 40), S["SETTINGS_TITLE"], font=F20, fill=STRONG)
    rows = [(S["SET_PREROLL"], S["ON"]), (S["SET_BRIGHTNESS"], "60%"),
            (S["SET_SCREEN_OFF"], "30 " + S["SECONDS"]), (S["SET_REPAIR"], ""),
            (S["SET_BACK"], "")]
    for i, (label, value) in enumerate(rows):
        y = 72 + i * 40  # SET_ROW_Y, SET_ROW_PITCH in main/pocket_ui.c
        sel = i == 1
        if sel:
            d.rounded_rectangle((16, y, 224, y + 36), radius=8, fill=BRAND_TINT)
            d.rectangle((16, y + 6, 19, y + 30), fill=BRAND)
        d.text((30, y + 5), label, font=F20, fill=STRONG if sel else BODY)
        if value:
            d.text((210 - d.textlength(value, font=F20), y + 5), value, font=F20,
                   fill=BRAND if sel else MUTED)
    hint(d, S["SETTINGS_HINT"])


def scr_repair(img, d):
    status_bar(d)
    text_block(d, S["REPAIR_TITLE"], 72, STRONG)
    text_block(d, S["REPAIR_SUB"], 116, MUTED, max_lines=5)
    hint(d, S["REPAIR_HINT"])


def scr_proto(img, d):
    state_screen(img, d, "sensesoff", S["PROTO_TITLE"], S["PROTO_SUB"], title_color=ATTENTION)


SCREENS = [
    ("01-pairing", scr_pairing),
    ("02-pair-confirm", scr_pair_confirm),
    ("03-offline", scr_offline),
    ("04-offline-press-refused", scr_offline_refused),
    ("05-home", scr_home),
    ("06-recording", scr_recording),
    ("07-sending", scr_sending),
    ("08-working-received", working("heard", "WORK_RECEIVED", 0)),
    ("09-reply-streaming", scr_reply_streaming),
    ("10-reply", scr_reply),
    ("11-reply-scrolled-server-banner", scr_reply_scrolled_banner),
    ("12-notice-empty", notice("deaf", "EMPTY_TITLE", "EMPTY_SUB")),
    ("13-notice-asr-failed", notice("deaf", "ASR_TITLE", "ASR_SUB")),
    ("14-notice-send-failed", notice("sensesoff", "SEND_TITLE", "SEND_SUB")),
    ("15-notice-server", notice("sensesoff", "SERVER_TITLE", "SERVER_SUB")),
    ("16-settings", scr_settings),
    ("17-repair-confirm", scr_repair),
    ("18-protocol-mismatch", scr_proto),
    ("19-working-thinking", working("thinking", "WORK_THINKING", 1)),
    ("20-working-tool", working("thinking", "WORK_TOOL", 2)),
]


def render(fn) -> Image.Image:
    img = Image.new("RGB", (W, H), BG)
    fn(img, ImageDraw.Draw(img))
    # The panel masks pixels outside the rounded rectangle to black (bsp_display.h).
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, W - 1, H - 1), radius=RADIUS, fill=255)
    out = Image.new("RGB", (W, H), "#000000")
    out.paste(img, (0, 0), mask)
    return out


def main() -> int:
    global S, TRANSCRIPT, REPLY
    for lang, out in OUTS.items():
        S = strings(lang)
        TRANSCRIPT, REPLY = SAMPLES[lang]
        render_set(out)
    return 0


def render_set(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    shots = []
    for name, fn in SCREENS:
        img = render(fn)
        img.resize((W * SCALE, H * SCALE), Image.NEAREST).save(out / f"{name}.png", optimize=True)
        shots.append((name, img))
    cols, pad, label_h = 6, 12, 22
    rows = (len(shots) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (W + pad) + pad, rows * (H + pad + label_h) + pad), "#2a2a28")
    d = ImageDraw.Draw(sheet)
    for i, (name, img) in enumerate(shots):
        x = pad + (i % cols) * (W + pad)
        y = pad + (i // cols) * (H + pad + label_h)
        sheet.paste(img, (x, y + label_h))
        d.text((x, y), name, font=F14, fill="#dddcd5")
    sheet.save(out / "overview.png", optimize=True)
    print(f"wrote {len(shots)} screens to {out.relative_to(REPO)}")


if __name__ == "__main__":
    raise SystemExit(main())
