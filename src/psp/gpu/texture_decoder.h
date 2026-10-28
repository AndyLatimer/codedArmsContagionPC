#pragma once
// Decodes PSP textures (all GE formats, swizzled or linear, CLUT lookups, DXT1/3/5) from
// guest memory into RGBA8888. Buffer-width rules, CLUT index transformation and the DXT block
// layout follow PPSSPP's GPU/Common/TextureDecoder.cpp and GPUState.h (GPL-2.0-or-later).
#include <cstdint>
#include <vector>

namespace psp::gpu {

enum TexFormat : int {
    TFMT_5650, TFMT_5551, TFMT_4444, TFMT_8888, TFMT_CLUT4, TFMT_CLUT8, TFMT_CLUT16, TFMT_CLUT32,
    TFMT_DXT1, TFMT_DXT3, TFMT_DXT5,
};

extern const uint8_t kTextureBitsPerPixel[16];

// Effective buffer width (pixels) for TEXBUFWIDTH data and format.
uint32_t textureBufw(uint32_t texbufwidthData, int format);

struct ClutState {
    uint32_t format = 0;          // CLUTFORMAT data
    uint8_t data[1024] = {};      // loaded palette bytes (LOADCLUT snapshot)
};

// Decodes one mip level. `out` receives w*h RGBA8888 pixels (R in the low byte).
void decodeTexture(uint32_t addr, int format, int w, int h, uint32_t bufw, bool swizzled,
                   const ClutState& clut, std::vector<uint32_t>& out);

// Number of guest bytes a level occupies (for hashing / invalidation).
uint32_t textureByteSize(int format, uint32_t bufw, int h, bool swizzled);

// Converts a PSP 16/32-bit color word into RGBA8888.
uint32_t convert565(uint16_t c);
uint32_t convert5551(uint16_t c);
uint32_t convert4444(uint16_t c);

} // namespace psp::gpu
