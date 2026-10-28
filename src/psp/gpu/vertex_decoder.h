#pragma once
// Decodes GE vertices (any VTYPE) from guest memory into a fixed float layout. Component
// sizes, alignment, scaling and the through-mode rules follow PPSSPP's
// GPU/Common/VertexDecoderCommon.cpp (GPL-2.0-or-later). Morphing and skinning are applied
// here, so the shaders only see model-space positions/normals.
#include <cstdint>
#include <vector>

#include "psp/gpu/ge_state.h"

namespace psp::gpu {

struct Vertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    float color[4]; // 0..1
};

struct VertexFormat {
    uint32_t vtype = 0;
    int tc = 0, col = 0, nrm = 0, pos = 0, weight = 0, idx = 0;
    int nweights = 1, morphs = 1;
    bool through = false;
    int weightOff = 0, tcOff = 0, colOff = 0, nrmOff = 0, posOff = 0;
    int oneSize = 0;  // one morph target
    int size = 0;     // stride between vertices (all morph targets)
    void set(uint32_t vtype);
    int indexSize() const { return idx == 0 ? 0 : (idx == 1 ? 1 : (idx == 2 ? 2 : 4)); }
};

struct DecodeContext {
    const Matrices* mtx = nullptr;
    float morphWeights[8] = {1, 0, 0, 0, 0, 0, 0, 0};
};

// Decodes the vertex with index `i` (relative to `base`).
void decodeVertex(const VertexFormat& fmt, const DecodeContext& ctx, uint32_t base, uint32_t i, Vertex& out);

} // namespace psp::gpu
