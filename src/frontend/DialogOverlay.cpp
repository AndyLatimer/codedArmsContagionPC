#include "frontend/DialogOverlay.h"

#include <algorithm>
#include <cstdio>

#include "frontend/DialogFont.h"

namespace ca {

namespace {

using psp::host::DialogKind;
using psp::host::DialogSaveEntry;
using psp::host::DialogView;

struct Color { uint8_t r, g, b, a; };
const Color kDim{0, 0, 0, 170}, kPanel{10, 28, 30, 235}, kAccent{80, 230, 170, 255}, kText{230, 240, 235, 255},
    kSoft{150, 175, 170, 255}, kRowFill{18, 44, 46, 235}, kSelFill{26, 70, 62, 245};

struct Canvas {
    std::vector<uint32_t>& px;
    int fade; // 0..255
    void blend(int x, int y, Color c, int cover) { // cover 0..255
        if (x < 0 || y < 0 || x >= kOverlayW || y >= kOverlayH) return;
        const int a = c.a * cover / 255 * fade / 255;
        if (!a) return;
        uint32_t& d = px[(size_t)y * kOverlayW + x];
        const int dr = d & 0xFF, dg = (d >> 8) & 0xFF, db = (d >> 16) & 0xFF, da = d >> 24;
        const int oa = a + da * (255 - a) / 255; // "over" with straight alpha
        if (!oa) return;
        auto mix = [&](int s, int dd) { return (s * a + dd * da * (255 - a) / 255) / oa; };
        d = (uint32_t)mix(c.r, dr) | (uint32_t)mix(c.g, dg) << 8 | (uint32_t)mix(c.b, db) << 16 | (uint32_t)oa << 24;
    }
    void rect(int x, int y, int w, int h, Color c) {
        for (int j = y; j < y + h; ++j)
            for (int i = x; i < x + w; ++i) blend(i, j, c, 255);
    }
    void frame(int x, int y, int w, int h, Color c, int t) {
        rect(x, y, w, t, c);
        rect(x, y + h - t, w, t, c);
        rect(x, y + t, t, h - 2 * t, c);
        rect(x + w - t, y + t, t, h - 2 * t, c);
    }
};

// UTF-8 to code points; full-width ASCII (as in the game's save titles) becomes ASCII.
std::vector<uint32_t> decode(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = (uint8_t)s[i];
        uint32_t cp = '?';
        int n = 1;
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 6 && i + 1 < s.size()) { cp = (c & 0x1F) << 6 | (s[i + 1] & 0x3F); n = 2; }
        else if ((c >> 4) == 14 && i + 2 < s.size()) { cp = (c & 0x0F) << 12 | (s[i + 1] & 0x3F) << 6 | (s[i + 2] & 0x3F); n = 3; }
        else if ((c >> 3) == 30) n = 4;
        if (cp >= 0xFF01 && cp <= 0xFF5E) cp -= 0xFEE0;
        if (cp == 0x3000) cp = ' ';
        if (cp != '\r') out.push_back(cp);
        i += n;
    }
    return out;
}

int glyphIndex(uint32_t cp) { return (cp >= 32 && cp <= 255) ? (int)cp - 32 : '?' - 32; }

void glyph(Canvas& cv, int x, int y, uint32_t cp, int scale, Color c) {
    const uint8_t* g = font::kGlyphs + (size_t)glyphIndex(cp) * (font::kW * font::kH / 2);
    for (int gy = 0; gy < font::kH; ++gy)
        for (int gx = 0; gx < font::kW; ++gx) {
            const int i = gy * font::kW + gx;
            const int v = (i & 1) ? (g[i / 2] & 15) : (g[i / 2] >> 4);
            if (!v) continue;
            for (int sy = 0; sy < scale; ++sy)
                for (int sx = 0; sx < scale; ++sx) cv.blend(x + gx * scale + sx, y + gy * scale + sy, c, v * 17);
        }
}

int textWidth(const std::string& s, int scale) { return (int)decode(s).size() * font::kW * scale; }

void text(Canvas& cv, int x, int y, const std::string& s, int scale, Color c, int maxW = 1 << 20) {
    int cx = x;
    for (uint32_t cp : decode(s)) {
        if (cx + font::kW * scale > x + maxW) break;
        glyph(cv, cx, y, cp, scale, c);
        cx += font::kW * scale;
    }
}

// Word wrap to `cols` characters per line; '\n' forces a break.
std::vector<std::string> wrap(const std::string& s, size_t cols) {
    std::vector<std::string> lines;
    std::string line, word;
    size_t lineLen = 0, wordLen = 0;
    auto putWord = [&] {
        if (lineLen && lineLen + 1 + wordLen > cols) { lines.push_back(line); line.clear(); lineLen = 0; }
        if (lineLen && wordLen) { line += ' '; ++lineLen; }
        line += word;
        lineLen += wordLen;
        word.clear();
        wordLen = 0;
    };
    for (char ch : s) {
        if (ch == ' ' || ch == '\n') {
            if (wordLen) putWord();
            if (ch == '\n') { lines.push_back(line); line.clear(); lineLen = 0; }
            continue;
        }
        word += ch;
        if (((uint8_t)ch & 0xC0) != 0x80) ++wordLen; // count characters, not UTF-8 bytes
    }
    if (wordLen) putWord();
    lines.push_back(line);
    return lines;
}

std::string sizeText(int64_t bytes) {
    char b[32];
    if (bytes >= 1024 * 1024) std::snprintf(b, sizeof b, "%.1f MB", bytes / (1024.0 * 1024.0));
    else std::snprintf(b, sizeof b, "%lld KB", (long long)((bytes + 1023) / 1024));
    return b;
}

void entryBox(Canvas& cv, int x, int y, int w, int h, const DialogSaveEntry& e, bool selected) {
    cv.rect(x, y, w, h, selected ? kSelFill : kRowFill);
    cv.frame(x, y, w, h, selected ? kAccent : Color{60, 100, 95, 255}, selected ? 3 : 1);
    const int tx = x + 20;
    if (e.sizeBytes == 0 && !e.broken) {
        text(cv, tx, y + (h - 32) / 2, "New Data", 2, selected ? kAccent : kText);
        return;
    }
    if (e.broken) {
        text(cv, tx, y + (h - 32) / 2, "Corrupted data", 2, kSoft);
        return;
    }
    const std::string title = e.savedataTitle.empty() ? e.title : e.title + "  " + e.savedataTitle;
    text(cv, tx, y + 8, title, 2, selected ? kAccent : kText, w - 40);
    const std::string detail = wrap(e.detail, 200).front();
    text(cv, tx, y + 44, detail, 1, kSoft, w - 300);
    char date[64];
    std::snprintf(date, sizeof date, "%04d/%02d/%02d %02d:%02d   %s", e.modified.tm_year + 1900, e.modified.tm_mon + 1,
                  e.modified.tm_mday, e.modified.tm_hour, e.modified.tm_min, sizeText(e.sizeBytes).c_str());
    text(cv, x + w - 20 - textWidth(date, 1), y + 44, date, 1, kSoft);
}

void keyHint(Canvas& cv, int& x, int y, const std::string& key, const std::string& label) {
    const int kw = textWidth(key, 1) + 16;
    x -= textWidth(label, 1) + 8 + kw;
    cv.rect(x, y - 4, kw, 24, Color{40, 80, 74, 255});
    cv.frame(x, y - 4, kw, 24, kAccent, 1);
    text(cv, x + 8, y, key, 1, kText);
    text(cv, x + kw + 8, y, label, 1, kText);
    x -= 28;
}

} // namespace

namespace draw {
void rect(std::vector<uint32_t>& rgba, int x, int y, int w, int h, Color c) {
    Canvas cv{rgba, 255};
    cv.rect(x, y, w, h, ca::Color{c.r, c.g, c.b, c.a});
}
void frame(std::vector<uint32_t>& rgba, int x, int y, int w, int h, Color c, int thickness) {
    Canvas cv{rgba, 255};
    cv.frame(x, y, w, h, ca::Color{c.r, c.g, c.b, c.a}, thickness);
}
void text(std::vector<uint32_t>& rgba, int x, int y, const std::string& utf8, int scale, Color c, int maxW) {
    Canvas cv{rgba, 255};
    ca::text(cv, x, y, utf8, scale, ca::Color{c.r, c.g, c.b, c.a}, maxW);
}
int textWidth(const std::string& utf8, int scale) { return ca::textWidth(utf8, scale); }
} // namespace draw

void paintDialog(const DialogView& v, const DialogKeys& keys, std::vector<uint32_t>& rgba) {
    rgba.assign((size_t)kOverlayW * kOverlayH, 0);
    if (v.kind == DialogKind::None) return;
    Canvas cv{rgba, std::clamp(v.fade, 0, 255)};
    cv.rect(0, 0, kOverlayW, kOverlayH, kDim);

    int textTop = 200;
    if (v.kind == DialogKind::Savedata) {
        if (!v.banner.empty()) {
            text(cv, 48, 28, v.banner, 2, kAccent);
            cv.rect(48, 66, kOverlayW - 96, 2, kAccent);
        }
        if (!v.entries.empty()) {
            const int rows = 5, n = (int)v.entries.size();
            const int first = std::clamp(v.selected - rows / 2, 0, std::max(0, n - rows));
            for (int i = first; i < std::min(n, first + rows); ++i)
                entryBox(cv, 48, 84 + (i - first) * 76, kOverlayW - 96, 68, v.entries[(size_t)i], i == v.selected);
            if (first > 0) text(cv, kOverlayW / 2 - 4, 68, "^", 1, kAccent);
            if (first + rows < n) text(cv, kOverlayW / 2 - 4, 84 + rows * 76 - 6, "v", 1, kAccent);
            textTop = -1;
        } else if (v.showsEntry) {
            entryBox(cv, 48, 84, kOverlayW - 96, 68, v.entry, false);
            textTop = 190;
        }
    }

    if (textTop >= 0 && !v.text.empty()) {
        const auto lines = wrap(v.text, 50);
        const int lineH = 36;
        int boxH = (int)lines.size() * lineH + 48 + ((v.yesNo || v.ok) ? 56 : 0);
        int boxY = v.kind == DialogKind::Message ? (kOverlayH - boxH) / 2 : textTop;
        cv.rect(96, boxY, kOverlayW - 192, boxH, kPanel);
        cv.frame(96, boxY, kOverlayW - 192, boxH, kAccent, 2);
        int y = boxY + 24;
        for (const std::string& l : lines) {
            text(cv, (kOverlayW - textWidth(l, 2)) / 2, y, l, 2, kText);
            y += lineH;
        }
        if (v.yesNo || v.ok) {
            y += 8;
            auto choice = [&](int cx, const char* label, bool sel) {
                const int w = textWidth(label, 2) + 32;
                if (sel) {
                    cv.rect(cx - w / 2, y - 6, w, 44, kSelFill);
                    cv.frame(cx - w / 2, y - 6, w, 44, kAccent, 2);
                }
                text(cv, cx - textWidth(label, 2) / 2, y, label, 2, sel ? kAccent : kSoft);
            };
            if (v.yesNo) {
                choice(kOverlayW / 2 - 90, "Yes", v.yesNoChoice == 1);
                choice(kOverlayW / 2 + 90, "No", v.yesNoChoice != 1);
            } else {
                choice(kOverlayW / 2, "OK", true);
            }
        }
    }

    // Key legend, bottom right (nothing while busy).
    if (!v.busy) {
        int x = kOverlayW - 40;
        const int y = kOverlayH - 40;
        if (v.cancelButton) keyHint(cv, x, y, keys.cancel, v.cancelLabel.empty() ? "Back" : v.cancelLabel);
        if (v.confirmButton) keyHint(cv, x, y, keys.confirm, v.confirmLabel.empty() ? "Enter" : v.confirmLabel);
        if (v.yesNo) keyHint(cv, x, y, keys.moveYesNo, "Select");
        else if (v.entries.size() > 1) keyHint(cv, x, y, keys.move, "Select");
    }
}

} // namespace ca
