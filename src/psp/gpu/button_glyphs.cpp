#include "psp/gpu/button_glyphs.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <mutex>

namespace psp::gpu {

namespace {

std::mutex g_mutex;
ButtonLabels g_labels;

// 3x5 pixel font (rows top to bottom, 3 bits each, MSB = left); the Enter glyph is 5 wide.
struct Glyph { char c; int w; uint8_t rows[5]; };
const Glyph kFont[] = {
    {'A', 3, {2, 5, 7, 5, 5}}, {'B', 3, {6, 5, 6, 5, 6}}, {'C', 3, {3, 4, 4, 4, 3}}, {'D', 3, {6, 5, 5, 5, 6}},
    {'E', 3, {7, 4, 6, 4, 7}}, {'F', 3, {7, 4, 6, 4, 4}}, {'G', 3, {3, 4, 5, 5, 3}}, {'H', 3, {5, 5, 7, 5, 5}},
    {'I', 3, {7, 2, 2, 2, 7}}, {'J', 3, {1, 1, 1, 5, 2}}, {'K', 3, {5, 5, 6, 5, 5}}, {'L', 3, {4, 4, 4, 4, 7}},
    {'M', 3, {5, 7, 7, 5, 5}}, {'N', 3, {6, 5, 5, 5, 5}}, {'O', 3, {2, 5, 5, 5, 2}}, {'P', 3, {6, 5, 6, 4, 4}},
    {'Q', 3, {2, 5, 5, 6, 3}}, {'R', 3, {6, 5, 6, 5, 5}}, {'S', 3, {3, 4, 2, 1, 6}}, {'T', 3, {7, 2, 2, 2, 2}},
    {'U', 3, {5, 5, 5, 5, 7}}, {'V', 3, {5, 5, 5, 5, 2}}, {'W', 3, {5, 5, 7, 7, 5}}, {'X', 3, {5, 5, 2, 5, 5}},
    {'Y', 3, {5, 5, 2, 2, 2}}, {'Z', 3, {7, 1, 2, 4, 7}}, {'0', 3, {7, 5, 5, 5, 7}}, {'1', 3, {2, 6, 2, 2, 7}},
    {'2', 3, {6, 1, 2, 4, 7}}, {'3', 3, {6, 1, 2, 1, 6}}, {'4', 3, {5, 5, 7, 1, 1}}, {'5', 3, {7, 4, 6, 1, 6}},
    {'6', 3, {3, 4, 7, 5, 7}}, {'7', 3, {7, 1, 2, 2, 2}}, {'8', 3, {7, 5, 7, 5, 7}}, {'9', 3, {7, 5, 7, 1, 6}},
    {'+', 3, {0, 2, 7, 2, 0}}, {'-', 3, {0, 0, 7, 0, 0}}, {'.', 3, {0, 0, 0, 0, 2}}, {'/', 3, {1, 1, 2, 4, 4}},
    {kGlyphUp, 3, {2, 7, 2, 2, 2}}, {kGlyphDown, 3, {2, 2, 2, 7, 2}}, {kGlyphLeft, 3, {0, 2, 7, 2, 0}},
    {kGlyphRight, 3, {0, 2, 7, 2, 0}}, {kGlyphEnter, 5, {1, 1, 9, 31, 8}},
};

const Glyph* glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (const Glyph& g : kFont)
        if (g.c == c) return &g;
    return nullptr;
}

// The arrows share a shaft; their heads are drawn here.
bool arrowHead(char c, int x, int y) {
    if (c == kGlyphLeft) return (x == 0 && y == 2) || (x == 1 && (y == 1 || y == 3));
    if (c == kGlyphRight) return (x == 2 && y == 2) || (x == 1 && (y == 1 || y == 3));
    return false;
}

struct Image {
    uint32_t* px;
    int w, h;
    uint32_t& at(int x, int y) { return px[(size_t)y * w + x]; }
};

int textWidth(const std::string& s) {
    int w = 0;
    for (char c : s)
        if (const Glyph* g = glyph(c)) w += g->w + 1;
    return w ? w - 1 : 0;
}

// Draws `s` centred in the rectangle, at 2x when it fits.
void drawText(Image& img, int rx, int ry, int rw, int rh, const std::string& s, uint32_t color) {
    const int tw = textWidth(s);
    const int scale = (tw * 2 <= rw && 10 <= rh) ? 2 : 1;
    int x = rx + (rw - tw * scale) / 2, y = ry + (rh - 5 * scale) / 2;
    for (char c : s) {
        const Glyph* g = glyph(c);
        if (!g) continue;
        for (int gy = 0; gy < 5; ++gy)
            for (int gx = 0; gx < g->w; ++gx) {
                const bool on = ((g->rows[gy] >> (g->w - 1 - gx)) & 1) || arrowHead(c, gx, gy);
                if (!on) continue;
                for (int sy = 0; sy < scale; ++sy)
                    for (int sx = 0; sx < scale; ++sx) {
                        const int px = x + gx * scale + sx, py = y + gy * scale + sy;
                        if (px >= 0 && py >= 0 && px < img.w && py < img.h) img.at(px, py) = color;
                    }
            }
        x += (g->w + 1) * scale;
    }
}

void fill(Image& img, int x0, int y0, int w, int h, uint32_t color) {
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x) img.at(x, y) = color;
}

int luma(uint32_t c) { return (int)((c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF)) / 3; }
bool opaque(uint32_t c) { return (c >> 24) >= 128; }

// Recognises an atlas by the shape of its glyphs: per pixel 0 = transparent, 1 = dark,
// 2 = bright, hashed with FNV-1a. Independent of where the game loaded it and of its palette
// brightness.
uint64_t shapeHash(Image& img, int x0, int y0, int w, int h) {
    uint64_t hsh = 1469598103934665603ull;
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x) {
            const uint32_t c = img.at(x, y);
            const uint8_t k = !opaque(c) ? 0 : (luma(c) >= 128 ? 2 : 1);
            hsh = (hsh ^ k) * 1099511628211ull;
        }
    return hsh;
}

// Atlas 1 (256x256): the round symbols in a row at y = 128 (circle, square, cross, triangle,
// each 14x14) and the shoulder buttons at y = 114 (20x11 light boxes with a dark letter).
constexpr uint64_t kAtlasIconsHash = 0x3259e54c97272426ull;
// Atlas 2 (512x512): the PSP drawing of the control type screen.
constexpr uint64_t kAtlasPspHash = 0x5a52fd23dd0dd659ull;

void keycap(Image& img, int x0, int y0, const std::string& label) {
    // Colors of the original symbol: its light outline and dark disc.
    uint32_t fg = 0, bg = 0;
    for (int y = y0; y < y0 + 14; ++y)
        for (int x = x0; x < x0 + 14; ++x) {
            const uint32_t c = img.at(x, y);
            if (!opaque(c)) continue;
            if (luma(c) >= 128) fg = c;
            else bg = c;
        }
    fill(img, x0, y0, 14, 14, 0);
    // Rounded square: outline in the symbol color, filled with the disc color.
    fill(img, x0 + 1, y0 + 1, 12, 12, fg);
    fill(img, x0 + 2, y0 + 2, 10, 10, bg);
    for (int k : {0, 11}) {
        img.at(x0 + 1, y0 + 1 + k) = 0;
        img.at(x0 + 12, y0 + 1 + k) = 0;
    }
    drawText(img, x0 + 2, y0 + 2, 10, 10, label, fg);
}

void shoulderBox(Image& img, int x0, int y0, const std::string& label) {
    const uint32_t light = img.at(x0 + 1, y0 + 1);
    uint32_t dark = light;
    for (int y = y0; y < y0 + 11; ++y)
        for (int x = x0; x < x0 + 20; ++x)
            if (opaque(img.at(x, y)) && luma(img.at(x, y)) < 128) dark = img.at(x, y);
    fill(img, x0 + 1, y0 + 1, 18, 9, light);
    drawText(img, x0 + 1, y0 + 1, 18, 9, label, dark);
}

// A symbol drawn in outline color on a transparent background (the PSP drawing).
void lineLabel(Image& img, int cx, int cy, int hw, int hh, const std::string& label, uint32_t color) {
    fill(img, cx - hw, cy - hh, 2 * hw + 1, 2 * hh + 1, 0);
    drawText(img, cx - hw, cy - hh, 2 * hw + 1, 2 * hh + 1, label, color);
}

} // namespace

void setButtonLabels(const ButtonLabels& labels) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_labels = labels;
}

bool patchButtonGlyphs(uint32_t* rgba, int w, int h) {
    if (!((w == 256 && h == 256) || (w == 512 && h == 512))) return false;
    ButtonLabels lb;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        lb = g_labels;
    }
    Image img{rgba, w, h};
    static const bool trace = std::getenv("CA_GLYPH_TRACE") != nullptr;
    if (w == 256) {
        const uint64_t hsh = shapeHash(img, 0, 114, 64, 28);
        if (trace) std::fprintf(stderr, "[GLYPH] 256 atlas shape %016llx\n", (unsigned long long)hsh);
        if (hsh != kAtlasIconsHash) return false;
        const std::string* faces[4] = {&lb.circle, &lb.square, &lb.cross, &lb.triangle};
        for (int i = 0; i < 4; ++i)
            if (!faces[i]->empty()) keycap(img, 16 * i, 128, *faces[i]);
        if (!lb.l.empty()) shoulderBox(img, 1, 114, lb.l);
        if (!lb.r.empty()) shoulderBox(img, 33, 114, lb.r);
        return true;
    }
    const uint64_t hsh = shapeHash(img, 170, 196, 82, 80);
    if (trace) std::fprintf(stderr, "[GLYPH] 512 atlas shape %016llx\n", (unsigned long long)hsh);
    if (hsh != kAtlasPspHash) return false;
    uint32_t line = 0; // the drawing's line color: its brightest pixel
    for (int y = 196; y < 276; ++y)
        for (int x = 170; x < 252; ++x)
            if (opaque(img.at(x, y)) && (!line || luma(img.at(x, y)) > luma(line))) line = img.at(x, y);
    // Face buttons: the symbol inside each circle.
    if (!lb.triangle.empty()) lineLabel(img, 211, 209, 7, 6, lb.triangle, line);
    if (!lb.square.empty()) lineLabel(img, 184, 236, 7, 6, lb.square, line);
    if (!lb.circle.empty()) lineLabel(img, 238, 236, 7, 6, lb.circle, line);
    if (!lb.cross.empty()) lineLabel(img, 211, 262, 7, 6, lb.cross, line);
    // Shoulder letters and the SELECT/START captions.
    if (!lb.l.empty()) lineLabel(img, 54, 157, 9, 5, lb.l, line);
    if (!lb.r.empty()) lineLabel(img, 205, 157, 9, 5, lb.r, line);
    if (!lb.select.empty()) lineLabel(img, 134, 323, 16, 3, lb.select, line);
    if (!lb.start.empty()) lineLabel(img, 169, 323, 15, 3, lb.start, line);
    return true;
}

} // namespace psp::gpu
