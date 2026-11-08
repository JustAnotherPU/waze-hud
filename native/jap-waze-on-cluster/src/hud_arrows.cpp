// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
#include "hud_arrows.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <cmath>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "log.h"

namespace woc {

namespace {

std::string reading_text(const Reading &r)
{
    return r.maneuver.empty() ? std::string("none")
                              : r.maneuver + (r.distance.empty() ? "" : " " + r.distance) +
                                    (r.street.empty() ? "" : ", " + r.street);
}

/* The wall clock in ms, for file names hudguide's lines pair with. */
long long wall_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A file name part: letters, digits, . and _ only. */
std::string safe(const std::string &s)
{
    std::string o;
    for (char c : s)
        o.push_back(std::isalnum((unsigned char)c) || c == '.' || c == '_' ? c : '_');
    return o.empty() ? "none" : o;
}

}  // namespace

ImageHud::ImageHud(const ImageHudConfig &cfg) : cfg_(cfg), client_("jap-waze-on-cluster-hud", cfg.socket)
{
    lo_.target = "hud";
    lo_.x = (int)cfg_.rect.x;
    lo_.y = (int)cfg_.rect.y;
    lo_.w = cfg_.rect.w;
    lo_.h = cfg_.rect.h;
    lo_.visible = false;
    lo_.contend = false;   /* never takes the slot from stock content */
    cr::ImageOptions io;
    io.w = cfg_.rect.w;
    io.h = cfg_.rect.h;
    io.fps = 2;
    layer_ = client_.image(lo_, io);
    img_.assign((size_t)io.w * io.h * 4, 0);
    client_.on_target = [this](const std::string &t, const std::string &state, const std::string &holder,
                               const std::string &) {
        if (t != "hud")
            return;
        target_state_ = state;
        holder_ = holder;
        if (state == "shown")
            give_ways_ = 0;
        /* Someone else's content holds the HUD: give way at once. */
        const bool taken = state == "parked" || (state == "waiting" && !holder.empty());
        if (taken && lo_.visible) {
            static const int kRetryS[] = {10, 30, 60};
            retry_ms_ = now_ + kRetryS[std::min(give_ways_, 2)] * 1000;
            give_ways_++;
            held_ = true;
            set_visible(false);
            log_info("hud: the slot is held by %s; arrows hidden, asking again in %lld s",
                     holder.empty() ? "other content" : holder.c_str(), (long long)(retry_ms_ - now_) / 1000);
        }
    };
    client_.on_budget = [this](unsigned cap, unsigned max, const std::string &reason) {
        cap_kb_ = cap;
        max_kb_ = max;
        budget_reason_ = reason;
    };
    client_.on_error = [](const std::string &op, uint32_t, const std::string &reason) {
        log_error("hud: %s refused: %s", op.c_str(), reason.c_str());
    };
}

unsigned ImageHud::interval_ms() const
{
    unsigned ms = cfg_.min_interval_ms;
    /* A lowered shared cap: the map needs it more than the HUD. */
    if (max_kb_ && cap_kb_ && cap_kb_ < max_kb_)
        ms = budget_reason_.compare(0, 12, "at the floor") == 0 ? 5000 : (unsigned)((uint64_t)ms * max_kb_ / cap_kb_ * 2);
    return std::min(ms, 5000u);
}

void ImageHud::set_visible(bool on)
{
    lo_.visible = on;
    client_.set(layer_, lo_);
}

void ImageHud::show(const Reading &r, int64_t) { want_ = r; }

void ImageHud::poll(int64_t now_ms)
{
    now_ = now_ms;
    client_.poll(now_ms);
    if (held_ && now_ms >= retry_ms_)
        held_ = false;
    if (!client_.connected())
        return;
    const bool clear = want_.maneuver.empty();
    if (clear && lo_.visible)
        set_visible(false);
    if (!clear && held_)
        return;
    if (want_ == sent_ || now_ms < next_write_ms_) {
        if (!clear && want_ == sent_ && !lo_.visible)
            set_visible(true);   /* back after giving way */
        return;
    }
    const unsigned w = cfg_.rect.w, h = cfg_.rect.h;
    render_hud(&next_, w, h, want_.maneuver, want_.distance);
    /* Only the rows that changed (a distance tick: the text's); a cleared
     * image is blanked too, so the next arrow never shows the old one. */
    unsigned r0 = h, r1 = 0;
    for (unsigned y = 0; y < h; y++)
        if (std::memcmp(&next_[(size_t)y * w * 4], &img_[(size_t)y * w * 4], (size_t)w * 4)) {
            r0 = std::min(r0, y);
            r1 = y;
        }
    if (r0 <= r1) {
        const std::vector<cr::FrameRect> rects{{0, (uint16_t)r0, (uint16_t)w, (uint16_t)(r1 - r0 + 1)}};
        client_.pixels(layer_, rects, &next_[(size_t)r0 * w * 4], (size_t)(r1 - r0 + 1) * w * 4);
        writes_++;
        next_write_ms_ = now_ms + interval_ms();
    }
    img_.swap(next_);
    sent_ = want_;
    if (!clear && !lo_.visible)
        set_visible(true);
}

std::string ImageHud::state() const
{
    char buf[256];
    std::snprintf(buf, sizeof buf, "image: %s; target %s%s%s%s; %u writes, at most every %u ms",
                  lo_.visible ? reading_text(sent_).c_str() : "nothing shown", target_state_.empty() ? "-" : target_state_.c_str(),
                  holder_.empty() ? "" : " by ", holder_.c_str(), held_ ? ", given way" : "", writes_, interval_ms());
    return buf;
}

void NativeGuidanceHud::show(const Reading &r, int64_t now_ms)
{
    if (r == want_ && r.street == want_.street)
        return;
    want_ = r;
    write(now_ms);
}

void NativeGuidanceHud::context(const std::string &source, bool stream, const std::string &map, int route, int64_t now_ms)
{
    if (source == source_ && stream == stream_ && map == map_ && route == route_)
        return;
    source_ = source, stream_ = stream, map_ = map, route_ = route;
    write(now_ms);
}

void NativeGuidanceHud::poll(int64_t now_ms)
{
    if (now_ms - written_ms_ >= 1000)   /* the heartbeat: hudguide clears a stale reading */
        write(now_ms);
}

static std::string json_str(const std::string &s)
{
    std::string o = "\"";
    for (char c : s)
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if ((unsigned char)c >= 0x20) o += c;
    return o + "\"";
}

void NativeGuidanceHud::write(int64_t now_ms)
{
    written_ms_ = now_ms;
    const std::string tmp = path_ + ".tmp";
    const std::string line = "{\"maneuver\":" + json_str(want_.maneuver) + ",\"distance\":" + json_str(want_.distance) +
                             (want_.street.empty() ? "" : ",\"street\":" + json_str(want_.street)) +
                             ",\"source\":" + json_str(source_) + ",\"stream\":" + (stream_ ? "1" : "0") +
                             ",\"map\":" + json_str(map_) + ",\"route\":" + std::to_string(route_) + ",\"at\":" + std::to_string((long long)now_ms) +
                             (geo_.empty() ? "" : ",\"geo\":" + geo_) + "}\n";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    bool ok = fd >= 0 && ::write(fd, line.data(), line.size()) == (ssize_t)line.size();
    if (fd >= 0) close(fd);
    ok = ok && rename(tmp.c_str(), path_.c_str()) == 0;
    if (!ok) {
        unlink(tmp.c_str());
        if (!failed_) log_warn("hud: cannot write %s: %s", path_.c_str(), strerror(errno));
    } else {
        writes_++;
    }
    failed_ = !ok;
}

std::string NativeGuidanceHud::state() const
{
    char buf[256];
    std::snprintf(buf, sizeof buf, "native: %s to %s, %u writes%s", reading_text(want_).c_str(), path_.c_str(), writes_,
                  failed_ ? ", the last failed" : "");
    return buf;
}

HudArrows::HudArrows(const HudConfig &cfg, std::unique_ptr<HudBackend> backend)
    : cfg_(cfg), rec_(&templates_, cfg.icon, cfg.dist, cfg.min_score), map_(cfg.map), geo_(cfg.geo), debounce_(cfg.stable),
      backend_(std::move(backend))
{
    if (cfg_.card) {
        load_card_templates();
        card_.reset(new CardReader(cfg_.card_cfg, &card_tpl_, &street_tpl_));
        grab_.reset(new ScreenGrab(cfg_.grab));
    }
    if (cfg_.reader == "card") {
        log_info("hud: CarPlay's card only: no arrow without it");
        return;
    }
    if (cfg_.reader == "map") {
        log_info("hud: reading the turn arrow on Waze's map (car at %u,%u)", cfg_.map.car_x, cfg_.map.car_y);
        return;
    }
    templates_.add_builtin(cfg_.builtin_icons);
    std::string log;
    const int n = templates_.load_dir(cfg_.templates_dir, &log);
    for (size_t a = 0, b; a < log.size(); a = b + 1) {
        b = log.find('\n', a);
        log_warn("hud template %s", log.substr(a, b - a).c_str());
    }
    log_info("hud: %d templates from %s; %zu icons, %zu glyphs%s", n, cfg_.templates_dir.c_str(), templates_.icons(),
             templates_.glyphs(), templates_.icons() ? "" : " (no icons: arrows need the calibration, README.md)");
}

/* The card's glyph templates: the folder's dist-*.pgm only (the built-in
 * stroke font is not the card's font), and its str-*.pgm apart for the
 * street; read again when the folder changes. */
void HudArrows::load_card_templates()
{
    struct stat st;
    tpl_mtime_ = stat(cfg_.templates_dir.c_str(), &st) == 0 ? (long)st.st_mtime : 0;
    card_tpl_ = Templates();
    std::string log;
    card_tpl_.load_dir(cfg_.templates_dir, &log);
    street_tpl_ = Templates();
    street_tpl_.load_dir(cfg_.templates_dir, &log, "str");
    log_info("hud: card distance: %zu glyphs, street: %zu, from %s%s", card_tpl_.glyphs(), street_tpl_.glyphs(),
             cfg_.templates_dir.c_str(),
             card_tpl_.glyphs() && street_tpl_.glyphs() ? "" : " (arrows without them: tools/hud_font_templates.py)");
}

void HudArrows::set_active(bool on, int64_t now_ms)
{
    if (on == active_)
        return;
    active_ = on;
    log_info("hud: arrows %s", on ? "on" : "off");
    if (!on) {
        if (grab_) grab_->want(false);
        debounce_ = Debounce(cfg_.stable);
        street_ = StreetHold();
        map_hint_.clear();
        source_ = "none";
        route_ = -1;
        backend_->show(Reading(), now_ms);
    }
    push_context(now_ms);
}

void HudArrows::set_stream(bool live, int64_t now_ms)
{
    if (live == stream_)
        return;
    stream_ = live;
    push_context(now_ms);
}

void HudArrows::push_context(int64_t now_ms)
{
    /* stream 0 while the arrows are off: hudguide never reckons then */
    backend_->context(source_, stream_ && active_, map_hint_, route_, now_ms);
}

void HudArrows::sample(const uint8_t *y, unsigned w, unsigned h, int64_t now_ms)
{
    if (now_ms - flag_checked_ms_ >= 1000) {
        flag_ = !cfg_.dump_flag.empty() && access(cfg_.dump_flag.c_str(), F_OK) == 0;
        flag_checked_ms_ = now_ms;
    }
    if (map_dump_) {
        map_dump_ = false;
        char n[64];
        std::snprintf(n, sizeof n, "/map-%ld-%ux%u", (long)time(nullptr), w, h);
        if (dump_dir_ok(cfg_.dump_dir) && write_map_dump(cfg_.dump_dir + n, y, w, h))
            log_info("hud: map frame written to %s%s.yuv and .ppm", cfg_.dump_dir.c_str(), n);
        else
            log_warn("hud: map dump to %s failed: %s", cfg_.dump_dir.c_str(), strerror(errno));
    }
    /* the flag: the map too, once a second, as hudguide's pairs.csv lines
     * (at: the wall clock in ms), the frame only (0.8 MB each) */
    if (flag_ && map_dumped_ < cfg_.dump_max && now_ms - last_map_dump_ms_ >= 1000) {
        last_map_dump_ms_ = now_ms;
        char n[64];
        std::snprintf(n, sizeof n, "/map-%lld", wall_ms());
        if (dump_dir_ok(cfg_.dump_dir) && write_map_dump(cfg_.dump_dir + n, y, w, h, false))
            map_dumped_++;
        else
            log_warn("hud: map dump to %s failed: %s", cfg_.dump_dir.c_str(), strerror(errno));
        if (map_dumped_ == cfg_.dump_max)
            log_info("hud: %u map frames written to %s, the most for one run", map_dumped_, cfg_.dump_dir.c_str());
    }
    const bool dumping = (flag_ || dump_left_) && dumped_ < cfg_.dump_max;
    if ((!active_ && !dumping) || now_ms - last_sample_ms_ < (int64_t)cfg_.sample_ms)
        return;
    last_sample_ms_ = now_ms;
    from_card_ = scr_ok_ = false;
    if (card_) {
        grab_->want(active_ || dumping);
        if (now_ms - tpl_checked_ms_ >= 10000) {
            tpl_checked_ms_ = now_ms;
            struct stat st;
            if ((stat(cfg_.templates_dir.c_str(), &st) == 0 ? (long)st.st_mtime : 0) != tpl_mtime_)
                load_card_templates();
        }
        scr_ok_ = grab_->latest(&scr_, now_ms, 1500);
        if (scr_ok_) {
            last_read_ = card_->read(scr_.data(), cfg_.grab.w, cfg_.grab.h);
            from_card_ = !last_read_.maneuver.empty();
        }
    }
    /* fresh card reads only: the hold's replay would count as a second read */
    if (from_card_) {
        street_.push(last_read_);
        card_seen_ms_ = now_ms;
        map_hint_.clear();
    } else if (cfg_.reader == "card") {
        /* the map's arrow goes apart (hudguide decides what it is worth), read
         * once a second: it costs; kept between reads, as hudguide counts it
         * over heartbeats */
        if (now_ms - map_ms_ >= 1000) {
            map_ms_ = now_ms;
            map_hint_ = map_.read(y, w, h).maneuver;
        }
        last_read_ = Reading();
    } else {
        last_read_ = cfg_.reader == "banner" ? rec_.read(y, w, h) : map_.read(y, w, h);
    }
    if (active_ && now_ms - route_ms_ >= 1000) {   /* cheap, but once a second is all hudguide reads */
        route_ms_ = now_ms;
        route_ = route_state(y, w, h, cfg_.route);
        trace(y, w, h, now_ms);
    }
    if (card_)
        last_read_ = hold_.pick(last_read_, from_card_, now_ms);
    if (last_read_.maneuver.empty())
        street_.push(Reading());   /* the card gone: its street too */
    if (dumping && now_ms - last_dump_ms_ >= 1000)
        dump(y, w, h, last_read_, now_ms);
    if (!active_)
        return;
    Reading out;
    if (debounce_.push(last_read_, &out))
        log_info("hud: %s", reading_text(out).c_str());
    Reading shown = debounce_.current();
    shown.street = shown.maneuver.empty() ? "" : street_.street();
    /* the map and banner readers (not the default) name themselves */
    source_ = shown.maneuver.empty() ? "none" : from_card_ ? "card" : cfg_.reader == "card" ? "hold" : cfg_.reader;
    backend_->show(shown, now_ms);   /* the backends write on change only */
    push_context(now_ms);
}

/* The route ahead, card or not (with the card: pairs to check it against).
 * passed, for hudguide (it ends the reckoned turn on two heartbeats): the
 * trace's own (the route leaving the chevron sideways, a bend within 40 px
 * of it), or no route ahead only when one was found within 5 s and no box
 * covers the seed window (a box near the turn hides the route there), or
 * a near bend (arc 120 px or less, within 10 s) followed by none or one
 * over 120 px farther (the turn behind us, the next one ahead). */
void HudArrows::trace(const uint8_t *atlas, unsigned w, unsigned h, int64_t now_ms)
{
    if (!cfg_.geo.ahead.set())
        return;
    GeoReading g = geo_.read(atlas, w, h);
    if (!g.found)
        g.passed = !g.seed_covered && now_ms - geo_found_ms_ <= 5000;
    else
        geo_found_ms_ = now_ms;
    if (g.found && now_ms - near_ms_ <= 10000 && near_arc_ > 0 && (!g.bend || g.arc > near_arc_ + 120))
        g.passed = true;
    if (g.bend && g.arc <= 120 && (now_ms - near_ms_ > 10000 || g.arc <= near_arc_ + 40))
        near_arc_ = g.arc, near_ms_ = now_ms;
    geo_last_ = g;
    backend_->geo(geo_json(g, now_ms), now_ms);   /* at: hudguide counts traces, not heartbeats */
}

void HudArrows::dump(const uint8_t *y, unsigned w, unsigned h, const Reading &r, int64_t now_ms)
{
    last_dump_ms_ = now_ms;
    if (dump_left_)
        dump_left_--;
    if (!dump_dir_ok(cfg_.dump_dir)) {
        log_warn("hud: dump to %s refused: not a folder of this user", cfg_.dump_dir.c_str());
        return;
    }
    char n[16];
    std::snprintf(n, sizeof n, "%04u", ++dumped_);
    const std::string base = cfg_.dump_dir + "/" + n;
    Gray g;
    if (scr_ok_) {
        Gray c;
        const std::string tag = safe(from_card_ ? r.maneuver : std::string()) + "_" + safe(card_->text());
        if (crop(scr_.data(), cfg_.grab.w, cfg_.grab.h, cfg_.card_roi, &c))
            write_png(base + "-card-" + tag + ".png", c);
    }
    if (cfg_.reader == "card")
        return;
    if (cfg_.reader == "map") {
        char a[32];
        std::snprintf(a, sizeof a, "%s%ld", map_.angle() < 0 ? "L" : "R", std::lround(std::fabs(map_.angle())));
        if (!(crop(y, w, h, cfg_.roi, &g) && write_png(base + "-map-" + safe(r.maneuver) + "_" + safe(a) + ".png", g)))
            log_warn("hud: dump to %s failed: %s", cfg_.dump_dir.c_str(), strerror(errno));
        return;
    }
    bool ok = crop(y, w, h, cfg_.roi, &g) && write_png(base + "-banner.png", g);
    ok = crop(y, w, h, cfg_.icon, &g) && write_png(base + "-icon-" + safe(r.maneuver) + ".png", g) && ok;
    if (crop(y, w, h, cfg_.dist, &g))
        ok = write_png(base + "-dist-" + safe(rec_.read_text(g)) + ".png", g) && ok;
    if (!ok)
        log_warn("hud: dump to %s failed: %s", cfg_.dump_dir.c_str(), strerror(errno));
    if (dumped_ == cfg_.dump_max)
        log_info("hud: %u dumps written to %s, the most for one run", dumped_, cfg_.dump_dir.c_str());
}

std::string HudArrows::report() const
{
    return std::string("hud: arrows ") + (active_ ? "on" : "off") + ", last read " + reading_text(last_read_) +
           ", shown " + reading_text(debounce_.current()) + (street_.street().empty() ? "" : ", " + street_.street()) +
           (map_hint_.empty() ? "" : ", map " + map_hint_) + ", route " + std::to_string(route_) +
           (cfg_.geo.ahead.set() ? ", geo " + geo_json(geo_last_) : std::string()) +
           (from_card_ ? std::string(" (centre screen card)") : std::string()) +
           (cfg_.reader == "map" ? ", map angle " + std::to_string((int)std::lround(map_.angle())) : std::string()) +
           (grab_ ? "; " + grab_->state() : std::string()) + "; " + backend_->state() + "\n";
}

}  // namespace woc
