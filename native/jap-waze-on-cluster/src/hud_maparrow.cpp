// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
#include "hud_maparrow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace woc {

std::string turn_label(float deg, float slight_max)
{
    const float a = std::fabs(deg);
    const char *side = deg < 0 ? "left" : "right";
    if (a < 20) return "straight";
    if (a < slight_max) return std::string("slight_") + side;
    if (a < 120) return side;
    if (a < 165) return std::string("sharp_") + side;
    return "uturn_left";
}

namespace {

/* test/data/carplay-card-30m.pgm's street reads at 0.81 at worst; the same
 * names rendered semibold read 0.70 and up, a wrong letter (G as C) 0.62 */
const float kStreetMin = 0.7f;

/* A road shield before the street (a grey road number box, the car 2026-11-08) is
 * a run of columns mostly mid-grey; a letter's antialiased edge is a column
 * or two. A run of 20 or more starting in the first quarter is blanked to
 * the card's black up to the shield's last lit column. */
void blank_shield(Gray *g)
{
    const unsigned need = g->h / 4 + 1;
    unsigned run = 0, x = 0;
    for (; x < g->w; x++) {
        unsigned n = 0;
        for (unsigned y = 0; y < g->h; y++) {
            const uint8_t v = g->px[(size_t)y * g->w + x];
            n += v >= 60 && v <= 200;
        }
        if (n >= need) {
            run++;
        } else if (run >= 20) {
            break;
        } else {
            run = 0;
            if (x > g->w / 4) return;
        }
    }
    if (run < 20) return;
    for (;; x++) {   /* through the shield's border */
        bool lit = false;
        for (unsigned y = 0; x < g->w && y < g->h && !lit; y++)
            lit = g->px[(size_t)y * g->w + x] >= 60;
        if (!lit) break;
    }
    for (unsigned y = 0; y < g->h; y++)
        std::fill(g->px.begin() + (size_t)y * g->w, g->px.begin() + (size_t)y * g->w + std::min(x, g->w), 0);
}

/* Geodesic BFS inside the part (lab == id) from s; the farthest pixel, and
 * each pixel's parent when par is given. */
int bfs(const std::vector<int> &lab, int id, unsigned w, unsigned h, int s, std::vector<int> *dist,
        std::vector<int> *par)
{
    std::vector<int> q;
    q.push_back(s);
    (*dist)[s] = 0;
    if (par) (*par)[s] = -1;
    int far = s;
    for (size_t i = 0; i < q.size(); i++) {
        const int p = q[i], px = p % (int)w, py = p / (int)w;
        if ((*dist)[p] > (*dist)[far]) far = p;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                const int x = px + dx, y = py + dy;
                if (x < 0 || y < 0 || x >= (int)w || y >= (int)h) continue;
                const int r = y * (int)w + x;
                if (lab[r] != id || (*dist)[r] >= 0) continue;
                (*dist)[r] = (*dist)[p] + 1;
                if (par) (*par)[r] = p;
                q.push_back(r);
            }
    }
    return far;
}

}  // namespace

Reading MapArrowReader::read(const uint8_t *y, unsigned w, unsigned h)
{
    angle_ = 0;
    Reading none;
    if (w < 16 || h < 16) return none;
    const size_t n = (size_t)w * h;
    const unsigned top = h * c_.skip_top_pct / 100;
    /* buffers kept between reads: no 7 MB of allocations every sample */
    std::vector<uint8_t> &m = m_, &e = e_;
    m.assign(n, 0);
    e.assign(n, 0);
    for (unsigned r = top; r < h; r++)
        for (unsigned x = 0; x < w; x++)
            m[(size_t)r * w + x] = y[(size_t)r * w + x] >= c_.min_luma;
    /* opening 3x3: erode into e, dilate back into m */
    for (unsigned r = 1; r + 1 < h; r++)
        for (unsigned x = 1; x + 1 < w; x++) {
            bool all = true;
            for (int dy = -1; dy <= 1 && all; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (!m[(r + dy) * w + x + dx]) { all = false; break; }
            e[(size_t)r * w + x] = all;
        }
    for (size_t i = 0; i < n; i++) m[i] = 0;
    for (unsigned r = 1; r + 1 < h; r++)
        for (unsigned x = 1; x + 1 < w; x++)
            if (e[(size_t)r * w + x])
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) m[(r + dy) * w + x + dx] = 1;
    /* integral image, for the 7x7 cores */
    std::vector<uint32_t> &ii = ii_;
    ii.assign((size_t)(w + 1) * (h + 1), 0);
    for (unsigned r = 0; r < h; r++)
        for (unsigned x = 0; x < w; x++)
            ii[(r + 1) * (w + 1) + x + 1] = m[(size_t)r * w + x] + ii[r * (w + 1) + x + 1] +
                                            ii[(r + 1) * (w + 1) + x] - ii[r * (w + 1) + x];
    auto core = [&](unsigned x, unsigned r) {
        if (x < 3 || r < 3 || x + 4 > w || r + 4 > h) return false;
        const unsigned x0 = x - 3, y0 = r - 3, x1 = x + 4, y1 = r + 4;
        return ii[y1 * (w + 1) + x1] - ii[y0 * (w + 1) + x1] - ii[y1 * (w + 1) + x0] + ii[y0 * (w + 1) + x0] == 49;
    };

    /* the parts */
    std::vector<int> &lab = lab_, &q = q_;
    lab.assign(n, 0);
    int best = 0, best_core = 0, best_low = -1;
    int id = 0;
    for (size_t s = 0; s < n; s++) {
        if (!m[s] || lab[s]) continue;
        id++;
        q.clear();
        q.push_back((int)s);
        lab[s] = id;
        unsigned x0 = w, y0 = h, x1 = 0, y1 = 0, cores = 0;
        int low = (int)s;
        for (size_t i = 0; i < q.size(); i++) {
            const int p = q[i], px = p % (int)w, py = p / (int)w;
            x0 = std::min(x0, (unsigned)px); x1 = std::max(x1, (unsigned)px);
            y0 = std::min(y0, (unsigned)py); y1 = std::max(y1, (unsigned)py);
            if (py > low / (int)w) low = p;
            if (core(px, py)) cores++;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    const int x = px + dx, yy = py + dy;
                    if (x < 0 || yy < 0 || x >= (int)w || yy >= (int)h) continue;
                    const int r = yy * (int)w + x;
                    if (m[r] && !lab[r]) { lab[r] = id; q.push_back(r); }
                }
        }
        const unsigned area = q.size(), bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        if (area < 250 || area > 20000 || bw < 16 || bw > 400 || bh < 16 || bh > 300 || cores < 30) continue;
        if (area * 100 >= bw * bh * 65) continue;   /* a filled box: a road shield */
        if ((int)cores > best_core) { best = id; best_core = cores; best_low = low; }
    }
    if (!best) return none;

    /* its ends: farthest from the lowest pixel, then farthest from that */
    std::vector<int> &dist = dist_;
    dist.assign(n, -1);
    const int e1 = bfs(lab, best, w, h, best_low, &dist, nullptr);
    std::fill(dist.begin(), dist.end(), -1);
    const int e2 = bfs(lab, best, w, h, e1, &dist, nullptr);
    auto d2car = [&](int p) {
        const double dx = p % (int)w - (double)c_.car_x, dy = p / (int)w - (double)c_.car_y;
        return dx * dx + dy * dy;
    };
    /* geodesic distance from the stem's end (the end nearer the car) */
    if (d2car(e1) > d2car(e2)) {
        std::fill(dist.begin(), dist.end(), -1);
        bfs(lab, best, w, h, e2, &dist, nullptr);
    }
    int L = 0;
    for (size_t i = 0; i < n; i++)
        if (lab[i] == best) L = std::max(L, dist[i]);
    if (L < 25) return none;
    /* centroids of bands of that distance: the head's corners do not move them */
    double cx[4] = {0}, cy[4] = {0}, cn[4] = {0};
    const float band[4][2] = {{0, 0.15f}, {0.25f, 0.4f}, {0.55f, 0.7f}, {0.85f, 1.01f}};
    for (size_t i = 0; i < n; i++) {
        if (lab[i] != best) continue;
        const float f = (float)dist[i] / L;
        for (int k = 0; k < 4; k++)
            if (f >= band[k][0] && f < band[k][1]) cx[k] += i % w, cy[k] += i / w, cn[k]++;
    }
    for (int k = 0; k < 4; k++) {
        if (!cn[k]) return none;
        cx[k] /= cn[k], cy[k] /= cn[k];
    }
    const double ax = cx[1] - cx[0], ay = cy[1] - cy[0], bx = cx[3] - cx[2], by = cy[3] - cy[2];
    angle_ = (float)(std::atan2(ax * by - ay * bx, ax * bx + ay * by) * 180 / M_PI);
    Reading r;
    r.maneuver = turn_label(angle_);
    r.score = 1;
    return r;
}

Reading CardReader::read(const uint8_t *y, unsigned w, unsigned h)
{
    angle_ = 0;
    text_.clear();
    Reading none;
    Gray g;
    if (!crop(y, w, h, c_.plain, &g)) return none;
    const auto mm = std::minmax_element(g.px.begin(), g.px.end());
    if (*mm.second - *mm.first > 24) return none;   /* not the card's plain background */
    if (*mm.first > 20 && *mm.second < 235) return none;   /* the card is black (night) or white (day), the map neither */
    std::vector<uint8_t> fg;
    if (!crop(y, w, h, c_.icon, &g) || !binarise_fg(g, &fg)) return none;
    for (size_t i = 0; i < fg.size(); i++) g.px[i] = fg[i] ? 255 : 0;
    MapArrowConfig mc;
    mc.car_x = g.w / 2;
    mc.car_y = g.h - 1;
    mc.skip_top_pct = 0;
    MapArrowReader arrow(mc);
    Reading r = arrow.read(g.px.data(), g.w, g.h);
    angle_ = arrow.angle();
    if (r.maneuver.empty()) return none;
    /* the card's fork (keep, exit) reads 50 degrees, its turn 83 (the car,
     * 2026-11-04); the map's perspective flattens turns, so only here */
    r.maneuver = turn_label(angle_, 65);
    if (crop(y, w, h, c_.dist, &g)) {
        text_ = rec_.read_text(g);
        r.distance = parse_distance(text_);
    }
    if (has_street_ && crop(y, w, h, c_.street, &g)) {
        blank_shield(&g);
        r.street = street_.read_words(g, kStreetMin);
    }
    return r;
}

bool parse_route_uv(const std::string &s, RouteBounds *b)
{
    int v[4];
    char tail;
    if (std::sscanf(s.c_str(), "%d,%d,%d,%d%c", &v[0], &v[1], &v[2], &v[3], &tail) != 4)
        return false;
    for (int x : v)
        if (x < 0 || x > 255)
            return false;
    if (v[0] > v[1] || v[2] > v[3])
        return false;
    b->umin = v[0], b->umax = v[1], b->vmin = v[2], b->vmax = v[3];
    return true;
}

unsigned route_pixels(const uint8_t *atlas, unsigned w, unsigned h, const RouteBounds &b)
{
    if (!b.set())
        return 0;
    const uint8_t *uv = atlas + (size_t)w * h;   /* row r: U in columns 0..w/2-1, V after */
    unsigned n = 0;
    for (unsigned cy = 0; cy < h / 2; cy += 4)
        for (unsigned cx = 0; cx < w / 2; cx += 4) {
            const int u = uv[(size_t)cy * w + cx], v = uv[(size_t)cy * w + w / 2 + cx];
            if (u >= b.umin && u <= b.umax && v >= b.vmin && v <= b.vmax && atlas[(size_t)2 * cy * w + 2 * cx] >= b.ymin)
                n++;
        }
    return n;
}

int route_state(const uint8_t *atlas, unsigned w, unsigned h, const RouteBounds &b)
{
    return b.set() ? route_pixels(atlas, w, h, b) >= b.min_px : -1;
}

bool write_map_dump(const std::string &base, const uint8_t *atlas, unsigned w, unsigned h, bool ppm)
{
    const uint8_t *uv = atlas + (size_t)w * h;
    FILE *f = std::fopen((base + ".yuv").c_str(), "wb");
    bool ok = f && std::fwrite(atlas, 1, (size_t)w * h, f) == (size_t)w * h;
    for (unsigned plane = 0; ok && plane < 2; plane++)
        for (unsigned r = 0; ok && r < h / 2; r++)
            ok = std::fwrite(uv + (size_t)r * w + plane * (w / 2), 1, w / 2, f) == w / 2;
    if (f) ok = std::fclose(f) == 0 && ok;
    if (!ppm) return ok;
    f = ok ? std::fopen((base + ".ppm").c_str(), "wb") : nullptr;
    ok = f && std::fprintf(f, "P6\n%u %u\n255\n", w, h) > 0;
    std::vector<uint8_t> row((size_t)w * 3);
    for (unsigned y = 0; ok && y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            const size_t c = (size_t)(y / 2) * w + x / 2;
            const double Y = atlas[(size_t)y * w + x], U = uv[c] - 128.0, V = uv[c + w / 2] - 128.0;
            const double rgb[3] = {Y + 1.402 * V, Y - 0.344136 * U - 0.714136 * V, Y + 1.772 * U};
            for (int k = 0; k < 3; k++)
                row[(size_t)x * 3 + k] = (uint8_t)std::lround(std::min(255.0, std::max(0.0, rgb[k])));
        }
        ok = std::fwrite(row.data(), 1, row.size(), f) == row.size();
    }
    if (f) ok = std::fclose(f) == 0 && ok;
    return ok;
}

}  // namespace woc
