// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
/* Host test of the HUD arrows (hud_arrows.h) next to the map feed, on one
 * shared renderer (cluster-render's service on fake display backends), as
 * on the unit:
 * - the arrows are a hidden, non-contending image layer on the hud target;
 *   the map stays a contending layer on the map target and keeps showing;
 * - a banner read from frames shows the arrow on the HUD after the
 *   debounce, its image our drawing; a distance tick writes only the
 *   changed rows;
 * - stock content taking the HUD slot: the arrows hidden at once, never
 *   taken over, asked again only after the back-off;
 * - no banner: hidden, and the HUD slot released after its linger;
 * - not active (the stream gone, hud_mode off): cleared;
 * - a lowered shared upload cap spaces the writes out;
 * - the native guidance stub draws nothing. */
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "src/service.h"
#include "test/fake_graph.h"
#include "test/fake_port.h"
#include "../src/hud_arrows.h"
#include "../src/map_feed.h"

using namespace woc;

static int failures;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

static const unsigned W = 1088, H = 480;

struct Rig {
    std::string path = "/tmp/jap-woc-hud-test-" + std::to_string(getpid()) + ".sock";
    crs::FakeGpu gpu;
    crs::FakePort port;
    crs::ServiceConfig sc;
    std::unique_ptr<crs::Service> svc;
    std::unique_ptr<MapFeed> feed;
    std::unique_ptr<HudArrows> hud;
    ImageHud *img = nullptr;
    int64_t now = 200000;
    std::vector<uint8_t> spare;

    Rig()
    {
        crs::TargetSpec m, u;
        m.name = "map"; m.slot = 0x100; m.scene_id = 0x010004f0; m.width = 1088; m.height = 480; m.linger_ms = 1500;
        m.take_over_stock = true;
        u.name = "hud"; u.slot = 0x323; u.scene_id = 0x01000400; u.width = 800; u.height = 480; u.linger_ms = 3000;
        u.take_over_stock = false;   /* as cluster-render.conf ships it */
        sc.targets = {m, u};
        sc.socket_path = path;
        sc.backend = "fake";
        svc.reset(new crs::Service(&gpu, &port, sc));
        port.m = &svc->slots();
        std::string err;
        if (!svc->start(&err))
            std::printf("start: %s\n", err.c_str());
        FeedConfig fc;
        fc.socket = path;
        fc.video.level_bits = 0;
        feed.reset(new MapFeed(fc));
        feed->set_stream(true, false, Picture::Shown);
        ImageHudConfig ic;
        ic.socket = path;
        img = new ImageHud(ic);
        HudConfig hc;
        hc.builtin_icons = true;   /* the tests' banners are our drawings */
        hc.templates_dir = "/nonexistent";
        hc.dump_flag = "";
        hud.reset(new HudArrows(hc, std::unique_ptr<HudBackend>(img)));
    }
    void pump(int64_t ms, const std::vector<uint8_t> *frame = nullptr)
    {
        for (int64_t t = 0; t < ms; t += 5) {
            now += 5;
            if (frame && t % 65 == 0) {
                spare.assign(frame->begin(), frame->end());
                hud->sample(spare.data(), W, H, now);
                feed->offer(&spare, (uint64_t)now, now);
            }
            for (int round = 0; round < 400; round++) {
                std::vector<pollfd> fds;
                const size_t first = svc->poll_fds(&fds);
                const int n = ::poll(fds.data(), fds.size(), 0);
                port.tick(now);
                svc->after_poll(fds, first, now);
                const bool pending = feed->wants_write() || img->wants_write();
                feed->poll(now);
                img->poll(now);
                if (round > 0 && n <= 0 && !pending)
                    break;
            }
        }
    }
    const crs::Layer *layer(int target)
    {
        for (const crs::Layer *l : svc->compositor().layers())
            if (l->target() == target)
                return l;
        return nullptr;
    }
    /* Whether the hud scene shows this RGBA image. */
    bool hud_shows(const std::vector<uint8_t> &rgba)
    {
        crs::FakeGraph *g = gpu.graph("hud");
        if (g)
            for (const auto &kv : g->shown.buffers)
                if (kv.second.px == rgba)
                    return true;
        return false;
    }
};

/* A frame with a banner card: our arrow and distance at the default rects. */
static std::vector<uint8_t> atlas(const std::string &label, const std::string &dist)
{
    std::vector<uint8_t> f(W * 720, 128);
    for (unsigned i = 0; i < W * H; i++)
        f[i] = (uint8_t)(90 + (i % W + i / W) % 60);
    Gray icon(88, 88), text(200, 56);
    if (!label.empty()) {
        draw_maneuver(&icon, label, 10, 8, 70);
        draw_text(&text, dist, 6, 10, 30);
        for (unsigned y = 0; y < 104; y++)
            for (unsigned x = 0; x < 320; x++)
                f[(72 + y) * W + 286 + x] = 45;
    }
    for (unsigned y = 0; y < 88; y++)
        for (unsigned x = 0; x < 88; x++)
            if (!label.empty())
                f[(80 + y) * W + 294 + x] = (uint8_t)(45 + icon.px[y * 88 + x] * 195 / 255);
    for (unsigned y = 0; y < 56; y++)
        for (unsigned x = 0; x < 200; x++)
            if (!label.empty())
                f[(96 + y) * W + 390 + x] = (uint8_t)(45 + text.px[y * 200 + x] * 195 / 255);
    return f;
}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, nullptr, _IONBF, 0);
    Rig r;
    r.hud->set_active(true, r.now);
    const std::vector<uint8_t> plain = atlas("", "");
    r.pump(1500, &plain);
    const crs::Layer *hl = r.layer(1), *ml = r.layer(0);
    ok("the arrows: a hidden, non-contending image layer on the hud target at 280,160, 240x160",
       hl && !hl->params.contend && !hl->params.visible && hl->params.rect.x == 280 && hl->params.rect.y == 160 &&
           hl->params.rect.w == 240 && hl->params.rect.h == 160);
    ok("the map: contending, on the map target, showing", ml && ml->params.contend &&
                                                           r.feed->status("").state == "showing");
    ok("no banner: the HUD slot not asked for", !r.port.row(0x323, 0x01000400) && r.img->writes() == 0);

    const std::vector<uint8_t> left300 = atlas("left", "300 m");
    r.pump(1500, &left300);
    std::vector<uint8_t> want;
    render_hud(&want, 240, 160, "left", "300 m");
    const crs::SlotInfo *row = r.port.row(0x323, 0x01000400);
    ok("a banner: the arrow on the HUD, our drawing, the slot granted",
       r.layer(1)->params.visible && r.hud_shows(want) && row && row->map_status == 1 && r.img->writes() == 1);
    ok("the map keeps showing", r.feed->status("").state == "showing");

    const std::vector<uint8_t> left200 = atlas("left", "200 m");
    r.pump(1500, &left200);
    render_hud(&want, 240, 160, "left", "200 m");
    ok("a distance tick: written again, shown", r.hud_shows(want) && r.img->writes() == 2);

    const int requests = r.port.requests;
    r.port.steal(r.sc.targets[1]);
    r.pump(300, &left200);
    ok("stock content takes the HUD slot: the arrows hidden at once, given way, nothing taken over",
       !r.layer(1)->params.visible && r.img->held() && r.port.taken_over.empty());
    r.pump(5000, &left200);
    ok("and not asked for again within the back-off", r.port.requests == requests && !r.layer(1)->params.visible);
    r.port.free_slot(r.sc.targets[1]);
    r.pump(6000, &left200);
    ok("after the back-off it asks again and shows", r.layer(1)->params.visible && r.port.requests > requests &&
                                                         r.port.taken_over.empty());

    r.pump(4500, &plain);
    ok("no banner: hidden, and the HUD slot released after its linger",
       !r.layer(1)->params.visible && !r.port.row(0x323, 0x01000400) && r.feed->status("").state == "showing");

    r.pump(1500, &left300);
    ok("a banner again: shown", r.layer(1)->params.visible);
    r.hud->set_active(false, r.now);
    r.pump(300, &left300);
    ok("not active (the stream gone, or hud_mode off): cleared", !r.layer(1)->params.visible);
    r.hud->set_active(true, r.now);
    r.pump(1500, &left300);

    const unsigned before = r.img->interval_ms();
    for (int i = 0; i < 2; i++) {
        r.svc->compositor().governor().activity(r.now);
        r.svc->compositor().governor().evidence(crs::Evidence::Removal, r.now);
    }
    r.pump(100, &left300);
    ok("a lowered shared cap spaces the HUD's writes out", r.img->interval_ms() > before);
    const unsigned w0 = r.img->writes();
    const char *ds[] = {"290 m", "280 m", "270 m", "260 m"};
    for (const char *d : ds) {
        const std::vector<uint8_t> f = atlas("left", d);
        r.pump(800, &f);
    }
    ok("so fewer writes go than distance ticks", r.img->writes() - w0 < 4);
    std::printf("     %s", r.hud->report().c_str());

    const std::string hj = "/tmp/jap-woc-hud-feed-test.json";
    unlink(hj.c_str());
    NativeGuidanceHud native(hj);
    Reading rd;
    rd.maneuver = "left";
    rd.distance = "450 m";
    native.show(rd, 5000);
    auto slurp = [&]() { std::string t; FILE *f = std::fopen(hj.c_str(), "r"); if (f) { char b[256]; size_t n = std::fread(b, 1, sizeof b, f); t.assign(b, n); std::fclose(f); } return t; };
    ok("native: the reading goes to the file", slurp() == "{\"maneuver\":\"left\",\"distance\":\"450 m\",\"at\":5000}\n");
    native.poll(5500);
    ok("native: no rewrite within a second", slurp().find("\"at\":5000") != std::string::npos);
    native.poll(6000);
    ok("native: the heartbeat rewrites it", slurp().find("\"at\":6000") != std::string::npos);
    native.show(Reading(), 6100);
    ok("native: an empty reading clears it", slurp().find("\"maneuver\":\"\"") != std::string::npos);
    unlink(hj.c_str());

    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
