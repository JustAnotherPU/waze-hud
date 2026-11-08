// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
// Modified 2026-11 by JustAnotherPU: Waze turn arrows on the HUD (hud_arrows, hud_banner, hud_draw).
/* jap-waze-on-cluster: decode the captured CarPlay 2nd-screen H.264 (the Waze/Maps
 * cluster map) and hand it to the shared cluster renderer (cluster-render),
 * which draws it in a cluster slot. The service runs from boot, so it decodes
 * from the stream's first keyframe; the map goes up with the first non-blank
 * picture and comes down when the stream ends, freezes or stays blank
 * (map_feed.h, freeze_watch.h). The control socket
 * reports its status and log to the Waze app.
 *
 *   jap-waze-on-cluster [-c FILE] [-s key=value]...     serve the control socket
 *   jap-waze-on-cluster [-c FILE] run sail|map|panel     map that target from now on,
 *                                                   and follow the status from a shell
 *                                                   (sail = map, the native central-map slot)
 *   jap-waze-on-cluster [-c FILE] run hud|hud-off|hud-video|hud-dump|map-dump
 *                                                   Waze's arrows on the HUD (hud_mode
 *                                                   arrows), none, the map video itself on
 *                                                   the HUD (the old `run hud`), or dump the
 *                                                   next 30 banner crops, or the next map
 *                                                   frame as YUV and PPM (README.md "HUD arrows")
 *   jap-waze-on-cluster [-c FILE] bench-pack I420_FILE  time the packed-motion packing
 *
 * The H.264 reaches this tool through the sink the carplay patch points at
 * (patch/carplay_patch.js --sink/--capture), after the patch advertises the 2nd
 * display and makes CarPlay self-start the stream (PATCH.md Edits 1 and 3). It
 * receives on the media bus (avb_stream, the AVB capture mode), a local UDP
 * socket (udp_port) and the FIFO. README.md. */
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <memory>
#include <vector>

#include "alt_screen.h"
#include "config.h"
#include "control.h"
#include "guide_setting.h"
#include "hud_arrows.h"
#include "log.h"
#include "block_pack.h"
#include "map_feed.h"
#include "map_geometry.h"
#include "video_producer.h"
#include "yuv_atlas.h"

using namespace cld;
using namespace woc;
using crk::atlas_height;
using crk::block_bytes;
using crk::pack_blocks;
using crk::texture_size_error;

/* component.json's version, given by the target build. */
#ifndef WOC_VERSION
#define WOC_VERSION ""
#endif

namespace {

/* Accepted and ignored, so a local_conf that sets them still starts the
 * service: the map is drawn by the shared renderer, whose own configuration
 * owns the slots, scene ids and the upload budget. */
const char *const kIgnoredKeys[] = {"tex_format",         "upload",          "dirty_tiles",
                                    "max_frame_textures", "resource_linger_ms", "resource_fetch_log",
                                    "texture_replay",     "texture_replay_frames", "mutable_r8",
                                    "upload_kb_per_s",    "upload_burst_ms"};

volatile sig_atomic_t g_stop;
void on_signal(int) { g_stop = 1; }

std::string status_line(const FeedStatus &s)
{
    return "{\"ev\":\"status\",\"state\":" + json_escape(s.state) + ",\"target\":" + json_escape(s.target) +
           ",\"page\":" + json_escape(s.page) + ",\"reason\":" + json_escape(s.reason) +
           ",\"upload_cap_kb_per_s\":" + std::to_string(s.upload_cap_kb) +
           ",\"upload_reason\":" + json_escape(s.upload_reason) +
           ",\"hide_bmw_guidance\":" + (s.guide_hold ? "1" : "0") +
           ",\"guide_disabled\":" + (s.guide_disabled ? "1" : "0") + "}";
}

class Server : public Control::Handler {
public:
    Server(MapFeed *feed, GuideSetting *guide, HudArrows *hud, const std::string &map_target, const std::string &hud_mode)
        : feed_(feed), guide_(guide), hud_(hud), map_target_(map_target), hud_mode_(hud_mode) {}
    const std::string &hud_mode() const { return hud_mode_; }
    void set_control(Control *c) { control_ = c; }

    /* start picks the target the map goes to (`run`); the map itself comes
     * and goes with the stream. guide is the app's switch for BMW's turn
     * box (guide_setting.h). */
    void on_command(const std::map<std::string, std::string> &msg) override
    {
        const std::string &cmd = msg.at("cmd");
        auto it = msg.find("target");
        auto hide = msg.find("hide");
        auto mode = msg.find("mode");
        if (cmd == "hud" && mode != msg.end() &&
            (mode->second == "arrows" || mode->second == "off" || mode->second == "video")) {
            log_info("hud %s", mode->second.c_str());
            hud_mode_ = mode->second;
            if (hud_mode_ == "video")
                feed_->set_target("hud");
            else if (feed_->target() == "hud")
                feed_->set_target(map_target_);   /* the map back where it was */
        } else if (cmd == "hud-dump") {
            log_info("hud: dumping the next 30 banner crops");
            hud_->arm_dump(30);
        } else if (cmd == "map-dump") {
            log_info("hud: dumping the next map frame");
            hud_->arm_map_dump();
        } else if (cmd == "start" && it != msg.end()) {
            log_info("start %s", it->second.c_str());
            if (!feed_->set_target(it->second))
                log_warn("start: the cluster renderer has no target %s", it->second.c_str());
        } else if (cmd == "guide" && hide != msg.end() && guide_->set(hide->second)) {
            log_info("guide: BMW's turn box %s while the map shows", guide_->hide() ? "hidden" : "shown");
            feed_->set_guide_hold(guide_->hide());
        } else {
            log_warn("control: unknown or malformed command %s", cmd.c_str());
        }
    }
    void on_owner_connected() override
    {
        log_seq_ = 0;
        last_.clear();
    }
    /* The map does not depend on the app: it stays while CarPlay is on the
     * CID and the app's page is hidden. */
    void on_owner_gone() override {}

    void report(const std::string &status)
    {
        if (control_->owner_fd() < 0)
            return;
        if (status != last_) {
            last_ = status;
            control_->send(status);
        }
        std::vector<std::string> lines;
        log_lines_since(&log_seq_, &lines);
        for (const std::string &l : lines)
            control_->send("{\"ev\":\"log\",\"line\":" + json_escape(l) + "}");
    }

private:
    MapFeed *feed_;
    GuideSetting *guide_;
    HudArrows *hud_;
    std::string map_target_, hud_mode_;
    Control *control_ = nullptr;
    uint64_t log_seq_ = 0;
    std::string last_;
};

/* Files next to the FIFO, in the render's own RuntimeDirectory, which it can
 * write: status.json and probe.txt, for the remote diagnostics, read
 * without taking the control socket's owner slot. 0644, world-readable. */
void write_sibling_file(const std::string &fifo_path, const char *name, const std::string &content)
{
    std::string::size_type slash = fifo_path.find_last_of('/');
    if (slash == std::string::npos)
        return;
    std::string path = fifo_path.substr(0, slash) + "/" + name;
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    std::string line = content + "\n";
    ssize_t n = write(fd, line.data(), line.size());
    close(fd);
    if (n == (ssize_t)line.size())
        rename(tmp.c_str(), path.c_str());
    else
        unlink(tmp.c_str());
}

int serve(const Config &cfg)
{
    if (cfg.has("log_file"))
        log_to_file(cfg.str("log_file"), log_file_cap(cfg.num("log_file_kb")), "jap-waze-on-cluster", WOC_VERSION);
    VideoConfig vc;
    vc.fifo_path = cfg.str("fifo_path");
    vc.carplay_group = cfg.str("carplay_group");
    vc.udp_port = (uint16_t)cfg.num("udp_port");
    vc.avb_stream = cfg.str("avb_stream");
    vc.drm_device = cfg.str("drm_device");
    if (!cfg.size("tex_size", &vc.tex_width, &vc.tex_height)) {
        log_error("tex_size must be WxH");
        return 1;
    }
    if (const char *e = texture_size_error(vc.tex_width, vc.tex_height)) {
        log_error("tex_size %ux%u: %s", vc.tex_width, vc.tex_height, e);
        return 1;
    }
    if (const char *why = parse_source_crop(cfg.str("src_crop"), &vc.src_crop))
        log_warn("src_crop = %s is ignored: %s; the whole picture is the map", cfg.str("src_crop").c_str(), why);
    for (const char *key : kIgnoredKeys)
        if (cfg.has(key))
            log_warn("%s = %s is ignored: the upload budget is the cluster renderer's "
                     "(upload_kb_per_s in /var/opt/cluster-render/cluster-render.local.conf)", key, cfg.str(key).c_str());
    struct Range { const char *key; long lo, hi; } ranges[] = {
        {"dirty_threshold", 0, 64}, {"dirty_settle_ms", 0, 3600000}, {"tex_level_bits", 0, 3},
        {"target_fps", 0, 60},      {"pan_blocks", 0, 1},            {"stall_ms", 1000, 600000},
        {"freeze_ms", 0, 600000},   {"freeze_min_fps", 1, 60},       {"alt_screen_port", 0, 65535},
        {"idle_ms", 1000, 600000},  {"blank_ms", 1000, 600000},
        {"picture_fps", 0, 60},     {"hidden_fps", 1, 60},           {"keep_ms", 0, 10000},
        {"still_ms", 0, 600000},
        {"alt_screen_uid", 0, 4294967294L}};
    for (const Range &r : ranges)
        if (cfg.num(r.key) < r.lo || cfg.num(r.key) > r.hi) {
            log_error("%s must be from %ld to %ld", r.key, r.lo, r.hi);
            return 1;
        }
    vc.tex_level_bits = (uint32_t)cfg.num("tex_level_bits");
    vc.stall_ms = (int)cfg.num("stall_ms");
    vc.freeze.ms = (unsigned)cfg.num("freeze_ms");
    vc.freeze.min_fps = (unsigned)cfg.num("freeze_min_fps");
    vc.freeze.blank_ms = (unsigned)cfg.num("blank_ms");
    /* hidden, pictures still feed the freeze watch, so never below its min_fps */
    const long picture_fps = cfg.num("picture_fps");
    const long hidden_fps = std::max(cfg.num("hidden_fps"), (long)vc.freeze.min_fps);
    const int64_t shown_interval_ms = picture_fps ? 1000 / picture_fps : 0;
    const int64_t hidden_interval_ms = picture_fps ? 1000 / std::min(hidden_fps, picture_fps) : 0;
    SendGate send_gate(cfg.num("keep_ms"));
    uint64_t throttled_seen = 0;
    /* a still map (parked, at a light) needs no more pictures than a hidden one */
    const int64_t still_ms = cfg.num("still_ms");
    int64_t changed_ms = -1;

    FeedConfig fc;
    fc.socket = cfg.str("render_socket");
    fc.target = cfg.str("target");
    if (const char *why = parse_map_rect(cfg.str("map_rect"), &fc.rect))
        log_warn("map_rect = %s is ignored: %s; the map area instead", cfg.str("map_rect").c_str(), why);
    fc.video.w = vc.tex_width;
    fc.video.h = vc.tex_height;
    fc.video.fps = (unsigned)cfg.num("target_fps");
    fc.video.level_bits = 0;   /* rounded on the decoder thread (video_producer.h) */
    fc.video.dirty_threshold = (unsigned)cfg.num("dirty_threshold");
    fc.video.settle_ms = (unsigned)cfg.num("dirty_settle_ms");
    fc.video.pan_blocks = cfg.num("pan_blocks") == 1 && vc.tex_height % 4 == 0;
    /* The layer's visibility follows the stream (set_stream); the renderer's
     * stall takes it down if the stream's connection outlives its frames. */
    fc.video.stall_ms = (unsigned)cfg.num("idle_ms");

    HudConfig hc;
    ImageHudConfig ic;
    ic.socket = fc.socket;
    const std::string hud_mode = cfg.str("hud_mode"), hud_backend = cfg.str("hud_backend");
    if (!parse_rect(cfg.str("hud_icon_rect"), &hc.icon) || !parse_rect(cfg.str("hud_dist_rect"), &hc.dist) ||
        !parse_rect(cfg.str("hud_roi"), &hc.roi) || !parse_rect(cfg.str("hud_rect"), &ic.rect)) {
        log_error("hud_icon_rect, hud_dist_rect, hud_roi and hud_rect must be WxH+X+Y");
        return 1;
    }
    if (hud_mode != "arrows" && hud_mode != "off" && hud_mode != "video") {
        log_error("hud_mode must be arrows, off or video");
        return 1;
    }
    hc.reader = cfg.str("hud_reader");
    if (hc.reader != "card" && hc.reader != "map" && hc.reader != "banner") {
        log_error("hud_reader must be card, map or banner");
        return 1;
    }
    hc.map.car_x = (unsigned)cfg.num("hud_car_x");
    hc.map.car_y = (unsigned)cfg.num("hud_car_y");
    hc.map.min_luma = (unsigned)cfg.num("hud_min_luma");
    hc.card = cfg.num("hud_card") == 1;
    if (hc.reader == "card" && !hc.card) {
        log_error("hud_reader = card needs hud_card = 1");
        return 1;
    }
    if (!parse_rect(cfg.str("hud_card_icon_rect"), &hc.card_cfg.icon) ||
        !parse_rect(cfg.str("hud_card_dist_rect"), &hc.card_cfg.dist) ||
        !parse_rect(cfg.str("hud_card_plain_rect"), &hc.card_cfg.plain) ||
        !parse_rect(cfg.str("hud_card_street_rect"), &hc.card_cfg.street) || !parse_rect(cfg.str("hud_card_roi"), &hc.card_roi)) {
        log_error("hud_card_icon_rect, hud_card_dist_rect, hud_card_plain_rect, hud_card_street_rect and hud_card_roi must be WxH+X+Y");
        return 1;
    }
    {   /* the grabber converts only what the card reader and its dumps read */
        unsigned x0 = ~0u, y0 = ~0u, x1 = 0, y1 = 0;
        for (const Rect &r : {hc.card_cfg.icon, hc.card_cfg.dist, hc.card_cfg.plain, hc.card_cfg.street, hc.card_roi}) {
            x0 = std::min(x0, r.x), y0 = std::min(y0, r.y);
            x1 = std::max(x1, r.x + r.w), y1 = std::max(y1, r.y + r.h);
        }
        hc.grab.crop_x = x0, hc.grab.crop_y = y0, hc.grab.crop_w = x1 - x0, hc.grab.crop_h = y1 - y0;
    }
    if (hud_backend != "image" && hud_backend != "native") {
        log_error("hud_backend must be image or native");
        return 1;
    }
    struct HudRange { const char *key; long lo, hi; } hud_ranges[] = {
        {"hud_min_score", 1, 100}, {"hud_sample_ms", 200, 5000}, {"hud_stable", 1, 10},
        {"hud_dump_max", 0, 10000}, {"hud_builtin_icons", 0, 1},
        {"hud_car_x", 0, 4096}, {"hud_car_y", 0, 4096}, {"hud_min_luma", 128, 255}, {"hud_card", 0, 1},
        {"hud_route_ymin", 0, 255}, {"hud_route_min_px", 1, 100000}, {"hud_bg_hold_s", 0, 3600},
        {"hud_route_bright_ymin", 0, 255}, {"hud_geo_car_x", 0, 4096}, {"hud_geo_car_y", 0, 4096}, {"hud_geo_next", 0, 1}};
    for (const HudRange &r : hud_ranges)
        if (cfg.num(r.key) < r.lo || cfg.num(r.key) > r.hi) {
            log_error("%s must be from %ld to %ld", r.key, r.lo, r.hi);
            return 1;
        }
    hc.templates_dir = cfg.str("hud_templates");
    hc.builtin_icons = cfg.num("hud_builtin_icons") == 1;
    hc.min_score = cfg.num("hud_min_score") / 100.0f;
    hc.sample_ms = (unsigned)cfg.num("hud_sample_ms");
    hc.stable = (unsigned)cfg.num("hud_stable");
    hc.dump_flag = cfg.str("hud_dump_flag");
    hc.dump_dir = cfg.str("hud_dump_dir");
    hc.dump_max = (unsigned)cfg.num("hud_dump_max");
    if (!cfg.str("hud_route_uv").empty() && !parse_route_uv(cfg.str("hud_route_uv"), &hc.route)) {
        log_error("hud_route_uv must be umin,umax,vmin,vmax (0-255, min <= max) or empty");
        return 1;
    }
    hc.route.ymin = (int)cfg.num("hud_route_ymin");
    hc.route.min_px = (unsigned)cfg.num("hud_route_min_px");
    /* the route ahead (hud_route.h): unset hud_geo_uv, no trace and no geo */
    hc.geo.ahead = RouteBounds();
    if ((!cfg.str("hud_geo_uv").empty() && !parse_route_uv(cfg.str("hud_geo_uv"), &hc.geo.ahead)) ||
        !parse_route_uv(cfg.str("hud_label_uv"), &hc.geo.label)) {
        log_error("hud_geo_uv (or empty) and hud_label_uv must be umin,umax,vmin,vmax (0-255, min <= max)");
        return 1;
    }
    hc.geo.ahead.ymin = (int)cfg.num("hud_route_bright_ymin");
    hc.geo.car_x = (unsigned)cfg.num("hud_geo_car_x");
    hc.geo.car_y = (unsigned)cfg.num("hud_geo_car_y");

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
    int sig_fd = signalfd(-1, &sigs, SFD_CLOEXEC);
    if (sig_fd < 0) { log_error("signalfd: %s", strerror(errno)); return 1; }

    GuideSetting guide(cfg.str("hide_bmw_guidance"), cfg.str("state_file"));
    fc.guide_hold = guide.hide();

    std::string err;
    VideoProducer producer(vc);
    MapFeed feed(fc);
    if (hud_mode == "video")
        feed.set_target("hud");
    std::unique_ptr<HudBackend> backend;
    if (hud_backend == "native")
        backend.reset(new NativeGuidanceHud());
    else
        backend.reset(new ImageHud(ic));
    HudArrows hud(hc, std::move(backend));
    Server server(&feed, &guide, &hud, fc.target, hud_mode);
    Control control(&server);
    server.set_control(&control);
    if (!control.listen(cfg.str("socket_path"), (unsigned)std::strtoul(cfg.str("socket_mode").c_str(), nullptr, 8),
                        cfg.str("socket_group"), &err)) {
        log_error("%s", err.c_str());
        return 1;
    }

    std::vector<uint8_t> frame;
    StatusLog status_log;
    int64_t last_status_write = 0;
    uint32_t errs_logged = 0;
    int64_t errs_logged_ms = -60000;
    const AltScreen alt((uint16_t)cfg.num("alt_screen_port"), (uint32_t)cfg.num("alt_screen_uid"));
    StreamLiveness liveness(vc.stall_ms);
    std::string last_detail;
    AltScreen::State alt_state = AltScreen::Unknown;
    int64_t alt_read_ms = 0;
    for (;;) {
        HudBackend *hb = hud.backend();
        struct pollfd p[6] = {{sig_fd, POLLIN, 0}, {control.listen_fd(), POLLIN, 0}, {control.owner_fd(), POLLIN, 0},
                              {feed.fd(), (short)(POLLIN | (feed.wants_write() ? POLLOUT : 0)), 0},
                              {producer.ready_fd(), POLLIN, 0},
                              {hb->fd(), (short)(POLLIN | (hb->wants_write() ? POLLOUT : 0)), 0}};
        /* A decoded frame wakes it (the producer's ready fd), and so does the
         * renderer's socket; four times a second otherwise, so the client
         * retries its connection, a source that is down is retried and the
         * status files stay current. */
        int n = poll(p, 6, 250);
        if (n < 0 && errno != EINTR)
            break;
        if (p[0].revents & POLLIN)
            break;
        if (control.owner_fd() >= 0 && (p[2].revents & (POLLIN | POLLHUP | POLLERR)))
            control.on_owner_readable();
        if (p[1].revents & POLLIN)
            control.on_listen_readable();
        const int64_t now = now_ms();
        uint64_t id = 0;
        bool changed = true;
        if (producer.take(&frame, &id, &changed)) {
            /* The banner is read from the luma before the frame goes. */
            hud.sample(frame.data(), vc.tex_width, vc.tex_height, now);
            if (changed)
                changed_ms = now;
            if (send_gate.send(now, changed, feed.visible(), feed.needs_frame()))
                feed.offer(&frame, id, now);
        }
        if (now - alt_read_ms >= 1000) {
            const AltScreen::State s = cfg.num("alt_screen_port") ? alt.read() : AltScreen::Unknown;
            if (s != alt_state)
                log_info("CarPlay's alternate-screen connection: %s",
                         s == AltScreen::Connected ? "connected" : s == AltScreen::Absent ? "absent" : "not readable");
            alt_state = s;
            alt_read_ms = now;
        }
        const bool live = liveness.live(alt_state, producer.last_frame_ms(), now);
        producer.set_live(live, now);
        feed.set_stream(live, producer.frozen(now), producer.picture(now));
        const bool still = still_ms && (changed_ms < 0 || now - changed_ms >= still_ms);
        producer.set_picture_interval_ms(feed.visible() && !still ? shown_interval_ms : hidden_interval_ms);
        feed.poll(now);
        /* Arrows only while Waze's stream is live and the map is on the map
         * target: the HUD shares its scene id with the panel. */
        hud.set_stream(live && !producer.frozen(now), now);
        hud.set_active(server.hud_mode() == "arrows" && feed.target() == "map" && live && !producer.frozen(now) &&
                       producer.picture(now) == Picture::Shown, now);
        hb->poll(now);
        producer.retry_sources(now);
        const FeedStatus fs = feed.status(producer.decode_status(), producer.stream_reason());
        const std::string status = status_line(fs);
        /* The frame rate changes the status every second; the log keeps
         * to its state, reason and target, so the app's last lines are not
         * all status. */
        if (status_log.due(fs, now))
            log_info("status %s", status.c_str());
        server.report(status);
        if (now - last_status_write > 1000) {
            FrameTally::Minute m;
            StreamHealth::Minute sm;
            Backlog::Minute bm;
            uint64_t overflows = 0;
            if (producer.minute(now, &m, &sm, &bm, &overflows)) {
                const uint64_t throttled = producer.throttled() - throttled_seen;
                throttled_seen += throttled;
                log_info("waze: %u frames decoded in the last minute, %u without a picture to catch up, %u above "
                         "the picture rate, %u pictures (%llu unchanged held back so far), %u changed, "
                         "%u corrupted; the last picture is %s; %u losses (%u packets), %llu dropped by the receive "
                         "queue; packets waited %.1f ms on average, %lld ms at most",
                         m.frames + bm.skipped + (unsigned)throttled, bm.skipped, (unsigned)throttled, m.frames,
                         (unsigned long long)send_gate.held(), m.changed, sm.corrupted, m.blank ? "blank" : "not blank",
                         sm.losses, sm.lost, (unsigned long long)overflows, bm.wait_us.mean() / 1000.0,
                         (long long)(bm.wait_us.max / 1000));
            }
            producer.track_keyframe_wait(now);
            const std::string detail = producer.stream_detail();
            if (producer.stream_lost() && detail != last_detail)
                log_warn("waze: stream lost: %s", detail.c_str());
            last_detail = detail;
            /* New decode errors, at most once a minute. */
            std::string last_err;
            const uint32_t errs = producer.decode_errors(&last_err);
            if (errs != errs_logged && now - errs_logged_ms >= 60000) {
                log_warn("waze: %u decode errors so far, the last: %s", errs, last_err.c_str());
                errs_logged = errs;
                errs_logged_ms = now;
            }
            write_sibling_file(cfg.str("fifo_path"), "status.json", status);
            /* The framing probe and the renderer's own status (targets, the
             * map layer's writes, the upload cap), for the diagnostics. */
            write_sibling_file(cfg.str("fifo_path"), "probe.txt",
                               producer.probe_report() + "stream: " + (detail.empty() ? "whole" : detail) + "\n" +
                                   hud.report() + feed.report());
            feed.request_report();
            last_status_write = now;
        }
    }
    log_info("stopped");
    return 0;
}

int run(const Config &cfg, const char *target)
{
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", cfg.str("socket_path").c_str());
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        std::fprintf(stderr, "jap-waze-on-cluster: %s: %s\n", addr.sun_path, strerror(errno));
        return 1;
    }
    const std::string t = target;
    std::string start = "{\"cmd\":\"start\",\"target\":" + json_escape(target) + "}\n";
    if (t == "hud" || t == "hud-off" || t == "hud-video")
        start = "{\"cmd\":\"hud\",\"mode\":\"" + std::string(t == "hud" ? "arrows" : t == "hud-off" ? "off" : "video") + "\"}\n";
    else if (t == "hud-dump" || t == "map-dump")
        start = "{\"cmd\":\"" + t + "\"}\n";
    if (write(fd, start.data(), start.size()) != (ssize_t)start.size())
        return 1;
    char buf[1024];
    while (!g_stop) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0)
            break;
        fwrite(buf, 1, (size_t)n, stdout);
        fflush(stdout);
    }
    close(fd);
    return 0;
}

bool load_local(Config *cfg, std::string *err)
{
    std::string path = cfg->str("local_conf");
    return path.empty() || access(path.c_str(), F_OK) != 0 || cfg->load(path, err);
}

void usage()
{
    std::fprintf(stderr,
                 "usage: jap-waze-on-cluster [-c FILE] [-s key=value]... [run sail|map|panel|hud|hud-off|hud-video|hud-dump|map-dump | bench-pack I420_FILE]\n");
}

/* Times pan_blocks' packing (4:2:0 to RGB and the block fit) of each
 * tex_size I420 frame in the file on 1 to 4 threads, ten rounds each. */
int bench_pack(const Config &cfg, const char *path)
{
    uint32_t w = 0, h = 0;
    if (!cfg.size("tex_size", &w, &h) || texture_size_error(w, h) || h % 4) {
        std::fprintf(stderr, "bench-pack: tex_size must be a valid map size with a height that is a multiple of 4\n");
        return 2;
    }
    FILE *f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "bench-pack: %s: %s\n", path, strerror(errno)); return 2; }
    const size_t frame = (size_t)w * h * 3 / 2;
    std::vector<uint8_t> i420(frame);
    std::vector<std::vector<uint8_t>> atlases;
    while (atlases.size() < 64 && std::fread(i420.data(), 1, frame, f) == frame) {
        std::vector<uint8_t> a((size_t)w * atlas_height(h));
        std::memcpy(a.data(), i420.data(), (size_t)w * h);
        const uint8_t *u = i420.data() + (size_t)w * h, *v = u + (size_t)w / 2 * (h / 2);
        for (uint32_t y = 0; y < h / 2; y++) {
            std::memcpy(&a[(size_t)(h + y) * w], u + (size_t)y * (w / 2), w / 2);
            std::memcpy(&a[(size_t)(h + y) * w + w / 2], v + (size_t)y * (w / 2), w / 2);
        }
        atlases.push_back(std::move(a));
    }
    std::fclose(f);
    if (atlases.empty()) { std::fprintf(stderr, "bench-pack: no whole %ux%u frame in %s\n", w, h, path); return 2; }
    std::vector<uint8_t> out(block_bytes(w, h));
    std::printf("bench-pack: %zu frames of %ux%u, %zu B packed each\n", atlases.size(), w, h, out.size());
    for (unsigned threads = 1; threads <= 4; threads++) {
        double sum = 0, worst = 0;
        int n = 0;
        for (int round = 0; round < 10; round++)
            for (const std::vector<uint8_t> &a : atlases) {
                const auto t0 = std::chrono::steady_clock::now();
                pack_blocks(a.data(), w, h, out.data(), threads);
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                sum += ms;
                worst = std::max(worst, ms);
                n++;
            }
        std::printf("bench-pack: %u thread%s: %.2f ms mean, %.2f ms max\n", threads, threads > 1 ? "s" : "",
                    sum / n, worst);
    }
    return 0;
}

}  // namespace

/* The keys beyond the shared Config's own that this tool reads. Registered
 * before any conf is loaded, so Config accepts them. */
void define_keys(Config *cfg)
{
    cfg->define("fifo_path", "/run/jap-waze-on-cluster/alt.h264");
    cfg->define("carplay_group", "video");
    cfg->define("udp_port", "20557");
    cfg->define("avb_stream", "media_transport.avb_streaming.15");
    cfg->define("drm_device", "/dev/dri/renderD128");
    cfg->define("tex_size", "1088x480");
    cfg->define("src_crop", "");
    cfg->define("map_rect", "");
    cfg->define("dirty_threshold", "8");
    cfg->define("dirty_settle_ms", "1000");
    cfg->define("stall_ms", "6000");
    cfg->define("idle_ms", "60000");
    cfg->define("freeze_ms", "10000");
    cfg->define("freeze_min_fps", "3");
    cfg->define("blank_ms", "10000");
    cfg->define("alt_screen_port", "6031");
    cfg->define("alt_screen_uid", "13002");
    cfg->define("target_fps", "30");
    cfg->define("pan_blocks", "1");
    cfg->define("picture_fps", "15");
    cfg->define("hidden_fps", "5");
    cfg->define("keep_ms", "500");
    cfg->define("still_ms", "1000");
    cfg->define("tex_level_bits", "2");
    cfg->define("target", "map");
    cfg->define("hide_bmw_guidance", "1");
    cfg->define("state_file", "/var/opt/jap-waze-on-cluster/state/jap-waze-on-cluster.state.conf");
    cfg->define("hud_mode", "arrows");
    cfg->define("hud_backend", "native");
    cfg->define("hud_rect", "240x160+280+160");
    cfg->define("hud_reader", "card");
    cfg->define("hud_card", "1");
    cfg->define("hud_card_icon_rect", "85x90+150+26");
    cfg->define("hud_card_dist_rect", "320x60+236+40");
    cfg->define("hud_card_plain_rect", "80x80+580+30");
    cfg->define("hud_card_street_rect", "510x60+150+114");
    cfg->define("hud_card_roi", "580x220+120+0");
    cfg->define("hud_car_x", "544");
    cfg->define("hud_car_y", "360");
    cfg->define("hud_min_luma", "235");
    cfg->define("hud_roi", "1088x480+0+0");
    cfg->define("hud_icon_rect", "88x88+294+80");
    cfg->define("hud_dist_rect", "200x56+390+96");
    cfg->define("hud_templates", "/var/opt/jap-waze-on-cluster/hud-templates");
    cfg->define("hud_builtin_icons", "0");
    cfg->define("hud_min_score", "60");
    cfg->define("hud_sample_ms", "250");
    cfg->define("hud_stable", "2");
    cfg->define("hud_dump_flag", "/tmp/jap-woc-hud-dump");
    cfg->define("hud_dump_dir", "/tmp/jap-woc-hud");
    cfg->define("hud_dump_max", "200");
    cfg->define("hud_bg_hold_s", "600");  /* hudguide's: the reckoned turn's safety cap */
    cfg->define("hud_route_uv", "");      /* unset: route -1, hudguide never reckons */
    cfg->define("hud_route_ymin", "0");
    cfg->define("hud_route_min_px", "40");
    cfg->define("hud_geo_uv", "152,190,28,90");   /* measured on the car's night frames, 2026-11-08 */
    cfg->define("hud_route_bright_ymin", "150");
    cfg->define("hud_label_uv", "156,168,76,90");
    cfg->define("hud_geo_car_x", "541");
    cfg->define("hud_geo_car_y", "250");
    cfg->define("hud_geo_next", "0");   /* read by hudguide.js only */
    cfg->define("render_socket", "/dev/shm/cluster-render/render.sock");
    cfg->define("log_file", "/var/opt/jap-waze-on-cluster/log/jap-waze-on-cluster.log");
    cfg->define("log_file_kb", "512");
    for (const char *key : kIgnoredKeys)
        cfg->define(key, "");
}

int main(int argc, char **argv)
{
    cld::log_name = "jap-waze-on-cluster";
    Config cfg;
    define_keys(&cfg);
    std::string err;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        bool has_arg = i + 1 < argc;
        if (!std::strcmp(argv[i], "-c") && has_arg) {
            if (!cfg.load(argv[++i], &err) || !load_local(&cfg, &err)) { std::fprintf(stderr, "jap-waze-on-cluster: %s\n", err.c_str()); return 2; }
        } else if (!std::strcmp(argv[i], "-s") && has_arg) {
            if (!cfg.set_pair(argv[++i], &err)) { std::fprintf(stderr, "jap-waze-on-cluster: %s\n", err.c_str()); return 2; }
        } else {
            usage();
            return 2;
        }
    }
    log_verbose = (int)cfg.num("verbose");
    signal(SIGPIPE, SIG_IGN);

    if (i == argc)
        return serve(cfg);
    if (i + 2 == argc && !std::strcmp(argv[i], "run"))
        return run(cfg, argv[i + 1]);
    if (i + 2 == argc && !std::strcmp(argv[i], "bench-pack"))
        return bench_pack(cfg, argv[i + 1]);
    usage();
    return 2;
}
