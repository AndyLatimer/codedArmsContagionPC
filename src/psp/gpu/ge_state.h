#pragma once
// GE register file helpers. The list walker (psp/ge.cpp) keeps the last 24-bit data word
// written to every command; matrices are uploaded through number/data command pairs and are
// tracked here. Command numbers and field layouts follow PPSSPP's GPU/ge_constants.h and
// GPU/GPUState.h (GPL-2.0-or-later).
#include <cmath>
#include <cstdint>
#include <cstring>

namespace psp::gpu {

enum GeCmd : uint32_t {
    CMD_VADDR = 0x01, CMD_IADDR = 0x02, CMD_PRIM = 0x04, CMD_BEZIER = 0x05, CMD_SPLINE = 0x06,
    CMD_BOUNDINGBOX = 0x07, CMD_VERTEXTYPE = 0x12, CMD_REGION1 = 0x15, CMD_REGION2 = 0x16,
    CMD_LIGHTINGENABLE = 0x17, CMD_LIGHTENABLE0 = 0x18, CMD_DEPTHCLIPENABLE = 0x1C,
    CMD_CULLFACEENABLE = 0x1D, CMD_TEXTUREMAPENABLE = 0x1E, CMD_FOGENABLE = 0x1F,
    CMD_DITHERENABLE = 0x20, CMD_ALPHABLENDENABLE = 0x21, CMD_ALPHATESTENABLE = 0x22,
    CMD_ZTESTENABLE = 0x23, CMD_STENCILTESTENABLE = 0x24, CMD_ANTIALIASENABLE = 0x25,
    CMD_PATCHCULLENABLE = 0x26, CMD_COLORTESTENABLE = 0x27, CMD_LOGICOPENABLE = 0x28,
    CMD_BONEMATRIXNUMBER = 0x2A, CMD_BONEMATRIXDATA = 0x2B, CMD_MORPHWEIGHT0 = 0x2C,
    CMD_PATCHDIVISION = 0x36, CMD_PATCHPRIMITIVE = 0x37, CMD_PATCHFACING = 0x38,
    CMD_WORLDMATRIXNUMBER = 0x3A, CMD_WORLDMATRIXDATA = 0x3B, CMD_VIEWMATRIXNUMBER = 0x3C,
    CMD_VIEWMATRIXDATA = 0x3D, CMD_PROJMATRIXNUMBER = 0x3E, CMD_PROJMATRIXDATA = 0x3F,
    CMD_TGENMATRIXNUMBER = 0x40, CMD_TGENMATRIXDATA = 0x41, CMD_VIEWPORTXSCALE = 0x42,
    CMD_VIEWPORTYSCALE = 0x43, CMD_VIEWPORTZSCALE = 0x44, CMD_VIEWPORTXCENTER = 0x45,
    CMD_VIEWPORTYCENTER = 0x46, CMD_VIEWPORTZCENTER = 0x47, CMD_TEXSCALEU = 0x48,
    CMD_TEXSCALEV = 0x49, CMD_TEXOFFSETU = 0x4A, CMD_TEXOFFSETV = 0x4B, CMD_OFFSETX = 0x4C,
    CMD_OFFSETY = 0x4D, CMD_SHADEMODE = 0x50, CMD_REVERSENORMAL = 0x51, CMD_MATERIALUPDATE = 0x53,
    CMD_MATERIALEMISSIVE = 0x54, CMD_MATERIALAMBIENT = 0x55, CMD_MATERIALDIFFUSE = 0x56,
    CMD_MATERIALSPECULAR = 0x57, CMD_MATERIALALPHA = 0x58, CMD_MATERIALSPECULARCOEF = 0x5B,
    CMD_AMBIENTCOLOR = 0x5C, CMD_AMBIENTALPHA = 0x5D, CMD_LIGHTMODE = 0x5E, CMD_LIGHTTYPE0 = 0x5F,
    CMD_LX0 = 0x63, CMD_LDX0 = 0x6F, CMD_LKA0 = 0x7B, CMD_LKS0 = 0x87, CMD_LKO0 = 0x8B,
    CMD_LAC0 = 0x8F, CMD_CULL = 0x9B, CMD_FRAMEBUFPTR = 0x9C, CMD_FRAMEBUFWIDTH = 0x9D,
    CMD_ZBUFPTR = 0x9E, CMD_ZBUFWIDTH = 0x9F, CMD_TEXADDR0 = 0xA0, CMD_TEXBUFWIDTH0 = 0xA8,
    CMD_CLUTADDR = 0xB0, CMD_CLUTADDRUPPER = 0xB1, CMD_TRANSFERSRC = 0xB2, CMD_TRANSFERSRCW = 0xB3,
    CMD_TRANSFERDST = 0xB4, CMD_TRANSFERDSTW = 0xB5, CMD_TEXSIZE0 = 0xB8, CMD_TEXMAPMODE = 0xC0,
    CMD_TEXSHADELS = 0xC1, CMD_TEXMODE = 0xC2, CMD_TEXFORMAT = 0xC3, CMD_LOADCLUT = 0xC4,
    CMD_CLUTFORMAT = 0xC5, CMD_TEXFILTER = 0xC6, CMD_TEXWRAP = 0xC7, CMD_TEXLEVEL = 0xC8,
    CMD_TEXFUNC = 0xC9, CMD_TEXENVCOLOR = 0xCA, CMD_TEXFLUSH = 0xCB, CMD_TEXSYNC = 0xCC,
    CMD_FOG1 = 0xCD, CMD_FOG2 = 0xCE, CMD_FOGCOLOR = 0xCF, CMD_TEXLODSLOPE = 0xD0,
    CMD_FRAMEBUFPIXFORMAT = 0xD2, CMD_CLEARMODE = 0xD3, CMD_SCISSOR1 = 0xD4, CMD_SCISSOR2 = 0xD5,
    CMD_MINZ = 0xD6, CMD_MAXZ = 0xD7, CMD_COLORTEST = 0xD8, CMD_COLORREF = 0xD9,
    CMD_COLORTESTMASK = 0xDA, CMD_ALPHATEST = 0xDB, CMD_STENCILTEST = 0xDC, CMD_STENCILOP = 0xDD,
    CMD_ZTEST = 0xDE, CMD_BLENDMODE = 0xDF, CMD_BLENDFIXEDA = 0xE0, CMD_BLENDFIXEDB = 0xE1,
    CMD_DITH0 = 0xE2, CMD_LOGICOP = 0xE6, CMD_ZWRITEDISABLE = 0xE7, CMD_MASKRGB = 0xE8,
    CMD_MASKALPHA = 0xE9, CMD_TRANSFERSTART = 0xEA, CMD_TRANSFERSRCPOS = 0xEB,
    CMD_TRANSFERDSTPOS = 0xEC, CMD_TRANSFERSIZE = 0xEE,
};

// GE primitive types (PRIM data bits 16-18).
enum : int { PRIM_POINTS, PRIM_LINES, PRIM_LINE_STRIP, PRIM_TRIANGLES, PRIM_TRIANGLE_STRIP, PRIM_TRIANGLE_FAN, PRIM_RECTANGLES };

// 24-bit GE float: the top 24 bits of an IEEE single.
inline float float24(uint32_t data) {
    uint32_t bits = data << 8;
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// Matrices as uploaded: world/view/tgen/bones are 4x3 (12 floats, column major: m[0..2] is the
// first column), projection is 4x4.
struct Matrices {
    float world[12], view[12], proj[16], tgen[12], bone[8 * 12];
    int worldNum = 0, viewNum = 0, projNum = 0, tgenNum = 0, boneNum = 0;
    Matrices() {
        static const float ident43[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        static const float ident44[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(world, ident43, sizeof world);
        std::memcpy(view, ident43, sizeof view);
        std::memcpy(tgen, ident43, sizeof tgen);
        std::memcpy(proj, ident44, sizeof proj);
        for (int i = 0; i < 8; ++i) std::memcpy(bone + 12 * i, ident43, sizeof ident43);
    }
};

} // namespace psp::gpu
