// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
/* The route ahead traced on Waze's cluster map (README.md "HUD arrows"),
 * for the turn while CarPlay's card is gone. Waze draws the route ahead of
 * the car bright cyan and the route behind it dark teal, and names the
 * streets along it in blue boxes with a pointer tail; the cluster stream
 * keeps coming while another app is on the centre screen.
 *
 * Measured on the car's own night frames (2026-11-08, test/data/map-route-*):
 * the route ahead is luma 168-175, U 172-179, V 36-47 (a thin far stretch,
 * 2 px, blends to U 156-160, V 68-88 at luma 172); the label boxes luma
 * 96-103, U 160-167, V 80-87 (their text white); the route behind luma
 * 88-95, U 152-155, V 76-79; Waze's chevron is rows 233-265, centre about
 * 541,250 (the 2026-11-03 frame had it at 360: the view can move it).
 * hud_route_uv's bounds take the boxes and the route behind, not the route
 * ahead: that one is a presence test only.
 *
 * read() looks at every 2nd chroma sample across and down (a 4 px grid,
 * 272x120 on 1088x480: 4 px keeps the far route's 4 px line connected):
 * - ahead: `ahead` bounds (chroma box, the brightest of the sample's four
 *   luma at least ymin); label: `label` bounds under luma 130; label boxes
 *   are label parts of at least 60 samples, everything in their bounds
 *   theirs (the text's antialiasing reads as route);
 * - the seed: the ahead sample nearest the chevron in a window just above
 *   it (car_x +- 40, car_y - 70 to car_y - 22: the chevron's own cyan rim
 *   stops 17 px above its centre); none and there is no route ahead
 *   (found false), the route behind is never looked at;
 * - the shortest walk (by length) from the seed over ahead samples above
 *   the chevron, hops of up to 2 samples (a thin line's gaps), through any
 *   label box it touches (a box often covers the junction); the farthest
 *   ahead sample and the path back to the seed are the route;
 * - along the path (frame pixels, by arc length) the heading before is the
 *   seed's first 24 px (straight up when a box sits right there); the
 *   first point whose next 24 px turn more than bend_deg (25) from its
 *   last 24 px is the bend (moved to the sharpest point near it). A box
 *   crossed first is judged by the route coming out of it (up to 48 px)
 *   against the route going in; past bend_deg, the bend is where the route
 *   went into the box and the box is the label. The walk stops at the
 *   first bend;
 * - the angle: the route 16 px either side of the bend (up to 48 px long)
 *   against each other, right positive; side its sign from side_deg (45)
 *   only: a fork or the road's own curve names none.
 *
 * passed: no route ahead at the chevron, the route leaving the chevron
 * sideways (the car at the turn), or the bend within 40 px of it along the
 * route (HudArrows decides what that is worth: hud_arrows.cpp, trace()). seed_covered:
 * a label box over the seed window, so "no route ahead" may be that box.
 * conf 0 (none) to 100: lowered for a bend judged across a box, a short
 * stretch past the bend, and a box the route leaves by more than one way. */
#ifndef WOC_HUD_ROUTE_H
#define WOC_HUD_ROUTE_H

#include <stdint.h>
#include <string>
#include <vector>

#include "hud_banner.h"
#include "hud_maparrow.h"

namespace woc {

struct RouteTraceConfig {
    RouteBounds ahead{152, 190, 28, 90, 150, 1};   /* hud_geo_uv, hud_route_bright_ymin */
    RouteBounds label{156, 168, 76, 90, 0, 1};     /* hud_label_uv (luma under 130 built in) */
    unsigned car_x = 541, car_y = 250;             /* hud_geo_car_x/y: the chevron's centre */
    float bend_deg = 25, side_deg = 45;
};

struct GeoReading {
    bool found = false;          /* a route ahead from the chevron */
    bool bend = false;           /* a bend past bend_deg on it */
    int x = 0, y = 0;            /* the bend, frame pixels */
    int side = 0;                /* -1 left, 1 right, 0 none */
    float angle = 0;             /* degrees, right positive */
    int arc = 0;                 /* px along the route from the chevron to the bend */
    bool has_label = false;
    Rect label;                  /* the box the bend sits in, frame pixels */
    int conf = 0;
    bool passed = false;
    bool seed_covered = false;
};

class RouteTrace {
public:
    explicit RouteTrace(const RouteTraceConfig &c) : c_(c) {}
    /* The decoded frame's 4:2:0 atlas (yuv_atlas.h), as route_pixels. */
    GeoReading read(const uint8_t *atlas, unsigned w, unsigned h);

private:
    RouteTraceConfig c_;
    std::vector<uint8_t> cls_;
    std::vector<int> blob_, dist_, parent_, q_;
};

/* hud.json's geo object for g, as {"x":..,"y":..,"side":"left"|"right"|"",
 * "angle":..,"arc":..,"label":[x,y,w,h]|null,"conf":..,"passed":0|1}, then
 * "at" (the trace's time, as hud.json's) when at is 0 or more. */
std::string geo_json(const GeoReading &g, int64_t at = -1);

}  // namespace woc

#endif
