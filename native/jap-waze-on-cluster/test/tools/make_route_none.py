#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 JustAnotherPU
"""Writes test/data/map-route-none.yuv: a synthetic Waze night map with no
route, 1088x480 I420, for hud_route_test ("nothing, passed"). The colours
are the ones Waze's night map uses on the cluster: the background, roads,
park areas and the car's chevron (dark: its luma stays under
hud_route_bright_ymin though its chroma is the route's), and no route
colour anywhere. Python 3 standard library only; the same bytes every run.

    make_route_none.py [OUT]"""
import math
import sys

W, H = 1088, 480
BACKGROUND = (38, 138, 122)
ROAD = (86, 128, 100)
ROAD_MINOR = (84, 144, 114)
PARK = (52, 146, 122)
CHEVRON = (136, 168, 36)


def frame():
    y = bytearray([BACKGROUND[0]]) * (W * H)
    u = bytearray([BACKGROUND[1]]) * (W * H // 4)
    v = bytearray([BACKGROUND[2]]) * (W * H // 4)

    def dot(px, py, c):
        if 0 <= px < W and 0 <= py < H:
            y[py * W + px] = c[0]
            u[(py // 2) * (W // 2) + px // 2] = c[1]
            v[(py // 2) * (W // 2) + px // 2] = c[2]

    def rect(x0, y0, x1, y1, c):
        for py in range(y0, y1):
            for px in range(x0, x1):
                dot(px, py, c)

    def line(ax, ay, bx, by, r, c):
        steps = int(math.hypot(bx - ax, by - ay)) + 1
        for i in range(steps + 1):
            cx, cy = ax + (bx - ax) * i / steps, ay + (by - ay) * i / steps
            for py in range(int(cy - r), int(cy + r) + 1):
                for px in range(int(cx - r), int(cx + r) + 1):
                    if (px - cx) ** 2 + (py - cy) ** 2 <= r * r:
                        dot(px, py, c)

    rect(120, 40, 330, 170, PARK)
    rect(760, 300, 1000, 450, PARK)
    rect(640, 30, 720, 120, PARK)
    line(0, 300, 1088, 260, 7, ROAD)          # a main road across
    line(541, 480, 560, 0, 6, ROAD)           # the road the car is on
    line(200, 480, 420, 0, 4, ROAD_MINOR)
    line(700, 480, 900, 140, 4, ROAD_MINOR)
    line(0, 110, 600, 150, 3, ROAD_MINOR)
    # the chevron, pointing up, over the road at the car's place
    for py in range(232, 268):
        half = (py - 232) * 0.45
        for px in range(int(541 - half), int(541 + half) + 1):
            dot(px, py, CHEVRON)
    return bytes(y) + bytes(u) + bytes(v)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "test/data/map-route-none.yuv"
    with open(out, "wb") as f:
        f.write(frame())


if __name__ == "__main__":
    main()
