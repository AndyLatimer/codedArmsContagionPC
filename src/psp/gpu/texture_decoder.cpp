#include "psp/gpu/texture_decoder.h"

#include <algorithm>
#include <cstring>

#include "psp/memory.h"

namespace psp::gpu {

const uint8_t kTextureBitsPerPixel[16] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0};

namespace {
// from PPSSPP TextureDecoder.cpp
const uint32_t kAlignMask16[16] = {
    0x7FF & ~(((8 * 16) / 16) - 1), 0x7FF & ~(((8 * 16) / 16) - 1), 0x7FF & ~(((8 * 16) / 16) - 1),
    0x7FF & ~(((8 * 16) / 32) - 1), 0x7FF & ~(((8 * 16) / 4) - 1),  0x7FF & ~(((8 * 16) / 8) - 1),
    0x7FF & ~(((8 * 16) / 16) - 1), 0x7FF & ~(((8 * 16) / 32) - 1), 0x7FF, 0x7FF, 0x7FF, 0, 0, 0, 0, 0,
};

inline uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
inline uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }

// Reads byte `x` of row `y` of a (possibly swizzled) texture whose rows are `rowBytes` long.
struct Texels {
    const uint8_t* base;
    uint32_t rowBytes;
    bool swizzled;
    uint32_t limit; // valid bytes from base
    uint8_t byte(uint32_t xb, uint32_t y) const {
        uint32_t off;
        if (swizzled) {
            uint32_t bxc = rowBytes / 16;
            off = ((y >> 3) * bxc + (xb >> 4)) * 128 + (y & 7) * 16 + (xb & 15);
        } else {
            off = y * rowBytes + xb;
        }
        return off < limit ? base[off] : 0;
    }
    uint16_t u16(uint32_t x, uint32_t y) const { return (uint16_t)(byte(2 * x, y) | (byte(2 * x + 1, y) << 8)); }
    uint32_t u32(uint32_t x, uint32_t y) const {
        return byte(4 * x, y) | (byte(4 * x + 1, y) << 8) | (byte(4 * x + 2, y) << 16) | ((uint32_t)byte(4 * x + 3, y) << 24);
    }
};

uint32_t clutLookup(const ClutState& clut, uint32_t index) {
    const int fmt = clut.format & 3;
    const uint32_t shift = (clut.format >> 2) & 0x1F, mask = (clut.format >> 8) & 0xFF;
    const uint32_t start = ((clut.format >> 16) & 0x1F) << 4;
    const uint32_t wrap = fmt == 3 ? 0xFF : 0x1FF;
    uint32_t i = ((index >> shift) & mask) | (start & wrap);
    if (fmt == 3) {
        i &= 0xFF;
        const uint8_t* p = clut.data + 4 * i;
        return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    i &= 0x1FF;
    const uint8_t* p = clut.data + 2 * i;
    uint16_t c = (uint16_t)(p[0] | (p[1] << 8));
    switch (fmt) {
    case 0: return convert565(c);
    case 1: return convert5551(c);
    default: return convert4444(c);
    }
}

// --- DXT (PSP block order: color lines first), from PPSSPP TextureDecoder.cpp ---------
inline uint32_t makecol(int r, int g, int b, int a) { return ((uint32_t)a << 24) | (b << 16) | (g << 8) | r; }
inline int mix_2_3(int c1, int c2) { return (c1 + c1 + c2) / 3; }

void dxtColors(const uint8_t* blk, uint32_t colors[4], bool dxt1) {
    uint16_t c1 = (uint16_t)(blk[4] | (blk[5] << 8)), c2 = (uint16_t)(blk[6] | (blk[7] << 8));
    int blue1 = (c1 << 3) & 0xF8, blue2 = (c2 << 3) & 0xF8;
    int green1 = (c1 >> 3) & 0xFC, green2 = (c2 >> 3) & 0xFC;
    int red1 = (c1 >> 8) & 0xF8, red2 = (c2 >> 8) & 0xF8;
    int alpha = dxt1 ? 255 : 0;
    colors[0] = makecol(red1, green1, blue1, alpha);
    colors[1] = makecol(red2, green2, blue2, alpha);
    if (c1 > c2) {
        colors[2] = makecol(mix_2_3(red1, red2), mix_2_3(green1, green2), mix_2_3(blue1, blue2), alpha);
        colors[3] = makecol(mix_2_3(red2, red1), mix_2_3(green2, green1), mix_2_3(blue2, blue1), alpha);
    } else {
        colors[2] = makecol((red1 + red2) / 2, (green1 + green2) / 2, (blue1 + blue2) / 2, alpha);
        colors[3] = makecol(0, 0, 0, 0);
    }
}

uint8_t lerp8(int a1, int a2, int n) { return (uint8_t)(((a1 * ((7 - n) << 8)) / 7 + (a2 * (n << 8)) / 7 + 31) >> 8); }
uint8_t lerp6(int a1, int a2, int n) { return (uint8_t)(((a1 * ((5 - n) << 8)) / 5 + (a2 * (n << 8)) / 5 + 31) >> 8); }

void decodeDxt(const uint8_t* src, uint32_t srcLimit, int n, int w, int h, uint32_t bufw, uint32_t* out) {
    const int blockSize = n == 1 ? 8 : 16;
    const int blocksPerRow = std::max<int>((int)bufw / 4, 1);
    for (int y = 0; y < h; y += 4) {
        uint32_t blockIndex = (uint32_t)(y / 4) * blocksPerRow;
        for (int x = 0; x < w; x += 4, ++blockIndex) {
            if ((blockIndex + 1) * blockSize > srcLimit) return;
            const uint8_t* blk = src + blockIndex * blockSize;
            uint32_t colors[4];
            dxtColors(blk, colors, n == 1);
            uint8_t alphaTab[8] = {};
            uint64_t allAlpha = 0;
            if (n == 5) {
                int a1 = blk[14], a2 = blk[15];
                alphaTab[0] = (uint8_t)a1;
                alphaTab[1] = (uint8_t)a2;
                if (a1 > a2) for (int k = 1; k <= 6; ++k) alphaTab[k + 1] = lerp8(a1, a2, k);
                else {
                    for (int k = 1; k <= 4; ++k) alphaTab[k + 1] = lerp6(a1, a2, k);
                    alphaTab[6] = 0;
                    alphaTab[7] = 255;
                }
                uint32_t ad2 = blk[8] | (blk[9] << 8) | (blk[10] << 16) | ((uint32_t)blk[11] << 24);
                uint16_t ad1 = (uint16_t)(blk[12] | (blk[13] << 8));
                allAlpha = ((uint64_t)ad1 << 32) | ad2;
            }
            for (int yy = 0; yy < 4 && y + yy < h; ++yy) {
                int colordata = blk[yy];
                uint32_t a3 = n == 3 ? (uint32_t)(blk[8 + 2 * yy] | (blk[9 + 2 * yy] << 8)) : 0;
                uint32_t a5 = (uint32_t)(allAlpha >> (12 * yy));
                for (int xx = 0; xx < 4 && x + xx < w; ++xx) {
                    uint32_t c = colors[colordata & 3];
                    if (n == 3) c |= (a3 & 0xF) << 28, a3 >>= 4;
                    if (n == 5) c |= (uint32_t)alphaTab[a5 & 7] << 24, a5 >>= 3;
                    out[(y + yy) * w + x + xx] = c;
                    colordata >>= 2;
                }
            }
        }
    }
}
} // namespace

uint32_t convert565(uint16_t c) {
    return 0xFF000000u | (expand5((c >> 11) & 31) << 16) | (expand6((c >> 5) & 63) << 8) | expand5(c & 31);
}
uint32_t convert5551(uint16_t c) {
    return ((c >> 15) ? 0xFF000000u : 0) | (expand5((c >> 10) & 31) << 16) | (expand5((c >> 5) & 31) << 8) | expand5(c & 31);
}
uint32_t convert4444(uint16_t c) {
    return (((c >> 12) & 15) * 0x11u << 24) | (((c >> 8) & 15) * 0x11u << 16) | (((c >> 4) & 15) * 0x11u << 8) | ((c & 15) * 0x11u);
}

uint32_t textureBufw(uint32_t texbufwidthData, int format) {
    uint32_t bufw = texbufwidthData & kAlignMask16[format & 15];
    if (bufw == 0 && format <= TFMT_DXT5) bufw = (8 * 16) / kTextureBitsPerPixel[format];
    return bufw;
}

uint32_t textureByteSize(int format, uint32_t bufw, int h, bool swizzled) {
    const uint32_t bpp = kTextureBitsPerPixel[format & 15];
    if (format >= TFMT_DXT1 && format <= TFMT_DXT5) {
        const uint32_t blockSize = format == TFMT_DXT1 ? 8 : 16;
        return std::max<uint32_t>(bufw / 4, 1) * (uint32_t)((h + 3) / 4) * blockSize;
    }
    const uint32_t rows = swizzled ? (uint32_t)((h + 7) & ~7) : (uint32_t)h;
    return (bpp * bufw * rows) / 8;
}

void decodeTexture(uint32_t addr, int format, int w, int h, uint32_t bufw, bool swizzled,
                   const ClutState& clut, std::vector<uint32_t>& out) {
    out.assign((size_t)w * h, 0);
    uint32_t bytes = textureByteSize(format, bufw, h, swizzled);
    // Clamp the readable range to valid memory.
    while (bytes && !validAddress(addr + bytes - 1)) bytes = bytes > 4096 ? bytes - 4096 : 0;
    if (!validAddress(addr) || !bytes) return;
    const uint8_t* src = hostPtr(addr);

    if (format >= TFMT_DXT1 && format <= TFMT_DXT5) {
        decodeDxt(src, bytes, format == TFMT_DXT1 ? 1 : (format == TFMT_DXT3 ? 3 : 5), w, h, bufw, out.data());
        return;
    }
    const uint32_t bpp = kTextureBitsPerPixel[format];
    uint32_t rowBytes = bufw * bpp / 8;
    if (swizzled) rowBytes = std::max<uint32_t>(rowBytes, 16);
    Texels t{src, rowBytes, swizzled, bytes};
    for (int y = 0; y < h; ++y) {
        uint32_t* row = out.data() + (size_t)y * w;
        for (int x = 0; x < w; ++x) {
            uint32_t c = 0;
            switch (format) {
            case TFMT_5650: c = convert565(t.u16(x, y)); break;
            case TFMT_5551: c = convert5551(t.u16(x, y)); break;
            case TFMT_4444: c = convert4444(t.u16(x, y)); break;
            case TFMT_8888: c = t.u32(x, y); break;
            case TFMT_CLUT4: { uint8_t b = t.byte(x >> 1, y); c = clutLookup(clut, (x & 1) ? (b >> 4) : (b & 15)); break; }
            case TFMT_CLUT8: c = clutLookup(clut, t.byte(x, y)); break;
            case TFMT_CLUT16: c = clutLookup(clut, t.u16(x, y)); break;
            case TFMT_CLUT32: c = clutLookup(clut, t.u32(x, y)); break;
            }
            row[x] = c;
        }
    }
}

} // namespace psp::gpu
