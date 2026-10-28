#include "psp/gpu/vertex_decoder.h"

#include <algorithm>
#include <cstring>

#include "psp/memory.h"

namespace psp::gpu {

namespace {
// from PPSSPP VertexDecoderCommon.cpp
const uint8_t tcsize[4] = {0, 2, 4, 8}, tcalign[4] = {0, 1, 2, 4};
const uint8_t colsize[8] = {0, 0, 0, 0, 2, 2, 2, 4}, colalign[8] = {0, 0, 0, 0, 2, 2, 2, 4};
const uint8_t nrmsize[4] = {0, 3, 6, 12}, nrmalign[4] = {0, 1, 2, 4};
const uint8_t possize[4] = {3, 3, 6, 12}, posalign[4] = {1, 1, 2, 4};
const uint8_t wtsize[4] = {0, 1, 2, 4}, wtalign[4] = {0, 1, 2, 4};

int alignUp(int v, int a) { return a > 1 ? (v + a - 1) & ~(a - 1) : v; }

float rdF(uint32_t a) { uint32_t v = read32(a); float f; std::memcpy(&f, &v, 4); return f; }

void decodeColor(int col, uint32_t a, float out[4]) {
    switch (col) {
    case 4: { // 565
        uint16_t c = read16(a);
        int r = c & 31, g = (c >> 5) & 63, b = (c >> 11) & 31;
        out[0] = ((r << 3) | (r >> 2)) / 255.0f;
        out[1] = ((g << 2) | (g >> 4)) / 255.0f;
        out[2] = ((b << 3) | (b >> 2)) / 255.0f;
        out[3] = 1.0f;
        break;
    }
    case 5: { // 5551
        uint16_t c = read16(a);
        int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        out[0] = ((r << 3) | (r >> 2)) / 255.0f;
        out[1] = ((g << 3) | (g >> 2)) / 255.0f;
        out[2] = ((b << 3) | (b >> 2)) / 255.0f;
        out[3] = (c >> 15) ? 1.0f : 0.0f;
        break;
    }
    case 6: { // 4444
        uint16_t c = read16(a);
        for (int k = 0; k < 4; ++k) out[k] = (((c >> (4 * k)) & 15) * 17) / 255.0f;
        break;
    }
    case 7: { // 8888
        uint32_t c = read32(a);
        for (int k = 0; k < 4; ++k) out[k] = ((c >> (8 * k)) & 255) / 255.0f;
        break;
    }
    }
}

void readVec3(int type, uint32_t a, float out[3], bool isSigned) {
    for (int k = 0; k < 3; ++k) {
        switch (type) {
        case 1: out[k] = isSigned ? (int8_t)read8(a + k) * (1.0f / 128.0f) : read8(a + k) * (1.0f / 128.0f); break;
        case 2: out[k] = isSigned ? (int16_t)read16(a + 2 * k) * (1.0f / 32768.0f) : read16(a + 2 * k) * (1.0f / 32768.0f); break;
        case 3: out[k] = rdF(a + 4 * k); break;
        default: out[k] = 0; break;
        }
    }
}

// 4x3 matrix (column major, 12 floats) times point/vector.
void mul43(const float* m, const float in[3], float out[3], bool point) {
    for (int r = 0; r < 3; ++r)
        out[r] = m[r] * in[0] + m[3 + r] * in[1] + m[6 + r] * in[2] + (point ? m[9 + r] : 0.0f);
}
} // namespace

void VertexFormat::set(uint32_t v) {
    vtype = v;
    tc = v & 3;
    col = (v >> 2) & 7;
    nrm = (v >> 5) & 3;
    pos = (v >> 7) & 3;
    weight = (v >> 9) & 3;
    idx = (v >> 11) & 3;
    nweights = ((v >> 14) & 7) + 1;
    morphs = ((v >> 18) & 7) + 1;
    through = (v >> 23) & 1;

    int s = 0, biggest = 1;
    if (weight) {
        weightOff = s;
        s += wtsize[weight] * nweights;
        biggest = std::max<int>(biggest, wtalign[weight]);
    }
    if (tc) {
        s = alignUp(s, tcalign[tc]);
        tcOff = s;
        s += tcsize[tc];
        biggest = std::max<int>(biggest, tcalign[tc]);
    }
    if (col) {
        s = alignUp(s, colalign[col]);
        colOff = s;
        s += colsize[col];
        biggest = std::max<int>(biggest, colalign[col]);
    }
    if (nrm) {
        s = alignUp(s, nrmalign[nrm]);
        nrmOff = s;
        s += nrmsize[nrm];
        biggest = std::max<int>(biggest, nrmalign[nrm]);
    }
    s = alignUp(s, posalign[pos]);
    posOff = s;
    s += possize[pos];
    biggest = std::max<int>(biggest, posalign[pos]);
    s = alignUp(s, biggest);
    oneSize = s;
    size = s * morphs;
}

void decodeVertex(const VertexFormat& f, const DecodeContext& ctx, uint32_t base, uint32_t i, Vertex& o) {
    const uint32_t v = base + i * (uint32_t)f.size;
    std::memset(&o, 0, sizeof o);
    o.color[0] = o.color[1] = o.color[2] = o.color[3] = 1.0f;

    // Texture coordinates
    for (int m = 0; m < f.morphs; ++m) {
        const uint32_t p = v + m * f.oneSize;
        const float w = f.morphs > 1 ? ctx.morphWeights[m] : 1.0f;
        float uv[2] = {0, 0};
        switch (f.tc) {
        case 1: uv[0] = read8(p + f.tcOff) / 128.0f; uv[1] = read8(p + f.tcOff + 1) / 128.0f; break;
        case 2:
            if (f.through) { uv[0] = read16(p + f.tcOff); uv[1] = read16(p + f.tcOff + 2); }
            else { uv[0] = read16(p + f.tcOff) / 32768.0f; uv[1] = read16(p + f.tcOff + 2) / 32768.0f; }
            break;
        case 3: uv[0] = rdF(p + f.tcOff); uv[1] = rdF(p + f.tcOff + 4); break;
        }
        if (f.through) { o.uv[0] = uv[0]; o.uv[1] = uv[1]; break; }
        o.uv[0] += uv[0] * w;
        o.uv[1] += uv[1] * w;
    }

    // Color
    if (f.col) {
        if (f.morphs > 1 && !f.through) {
            float acc[4] = {0, 0, 0, 0};
            for (int m = 0; m < f.morphs; ++m) {
                float c[4];
                decodeColor(f.col, v + m * f.oneSize + f.colOff, c);
                for (int k = 0; k < 4; ++k) acc[k] += c[k] * ctx.morphWeights[m];
            }
            for (int k = 0; k < 4; ++k) o.color[k] = std::min(1.0f, std::max(0.0f, acc[k]));
        } else {
            decodeColor(f.col, v + f.colOff, o.color);
        }
    }

    // Normal
    if (f.nrm) {
        for (int m = 0; m < f.morphs; ++m) {
            float n[3];
            readVec3(f.nrm, v + m * f.oneSize + f.nrmOff, n, true);
            const float w = f.morphs > 1 ? ctx.morphWeights[m] : 1.0f;
            for (int k = 0; k < 3; ++k) o.nrm[k] += n[k] * w;
            if (f.through) break;
        }
    }

    // Position
    if (f.through) {
        const uint32_t p = v + f.posOff;
        switch (f.pos) {
        case 1: break; // 8-bit positions decode to 0 in through mode
        case 2: o.pos[0] = (int16_t)read16(p); o.pos[1] = (int16_t)read16(p + 2); o.pos[2] = read16(p + 4); break;
        case 3: {
            o.pos[0] = rdF(p);
            o.pos[1] = rdF(p + 4);
            float z = rdF(p + 8);
            o.pos[2] = z >= 65535.0f ? 65535.0f : (z > 0.0f ? (float)(int)z : 0.0f);
            break;
        }
        }
        return;
    }
    for (int m = 0; m < f.morphs; ++m) {
        float p[3];
        readVec3(f.pos, v + m * f.oneSize + f.posOff, p, true);
        const float w = f.morphs > 1 ? ctx.morphWeights[m] : 1.0f;
        for (int k = 0; k < 3; ++k) o.pos[k] += p[k] * w;
    }

    // Skinning: blend of the bone matrices by the vertex weights.
    if (f.weight && ctx.mtx) {
        float skin[12] = {0};
        for (int b = 0; b < f.nweights; ++b) {
            float w = 0;
            const uint32_t wa = v + f.weightOff;
            switch (f.weight) {
            case 1: w = read8(wa + b) / 128.0f; break;
            case 2: w = read16(wa + 2 * b) / 32768.0f; break;
            case 3: w = rdF(wa + 4 * b); break;
            }
            for (int k = 0; k < 12; ++k) skin[k] += ctx.mtx->bone[12 * b + k] * w;
        }
        float p[3], n[3];
        mul43(skin, o.pos, p, true);
        mul43(skin, o.nrm, n, false);
        std::memcpy(o.pos, p, sizeof p);
        std::memcpy(o.nrm, n, sizeof n);
    }
}

} // namespace psp::gpu
