// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
/* How much of the decoded stream reaches the renderer (measured 05/11 on the
 * unit while driving: of 15075 frames sent, 11403 were replaced before the
 * renderer used them and 3399 changed nothing, at about 41% of a core here
 * and 26% in cluster-render).
 *
 * PictureGate, on the decode thread: a frame gets a picture (VPP, packing,
 * level rounding, change sampling, hand-off) at most once per interval; the
 * others are decoded only, as catch-up already does. The interval is the
 * main thread's choice: 1000 / picture_fps while the layer is shown,
 * 1000 / hidden_fps otherwise (at least the freeze watch's min_fps, so a
 * frozen stream is still seen); 0 gives every frame a picture.
 *
 * SendGate, on the main thread: a picture goes to the renderer when it
 * changed (ChangeSampler), when the layer has just been shown, and at least
 * every keep_ms otherwise, so a change the sampler's grid misses shows
 * within keep_ms and the renderer's stall timer never fires. */
#ifndef WOC_SEND_GATE_H
#define WOC_SEND_GATE_H

#include <stdint.h>

#include <algorithm>

namespace woc {

class PictureGate {
public:
    /* Whether a frame decoded at now_ms gets a picture under interval_ms. */
    bool due(int64_t now_ms, int64_t interval_ms) const
    {
        return interval_ms <= 0 || last_ms_ < 0 || now_ms - last_ms_ >= interval_ms || now_ms < last_ms_;
    }
    /* A picture was made at now_ms. Phase-locked to the interval's grid, not
     * spaced from the last picture: frames at 45 ms under 66 ms give 15 a
     * second, not every other one (11). */
    void made(int64_t now_ms, int64_t interval_ms)
    {
        if (last_ms_ < 0 || interval_ms <= 0 || now_ms < last_ms_)
            last_ms_ = now_ms;
        else
            last_ms_ = std::min(now_ms, std::max(last_ms_ + interval_ms, now_ms - interval_ms));
    }

private:
    int64_t last_ms_ = -1;
};

class SendGate {
public:
    explicit SendGate(int64_t keep_ms = 500) : keep_ms_(keep_ms) {}
    /* Whether the picture taken at now_ms goes to the renderer; force: the
     * renderer's layer has no frame yet. */
    bool send(int64_t now_ms, bool changed, bool shown, bool force = false)
    {
        const bool just_shown = shown && !was_shown_;
        was_shown_ = shown;
        if (!(changed || just_shown || force || last_ms_ < 0 || now_ms - last_ms_ >= keep_ms_ || now_ms < last_ms_)) {
            held_++;
            return false;
        }
        last_ms_ = now_ms;
        return true;
    }
    uint64_t held() const { return held_; }

private:
    int64_t keep_ms_;
    int64_t last_ms_ = -1;
    bool was_shown_ = false;
    uint64_t held_ = 0;
};

}  // namespace woc

#endif
