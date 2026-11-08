// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
/* Waze's next turn read from the map itself (README.md "HUD arrows").
 * Waze's cluster stream has no instruction card (no icon, no distance, seen
 * on the car 2026-11-03): only the map, the car's chevron and, at the next
 * junction, a white arrow drawn on the route. That arrow is read here.
 *
 * The luma at or above min_luma, below the top 15% of the frame, is opened
 * (3x3: drops the chevron's 1-2 px white rim and thin text strokes) and
 * split into 8-connected parts. The arrow is the part with the most pixels
 * whose whole 7x7 neighbourhood is white (a thick stroke; text and labels
 * have none) that is not a filled box (road shields), within size limits.
 * Its two farthest-apart ends (geodesic, inside the part) are the stem and
 * the tip; the stem is the end nearer the car. The turn is the signed angle
 * between the stem's direction (stem to 30% along) and the tip's (70% to
 * the tip), right positive, named by turn_label(). The distance is not on
 * screen, so a reading has none. */
#ifndef WOC_HUD_MAPARROW_H
#define WOC_HUD_MAPARROW_H

#include <stdint.h>
#include <string>
#include <vector>

#include "hud_banner.h"

namespace woc {

struct MapArrowConfig {
    unsigned car_x = 544, car_y = 360;   /* the chevron, in frame pixels */
    unsigned min_luma = 235;
    unsigned skip_top_pct = 15;   /* Waze's labels and the sky */
};

/* straight, slight_*, left/right, sharp_*, uturn_left (any turn past 165
 * degrees: right-hand traffic) for a signed angle in degrees; slight below
 * slight_max. */
std::string turn_label(float deg, float slight_max = 45);

class MapArrowReader {
public:
    explicit MapArrowReader(const MapArrowConfig &c) : c_(c) {}
    /* The w x h luma plane (stride w). */
    Reading read(const uint8_t *y, unsigned w, unsigned h);
    /* The last arrow's angle, for the probe; 0 with none. */
    float angle() const { return angle_; }

private:
    MapArrowConfig c_;
    float angle_ = 0;
    std::vector<uint8_t> m_, e_;
    std::vector<uint32_t> ii_;
    std::vector<int> lab_, q_, dist_;
};

/* The manoeuvre card iOS draws on the centre screen (CarPlay's guidance
 * panel, seen on the car 2026-11-03: Waze's arrow icon, the distance in SF
 * Pro Bold, the street), read from the centre screen's luma. The card is
 * there when `plain` (a rect of the card right of the text) is flat. Its
 * icon is binarised (day or night alike) and read as an arrow (the
 * MapArrowReader, the stem the end nearer the icon's bottom centre): no icon
 * templates. Its distance is read with glyph templates (dist-*.pgm, made by
 * tools/hud_font_templates.py); unknown glyphs leave the distance out. Its
 * street, when a street set is given (str-*.pgm, the same tool), is read the
 * same way; one glyph under kStreetMin and there is none. */
struct CardConfig {
    /* street: on test/data/carplay-card-30m.pgm the line is at y 137-163, x
     * from 165; up 15 with lane arrows (as the distance), with room for
     * accents and descenders, short of the card's right corners */
    Rect icon{150, 26, 85, 90}, dist{236, 40, 320, 60}, plain{580, 30, 80, 80}, street{150, 114, 510, 60};
};

class CardReader {
public:
    CardReader(const CardConfig &c, const Templates *t, const Templates *street = nullptr)
        : c_(c), rec_(t, c.icon, c.dist), street_(street, c.icon, c.street), has_street_(street) {}
    Reading read(const uint8_t *y, unsigned w, unsigned h);
    float angle() const { return angle_; }
    const std::string &text() const { return text_; }   /* the distance's glyphs as read */

private:
    CardConfig c_;
    Recogniser rec_, street_;
    bool has_street_;
    float angle_ = 0;
    std::string text_;
};

/* Waze's route line on the cluster map (light blue, drawn only while it
 * navigates): the chroma samples of the 4:2:0 atlas (yuv_atlas.h) within
 * [umin, umax] x [vmin, vmax] whose luma is at least ymin, every 4th sample
 * across and down (1088x480: 8160 looked at). The colour is the car's to
 * calibrate (run map-dump, tools/route_calibrate.py): unset (umin < 0),
 * there is no answer. */
struct RouteBounds {
    int umin = -1, umax = -1, vmin = -1, vmax = -1, ymin = 0;
    unsigned min_px = 1;
    bool set() const { return umin >= 0; }
};
/* "umin,umax,vmin,vmax" (0-255 each, min <= max) into b; false when not that. */
bool parse_route_uv(const std::string &s, RouteBounds *b);
unsigned route_pixels(const uint8_t *atlas, unsigned w, unsigned h, const RouteBounds &b);
/* 1: at least min_px route samples, 0: fewer, -1: bounds unset. */
int route_state(const uint8_t *atlas, unsigned w, unsigned h, const RouteBounds &b);

/* The atlas as base.yuv (I420: Y, U, V planes) and, with ppm, base.ppm
 * (full-range BT.601, as the map shader draws it, chroma nearest), both new
 * files (open_dump); false on an error or when one exists. */
bool write_map_dump(const std::string &base, const uint8_t *atlas, unsigned w, unsigned h, bool ppm = true);

}  // namespace woc

#endif
