#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 JustAnotherPU
"""Suggests hud_route_uv, hud_route_ymin and hud_route_min_px from map dumps
(README.md "HUD arrows": the route gate).

    route_calibrate.py [--ymin N] --on DUMP.yuv... --off DUMP.yuv...

DUMP.yuv: what `jap-waze-on-cluster run map-dump` writes on the unit
(map-<time>-<w>x<h>.yuv, I420), --on with Waze guiding (its route line on
the map), --off with no route. The chroma is looked at as jap-waze-on-cluster
does: every 4th sample across and down, luma at least --ymin. Prints the
chroma bins (8 levels wide) the route frames have and the others do not,
each frame's count with the suggested bounds, and the local-conf lines.
Python 3 standard library only."""
import os
import re
import sys
import tempfile

BIN = 8


def samples(path, ymin):
    """[(u, v)] of the dump's looked-at chroma samples with luma >= ymin."""
    m = re.search(r"(\d+)x(\d+)\.yuv$", path)
    if not m:
        raise ValueError("%s: no <w>x<h>.yuv in the name" % path)
    w, h = int(m.group(1)), int(m.group(2))
    data = open(path, "rb").read()
    if len(data) != w * h * 3 // 2:
        raise ValueError("%s: %d bytes, not a %dx%d I420 frame" % (path, len(data), w, h))
    cw, uo, vo = w // 2, w * h, w * h + (w // 2) * (h // 2)
    out = []
    for cy in range(0, h // 2, 4):
        for cx in range(0, cw, 4):
            if data[2 * cy * w + 2 * cx] >= ymin:
                out.append((data[uo + cy * cw + cx], data[vo + cy * cw + cx]))
    return out


def hist(s):
    h = {}
    for u, v in s:
        k = (u // BIN, v // BIN)
        h[k] = h.get(k, 0) + 1
    return h


def count(s, b):
    return sum(1 for u, v in s if b[0] <= u <= b[1] and b[2] <= v <= b[3])


def suggest(on, off):
    """(bounds (umin, umax, vmin, vmax), min_px, report lines); bounds None when nothing tells them apart."""
    lines = []
    hon = [hist(s) for s in on]
    hoff = [hist(s) for s in off]
    keys = set(k for h in hon for k in h)
    # bins every route frame has and no other frame has
    cand = {k for k in keys if all(h.get(k, 0) for h in hon) and not any(h.get(k, 0) for h in hoff)}
    lines.append("bins (u, v from-to) in every route frame and no other, mean count:")
    for k in sorted(cand, key=lambda k: -sum(h.get(k, 0) for h in hon)):
        lines.append("  u %3d-%3d v %3d-%3d  %6.1f" % (k[0] * BIN, k[0] * BIN + BIN - 1, k[1] * BIN, k[1] * BIN + BIN - 1,
                                                       sum(h.get(k, 0) for h in hon) / len(hon)))
    if not cand:
        return None, 0, lines + ["  none: no colour is there only with the route (more dumps, or another --ymin)"]
    # the connected bins around the most frequent one: the line's colour, not stray ones
    top = max(cand, key=lambda k: sum(h.get(k, 0) for h in hon))
    comp, todo = {top}, [top]
    while todo:
        a, b = todo.pop()
        for k in ((a + i, b + j) for i in (-1, 0, 1) for j in (-1, 0, 1)):
            if k in cand and k not in comp:
                comp.add(k)
                todo.append(k)
    bounds = (min(k[0] for k in comp) * BIN, max(k[0] for k in comp) * BIN + BIN - 1,
              min(k[1] for k in comp) * BIN, max(k[1] for k in comp) * BIN + BIN - 1)
    con, coff = [count(s, bounds) for s in on], [count(s, bounds) for s in off]
    lines.append("with u %d-%d v %d-%d: route frames %s, others %s" % (bounds + (con, coff)))
    if min(con) <= max(coff + [0]):
        return None, 0, lines + ["  not separable by count"]
    # halfway, leaning to the route frames' fewest: a route half hidden still counts
    return bounds, max(1, (min(con) + max(coff + [0]) + 1) // 2), lines


def self_test():
    d = tempfile.mkdtemp()
    w, h = 64, 32

    def frame(name, line):
        y = bytearray([150]) * (w * h)
        u = bytearray([128]) * (w * h // 4)
        v = bytearray([128]) * (w * h // 4)
        for i in range(0, w * h // 4, 7):   # grey-green clutter, on and off alike
            u[i], v[i] = 100, 110
        if line:
            for cx in range(w // 2):
                u[8 * (w // 2) + cx], v[8 * (w // 2) + cx] = 170, 90
        p = os.path.join(d, "map-%s-%dx%d.yuv" % (name, w, h))
        open(p, "wb").write(bytes(y + u + v))
        return p

    on = [samples(frame("on%d" % i, True), 0) for i in range(2)]
    off = [samples(frame("off%d" % i, False), 0) for i in range(2)]
    b, n, _ = suggest(on, off)
    assert b and b[0] <= 170 <= b[1] and b[2] <= 90 <= b[3], b
    assert not (b[0] <= 100 <= b[1] and b[2] <= 110 <= b[3]), "the clutter is outside"
    assert 1 <= n <= count(on[0], b), n
    assert suggest(off, off)[0] is None
    try:
        samples(os.path.join(d, "x.yuv"), 0)
        raise AssertionError("a name without the size")
    except ValueError:
        pass
    print("route_calibrate.py: self-test ok")


def main(argv):
    if argv[1:] == ["--self-test"]:
        self_test()
        return 0
    ymin, on, off, cur = 0, [], [], None
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--ymin" and args:
            ymin = int(args.pop(0))
        elif a in ("--on", "--off"):
            cur = on if a == "--on" else off
        elif cur is not None:
            cur.append(a)
        else:
            cur = None
            break
    if not on or not off or cur is None:
        sys.stderr.write(__doc__)
        return 2
    son, soff = [samples(p, ymin) for p in on], [samples(p, ymin) for p in off]
    b, n, lines = suggest(son, soff)
    print("\n".join(lines))
    if not b:
        return 1
    print("\nfor jap-waze-on-cluster.local.conf:\nhud_route_uv = %d,%d,%d,%d\nhud_route_ymin = %d\nhud_route_min_px = %d" % (b + (ymin, n)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
