// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
/* Host test of the route ahead traced on Waze's cluster map (hud_route.h).
 * Frames from the car (2026-11-08, night, CarPlay in front; I420 as `run
 * map-dump` writes them; the street names and road numbers painted over,
 * the boxes kept as boxes; map-route-short rebuilt from its .ppm: on
 * another frame so rebuilt, about 0.5 % of samples off by 5 or more):
 * - map-route-right: the route up from the chevron into a street box, out
 *   at its top left and off up to the right (a hairpin onto a ramp, Waze's
 *   white arrow at 430,82): right, judged across the box, which is the
 *   label; the card said right 450 m;
 * - map-route-short: up into a street box, out at its top left and up to
 *   the left, about 39 degrees: a bend short of a side (side_deg 45), while
 *   the card said right 190 m (unexplained: README.md "HUD arrows", why
 *   hud_geo_next is off); the bend where the route goes into the box;
 * - map-route-left: the same junction, another route, out of the box's
 *   left side: left;
 * - map-route-none, synthetic (test/tools/make_route_none.py: Waze's night
 *   background, roads, parks and the dark chevron, no route colour):
 *   nothing, passed (no route ahead);
 * - drawn frames: a straight route (found, no bend), a right angle turn
 *   with no box (right, the bend at the corner), up then up-left (left),
 *   the route leaving the chevron sideways or bending within 40 px of it
 *   (passed), a box over the seed window;
 * - geo_json; the cost of one read. */
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../src/hud_route.h"

using namespace woc;

static int failures;

static void ok(const std::string &name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name.c_str());
    if (!cond) failures++;
}

static const unsigned W = 1088, H = 480;

/* An I420 file as the decoder's atlas (yuv_atlas.h: luma, then each chroma
 * row its U then its V). */
static bool load(const std::string &path, std::vector<uint8_t> *a)
{
    std::vector<uint8_t> f((size_t)W * H * 3 / 2);
    FILE *fp = std::fopen(path.c_str(), "rb");
    const bool got = fp && std::fread(f.data(), 1, f.size(), fp) == f.size();
    if (fp) std::fclose(fp);
    if (!got) return false;
    a->assign(f.begin(), f.begin() + (size_t)W * H);
    a->resize((size_t)W * H * 3 / 2);
    for (unsigned r = 0; r < H / 2; r++)
        for (unsigned c = 0; c < W / 2; c++) {
            (*a)[(size_t)W * H + (size_t)r * W + c] = f[(size_t)W * H + (size_t)r * (W / 2) + c];
            (*a)[(size_t)W * H + (size_t)r * W + W / 2 + c] = f[(size_t)W * H * 5 / 4 + (size_t)r * (W / 2) + c];
        }
    return true;
}

/* A dark map atlas. */
static std::vector<uint8_t> dark()
{
    std::vector<uint8_t> a((size_t)W * H * 3 / 2, 40);
    std::fill(a.begin() + (size_t)W * H, a.end(), 128);
    return a;
}

/* A disc of colour (y, u, v) of radius r at every 2 px from a to b. */
static void paint(std::vector<uint8_t> *a, float ax, float ay, float bx, float by, float r, int y, int u, int v)
{
    const int steps = (int)(std::hypot(bx - ax, by - ay) / 2) + 1;
    for (int s = 0; s <= steps; s++) {
        const float px = ax + (bx - ax) * s / steps, py = ay + (by - ay) * s / steps;
        for (int yy = (int)(py - r); yy <= (int)(py + r); yy++)
            for (int xx = (int)(px - r); xx <= (int)(px + r); xx++) {
                if (xx < 0 || yy < 0 || xx >= (int)W || yy >= (int)H || std::hypot(xx - px, yy - py) > r) continue;
                (*a)[(size_t)yy * W + xx] = (uint8_t)y;
                const size_t c = (size_t)W * H + (size_t)(yy / 2) * W + xx / 2;
                (*a)[c] = (uint8_t)u;
                (*a)[c + W / 2] = (uint8_t)v;
            }
    }
}
static void route(std::vector<uint8_t> *a, float ax, float ay, float bx, float by) { paint(a, ax, ay, bx, by, 8, 172, 176, 40); }

static std::string show(const GeoReading &g)
{
    return (g.found ? "found " : "none ") + geo_json(g) + (g.seed_covered ? " seed covered" : "");
}

int main()
{
    RouteTraceConfig cfg;
    RouteTrace rt(cfg);
    std::vector<uint8_t> a;
    struct Case { const char *file; int side; } cases[] = {
        {"test/data/map-route-right.yuv", 1}, {"test/data/map-route-short.yuv", 0}, {"test/data/map-route-left.yuv", -1}};
    GeoReading on3, m219;
    for (const Case &c : cases) {
        if (!load(c.file, &a)) {
            ok(std::string("cannot read ") + c.file, false);
            continue;
        }
        const GeoReading g = rt.read(a.data(), W, H);
        std::printf("     %s: %s\n", c.file, show(g).c_str());
        ok(std::string(c.file) + ": " + (c.side < 0 ? "left" : c.side > 0 ? "right" : "a bend short of a side"),
           g.found && g.bend && g.side == c.side && !g.passed);
        ok(std::string(c.file) + ": a label box, the bend at its bottom edge",
           g.has_label && g.x >= (int)g.label.x && g.x <= (int)(g.label.x + g.label.w) &&
               std::abs(g.y - (int)(g.label.y + g.label.h)) <= 16 && g.conf > 0);
        if (c.side == 1) on3 = g;
        if (std::string(c.file).find("route-short") != std::string::npos) m219 = g;
    }
    ok("map-route-right: the box (about 464-624 x 88-140)",
       std::abs((int)on3.label.x - 464) <= 12 && std::abs((int)on3.label.y - 88) <= 12 && std::abs((int)(on3.label.x + on3.label.w) - 628) <= 12);
    ok("map-route-short: the box over the junction (about 490-690 x 125-185), not the farther one",
       std::abs((int)m219.label.x - 492) <= 12 && std::abs((int)(m219.label.y + m219.label.h) - 186) <= 12);
    if (load("test/data/map-route-none.yuv", &a)) {
        const GeoReading g = rt.read(a.data(), W, H);
        std::printf("     map-route-none: %s\n", show(g).c_str());
        ok("map-route-none: no route ahead: nothing", !g.found && !g.bend && g.side == 0 && g.conf == 0 && g.passed);
    } else {
        ok("cannot read test/data/map-route-none.yuv", false);
    }

    const float cx = cfg.car_x, cy = cfg.car_y;
    a = dark();
    route(&a, cx, H, cx, 20);
    GeoReading g = rt.read(a.data(), W, H);
    std::printf("     straight: %s\n", show(g).c_str());
    ok("drawn: straight on: found, no bend, no side", g.found && !g.bend && g.side == 0 && g.conf == 60 && !g.passed);
    a = dark();
    route(&a, cx, H, cx, 120);
    route(&a, cx, 120, cx + 300, 120);
    g = rt.read(a.data(), W, H);
    std::printf("     right angle: %s\n", show(g).c_str());
    ok("drawn: a right turn, no box: right, the bend at the corner",
       g.bend && g.side == 1 && std::fabs(g.angle - 90) < 20 && std::abs(g.x - (int)cx) <= 12 && std::abs(g.y - 120) <= 12 &&
           !g.has_label && std::abs(g.arc - 130) <= 16 && g.conf == 100);
    a = dark();
    route(&a, cx, H, cx, 120);
    route(&a, cx, 120, cx - 200, 20);
    g = rt.read(a.data(), W, H);
    ok("drawn: up then up to the left: left (" + show(g) + ")", g.bend && g.side == -1 && g.angle < -45 && g.angle > -80);
    a = dark();
    route(&a, cx, H, cx, cy - 30);
    route(&a, cx, cy - 30, cx + 300, cy - 30);
    g = rt.read(a.data(), W, H);
    ok("drawn: the route leaving the chevron sideways: passed (" + show(g) + ")", g.found && g.passed);
    a = dark();
    route(&a, cx, H, cx, cy - 40);
    route(&a, cx, cy - 40, cx + 300, cy - 40);
    g = rt.read(a.data(), W, H);
    ok("drawn: a bend within 40 px of the chevron: passed (" + show(g) + ")", g.found && g.passed);
    a = dark();
    route(&a, cx, H, cx, 40);
    paint(&a, cx - 80, cy - 60, cx + 80, cy - 60, 20, 98, 162, 82);   /* a box over the seed window */
    g = rt.read(a.data(), W, H);
    ok("drawn: a box over the seed window: seed_covered (" + show(g) + ")", g.seed_covered);
    a = dark();
    route(&a, cx, H, cx, 200);
    route(&a, cx + 300, 160, cx + 300, 20);   /* bright, but not from the chevron */
    paint(&a, cx - 400, 100, cx - 300, 100, 3, 172, 176, 40);
    g = rt.read(a.data(), W, H);
    ok("drawn: route elsewhere, none ahead past 200: straight, the far parts never joined", g.found && !g.bend);
    RouteTraceConfig unset;
    unset.ahead = RouteBounds();
    ok("unset bounds: nothing", !RouteTrace(unset).read(a.data(), W, H).found);

    GeoReading j;
    j.x = 536, j.y = 140, j.side = -1, j.angle = -41.6f, j.arc = 110, j.conf = 70, j.has_label = true;
    j.label.x = 488, j.label.y = 120, j.label.w = 200, j.label.h = 68;
    ok("geo_json", geo_json(j) == "{\"x\":536,\"y\":140,\"side\":\"left\",\"angle\":-42,\"arc\":110,\"label\":[488,120,200,68],\"conf\":70,\"passed\":0}");
    ok("geo_json: the trace's time last", geo_json(GeoReading(), 7200).find("\"passed\":0,\"at\":7200}") != std::string::npos);
    ok("geo_json: no label, passed", geo_json(GeoReading()).find("\"side\":\"\",\"angle\":0,\"arc\":0,\"label\":null,\"conf\":0,\"passed\":0") != std::string::npos);

    if (load("test/data/map-route-short.yuv", &a)) {
        const int n = 200;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; i++) rt.read(a.data(), W, H);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
        std::printf("     one read: %.0f us\n", us);
        ok("one read is small (under 5 ms even emulated)", us < 5000);
    }
    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
