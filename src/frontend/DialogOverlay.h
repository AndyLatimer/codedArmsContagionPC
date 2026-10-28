#pragma once
// Draws the PSP system dialogs (savedata list/confirm/progress, messages) that the HLE runs
// without a picture (psp/host_dialog.h). The dialog itself reads the controller, so the keys
// bound to CROSS/CIRCLE/the d-pad drive it; this only paints what it shows, into an RGBA image
// laid over the game picture.
#include <cstdint>
#include <string>
#include <vector>

#include "psp/host_dialog.h"

namespace ca {

struct DialogKeys {
    std::string confirm, cancel, move, moveYesNo; // e.g. "Enter", "Backspace", "Up/Down", "Left/Right"
};

constexpr int kOverlayW = 960, kOverlayH = 544; // twice the PSP screen

// Paints `view` into `rgba` (kOverlayW x kOverlayH, RGBA8888, row 0 = top, premultiplied
// alpha is not used).
void paintDialog(const psp::host::DialogView& view, const DialogKeys& keys, std::vector<uint32_t>& rgba);

// Drawing into such an image, for other overlays (OptionsMenu): straight alpha, the dialogs'
// 8x16 font (Latin-1) at `scale`.
namespace draw {
struct Color { uint8_t r, g, b, a; };
inline constexpr Color kPanel{10, 28, 30, 235}, kAccent{80, 230, 170, 255}, kText{230, 240, 235, 255},
    kSoft{150, 175, 170, 255}, kRowFill{18, 44, 46, 235}, kSelFill{26, 70, 62, 245};
void rect(std::vector<uint32_t>& rgba, int x, int y, int w, int h, Color c);
void frame(std::vector<uint32_t>& rgba, int x, int y, int w, int h, Color c, int thickness);
void text(std::vector<uint32_t>& rgba, int x, int y, const std::string& utf8, int scale, Color c, int maxW = 1 << 20);
int textWidth(const std::string& utf8, int scale);
} // namespace draw

} // namespace ca
