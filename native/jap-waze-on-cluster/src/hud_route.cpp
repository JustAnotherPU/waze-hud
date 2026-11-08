// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
#include "hud_route.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <queue>

namespace woc {

namespace {

const unsigned kStep = 2;        /* chroma samples per grid cell: 4 frame px */
const unsigned kPx = 2 * kStep;  /* frame px per grid cell */
const int kHop = 2;              /* a hop: up to 2 cells (antialiasing gaps in a thin line) */
const int kLabelMin = 60;        /* cells: Waze's smallest box (one short word) is about 110 */
const int kLabelYMax = 130;      /* a box's blue is luma 96-103; a thin route in its chroma 172 */
const float kWin = 24, kExit = 32, kAfter = 48, kOff = 16;   /* px along the route */

enum { kNone = 0, kAhead = 1, kLabel = 2 };

/* degrees from straight up, right positive (frame y grows down) */
float heading(float ax, float ay, float bx, float by) { return std::atan2(bx - ax, ay - by) * 57.29578f; }

float wrap(float d)
{
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    return d;
}

struct Pt {
    float x, y, s;   /* frame px, arc length from the seed */
    int box;         /* the label box this point is under, -1 on the route */
};

/* The first index in [i, end] at least `ds` px along from i, or -1. */
int ahead_of(const std::vector<Pt> &p, int i, int end, float ds)
{
    for (int j = i; j <= end; j++)
        if (p[j].s - p[i].s >= ds) return j;
    return -1;
}

/* As ahead_of, back towards begin. */
int behind_of(const std::vector<Pt> &p, int i, int begin, float ds)
{
    for (int j = i; j >= begin; j--)
        if (p[i].s - p[j].s >= ds) return j;
    return -1;
}

float head(const std::vector<Pt> &p, int a, int b) { return heading(p[a].x, p[a].y, p[b].x, p[b].y); }

/* The heading leaving i (within [i, end]): over kAfter px, else to the end
 * when that is at least `least` px; nan without. */
float leaving(const std::vector<Pt> &p, int i, int end, float least)
{
    const int j = ahead_of(p, i, end, kAfter);
    if (j >= 0) return head(p, i, j);
    return p[end].s - p[i].s >= least ? head(p, i, end) : NAN;
}

/* As leaving, arriving at i from within [begin, i]. */
float arriving(const std::vector<Pt> &p, int i, int begin, float least)
{
    const int j = behind_of(p, i, begin, kAfter);
    if (j >= 0) return head(p, j, i);
    return p[i].s - p[begin].s >= least ? head(p, begin, i) : NAN;
}

}  // namespace

GeoReading RouteTrace::read(const uint8_t *atlas, unsigned w, unsigned h)
{
    GeoReading g;
    if (!c_.ahead.set() || w < 64 || h < 64)
        return g;
    const int gw = (int)(w / 2 / kStep), gh = (int)(h / 2 / kStep), n = gw * gh;
    const uint8_t *uv = atlas + (size_t)w * h;   /* row r: U in columns 0..w/2-1, V after */
    const RouteBounds &A = c_.ahead, &L = c_.label;
    cls_.assign(n, kNone);
    for (int gy = 0; gy < gh; gy++)
        for (int gx = 0; gx < gw; gx++) {
            const unsigned cx = gx * kStep, cy = gy * kStep;
            const int u = uv[(size_t)cy * w + cx], v = uv[(size_t)cy * w + w / 2 + cx];
            const uint8_t *y0 = atlas + (size_t)2 * cy * w + 2 * cx, *y1 = y0 + w;
            /* the brightest of the sample's four luma: a 2 px line may miss one */
            const int y = std::max(std::max(y0[0], y0[1]), std::max(y1[0], y1[1]));
            if (u >= A.umin && u <= A.umax && v >= A.vmin && v <= A.vmax && y >= A.ymin)
                cls_[gy * gw + gx] = kAhead;
            else if (L.set() && u >= L.umin && u <= L.umax && v >= L.vmin && v <= L.vmax && y0[0] < kLabelYMax)
                cls_[gy * gw + gx] = kLabel;
        }

    /* label boxes: label parts (hops of 2 over the text's holes) of kLabelMin
     * cells; everything inside one's bounds is that box (its text's
     * antialiasing reads as route) */
    blob_.assign(n, -1);
    std::vector<Rect> boxes;
    for (int i = 0; i < n; i++) {
        if (cls_[i] != kLabel || blob_[i] != -1) continue;
        const int id = (int)boxes.size();
        q_.assign(1, i);
        blob_[i] = id;
        int x0 = gw, y0 = gh, x1 = 0, y1 = 0;
        for (size_t k = 0; k < q_.size(); k++) {
            const int c = q_[k], cx = c % gw, cy = c / gw;
            x0 = std::min(x0, cx), x1 = std::max(x1, cx), y0 = std::min(y0, cy), y1 = std::max(y1, cy);
            for (int dy = -kHop; dy <= kHop; dy++)
                for (int dx = -kHop; dx <= kHop; dx++) {
                    const int nx = cx + dx, ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) continue;
                    const int m = ny * gw + nx;
                    if (cls_[m] == kLabel && blob_[m] == -1) blob_[m] = id, q_.push_back(m);
                }
        }
        Rect r;
        if ((int)q_.size() >= kLabelMin) {
            r.x = x0 * kPx, r.y = y0 * kPx, r.w = (x1 - x0 + 1) * kPx, r.h = (y1 - y0 + 1) * kPx;
            for (int yy = y0; yy <= y1; yy++)
                for (int xx = x0; xx <= x1; xx++) blob_[yy * gw + xx] = id;
        }
        boxes.push_back(r);   /* w 0: a stray, not a box */
    }
    auto box_of = [&](int i) { return blob_[i] >= 0 && boxes[blob_[i]].w ? blob_[i] : -1; };

    /* the seed: the route ahead nearest the chevron, in the window above it */
    const int car_gx = (int)(c_.car_x / kPx), top = std::max(0, (int)c_.car_y - 70) / (int)kPx,
              bottom = std::min(gh - 1, std::max(0, (int)c_.car_y - 22) / (int)kPx);
    int seed = -1;
    for (int gy = bottom; gy >= top && seed < 0; gy--)
        for (int d = 0; d <= 40 / (int)kPx && seed < 0; d++)
            for (int sgn = -1; sgn <= 1 && seed < 0; sgn += 2) {
                const int gx = car_gx + sgn * d, i = gy * gw + gx;
                if (gx >= 0 && gx < gw && cls_[i] == kAhead && box_of(i) < 0) seed = i;
            }
    for (int gy = top; gy <= bottom && !g.seed_covered; gy++)
        for (int gx = std::max(0, car_gx - 10); gx <= std::min(gw - 1, car_gx + 10); gx++)
            if (box_of(gy * gw + gx) >= 0) g.seed_covered = true;
    if (seed < 0) {
        g.passed = true;   /* no route ahead at the chevron */
        return g;
    }
    g.found = true;

    /* the walk: ahead cells above the chevron and the boxes they touch,
     * shortest by length (Dijkstra, tenths of a cell) so the path keeps to
     * the line's shape */
    dist_.assign(n, -1);
    parent_.assign(n, -1);
    typedef std::pair<int, int> Item;   /* cost, cell */
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    dist_[seed] = 0;
    pq.push(Item(0, seed));
    int far = seed;
    while (!pq.empty()) {
        const Item it = pq.top();
        pq.pop();
        const int c = it.second, cx = c % gw, cy = c / gw;
        if (it.first != dist_[c]) continue;
        if (box_of(c) < 0 && dist_[c] > dist_[far]) far = c;
        for (int dy = -kHop; dy <= kHop; dy++)
            for (int dx = -kHop; dx <= kHop; dx++) {
                const int nx = cx + dx, ny = cy + dy;
                if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= gw || ny > bottom) continue;
                const int m = ny * gw + nx;
                if (!(cls_[m] == kAhead || box_of(m) >= 0)) continue;
                const int d = dist_[c] + (int)std::lround(10 * std::hypot((float)dx, (float)dy));
                if (dist_[m] >= 0 && dist_[m] <= d) continue;
                dist_[m] = d;
                parent_[m] = c;
                pq.push(Item(d, m));
            }
    }
    std::vector<Pt> p;
    for (int c = far; c != -1; c = parent_[c])
        p.push_back(Pt{(float)(c % gw * kPx), (float)(c / gw * kPx), 0, box_of(c)});
    std::reverse(p.begin(), p.end());
    for (size_t i = 1; i < p.size(); i++)
        p[i].s = p[i - 1].s + std::hypot(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y);
    const float to_car = (float)c_.car_y - p[0].y;   /* the seed's distance from the chevron */

    /* runs on the route, apart where the path is under a box */
    std::vector<std::pair<int, int>> runs;
    for (int i = 0; i < (int)p.size(); i++) {
        if (p[i].box >= 0) continue;
        if (runs.empty() || p[i - 1].box >= 0) runs.push_back({i, i});
        else runs.back().second = i;
    }
    float before = 0;   /* straight up until the route says otherwise */
    {
        const int j = ahead_of(p, 0, runs[0].second, kWin);
        if (j >= 0) before = head(p, 0, j);
    }
    int bend = -1, crossed = -1;
    float angle = 0;
    for (size_t k = 0; k < runs.size() && bend < 0; k++) {
        const int a = runs[k].first, b = runs[k].second;
        if (k > 0) {   /* out of a box: the way it comes out against the way it went in */
            const int pa = runs[k - 1].first, pb = runs[k - 1].second;
            const float in_h = arriving(p, pb, pa, kWin), out = leaving(p, a, b, kExit / 2);
            if (std::isnan(out)) continue;   /* a scrap between boxes */
            const float d = wrap(out - (std::isnan(in_h) ? before : in_h));
            if (std::fabs(d) > c_.bend_deg) {
                bend = pb;   /* where it went in */
                crossed = p[a - 1].box;
                angle = d;
                break;
            }
            before = out;
        }
        for (int i = a; i <= b; i++) {
            const int j = ahead_of(p, i, b, kWin);
            if (j < 0) break;
            const int pb = behind_of(p, i, a, kWin);
            const float in_h = pb >= 0 ? head(p, pb, i) : before;
            if (pb >= 0) before = in_h;
            const float d = wrap(head(p, i, j) - in_h);
            if (std::fabs(d) <= c_.bend_deg) continue;
            /* the vertex: the sharpest point in the next window, the same way */
            bend = i;
            float best = std::fabs(d);
            for (int m = i + 1; m <= b && p[m].s - p[i].s <= kWin; m++) {
                const int mj = ahead_of(p, m, b, kWin), mb = behind_of(p, m, a, kWin);
                if (mj < 0) break;
                const float md = wrap(head(p, m, mj) - (mb >= 0 ? head(p, mb, m) : before));
                if (md * d > 0 && std::fabs(md) > best) best = std::fabs(md), bend = m;
            }
            /* the turn: the route kOff px either side of the vertex, away from
             * its rounding, against each other */
            const int ia = behind_of(p, bend, a, kOff), ib = ahead_of(p, bend, b, kOff);
            const float hin = ia >= 0 ? arriving(p, ia, a, kWin / 2) : NAN, hout = ib >= 0 ? leaving(p, ib, b, kWin / 2) : NAN;
            angle = wrap((std::isnan(hout) ? head(p, i, j) : hout) - (std::isnan(hin) ? before : hin));
            break;
        }
    }
    /* the route leaving the chevron sideways: the car is at the turn */
    const bool sideways = std::fabs(wrap(runs[0].second > 0 && ahead_of(p, 0, runs[0].second, kWin) >= 0
                                             ? head(p, 0, ahead_of(p, 0, runs[0].second, kWin)) : 0)) >= c_.side_deg;
    if (bend < 0) {
        g.conf = 60;   /* straight on as far as it goes */
        g.passed = sideways;
        return g;
    }
    g.bend = true;
    g.x = (int)p[bend].x, g.y = (int)p[bend].y;
    g.angle = angle;
    /* a bend short of side_deg (a fork, the road's own curve) names no side:
     * nothing rather than wrong, and the walk stops there all the same */
    g.side = std::fabs(angle) >= c_.side_deg ? (angle < 0 ? -1 : 1) : 0;
    g.arc = (int)std::lround(p[bend].s + to_car);
    g.passed = sideways || g.y >= (int)c_.car_y - 24 || g.arc <= 40;
    g.conf = 100;
    if (crossed >= 0) {
        g.has_label = true;
        g.label = boxes[crossed];
        g.conf -= 30;
        /* ways in and out of the box: walked route cells beside it, in parts */
        const Rect &r = boxes[crossed];
        const int x0 = r.x / kPx - kHop, x1 = (r.x + r.w) / kPx - 1 + kHop, y0 = r.y / kPx - kHop, y1 = (r.y + r.h) / kPx - 1 + kHop;
        q_.clear();
        for (int yy = std::max(0, y0); yy <= std::min(gh - 1, y1); yy++)
            for (int xx = std::max(0, x0); xx <= std::min(gw - 1, x1); xx++) {
                const int m = yy * gw + xx;
                if (cls_[m] == kAhead && box_of(m) < 0 && dist_[m] >= 0) dist_[m] = -2, q_.push_back(m);
            }
        int parts = 0;
        const std::vector<int> ring = q_;
        for (int s : ring) {
            if (dist_[s] != -2) continue;
            parts++;
            q_.assign(1, s);
            dist_[s] = -3;
            for (size_t k = 0; k < q_.size(); k++) {
                const int c = q_[k], cx = c % gw, cy = c / gw;
                for (int dy = -kHop; dy <= kHop; dy++)
                    for (int dx = -kHop; dx <= kHop; dx++) {
                        const int nx = cx + dx, ny = cy + dy;
                        if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) continue;
                        const int m = ny * gw + nx;
                        if (dist_[m] == -2) dist_[m] = -3, q_.push_back(m);
                    }
            }
        }
        if (parts > 2) g.conf -= 30;   /* in, and more than one way out */
    } else {
        /* a box the bend sits in or beside, for the street later */
        for (const Rect &r : boxes)
            if (r.w && g.x + 8 >= (int)r.x && g.x <= (int)(r.x + r.w) + 8 && g.y + 8 >= (int)r.y && g.y <= (int)(r.y + r.h) + 8) {
                g.has_label = true;
                g.label = r;
                break;
            }
    }
    if (p.back().s - p[bend].s < kExit) g.conf -= 20;   /* little route past it */
    g.conf = std::max(10, g.conf);
    return g;
}

std::string geo_json(const GeoReading &g, int64_t at)
{
    char buf[256], lab[64] = "null";
    if (g.has_label)
        std::snprintf(lab, sizeof lab, "[%u,%u,%u,%u]", g.label.x, g.label.y, g.label.w, g.label.h);
    std::snprintf(buf, sizeof buf, "{\"x\":%d,\"y\":%d,\"side\":\"%s\",\"angle\":%ld,\"arc\":%d,\"label\":%s,\"conf\":%d,\"passed\":%d",
                  g.x, g.y, g.side < 0 ? "left" : g.side > 0 ? "right" : "", std::lround(g.angle), g.arc, lab, g.conf,
                  g.passed ? 1 : 0);
    return std::string(buf) + (at >= 0 ? ",\"at\":" + std::to_string((long long)at) : std::string()) + "}";
}

}  // namespace woc
