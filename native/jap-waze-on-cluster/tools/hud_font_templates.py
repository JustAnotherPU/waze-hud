#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 JustAnotherPU
"""Glyph templates for the CarPlay manoeuvre card (README.md "HUD arrows"):
iOS draws the card in SF Pro, so the glyphs are rendered from a copy of the
font you supply instead of labelled crops (no font and nothing rendered from
one ships with this repository). Two separate sets:
- the distance: dist-<text>-1.pgm lines, SF Pro Bold, digits DIGIT_PX tall;
- the street (the card's second line): str-<hex>-1.pgm, one character each
  after an 'H' (its height is the reference every glyph's height is taken
  against), SF Pro at STREET_WEIGHT, x STREET_X_PX tall. The label is its
  code points in hex (str-u00e9-1.pgm: e acute), as a case-folding file
  system would make a/A collide.
White on black. The PGMs are renders of Apple's font: install them on your
own unit only, never commit or share them.
    usage: hud_font_templates.py OUT_DIR FONT"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

LINES = ["0123456789", "km", "m", "ft", "mi", "1.5km", "0,5km"]
DIGIT_PX = 32
# Measured on a card's street line on the car: x 19 px tall; weight 700 at
# optical size 17 gives its width and its ink (2504 lit pixels for 2503),
# semibold 10% less ink.
STREET_X_PX = 19
STREET_WEIGHT = 700
STREET_OPSZ = 17
STREET_CHARS = ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789" +
                "".join(chr(c) for c in range(0xC0, 0x100) if c not in (0xD7, 0xF7)) +
                "œŒ’'-./")


def write_pgm(path, img):
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % img.size + img.tobytes())


def scaled(font_path, size, px, probe, setup, ndigits=None):
    """The font at the size where probe's ink is px tall."""
    font = ImageFont.truetype(font_path, size)
    setup(font)
    box = font.getbbox(probe)
    font = ImageFont.truetype(font_path, round(size * px / (box[3] - box[1]), ndigits))
    setup(font)
    return font


def bold(font):
    try:
        font.set_variation_by_name("Bold")
    except (OSError, ValueError):
        pass


def street_weight(font):
    try:
        font.set_variation_by_axes([100, STREET_OPSZ, 400, STREET_WEIGHT])   # width, opsz, GRAD, weight (the variable SF Pro)
    except (OSError, ValueError):
        bold(font)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: hud_font_templates.py OUT_DIR FONT (a SF Pro .ttf/.otf you are licensed to use)")
    out, font_path = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    font = scaled(font_path, 44, DIGIT_PX, "0", bold)
    for text in LINES:
        l, t, r, b = font.getbbox(text)
        img = Image.new("L", (r - l + 24, b - t + 24), 0)
        ImageDraw.Draw(img).text((12 - l, 12 - t), text, fill=255, font=font)
        write_pgm(os.path.join(out, "dist-%s-1.pgm" % text), img)
        print("dist-%s-1.pgm %dx%d" % ((text,) + img.size))

    font = scaled(font_path, 34, STREET_X_PX, "x", street_weight, 2)
    asc, desc = font.getmetrics()
    hl, _, hr, _ = font.getbbox("H")
    for ch in STREET_CHARS:
        # drawn apart, not as one string: no kerning pulls them into one part
        cl, _, cr, _ = font.getbbox(ch)
        img = Image.new("L", (hr - hl + cr - cl + 36, asc + desc + 24), 0)
        d = ImageDraw.Draw(img)
        d.text((12 - hl, 12), "H", fill=255, font=font)
        d.text((hr - hl + 24 - cl, 12), ch, fill=255, font=font)
        write_pgm(os.path.join(out, "str-%s-1.pgm" % "".join("u%04x" % ord(c) for c in ch)), img)
    print("%d str-*.pgm, x %d px" % (len(STREET_CHARS), STREET_X_PX))


if __name__ == "__main__":
    main()
