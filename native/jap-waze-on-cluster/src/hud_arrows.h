// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
/* HUD arrows for Waze (README.md "HUD arrows"). Waze sends CarPlay no
 * route-guidance metadata, so BMW's HUD shows no arrows for it, while it
 * does for Apple Maps and Google Maps. HudArrows reads Waze's banner from
 * the decoded cluster frames (hud_banner.h), at most every sample_ms, and
 * hands each settled reading to a HUD backend; the cluster map is
 * unaffected.
 *
 * Arrows show only while CarPlay's cluster stream is live and not frozen,
 * the map is on the map target, and a Waze banner is read: Apple Maps or
 * Google Maps on the cluster read as no banner (and send their own
 * guidance, which the HUD shows), so nothing changes for them.
 *
 * Backends:
 * - ImageHud: an image layer of the shared renderer on its `hud` target,
 *   our arrow drawn in BMW's style (hud_draw.h), written on change only
 *   (a distance tick only the changed rows), at most once a second, less
 *   often while the shared upload cap is lowered. It never takes stock
 *   content over (hud_take_over_stock = 0, not contending): the moment the
 *   slot is held by anyone else (BMW's own HUD content, another source's
 *   guidance) it hides its layer and asks again only after 10, 30, then 60 s.
 * - NativeGuidanceHud: BMW's own HUD widget draws the manoeuvre and the
 *   distance, sent to the navigation's ProjectedModeGuiding through the
 *   woc-hudguide service, as carplay sends Apple's and Google's. */
#ifndef WOC_HUD_ARROWS_H
#define WOC_HUD_ARROWS_H

#include <stdint.h>
#include <memory>
#include <string>
#include <vector>

#include "cluster_render/client.h"
#include "hud_banner.h"
#include "hud_maparrow.h"
#include "hud_route.h"
#include "screen_grab.h"

namespace woc {

class HudBackend {
public:
    virtual ~HudBackend() {}
    /* The manoeuvre to show; an empty one clears the HUD. */
    virtual void show(const Reading &r, int64_t now_ms) = 0;
    virtual int fd() const { return -1; }
    virtual bool wants_write() const { return false; }
    /* Where the reading came from (card, hold, none), whether the arrows
     * are on and the cluster stream is live and not frozen, the cluster
     * map's arrow while no card shows, and Waze's route line on the map (1,
     * 0, -1 unknown); only the native backend passes them on. */
    virtual void context(const std::string &, bool, const std::string &, int, int64_t) {}
    /* The route ahead on the cluster map (hud_route.h's geo_json), once a
     * second while it is traced; only the native backend passes it on. */
    virtual void geo(const std::string &, int64_t) {}
    virtual void poll(int64_t) {}
    /* One line for the diagnostics. */
    virtual std::string state() const = 0;
};

struct ImageHudConfig {
    std::string socket = "/dev/shm/cluster-render/render.sock";
    Rect rect{280, 160, 240, 160};   /* on the 800x480 hud target */
    unsigned min_interval_ms = 1000;
};

class ImageHud : public HudBackend {
public:
    explicit ImageHud(const ImageHudConfig &cfg);
    void show(const Reading &r, int64_t now_ms) override;
    int fd() const override { return client_.fd(); }
    bool wants_write() const override { return client_.wants_write(); }
    void poll(int64_t now_ms) override;
    std::string state() const override;

    /* For the tests. */
    unsigned writes() const { return writes_; }
    unsigned interval_ms() const;
    bool held() const { return held_; }

private:
    void set_visible(bool on);

    ImageHudConfig cfg_;
    cr::Client client_;
    uint32_t layer_ = 0;
    cr::LayerOptions lo_;
    Reading want_, sent_;
    std::vector<uint8_t> img_, next_;
    int64_t next_write_ms_ = 0;
    unsigned cap_kb_ = 0, max_kb_ = 0;
    std::string budget_reason_, target_state_, holder_;
    bool held_ = false;
    int64_t retry_ms_ = 0;
    int give_ways_ = 0;
    unsigned writes_ = 0;
    int64_t now_ = 0;
};

/* BMW's own HUD guidance widget, as for Apple Maps and Google Maps: the
 * reading goes to `path`, rewritten on change and every second, and the
 * woc-hudguide service (patch/hudguide.js) sends it to the navigation's
 * ProjectedModeGuiding as carplay does for iAP2 route guidance. An empty
 * maneuver clears it. JSON (v2, v1's keys unchanged): maneuver, distance,
 * street (only when read), source ("card", "hold", "none"), stream (1 while
 * the arrows are on and the stream is live, else 0), map (the cluster map's
 * arrow, "" while a card shows), route (1 Waze's route line on the map, 0
 * none, -1 hud_route_uv unset), at, then geo (the route ahead traced on
 * the map, hud_route.h's geo_json) once traced, card or not. */
class NativeGuidanceHud : public HudBackend {
public:
    explicit NativeGuidanceHud(const std::string &path = "/run/jap-waze-on-cluster/hud.json") : path_(path) {}
    void show(const Reading &r, int64_t now_ms) override;
    void context(const std::string &source, bool stream, const std::string &map, int route, int64_t now_ms) override;
    void geo(const std::string &json, int64_t) override { geo_ = json; }   /* the next write carries it */
    void poll(int64_t now_ms) override;
    std::string state() const override;

private:
    void write(int64_t now_ms);

    std::string path_;
    Reading want_;
    std::string source_ = "none", map_, geo_;
    bool stream_ = false;
    int route_ = -1;
    int64_t written_ms_ = -1000000;
    unsigned writes_ = 0;
    bool failed_ = false;
};

struct HudConfig {
    std::string reader = "banner";   /* map: the arrow on Waze's map (main.cpp's default); banner: an instruction card */
    MapArrowConfig map;
    /* the CarPlay card on the centre screen, read first when on */
    bool card = false;
    CardConfig card_cfg;
    Rect card_roi{120, 0, 580, 220};   /* what a dump keeps of the centre screen */
    ScreenGrabConfig grab;
    Rect icon{294, 80, 88, 88}, dist{390, 96, 200, 56}, roi{286, 72, 504, 368};
    std::string templates_dir = "/var/opt/jap-waze-on-cluster/hud-templates";   /* the card's str-*.pgm too */
    bool builtin_icons = false;
    float min_score = 0.6f;
    unsigned sample_ms = 400;
    RouteBounds route;            /* hud_route_uv, hud_route_ymin, hud_route_min_px */
    RouteTraceConfig geo;         /* hud_geo_uv (unset: no geo), hud_route_bright_ymin, hud_label_uv, hud_geo_car_x/y */
    unsigned stable = 2;
    std::string dump_flag = "/tmp/jap-woc-hud-dump";
    std::string dump_dir = "/tmp/jap-woc-hud";
    unsigned dump_max = 200;
};

class HudArrows {
public:
    HudArrows(const HudConfig &cfg, std::unique_ptr<HudBackend> backend);
    /* Whether arrows may show now (the mode, the stream, the map's target);
     * off clears the HUD. */
    void set_active(bool on, int64_t now_ms);
    /* Whether CarPlay's cluster stream is live and not frozen, for hud.json. */
    void set_stream(bool live, int64_t now_ms);
    /* The decoded frame's 4:2:0 atlas (yuv_atlas.h: w x h luma, then the
     * chroma), before it is handed to the map feed. */
    void sample(const uint8_t *y, unsigned w, unsigned h, int64_t now_ms);
    /* Dumps the next n samples' crops as PNG (the flag file does too). */
    void arm_dump(unsigned n) { dump_left_ = n; dumped_ = 0; }
    /* The next frame's atlas to the dump folder (run map-dump), for the route's colour. */
    void arm_map_dump() { map_dump_ = true; }
    HudBackend *backend() { return backend_.get(); }
    std::string report() const;

private:
    void dump(const uint8_t *y, unsigned w, unsigned h, const Reading &r, int64_t now_ms);
    void trace(const uint8_t *atlas, unsigned w, unsigned h, int64_t now_ms);

    HudConfig cfg_;
    Templates templates_;
    Recogniser rec_;
    MapArrowReader map_;
    RouteTrace geo_;
    GeoReading geo_last_;
    int64_t geo_found_ms_ = -1000000, near_ms_ = -1000000;
    int near_arc_ = 0;
    Templates card_tpl_, street_tpl_;
    std::unique_ptr<CardReader> card_;
    std::unique_ptr<ScreenGrab> grab_;
    std::vector<uint8_t> scr_;
    bool scr_ok_ = false, from_card_ = false;
    int64_t tpl_checked_ms_ = -1000000;
    long tpl_mtime_ = 0;
    void load_card_templates();
    Debounce debounce_;
    CardHold hold_;
    StreetHold street_;
    std::string map_hint_, source_ = "none";
    bool stream_ = false, map_dump_ = false;
    int route_ = -1;
    int64_t route_ms_ = -1000000, map_ms_ = -1000000, card_seen_ms_ = -1000000000;
    void push_context(int64_t now_ms);
    std::unique_ptr<HudBackend> backend_;
    bool active_ = false;
    int64_t last_sample_ms_ = -1000000, flag_checked_ms_ = -1000000, last_dump_ms_ = -1000000;
    unsigned dump_left_ = 0, dumped_ = 0, map_dumped_ = 0;
    int64_t last_map_dump_ms_ = -1000000;
    bool flag_ = false;
    Reading last_read_;
};

}  // namespace woc

#endif
