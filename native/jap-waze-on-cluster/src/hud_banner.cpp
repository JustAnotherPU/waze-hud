// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
#include "hud_banner.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace woc {

namespace {

const unsigned kIconN = 24, kGlyphN = 16;
const float kGlyphMin = 0.35f;   /* below: an unreadable glyph */

struct Comp {
    int x0, y0, x1, y1;   /* inclusive */
    int area;
};

/* Otsu's threshold, the foreground the class the border mostly is not;
 * false when the two classes are within 40 levels (a flat rect). */
bool binarise(const Gray &g, std::vector<uint8_t> *mask)
{
    unsigned hist[256] = {0};
    for (uint8_t v : g.px)
        hist[v]++;
    const double total = (double)g.px.size();
    double sum = 0;
    for (int i = 0; i < 256; i++)
        sum += (double)i * hist[i];
    double sum_b = 0, w_b = 0, best = -1;
    int thr = 0;
    for (int t = 0; t < 256; t++) {
        w_b += hist[t];
        if (w_b == 0 || w_b == total)
            continue;
        sum_b += (double)t * hist[t];
        const double m_b = sum_b / w_b, m_f = (sum - sum_b) / (total - w_b);
        const double between = w_b * (total - w_b) * (m_b - m_f) * (m_b - m_f);
        if (between > best) {
            best = between;
            thr = t;
        }
    }
    double lo = 0, hi = 0, nlo = 0, nhi = 0;
    for (int i = 0; i < 256; i++)
        (i <= thr ? lo : hi) += (double)i * hist[i], (i <= thr ? nlo : nhi) += hist[i];
    if (!nlo || !nhi || hi / nhi - lo / nlo < 40)
        return false;
    unsigned border_hi = 0, border = 0;
    for (unsigned y = 0; y < g.h; y++)
        for (unsigned x = 0; x < g.w; x++)
            if (x == 0 || y == 0 || x + 1 == g.w || y + 1 == g.h) {
                border++;
                border_hi += g.px[(size_t)y * g.w + x] > thr;
            }
    const bool fg_hi = border_hi * 2 < border;
    mask->resize(g.px.size());
    for (size_t i = 0; i < g.px.size(); i++)
        (*mask)[i] = (g.px[i] > thr) == fg_hi;
    return true;
}

std::vector<Comp> components(const std::vector<uint8_t> &mask, unsigned w, unsigned h)
{
    std::vector<int> label(mask.size(), 0);
    std::vector<Comp> out;
    std::vector<size_t> stack;
    for (size_t start = 0; start < mask.size(); start++) {
        if (!mask[start] || label[start])
            continue;
        Comp c{(int)(start % w), (int)(start / w), (int)(start % w), (int)(start / w), 0};
        label[start] = 1;
        stack.assign(1, start);
        while (!stack.empty()) {
            const size_t i = stack.back();
            stack.pop_back();
            const int x = (int)(i % w), y = (int)(i / w);
            c.area++;
            c.x0 = std::min(c.x0, x), c.x1 = std::max(c.x1, x), c.y0 = std::min(c.y0, y), c.y1 = std::max(c.y1, y);
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= (int)w || ny >= (int)h)
                        continue;
                    const size_t j = (size_t)ny * w + nx;
                    if (mask[j] && !label[j]) {
                        label[j] = 1;
                        stack.push_back(j);
                    }
                }
        }
        out.push_back(c);
    }
    return out;
}

/* The box of the mask fitted into n x n, its aspect kept and centred: each
 * cell the share of 4x4 samples that hit the foreground. */
std::vector<float> fit(const std::vector<uint8_t> &mask, unsigned w, const Comp &b, unsigned n)
{
    const float bw = (float)(b.x1 - b.x0 + 1), bh = (float)(b.y1 - b.y0 + 1);
    const float s = std::max(bw, bh) / n;
    const float ox = b.x0 - (std::max(bw, bh) - bw) / 2, oy = b.y0 - (std::max(bw, bh) - bh) / 2;
    std::vector<float> v((size_t)n * n, 0);
    for (unsigned cy = 0; cy < n; cy++)
        for (unsigned cx = 0; cx < n; cx++) {
            int hit = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    const int x = (int)std::floor(ox + (cx + (sx + 0.5f) / 4) * s);
                    const int y = (int)std::floor(oy + (cy + (sy + 0.5f) / 4) * s);
                    if (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1)
                        hit += mask[(size_t)y * w + x];
                }
            v[(size_t)cy * n + cx] = hit / 16.0f;
        }
    return v;
}

float similarity(const std::vector<float> &a, const std::vector<float> &b)
{
    float lo = 0, hi = 0;
    for (size_t i = 0; i < a.size(); i++) {
        lo += std::min(a[i], b[i]);
        hi += std::max(a[i], b[i]);
    }
    return hi > 0 ? lo / hi : 0;
}

/* The icon's feature: the box of its parts of at least 5% of the largest. */
bool icon_feature(const Gray &g, std::vector<float> *v)
{
    std::vector<uint8_t> mask;
    if (!binarise(g, &mask))
        return false;
    const std::vector<Comp> cs = components(mask, g.w, g.h);
    int big = 0;
    for (const Comp &c : cs)
        big = std::max(big, c.area);
    if (big < 16 || big * 100 < (int)(g.w * g.h))
        return false;
    Comp box{(int)g.w, (int)g.h, -1, -1, 0};
    for (const Comp &c : cs)
        if (c.area * 20 >= big) {
            box.x0 = std::min(box.x0, c.x0), box.y0 = std::min(box.y0, c.y0);
            box.x1 = std::max(box.x1, c.x1), box.y1 = std::max(box.y1, c.y1);
        }
    *v = fit(mask, g.w, box, kIconN);
    return true;
}

struct Line {
    std::vector<std::vector<float>> v;
    std::vector<float> rel_h;
    std::vector<int> h;        /* in pixels */
    std::vector<bool> point;   /* a decimal point or comma */
    std::vector<int> gap;      /* pixels from the glyph before */
};

/* The crop's glyphs, left to right. Parts overlapping in x are one glyph
 * when one is small beside the other (an i and its dot, an accent) or they
 * overlap by half the narrower: a kerned pair (Te, Vo) stays two. */
bool split_line(const Gray &g, Line *line)
{
    std::vector<uint8_t> mask;
    if (!binarise(g, &mask))
        return false;
    std::vector<Comp> cs;
    for (const Comp &c : components(mask, g.w, g.h))
        if (c.area >= 3)
            cs.push_back(c);
    std::sort(cs.begin(), cs.end(), [](const Comp &a, const Comp &b) { return a.x0 < b.x0; });
    std::vector<Comp> gl;
    for (const Comp &c : cs) {
        const int ov = gl.empty() ? 0 : std::min(gl.back().x1, c.x1) - c.x0 + 1;
        if (ov > 0 && (std::min(c.area, gl.back().area) * 4 < std::max(c.area, gl.back().area) ||
                       ov * 2 >= std::min(c.x1 - c.x0, gl.back().x1 - gl.back().x0) + 1)) {
            Comp &m = gl.back();
            m.x1 = std::max(m.x1, c.x1), m.y0 = std::min(m.y0, c.y0), m.y1 = std::max(m.y1, c.y1);
            m.area += c.area;
        } else {
            gl.push_back(c);
        }
    }
    int hmax = 0;
    for (const Comp &c : gl)
        hmax = std::max(hmax, c.y1 - c.y0 + 1);
    if (hmax < 6)
        return false;
    int top = (int)g.h, bottom = 0;
    std::vector<int> foot(g.h, 0);
    for (const Comp &c : gl)
        if ((c.y1 - c.y0 + 1) * 2 > hmax)
            top = std::min(top, c.y0), bottom = std::max(bottom, c.y1), foot[c.y1]++;
    /* the baseline: where most glyphs end (a g or a p goes below it) */
    const int base = (int)(std::max_element(foot.begin(), foot.end()) - foot.begin());
    for (size_t i = 0; i < gl.size(); i++) {
        const Comp &c = gl[i];
        const int ch = c.y1 - c.y0 + 1;
        /* on the baseline: a hyphen is as small but higher */
        line->point.push_back(ch * 100 < hmax * 45 && c.y0 > top + (bottom - top) * 55 / 100 &&
                              c.y1 >= base - std::max(2, hmax / 10));
        line->gap.push_back(i > 0 ? c.x0 - gl[i - 1].x1 - 1 : 0);
        line->h.push_back(ch);
        line->rel_h.push_back((float)ch / hmax);
        line->v.push_back(fit(mask, g.w, c, kGlyphN));
    }
    return true;
}

/* UTF-8 text split into its characters, each a string. */
std::vector<std::string> utf8_chars(const std::string &s)
{
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        size_t n = 1;
        while (i + n < s.size() && ((unsigned char)s[i + n] & 0xc0) == 0x80)
            n++;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

/* "u00e9u0048" as UTF-8 ("éH"); empty when it is not that. */
std::string hex_label(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] != 'u' || i + 5 > s.size())
            return "";
        char *end;
        const unsigned long c = std::strtoul(s.substr(i + 1, 4).c_str(), &end, 16);
        if (*end || !c)
            return "";
        if (c < 0x80) {
            out.push_back((char)c);
        } else if (c < 0x800) {
            out.push_back((char)(0xc0 | c >> 6));
            out.push_back((char)(0x80 | (c & 0x3f)));
        } else {
            out.push_back((char)(0xe0 | c >> 12));
            out.push_back((char)(0x80 | ((c >> 6) & 0x3f)));
            out.push_back((char)(0x80 | (c & 0x3f)));
        }
        i += 5;
    }
    return out;
}

uint32_t crc32(const uint8_t *p, size_t n, uint32_t c = 0)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++)
                v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
    c = ~c;
    for (size_t i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return ~c;
}

void put32(std::string *s, uint32_t v)
{
    for (int i = 3; i >= 0; i--)
        s->push_back((char)(v >> (8 * i)));
}

void chunk(std::string *png, const char *type, const std::string &data)
{
    put32(png, (uint32_t)data.size());
    std::string body = std::string(type, 4) + data;
    png->append(body);
    put32(png, crc32(reinterpret_cast<const uint8_t *>(body.data()), body.size()));
}

}  // namespace

bool binarise_fg(const Gray &g, std::vector<uint8_t> *mask)
{
    return binarise(g, mask);
}

bool parse_rect(const std::string &s, Rect *r)
{
    unsigned w, h, x, y;
    char tail;
    if (std::sscanf(s.c_str(), "%ux%u+%u+%u%c", &w, &h, &x, &y, &tail) != 4 || !w || !h)
        return false;
    *r = Rect{x, y, w, h};
    return true;
}

bool crop(const uint8_t *y, unsigned w, unsigned h, const Rect &r, Gray *out)
{
    if (r.x + r.w > w || r.y + r.h > h || !r.w || !r.h)
        return false;
    out->w = r.w;
    out->h = r.h;
    out->px.resize((size_t)r.w * r.h);
    for (unsigned row = 0; row < r.h; row++)
        std::memcpy(&out->px[(size_t)row * r.w], y + (size_t)(r.y + row) * w + r.x, r.w);
    return true;
}

void Templates::add_builtin(bool icons)
{
    if (icons)
        for (const std::string &l : maneuver_labels()) {
            Gray g(64, 64);
            draw_maneuver(&g, l, 4, 4, 56);
            if (add_icon(l, g))
                icons_.back().builtin = true;
        }
    const std::string text = "0123456789.mkfti";
    Gray g((unsigned)text_width(text, 32) + 8, 48);
    draw_text(&g, text, 4, 6, 32);
    const size_t before = glyphs_.size();
    add_line(text, g);
    for (size_t i = before; i < glyphs_.size(); i++)
        glyphs_[i].builtin = true;
}

bool Templates::add_icon(const std::string &label, const Gray &g)
{
    T t;
    if (!maneuver_known(label) || !icon_feature(g, &t.v))
        return false;
    t.label = label;
    icons_.push_back(t);
    return true;
}

bool Templates::add_line(const std::string &text, const Gray &g, bool street)
{
    std::vector<std::string> chars;
    for (const std::string &c : utf8_chars(street ? "H" + text : text))
        if (c != " ")
            chars.push_back(c);
    Line line;
    if (!split_line(g, &line) || line.v.size() != chars.size())
        return false;
    if (street) {
        ref_sum_ += (float)line.h[0];
        ref_n_++;
    }
    for (size_t i = street ? 1 : 0; i < chars.size(); i++) {
        if (chars[i] == "." || chars[i] == ",")
            continue;   /* told by its size and place */
        T t;
        t.label = chars[i];
        t.v = line.v[i];
        t.rel_h = street ? (float)line.h[i] / line.h[0] : line.rel_h[i];
        glyphs_.push_back(t);
    }
    return true;
}

int Templates::load_dir(const std::string &dir, std::string *log, const std::string &glyph_kind)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return 0;
    std::vector<std::string> names;
    while (struct dirent *e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".pgm") == 0)
            names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    std::vector<T> icons, glyphs;
    std::swap(icons, icons_);
    std::swap(glyphs, glyphs_);
    const float ref_sum = ref_sum_;
    const int ref_n = ref_n_;
    ref_sum_ = 0, ref_n_ = 0;
    int added = 0;
    for (const std::string &n : names) {
        const std::string stem = n.substr(0, n.size() - 4);
        const size_t a = stem.find('-'), b = stem.rfind('-');
        const std::string kind = stem.substr(0, a);
        const std::string label = a == std::string::npos ? "" : stem.substr(a + 1, (b > a ? b : stem.size()) - a - 1);
        const bool street = glyph_kind == "str";
        if (kind == (street ? "dist" : "str") || (street && kind == "icon"))
            continue;   /* the other set's */
        Gray g;
        bool used = false;
        if (!read_pgm(dir + "/" + n, &g))
            *log += n + ": not a binary 8-bit PGM\n";
        else if (kind == "icon")
            used = add_icon(label, g);
        else if (kind == "dist")
            used = add_line(label, g);
        else if (kind == "str")
            used = !hex_label(label).empty() && add_line(hex_label(label), g, true);
        if (used)
            added++;
        else if (!g.px.empty())
            *log += n + (kind == "dist" || kind == "str" ? ": its glyphs do not match its text\n"
                                        : ": not an icon-<manoeuvre>-<n>, dist-<text>-<n> or str-<hex>-<n> template\n");
    }
    /* What the folder has replaces our drawings of the same kind. */
    if (icons_.empty())
        icons_ = icons;
    if (glyphs_.empty()) {
        glyphs_ = glyphs;
        ref_sum_ = ref_sum, ref_n_ = ref_n;
    }
    return added;
}

Recogniser::Recogniser(const Templates *t, const Rect &icon, const Rect &dist, float min_score)
    : t_(t), icon_(icon), dist_(dist), min_score_(min_score)
{
}

std::string Recogniser::read_text(const Gray &g) const
{
    Line line;
    if (!split_line(g, &line))
        return "";
    std::string out;
    for (size_t i = 0; i < line.v.size(); i++) {
        if (line.point[i]) {
            out.push_back('.');
            continue;
        }
        float best = kGlyphMin;
        std::string c = "?";
        for (const Templates::T &t : t_->glyph_set()) {
            const float s = similarity(line.v[i], t.v) - 0.6f * std::fabs(line.rel_h[i] - t.rel_h);
            if (s > best) {
                best = s;
                c = t.label;
            }
        }
        out += c;
    }
    return out;
}

std::string Recogniser::read_words(const Gray &g, float min_score) const
{
    Line line;
    const float ref = t_ ? t_->ref_h() : 0;
    if (!ref || !split_line(g, &line))
        return "";
    size_t n = line.v.size();
    if (n >= 3 && line.point[n - 1] && line.point[n - 2] && line.point[n - 3])
        n -= 3;   /* iOS cut the name: what is left is still its start */
    else if (n >= 2 && line.point[n - 1] && line.point[n - 2])
        return "";   /* an ellipsis with a dot lost in a letter: no name ends ".." */
    std::string out;
    for (size_t i = 0; i < n; i++) {
        /* SF Pro on the card: letters 6 px apart at most, words 9 or more
         * (an H 24 px tall) */
        if (line.gap[i] * 10 > ref * 3 && !out.empty())
            out.push_back(' ');
        if (line.point[i]) {
            out.push_back('.');
            continue;
        }
        float best = -1;
        const std::string *c = nullptr;
        for (const Templates::T &t : t_->glyph_set()) {
            const float s = similarity(line.v[i], t.v) - 0.6f * std::fabs(line.h[i] / ref - t.rel_h);
            if (s > best) {
                best = s;
                c = &t.label;
            }
        }
        if (!c || best < min_score)
            return "";
        out += *c;
    }
    return out;
}

Reading Recogniser::read(const uint8_t *y, unsigned w, unsigned h) const
{
    Reading r;
    Gray g;
    std::vector<float> v;
    if (!crop(y, w, h, icon_, &g) || !icon_feature(g, &v))
        return r;
    for (const Templates::T &t : t_->icon_set()) {
        const float s = similarity(v, t.v);
        if (s > r.score) {
            r.score = s;
            r.maneuver = t.label;
        }
    }
    if (r.score < min_score_) {
        r.maneuver.clear();
        return r;
    }
    if (crop(y, w, h, dist_, &g))
        r.distance = parse_distance(read_text(g));
    return r;
}

bool Debounce::push(const Reading &r, Reading *out)
{
    if (r == cand_) {
        if (seen_ < stable_)
            seen_++;
    } else {
        cand_ = r;
        seen_ = 1;
    }
    if (seen_ < stable_ || cand_ == shown_)
        return false;
    shown_ = cand_;
    *out = shown_;
    return true;
}

Reading CardHold::pick(const Reading &r, bool from_card, int64_t now_ms)
{
    const bool fresh = now_ms - card_ms_ <= hold_ms_;
    if (!from_card)
        return fresh ? card_ : r;
    Reading out = r;
    if (out.distance.empty() && fresh && card_.maneuver == out.maneuver && !card_.distance.empty())
        return card_;   /* not a new reading: the 3 s still run from the distance's */
    card_ = out;
    card_ms_ = now_ms;
    return out;
}

double distance_metres(const std::string &d)
{
    double v;
    char unit[4];
    if (std::sscanf(d.c_str(), "%lf %3s", &v, unit) != 2)
        return -1;
    const std::string u = unit;
    return u == "m" ? v : u == "km" ? v * 1000 : u == "ft" ? v * 0.3048 : u == "mi" ? v * 1609.344 : -1;
}

void StreetHold::push(const Reading &card)
{
    const double m = distance_metres(card.distance);
    if (card.maneuver != maneuver_ || (m >= 0 && metres_ >= 0 && m > metres_ + 30)) {
        maneuver_ = card.maneuver;
        cand_.clear();
        shown_.clear();
        seen_ = 0;
        metres_ = -1;
    }
    if (m >= 0)
        metres_ = m;
    if (card.street.empty())
        return;
    seen_ = card.street == cand_ ? seen_ + 1 : 1;
    cand_ = card.street;
    if (seen_ >= 2)
        shown_ = cand_;
}

std::string parse_distance(const std::string &text)
{
    size_t i = 0;
    std::string num;
    while (i < text.size() && (std::isdigit((unsigned char)text[i]) || text[i] == '.' || text[i] == ','))
        num.push_back(text[i] == ',' ? '.' : text[i]), i++;
    const std::string unit = text.substr(i);
    if (num.empty() || !std::isdigit((unsigned char)num[0]) || !std::isdigit((unsigned char)num.back()) ||
        std::count(num.begin(), num.end(), '.') > 1 || (num[0] == '0' && num.size() > 1 && num[1] != '.'))
        return "";
    if (unit != "m" && unit != "km" && unit != "ft" && unit != "mi")
        return "";
    return num + " " + unit;
}

bool write_png(const std::string &path, const Gray &g)
{
    std::string png("\x89PNG\r\n\x1a\n", 8), ihdr;
    put32(&ihdr, g.w);
    put32(&ihdr, g.h);
    ihdr += std::string("\x08\x00\x00\x00\x00", 5);
    chunk(&png, "IHDR", ihdr);
    std::string raw;
    for (unsigned y = 0; y < g.h; y++) {
        raw.push_back(0);
        raw.append(reinterpret_cast<const char *>(&g.px[(size_t)y * g.w]), g.w);
    }
    std::string z("\x78\x01", 2);
    for (size_t off = 0; off < raw.size() || off == 0; off += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n >= raw.size() ? 1 : 0);
        z.push_back((char)(n & 0xff));
        z.push_back((char)(n >> 8));
        z.push_back((char)(~n & 0xff));
        z.push_back((char)((~n >> 8) & 0xff));
        z.append(raw, off, n);
        if (raw.empty())
            break;
    }
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw)
        a = (a + c) % 65521, b = (b + a) % 65521;
    put32(&z, (b << 16) | a);
    chunk(&png, "IDAT", z);
    chunk(&png, "IEND", "");
    FILE *f = open_dump(path, false);
    if (!f)
        return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}

bool dump_dir_ok(const std::string &dir)
{
    mkdir(dir.c_str(), 0755);
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) {
        errno = EPERM;
        return false;
    }
    if (st.st_mode & 022)   /* 0.7.84 made it 1777; /tmp is sticky, so it is still ours */
        chmod(dir.c_str(), 0755);
    return true;
}

FILE *open_dump(const std::string &path, bool excl)
{
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | (excl ? O_EXCL : O_TRUNC), 0644);
    if (fd < 0)
        return nullptr;
    FILE *f = fdopen(fd, "wb");
    if (!f)
        close(fd);
    return f;
}

bool read_pgm(const std::string &path, Gray *g)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && s.size() < (4u << 20))
        s.append(buf, n);
    std::fclose(f);
    size_t i = 2;
    long v[3];
    if (s.compare(0, 2, "P5") != 0)
        return false;
    for (int k = 0; k < 3; k++) {
        while (i < s.size() && (std::isspace((unsigned char)s[i]) || s[i] == '#')) {
            if (s[i] == '#')
                while (i < s.size() && s[i] != '\n')
                    i++;
            else
                i++;
        }
        char *end;
        v[k] = std::strtol(s.c_str() + i, &end, 10);
        i = (size_t)(end - s.c_str());
    }
    i++;   /* the one whitespace before the pixels */
    if (v[0] <= 0 || v[1] <= 0 || v[0] > 2048 || v[1] > 2048 || v[2] != 255 || s.size() < i + (size_t)(v[0] * v[1]))
        return false;
    g->w = (unsigned)v[0];
    g->h = (unsigned)v[1];
    g->px.assign(s.begin() + i, s.begin() + i + (size_t)(v[0] * v[1]));
    return true;
}

}  // namespace woc
