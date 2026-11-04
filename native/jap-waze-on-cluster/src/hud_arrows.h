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
#include "screen_grab.h"

namespace woc {

class HudBackend {
public:
    virtual ~HudBackend() {}
    /* The manoeuvre to show; an empty one clears the HUD. */
    virtual void show(const Reading &r, int64_t now_ms) = 0;
    virtual int fd() const { return -1; }
    virtual bool wants_write() const { return false; }
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
 * reading goes to `path` (JSON: maneuver, distance, at), rewritten on change
 * and every second, and the woc-hudguide service (patch/hudguide.js) sends it
 * to the navigation's ProjectedModeGuiding as carplay does for iAP2 route
 * guidance. An empty maneuver clears it. */
class NativeGuidanceHud : public HudBackend {
public:
    explicit NativeGuidanceHud(const std::string &path = "/run/jap-waze-on-cluster/hud.json") : path_(path) {}
    void show(const Reading &r, int64_t now_ms) override;
    void poll(int64_t now_ms) override;
    std::string state() const override;

private:
    void write(int64_t now_ms);

    std::string path_;
    Reading want_;
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
    std::string templates_dir = "/var/opt/jap-waze-on-cluster/hud-templates";
    bool builtin_icons = false;
    float min_score = 0.6f;
    unsigned sample_ms = 400;
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
    /* The decoded frame's luma (w x h, the atlas's first plane), before it
     * is handed to the map feed. */
    void sample(const uint8_t *y, unsigned w, unsigned h, int64_t now_ms);
    /* Dumps the next n samples' crops as PNG (the flag file does too). */
    void arm_dump(unsigned n) { dump_left_ = n; dumped_ = 0; }
    HudBackend *backend() { return backend_.get(); }
    std::string report() const;

private:
    void dump(const uint8_t *y, unsigned w, unsigned h, const Reading &r, int64_t now_ms);

    HudConfig cfg_;
    Templates templates_;
    Recogniser rec_;
    MapArrowReader map_;
    Templates card_tpl_;
    std::unique_ptr<CardReader> card_;
    std::unique_ptr<ScreenGrab> grab_;
    std::vector<uint8_t> scr_;
    bool scr_ok_ = false, from_card_ = false;
    int64_t tpl_checked_ms_ = -1000000;
    long tpl_mtime_ = 0;
    void load_card_templates();
    Debounce debounce_;
    std::unique_ptr<HudBackend> backend_;
    bool active_ = false;
    int64_t last_sample_ms_ = -1000000, flag_checked_ms_ = -1000000, last_dump_ms_ = -1000000;
    unsigned dump_left_ = 0, dumped_ = 0;
    bool flag_ = false;
    Reading last_read_;
};

}  // namespace woc

#endif
