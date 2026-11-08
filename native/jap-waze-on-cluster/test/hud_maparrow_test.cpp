// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
/* Host test of the map arrow reader (hud_maparrow.h).
 * - a real Waze cluster frame (night, a right turn just ahead, the chevron's
 *   white rim touching the arrow; test/data/waze-map-right.pgm, cropped)
 *   reads right, and mirrored reads left;
 * - the same frame with the arrow blanked, and a dark frame, read none;
 * - our own arrows (hud_draw.h) on a map-like frame, the stem towards the
 *   car: straight, slight, turn and sharp, both sides, read as drawn;
 * - the CarPlay card (test/data/carplay-card-30m.pgm, its street line
 *   painted over): right, and with HUD_FONT_TPL its 30 m and no street;
 *   with a road shield before the street (carplay-card-450m-shield.pgm, the
 *   shield's number painted over and the street cut to "Laan" from its own
 *   letters) 450 m, the shield skipped, Laan;
 * - a solid white box (a road shield) and white text strokes read none;
 * - the cost of one read on a full 1088x480 frame. */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/hud_draw.h"
#include "../src/hud_maparrow.h"

using namespace woc;

static int failures;

static void ok(const std::string &name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name.c_str());
    if (!cond) failures++;
}

static const unsigned W = 1088, H = 480, CX = 544, CY = 360;

/* A dark map with grey roads and a grey route line up from the car. */
static Gray map_frame()
{
    Gray g(W, H, 44);
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++)
            if (x % 160 < 10 || y % 120 < 8) g.px[y * W + x] = 80;
    for (unsigned y = 200; y < H; y++)
        for (unsigned x = CX - 8; x < CX + 8; x++) g.px[y * W + x] = 172;
    return g;
}

/* a white line r px either side of a-b */
static void thick(Gray *g, float ax, float ay, float bx, float by, float r)
{
    const float len = std::hypot(bx - ax, by - ay);
    for (int y = (int)(std::min(ay, by) - r - 1); y <= (int)(std::max(ay, by) + r + 1); y++)
        for (int x = (int)(std::min(ax, bx) - r - 1); x <= (int)(std::max(ax, bx) + r + 1); x++) {
            if (x < 0 || y < 0 || x >= (int)g->w || y >= (int)g->h) continue;
            float t = len > 0 ? ((x - ax) * (bx - ax) + (y - ay) * (by - ay)) / (len * len) : 0;
            t = std::max(0.f, std::min(1.f, t));
            if (std::hypot(x - (ax + t * (bx - ax)), y - (ay + t * (by - ay))) <= r) g->px[y * g->w + x] = 255;
        }
}

static std::string read_label(const Gray &g, unsigned cx, unsigned cy, float *deg = nullptr)
{
    MapArrowConfig c;
    c.car_x = cx;
    c.car_y = cy;
    MapArrowReader r(c);
    const Reading out = r.read(g.px.data(), g.w, g.h);
    if (deg) *deg = r.angle();
    return out.maneuver;
}

int main()
{
    /* the car's frame */
    Gray real;
    if (!read_pgm("test/data/waze-map-right.pgm", &real)) {
        std::printf("FAIL cannot read test/data/waze-map-right.pgm\n");
        return 1;
    }
    /* the crop's origin in the 1088x480 frame was 344,180 */
    const unsigned rx = CX - 344, ry = CY - 180;
    float deg = 0;
    std::string l = read_label(real, rx, ry, &deg);
    ok("real frame: right (" + l + ", " + std::to_string((int)deg) + " deg)", l == "right");
    Gray mir = real;
    for (unsigned y = 0; y < mir.h; y++)
        for (unsigned x = 0; x < mir.w; x++) mir.px[y * mir.w + x] = real.px[y * real.w + real.w - 1 - x];
    l = read_label(mir, real.w - 1 - rx, ry, &deg);
    ok("real frame mirrored: left (" + l + ", " + std::to_string((int)deg) + " deg)", l == "left");
    Gray blank = real;
    for (unsigned y = 120; y < 175; y++)
        for (unsigned x = 160; x < 250; x++)
            if (blank.px[y * blank.w + x] >= 200) blank.px[y * blank.w + x] = 60;
    ok("real frame, arrow removed: none", read_label(blank, rx, ry).empty());
    ok("dark frame: none", read_label(Gray(W, H, 30), CX, CY).empty());

    /* the centre screen's CarPlay card (test/data/carplay-card-30m.pgm: the
     * screen's 580x220+120+0); glyph templates from HUD_FONT_TPL (made by
     * tools/hud_font_templates.py, Apple's font: not in the tree) */
    Gray card;
    if (!read_pgm("test/data/carplay-card-30m.pgm", &card)) {
        std::printf("FAIL cannot read test/data/carplay-card-30m.pgm\n");
        return 1;
    }
    {
        CardConfig cc;
        cc.icon.x -= 120;
        cc.dist.x -= 120;
        cc.plain.x -= 120;
        cc.street.x -= 120;
        Templates tpl, street;
        const char *dir = std::getenv("HUD_FONT_TPL");
        std::string log;
        if (dir) tpl.load_dir(dir, &log), street.load_dir(dir, &log, "str");
        CardReader cr(cc, &tpl, &street);
        const Reading r = cr.read(card.px.data(), card.w, card.h);
        ok("card: right (" + r.maneuver + ", " + std::to_string((int)cr.angle()) + " deg)", r.maneuver == "right");
        if (dir)
            ok("card: 30 m (" + r.distance + ", glyphs " + cr.text() + ")", r.distance == "30 m"),
            ok("card: the street line painted over: no street (" + r.street + ")", r.street.empty());
        else
            std::printf("skip card distance and street: HUD_FONT_TPL not set\n");
        Gray shield;   /* the car, 2026-11-08: a grey road shield before the name */
        if (dir && read_pgm("test/data/carplay-card-450m-shield.pgm", &shield)) {
            const Reading s = cr.read(shield.px.data(), shield.w, shield.h);
            ok("card: shield skipped, 450 m Laan (" + s.distance + ", " + s.street + ")",
               s.distance == "450 m" && s.street == "Laan");
        }
        ok("card without street templates: no street", CardReader(cc, &tpl).read(card.px.data(), card.w, card.h).street.empty());
        ok("card on the cluster map frame: none", CardReader(cc, &tpl).read(real.px.data(), real.w, real.h).maneuver.empty());
    }

    /* plain arrows like Waze's: a thick polyline from below, a triangle head,
     * at the turn angle (screen, right positive) */
    ok("card: a fork (50) is slight, a turn (83) is not; the map's 50 a turn",
       turn_label(50, 65) == "slight_right" && turn_label(-50, 65) == "slight_left" && turn_label(83, 65) == "right" &&
           turn_label(50) == "right");

    struct Case { const char *want; float deg; } cases[] = {
        {"straight", 0}, {"slight_left", -35}, {"slight_right", 35}, {"left", -90}, {"right", 90},
        {"sharp_left", -140}, {"sharp_right", 140}, {"uturn_left", -180}};
    for (const Case &c : cases) {
        Gray g = map_frame();
        const float bx = CX, by = 250, rad = c.deg * (float)M_PI / 180;
        const float dx = std::sin(rad), dy = -std::cos(rad);
        const float off = c.deg <= -179 ? -36 : 0;   /* a U-turn comes back beside the stem */
        thick(&g, bx, by + 90, bx, by, 7);
        const float ex = bx + off + dx * 70, ey = by + dy * 70;
        if (c.deg <= -179) {
            thick(&g, bx, by, bx + off, by, 7);
            thick(&g, bx + off, by, ex, ey, 7);
        } else {
            thick(&g, bx, by, ex, ey, 7);
        }
        /* the head: a triangle 30 long, 40 wide, past the end */
        for (float t = 0; t <= 30; t += 0.5f) {
            const float half = 20 * (1 - t / 30), hx = ex + dx * t, hy = ey + dy * t;
            thick(&g, hx - dy * half, hy + dx * half, hx + dy * half, hy - dx * half, 1);
        }
        l = read_label(g, CX, CY, &deg);
        ok(std::string("arrow ") + c.want + ": " + l + " (" + std::to_string((int)deg) + " deg)", l == c.want);
    }

    /* a road shield and text */
    Gray g = map_frame();
    for (unsigned y = 150; y < 190; y++)
        for (unsigned x = 500; x < 580; x++) g.px[y * W + x] = 250;
    draw_text(&g, "12345", 650, 250, 30);   /* coverage straight into luma: white strokes */
    ok("shield and text: none", read_label(g, CX, CY).empty());

    {   /* the route line: chroma within the bounds, every 4th sample */
        const unsigned aw = 64, ah = 32;
        std::vector<uint8_t> a(aw * ah * 3 / 2, 128);
        for (unsigned i = 0; i < aw * ah; i++) a[i] = 160;
        RouteBounds b;
        ok("route: unset bounds: -1", route_state(a.data(), aw, ah, b) == -1);
        ok("route: bounds parse", parse_route_uv("150,200,60,100", &b) && b.umin == 150 && b.vmax == 100 &&
                                      !parse_route_uv("150,200,60", &b) && !parse_route_uv("200,150,60,100", &b) &&
                                      !parse_route_uv("150,200,60,300", &b) && !parse_route_uv("1,2,3,4x", &b));
        b.ymin = 100;
        b.min_px = 3;
        ok("route: grey map: 0", route_state(a.data(), aw, ah, b) == 0);
        uint8_t *uv = a.data() + aw * ah;
        for (unsigned cx = 0; cx < 12; cx++) uv[4 * aw + cx] = 170, uv[4 * aw + aw / 2 + cx] = 80;   /* row 4: samples 0, 4, 8 */
        ok("route: three line samples: 1", route_pixels(a.data(), aw, ah, b) == 3 && route_state(a.data(), aw, ah, b) == 1);
        a[8 * aw + 0] = 50;   /* luma under the first one: too dark */
        ok("route: dark luma is not the line", route_pixels(a.data(), aw, ah, b) == 2);
        const std::string base = "/tmp/woc-map-dump-test";
        ok("map dump: written", write_map_dump(base, a.data(), aw, ah));
        FILE *f = std::fopen((base + ".yuv").c_str(), "rb");
        std::vector<uint8_t> yuv(aw * ah * 3 / 2 + 1);
        const size_t n = f ? std::fread(yuv.data(), 1, yuv.size(), f) : 0;
        if (f) std::fclose(f);
        ok("map dump: I420, U then V planes", n == aw * ah * 3 / 2 && yuv[aw * ah + 4 * (aw / 2) + 4] == 170 &&
                                                  yuv[aw * ah * 5 / 4 + 4 * (aw / 2) + 4] == 80);
        f = std::fopen((base + ".ppm").c_str(), "rb");
        char hdr[32] = {0};
        const size_t m = f ? std::fread(hdr, 1, 13, f) : 0;
        if (f) std::fclose(f);
        ok("map dump: a PPM", m == 13 && std::string(hdr, 13) == "P6\n64 32\n255\n");
        std::remove((base + ".yuv").c_str());
        std::remove((base + ".ppm").c_str());
    }

    /* cost */
    Gray full = map_frame(), cov(W, H, 0);
    draw_maneuver(&cov, "right", CX - 70, 150, 140);
    for (size_t i = 0; i < full.px.size(); i++)
        if (cov.px[i] > 128) full.px[i] = 255;
    MapArrowConfig c;
    MapArrowReader r(c);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; i++) r.read(full.px.data(), W, H);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
    std::printf("info one read of a %ux%u frame: %.2f ms\n", W, H, ms);

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
