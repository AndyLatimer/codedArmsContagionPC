#include "frontend/OptionsMenu.h"

#include <algorithm>

#include "frontend/DialogOverlay.h"

namespace ca {

namespace {

// Layout in the 960x544 image (twice the PSP screen), after the game's OPTIONS screen: label
// panels on the left, value panels on the right.
constexpr int kListY = 24, kRowH = 34, kRows = 12, kPanelH = 28;
constexpr int kLabelX = 96, kLabelW = 360, kValueX = kLabelX + kLabelW + 10, kValueW = 420;
constexpr int kValueRight = kValueX + kValueW, kPanelX = kLabelX - 40, kPanelW = kValueRight - kPanelX + 20;

// The game's colours (sampled from its OPTIONS screen).
constexpr draw::Color kGreen{0x77, 0xFF, 0xAC, 255}, kOrange{0xFF, 0xA7, 0x37, 255}, kVeil{3, 36, 42, 248},
    kPanelFill{13, 53, 57, 215}, kSelectedFill{8, 28, 30, 235}, kDimGreen{0x31, 0x75, 0x5B, 255};

// Latin-1 letters to upper case, as the game writes its labels (UTF-8 in, UTF-8 out).
std::string upper(const std::string& s) {
    std::string o = s;
    for (size_t i = 0; i < o.size(); ++i) {
        unsigned char c = (unsigned char)o[i];
        if (c >= 'a' && c <= 'z') o[i] = (char)(c - 32);
        else if (c == 0xC3 && i + 1 < o.size()) {
            unsigned char d = (unsigned char)o[i + 1];
            if (d >= 0xA0 && d <= 0xBE && d != 0xB7) o[i + 1] = (char)(d - 0x20);
            ++i;
        }
    }
    return o;
}

} // namespace

void OptionsMenu::open(std::vector<MenuItem> items) {
    items_ = std::move(items);
    open_ = true;
    prompt_.clear();
    if (!selectable(selected_)) selected_ = 0;
    while (selected_ < (int)items_.size() && !selectable(selected_)) ++selected_;
    keepVisible();
}

void OptionsMenu::keepVisible() {
    if (selected_ < first_) first_ = selected_;
    if (selected_ >= first_ + kRows) first_ = selected_ - kRows + 1;
    // Show a section's title with its first item.
    if (selected_ == first_ && first_ > 0 && items_[(size_t)first_ - 1].header) --first_;
    first_ = std::clamp(first_, 0, std::max(0, (int)items_.size() - kRows));
}

void OptionsMenu::move(int dir) {
    for (int i = selected_ + dir; i >= 0 && i < (int)items_.size(); i += dir)
        if (selectable(i)) { selected_ = i; break; }
    keepVisible();
}

void OptionsMenu::change(int dir) {
    if (!selectable(selected_)) return;
    if (auto& f = items_[(size_t)selected_].change) f(dir);
}

void OptionsMenu::activate() {
    if (!selectable(selected_)) return;
    MenuItem& it = items_[(size_t)selected_];
    if (it.activate) it.activate();
    else if (it.change) it.change(1);
}

void OptionsMenu::wheel(int dir) {
    first_ = std::clamp(first_ + dir, 0, std::max(0, (int)items_.size() - kRows));
}

int OptionsMenu::rowAt(int x, int y) const {
    if (x < kPanelX || x >= kValueRight || y < kListY || y >= kListY + kRows * kRowH) return -1;
    const int i = first_ + (y - kListY) / kRowH;
    return i < (int)items_.size() ? i : -1;
}

void OptionsMenu::hover(int x, int y) {
    const int i = rowAt(x, y);
    if (selectable(i)) selected_ = i;
}

void OptionsMenu::click(int x, int y) {
    const int i = rowAt(x, y);
    if (!selectable(i)) return;
    selected_ = i;
    MenuItem& it = items_[(size_t)i];
    if (it.change && !it.activate && x >= kValueX) it.change(x < kValueX + kValueW / 2 ? -1 : 1);
    else activate();
}

void OptionsMenu::paint(std::vector<uint32_t>& rgba) const {
    using namespace draw;
    rgba.assign((size_t)kOverlayW * kOverlayH, 0);
    rect(rgba, 0, 0, kOverlayW, kOverlayH, kVeil);

    for (int r = 0; r < kRows && first_ + r < (int)items_.size(); ++r) {
        const int i = first_ + r;
        const MenuItem& it = items_[(size_t)i];
        const int y = kListY + r * kRowH;
        if (it.header) { // like the game's small captions above its fields
            text(rgba, kLabelX, y + 14, upper(it.label), 1, kGreen);
            rect(rgba, kLabelX, y + 31, kValueRight - kLabelX, 1, kDimGreen);
            continue;
        }
        const bool sel = i == selected_ && prompt_.empty();
        const Color fg = sel ? kOrange : kGreen;
        rect(rgba, kLabelX, y + 3, kLabelW, kPanelH, sel ? kSelectedFill : kPanelFill);
        rect(rgba, kValueX, y + 3, kValueW, kPanelH, sel ? kSelectedFill : kPanelFill);
        if (sel) { // the game underlines the selected row in orange and marks its icon
            rect(rgba, kLabelX, y + 3 + kPanelH - 2, kLabelW, 2, kOrange);
            rect(rgba, kValueX, y + 3 + kPanelH - 2, kValueW, 2, kOrange);
            rect(rgba, kPanelX, y + 7, 24, kPanelH - 8, kOrange);
        } else {
            frame(rgba, kPanelX, y + 7, 24, kPanelH - 8, kDimGreen, 2);
        }
        text(rgba, kLabelX + 12, y + 9, upper(it.label), 1, fg, kLabelW - 16);
        std::string v = it.value ? it.value() : "";
        if (!it.note.empty()) v = v.empty() ? it.note : v + "  (" + it.note + ")";
        const bool arrows = it.change && it.value;
        const int vx = kValueX + (arrows ? 40 : 14);
        text(rgba, vx, y + 9, v, 1, fg, kValueW - (arrows ? 80 : 28));
        if (arrows) { // the game's arrow boxes
            for (int k = 0; k < 2; ++k) {
                const int bx = k ? kValueRight - 30 : kValueX + 6;
                frame(rgba, bx, y + 7, 22, 20, sel ? kOrange : kDimGreen, 1);
                text(rgba, bx + 7, y + 9, k ? ">" : "<", 1, sel ? kOrange : kDimGreen);
            }
        }
    }
    if (first_ > 0) text(rgba, (kLabelX + kValueRight) / 2, kListY - 16, "^", 1, kGreen);
    if (first_ + kRows < (int)items_.size()) text(rgba, (kLabelX + kValueRight) / 2, kListY + kRows * kRowH, "v", 1, kGreen);

    // Help line, keys and the page's name, where the game has them.
    const MenuItem* cur = selectable(selected_) ? &items_[(size_t)selected_] : nullptr;
    const std::string help = cur && !cur->help.empty() ? cur->help : (cur ? cur->label : "");
    rect(rgba, 52, 452, 6, 6, kGreen);
    rect(rgba, 60, 452, 4, 4, kGreen);
    text(rgba, 72, 446, help, 1, kGreen, kOverlayW - 120);
    if (!footer_.empty()) text(rgba, 16, 518, footer_, 1, kGreen, 620);
    if (!title_.empty()) text(rgba, kOverlayW - 24 - textWidth(title_, 2), 500, title_, 2, kGreen);

    if (!prompt_.empty()) {
        const int w = std::max(textWidth(prompt_, 1) + 80, 420), h = 90;
        const int x = (kOverlayW - w) / 2, y = (kOverlayH - h) / 2;
        rect(rgba, x, y, w, h, Color{3, 28, 34, 250});
        frame(rgba, x, y, w, h, kOrange, 2);
        text(rgba, (kOverlayW - textWidth(prompt_, 1)) / 2, y + (h - 16) / 2, prompt_, 1, kGreen);
    }
}

} // namespace ca
