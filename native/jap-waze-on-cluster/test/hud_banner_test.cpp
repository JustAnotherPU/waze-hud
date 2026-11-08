// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
/* Host test of the banner recogniser (hud_banner.h) on synthetic frames:
 * a 1088x480 luma plane with a busy map-like background and a banner card
 * holding an arrow and a distance, drawn by hud_draw.h at another size and
 * offset than the built-in templates, day (light on dark) and night (dark on
 * light), with noise. It checks the pipeline, not Waze's real icons, which
 * only the owner's captures can (README.md "HUD arrows").
 * - every manoeuvre is read, day and night;
 * - the distance is read: m, km, ft, mi, with a decimal point;
 * - no banner (the map alone, a flat card) reads as none;
 * - templates from a folder replace the built-in icons; a dist template
 *   whose glyphs do not match its text is reported;
 * - the PNG dump is a valid PNG; the debounce reports a reading only once
 *   it is stable and changed;
 * - the street set (str-<hex>-<n>.pgm, our stroke glyphs labelled as other
 *   characters): UTF-8 labels whole, a space at a wide gap, iOS's trailing
 *   ellipsis left out, an unknown glyph no street; kept apart from the
 *   distance's; with HUD_FONT_TPL, the SF Pro sets made by
 *   tools/hud_font_templates.py load apart;
 * - the street hold: twice the same to show, kept, gone with the manoeuvre;
 * - the HUD image is drawn for every manoeuvre, transparent when empty;
 * - the cost of one read, for the CPU estimate. */
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/hud_banner.h"

using namespace woc;

static int failures;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

static const unsigned W = 1088, H = 480;
static const Rect kIcon{294, 80, 88, 88}, kDist{390, 96, 200, 56};

static uint32_t rng = 12345;
static unsigned rnd(unsigned n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }

/* The map: a gradient with road-like bright and dark lines. */
static std::vector<uint8_t> map_frame()
{
    std::vector<uint8_t> f(W * H);
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++)
            f[y * W + x] = (uint8_t)(90 + (x + y) % 60);
    for (int i = 0; i < 40; i++) {
        const bool vert = rnd(2);
        const unsigned at = rnd(vert ? W : H), v = rnd(2) ? 230 : 20;
        for (unsigned k = 0; k < (vert ? H : W); k++)
            for (unsigned t = 0; t < 4; t++) {
                const unsigned x = vert ? std::min(at + t, W - 1) : k, y = vert ? k : std::min(at + t, H - 1);
                f[y * W + x] = (uint8_t)v;
            }
    }
    return f;
}

static void paint(std::vector<uint8_t> *f, const Gray &cov, unsigned ox, unsigned oy, uint8_t bg, uint8_t fg)
{
    for (unsigned y = 0; y < cov.h; y++)
        for (unsigned x = 0; x < cov.w; x++) {
            const int c = cov.px[y * cov.w + x];
            int v = bg + (fg - bg) * c / 255 + (int)rnd(13) - 6;
            (*f)[(oy + y) * W + ox + x] = (uint8_t)std::max(0, std::min(255, v));
        }
}

static std::vector<uint8_t> banner(const std::string &label, const std::string &dist, bool night)
{
    std::vector<uint8_t> f = map_frame();
    const uint8_t bg = night ? 215 : 45, fg = night ? 30 : 240;
    Gray card(320, 104);
    paint(&f, card, 286, 72, bg, fg);
    Gray icon(kIcon.w, kIcon.h);
    draw_maneuver(&icon, label, 11, 8, 70);
    paint(&f, icon, kIcon.x, kIcon.y, bg, fg);
    Gray text(kDist.w, kDist.h);
    draw_text(&text, dist, 6, 10, 30);
    paint(&f, text, kDist.x, kDist.y, bg, fg);
    return f;
}

int main()
{
    Templates t;
    t.add_builtin(true);
    ok("built-in templates: every manoeuvre and the glyphs", t.icons() == maneuver_labels().size() && t.glyphs() == 15);
    Recogniser rec(&t, kIcon, kDist);

    int right = 0, total = 0;
    for (int night = 0; night < 2; night++)
        for (const std::string &l : maneuver_labels()) {
            const Reading r = rec.read(banner(l, "300 m", night).data(), W, H);
            total++;
            if (r.maneuver == l && r.distance == "300 m")
                right++;
            else
                std::printf("     %s %s: read %s (%.2f) %s\n", l.c_str(), night ? "night" : "day", r.maneuver.c_str(),
                            r.score, r.distance.c_str());
        }
    ok("every manoeuvre and its distance, day and night", right == total);

    const char *dists[] = {"1.2 km", "50 m", "800 ft", "0.5 mi", "15 km", "1,4 km"};
    const char *want[] = {"1.2 km", "50 m", "800 ft", "0.5 mi", "15 km", "1.4 km"};
    bool all = true;
    for (int i = 0; i < 6; i++) {
        const Reading r = rec.read(banner("left", dists[i], i % 2).data(), W, H);
        if (r.distance != want[i]) {
            std::printf("     %s: read \"%s\"\n", dists[i], r.distance.c_str());
            all = false;
        }
    }
    ok("distances in m, km, ft, mi, with a decimal point or comma", all);

    int none = 0;
    for (int i = 0; i < 20; i++)
        none += rec.read(map_frame().data(), W, H).maneuver.empty();
    ok("the map alone reads as no banner", none == 20);
    std::vector<uint8_t> flat = map_frame();
    paint(&flat, Gray(320, 104), 286, 72, 45, 45);
    ok("an empty card reads as no banner", rec.read(flat.data(), W, H).maneuver.empty());

    /* Templates from a folder: dumps of our own banners as the owner's
     * labelled captures would be. */
    const std::string dir = "/tmp/jap-woc-hud-test-" + std::to_string(getpid());
    mkdir(dir.c_str(), 0755);
    Gray g;
    crop(banner("keep_left", "300 m", false).data(), W, H, kIcon, &g);
    ok("a dump is a PNG", write_png(dir + "/icon.png", g));
    FILE *f = std::fopen((dir + "/icon.png").c_str(), "rb");
    unsigned char sig[8] = {0};
    ok("with the PNG signature", f && std::fread(sig, 1, 8, f) == 8 && !std::memcmp(sig, "\x89PNG\r\n\x1a\n", 8));
    if (f) std::fclose(f);
    auto pgm = [&](const std::string &name, const Gray &img) {
        FILE *o = std::fopen((dir + "/" + name).c_str(), "wb");
        std::fprintf(o, "P5\n%u %u\n255\n", img.w, img.h);
        std::fwrite(img.px.data(), 1, img.px.size(), o);
        std::fclose(o);
    };
    pgm("icon-keep_left-1.pgm", g);
    crop(banner("right", "300 m", false).data(), W, H, kIcon, &g);
    pgm("icon-right-1.pgm", g);
    crop(banner("right", "1.2 km", false).data(), W, H, kDist, &g);
    pgm("dist-1.2km-1.pgm", g);
    pgm("dist-99m-1.pgm", g);
    Templates own;
    own.add_builtin(false);
    std::string log;
    const int n = own.load_dir(dir, &log);
    ok("the folder's templates load; a dist whose glyphs do not match its text is reported",
       n == 3 && own.icons() == 2 && log.find("dist-99m-1.pgm") != std::string::npos);
    Recogniser rec2(&own, kIcon, kDist);
    ok("the folder's icons replace the built-in ones",
       rec2.read(banner("keep_left", "300 m", true).data(), W, H).maneuver == "keep_left" &&
           rec2.read(banner("straight", "300 m", false).data(), W, H).score <
               rec.read(banner("straight", "300 m", false).data(), W, H).score);
    ok("no folder: none added", own.load_dir(dir + "/none", &log) == 0);
    ok("cleaned up", std::system(("rm -rf " + dir).c_str()) == 0);

    Debounce d(2);
    Reading a, b, out;
    a.maneuver = "left";
    a.distance = "300 m";
    b = a;
    b.distance = "200 m";
    ok("debounce: one reading is not yet stable", !d.push(a, &out));
    ok("debounce: twice is reported", d.push(a, &out) && out == a);
    ok("debounce: the same again is not", !d.push(a, &out));
    ok("debounce: a flicker is not", !d.push(b, &out) && !d.push(a, &out));
    ok("debounce: a change held twice is", !d.push(b, &out) && d.push(b, &out) && out == b);
    ok("debounce: no banner held twice clears", !d.push(Reading(), &out) && d.push(Reading(), &out) &&
                                                    out.maneuver.empty());

    ok("parse: plain and unit-less", parse_distance("300m") == "300 m" && parse_distance("300").empty() &&
                                         parse_distance("1.2.3km").empty() && parse_distance("?00m").empty());
    ok("parse: a lost leading digit is no distance", parse_distance("00m").empty() && parse_distance("0m") == "0 m" &&
                                                         parse_distance("07m").empty() &&
                                                         parse_distance("0.5km") == "0.5 km" &&
                                                         parse_distance("0,3mi") == "0.3 mi");

    CardHold hold(3000);
    Reading c50, c, mapr;
    c50.maneuver = c.maneuver = "left";
    c50.distance = "50 m";
    mapr.maneuver = "straight";
    ok("hold: no card read before: the map's reading", hold.pick(mapr, false, 0) == mapr);
    ok("hold: a card reading passes", hold.pick(c50, true, 1000) == c50);
    ok("hold: a miss within 3 s gives the card's", hold.pick(mapr, false, 2000) == c50);
    ok("hold: the same turn without a distance keeps it", hold.pick(c, true, 2500) == c50);
    ok("hold: a distance borrowed does not renew the 3 s", hold.pick(c, true, 3500) == c50 &&
                                                               hold.pick(c, true, 4100) == c);
    ok("hold: after 3 s the map's again", hold.pick(mapr, false, 7200) == mapr);
    Reading r2 = c50;
    r2.maneuver = "right";
    r2.distance.clear();
    ok("hold: another turn does not take the distance", hold.pick(r2, true, 7300) == r2);
    ok("hold: no card and no map: the card's within 3 s", hold.pick(Reading(), false, 8000) == r2 &&
                                                              hold.pick(Reading(), false, 10400).maneuver.empty());

    {
        /* The street set: the H a filled box as tall as a digit, then our
         * stroke glyph standing for another character. */
        const std::string sdir = "/tmp/jap-woc-hud-str-test-" + std::to_string(getpid());
        mkdir(sdir.c_str(), 0755);
        auto save = [&](const std::string &name, const Gray &img) {
            FILE *o = std::fopen((sdir + "/" + name).c_str(), "wb");
            std::fprintf(o, "P5\n%u %u\n255\n", img.w, img.h);
            std::fwrite(img.px.data(), 1, img.px.size(), o);
            std::fclose(o);
        };
        const struct { const char *glyph, *hex; } map[] = {{"1", "u00e9"}, {"m", "u00df"}, {"k", "u0061"}, {"7", "u0152"}};
        for (const auto &m : map) {
            Gray img(120, 60);
            for (unsigned y = 12; y < 44; y++)
                for (unsigned x = 8; x < 30; x++) img.px[y * img.w + x] = 255;
            draw_text(&img, m.glyph, 50, 12, 32);
            save(std::string("str-") + m.hex + "-1.pgm", img);
        }
        Gray dl(160, 60);
        draw_text(&dl, "1m", 8, 12, 32);
        save("dist-1m-1.pgm", dl);
        Templates st, dt;
        std::string slog;
        ok("street set: only the str- files, the H the height reference",
           st.load_dir(sdir, &slog, "str") == 4 && st.glyphs() == 4 && std::fabs(st.ref_h() - 32) < 1.5f && slog.empty());
        ok("distance set beside it: only the dist- file, no letters",
           dt.load_dir(sdir, &slog) == 1 && dt.glyphs() == 2 && dt.ref_h() == 0 && slog.empty());
        /* a line: "1m", a word gap, "k7", then three low dots (iOS's cut) */
        Gray line(400, 60);
        float x = 10 + draw_text(&line, "1m", 10, 12, 32);
        x += 16 + draw_text(&line, "k7", x + 16, 12, 32);
        for (int i = 0; i < 3; i++)
            for (unsigned yy = 40; yy < 44; yy++)
                for (unsigned xx = 0; xx < 4; xx++) line.px[yy * line.w + (unsigned)x + 4 + i * 8 + xx] = 255;
        Recogniser sr(&st, Rect{}, Rect{});
        const std::string words = sr.read_words(line, 0.7f);
        std::printf("     street line read \"%s\"\n", words.c_str());
        ok("street: UTF-8 labels whole, a space at the gap, the ellipsis left out",
           words == "\xc3\xa9\xc3\x9f a\xc5\x92");
        Gray odd(200, 60);
        draw_text(&odd, "1f", 10, 12, 32);
        ok("street: a glyph not in the set: no street", sr.read_words(odd, 0.7f).empty());
        ok("street: no street set: none", Recogniser(nullptr, Rect{}, Rect{}).read_words(line, 0.7f).empty());
        Templates u;
        ok("a UTF-8 line counts characters, not bytes", u.add_line("\xc3\xa9\xc3\x9f", dl) && u.glyphs() == 2);
        ok("and reads them back whole", Recogniser(&u, Rect{}, Rect{}).read_text(dl) == "\xc3\xa9\xc3\x9f");
        ok("cleaned up", std::system(("rm -rf " + sdir).c_str()) == 0);

        if (const char *fd = std::getenv("HUD_FONT_TPL")) {
            Templates fs, fdst;
            std::string flog;
            fs.load_dir(fd, &flog, "str");
            fdst.load_dir(fd, &flog);
            bool letters = false;
            for (const Templates::T &t : fdst.glyph_set()) letters = letters || std::string("0123456789mkfti").find(t.label) == std::string::npos;
            std::printf("     SF Pro: %zu street glyphs, H %.1f px; %zu distance glyphs\n", fs.glyphs(), fs.ref_h(), fdst.glyphs());
            ok("SF Pro sets: the street's every character, the distance's no letter of it",
               fs.glyphs() >= 120 && fs.ref_h() > 20 && !letters && flog.empty());
        } else {
            std::printf("skip SF Pro sets: HUD_FONT_TPL not set\n");
        }
    }

    StreetHold sh;
    Reading s1;
    s1.maneuver = "right";
    s1.street = "Kerkstraat";
    Reading miss = s1;
    miss.street.clear();
    sh.push(s1);
    ok("street hold: once is not enough", sh.street().empty());
    sh.push(miss);
    sh.push(s1);
    ok("street hold: twice, an unreadable read between, shows", sh.street() == "Kerkstraat");
    Reading s2 = s1;
    s2.street = "Kerkstrat";
    sh.push(s2);
    sh.push(miss);
    ok("street hold: a misread once and a miss keep it", sh.street() == "Kerkstraat");
    sh.push(s2);
    ok("street hold: a new name read twice replaces it", sh.street() == "Kerkstrat");
    Reading l = s1;
    l.maneuver = "left";
    sh.push(l);
    ok("street hold: another manoeuvre clears it", sh.street().empty());
    sh.push(l);
    sh.push(Reading());
    ok("street hold: no card clears it", sh.street().empty());
    Reading sd = s1;
    sd.distance = "80 m";
    sh.push(sd);
    sh.push(sd);
    sd.distance.clear();
    sh.push(sd);
    sd.distance = "100 m";
    sh.push(sd);
    ok("street hold: a read without distance, then 20 m up, keep it", sh.street() == "Kerkstraat");
    sd.distance = "0.4 km";
    sd.street.clear();
    sh.push(sd);
    ok("street hold: the distance up by over 30 m (the next one, same kind) clears it", sh.street().empty());
    ok("distance_metres", distance_metres("1.2 km") == 1200 && distance_metres("300 m") == 300 &&
                              distance_metres("") < 0 && std::fabs(distance_metres("0.5 mi") - 804.672) < 1e-6);

    std::vector<uint8_t> img;
    bool drawn = true;
    for (const std::string &l : maneuver_labels()) {
        render_hud(&img, 240, 160, l, "1.2 km");
        unsigned lit = 0;
        for (size_t i = 3; i < img.size(); i += 4)
            lit += img[i] > 128;
        drawn = drawn && lit > 500;
    }
    render_hud(&img, 240, 160, "", "");
    bool clear = true;
    for (size_t i = 3; i < img.size(); i += 4)
        clear = clear && !img[i];
    ok("the HUD image: every manoeuvre drawn; empty, all transparent", drawn && clear && img.size() == 240 * 160 * 4);

    const std::vector<uint8_t> frame = banner("roundabout_2", "1.2 km", false);
    const auto t0 = std::chrono::steady_clock::now();
    const int reps = 200;
    for (int i = 0; i < reps; i++)
        rec.read(frame.data(), W, H);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / reps;
    std::printf("     one read: %.3f ms on this host (at 3 a second: %.2f%% of one core)\n", ms, ms * 3 / 10);
    ok("one read costs under 5 ms here", ms < 5);

    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
