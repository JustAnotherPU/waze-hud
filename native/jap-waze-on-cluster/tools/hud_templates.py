#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 JustAnotherPU
"""Turns labelled banner crops into the HUD template folder (README.md "HUD arrows").

    hud_templates.py LABELLED OUT

LABELLED holds the PNG crops the unit dumped (/tmp/jap-woc-hud), sorted by hand:
    LABELLED/icon/<manoeuvre>/*.png    an icon crop (NNNN-icon-*.png)
    LABELLED/dist/<text>/*.png         a distance crop (NNNN-dist-*.png) reading <text>,
                                       spaces left out: 300m, 1.2km, 800ft, 0.5mi
OUT gets icon-<manoeuvre>-<n>.pgm and dist-<text>-<n>.pgm, for
/var/opt/jap-waze-on-cluster/hud-templates/ on the unit. Python 3 standard
library only; reads 8-bit, non-interlaced PNGs (grey, RGB, with or without
alpha)."""
import os
import struct
import sys
import tempfile
import zlib

MANOEUVRES = {"straight", "slight_left", "left", "sharp_left", "slight_right", "right", "sharp_right",
              "uturn_left", "uturn_right", "keep_left", "keep_right", "merge", "arrive"} | \
             {"roundabout_%d" % n for n in range(1, 10)}
DIST_CHARS = set("0123456789.,mkfti")


def read_png(path):
    """(w, h, luma bytes)"""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, hdr = 8, b"", None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    w, h, depth, ctype, _, _, interlace = hdr
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or channels is None or interlace:
        raise ValueError("only 8-bit non-interlaced grey/RGB(A) PNGs")
    raw, stride = zlib.decompress(idat), w * channels
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):
        f, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b, c = prev[x], prev[x - channels] if x >= channels else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    out = bytearray()
    for line in rows:
        for x in range(w):
            px = line[x * channels:(x + 1) * channels]
            out.append(px[0] if channels < 3 else (77 * px[0] + 150 * px[1] + 29 * px[2]) >> 8)
    return w, h, bytes(out)


def write_pgm(path, w, h, luma):
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (w, h) + luma)


def convert(labelled, out):
    os.makedirs(out, exist_ok=True)
    made, problems = 0, []
    for kind in ("icon", "dist"):
        top = os.path.join(labelled, kind)
        if not os.path.isdir(top):
            continue
        for label in sorted(os.listdir(top)):
            folder = os.path.join(top, label)
            if not os.path.isdir(folder):
                continue
            text = label.replace(" ", "")
            if kind == "icon" and label not in MANOEUVRES:
                problems.append("%s: not a manoeuvre (%s)" % (folder, ", ".join(sorted(MANOEUVRES))))
                continue
            if kind == "dist" and (not text or set(text) - DIST_CHARS or "-" in text):
                problems.append("%s: a distance is digits, . or , and m, km, ft or mi" % folder)
                continue
            n = 0
            for name in sorted(os.listdir(folder)):
                if not name.lower().endswith(".png"):
                    continue
                try:
                    w, h, luma = read_png(os.path.join(folder, name))
                except (ValueError, zlib.error, struct.error, TypeError) as e:
                    problems.append("%s/%s: %s" % (folder, name, e))
                    continue
                n += 1
                write_pgm(os.path.join(out, "%s-%s-%d.pgm" % (kind, text, n)), w, h, luma)
                made += 1
    return made, problems


def png(w, h, rows, ctype=0, filt=0):
    """A PNG of raw rows, every row with filter filt (for the self-test)."""
    raw = b"".join(bytes([filt]) + r for r in rows)
    def chunk(k, b):
        return struct.pack(">I", len(b)) + k + b + struct.pack(">I", zlib.crc32(k + b) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def self_test():
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "in/icon/left"))
        os.makedirs(os.path.join(d, "in/icon/sideways"))
        os.makedirs(os.path.join(d, "in/dist/1.2km"))
        open(os.path.join(d, "in/icon/left/a.png"), "wb").write(png(2, 2, [b"\x00\xff", b"\x10\x20"]))
        # filter 1 (sub) RGB: the second pixel is the first plus its bytes
        open(os.path.join(d, "in/dist/1.2km/b.png"), "wb").write(png(2, 1, [b"\xff\xff\xff\x01\x01\x01"], 2, 1))
        open(os.path.join(d, "in/icon/sideways/c.png"), "wb").write(png(1, 1, [b"\x00"]))
        made, problems = convert(os.path.join(d, "in"), os.path.join(d, "out"))
        icon = open(os.path.join(d, "out/icon-left-1.pgm"), "rb").read()
        dist = open(os.path.join(d, "out/dist-1.2km-1.pgm"), "rb").read()
        assert made == 2, made
        assert icon == b"P5\n2 2\n255\n\x00\xff\x10\x20", icon
        assert dist == b"P5\n2 1\n255\n\xff\x00", dist
        assert len(problems) == 1 and "sideways" in problems[0], problems
    print("ok   hud_templates.py: PNG crops to labelled PGM templates, a bad label reported")


def main(argv):
    if argv[1:] == ["--self-test"]:
        self_test()
        return 0
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    made, problems = convert(argv[1], argv[2])
    for p in problems:
        print("skipped " + p)
    print("%d templates in %s" % (made, argv[2]))
    return 0 if made else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
