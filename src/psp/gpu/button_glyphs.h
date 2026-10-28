#pragma once
// PC key labels in place of the PSP button symbols of the game's interface. The game draws
// its button hints (key configuration, control type, footers) from two texture atlases; when
// one of them is decoded, the PSP symbols are repainted as keycaps labelled with the PC key
// bound to that button, in a 3x5 pixel font that matches the atlas's own lettering.
#include <cstdint>
#include <string>

namespace psp::gpu {

// Labels: A-Z, 0-9 and the codes below; an empty label keeps the original symbol.
constexpr char kGlyphEnter = '\x01', kGlyphUp = '\x02', kGlyphDown = '\x03', kGlyphLeft = '\x04', kGlyphRight = '\x05';
struct ButtonLabels {
    std::string cross, circle, square, triangle, l, r, start, select;
};
void setButtonLabels(const ButtonLabels& labels);

// Called with every decoded texture (top level, RGBA8888 rows); returns true if it was patched.
bool patchButtonGlyphs(uint32_t* rgba, int w, int h);

} // namespace psp::gpu
