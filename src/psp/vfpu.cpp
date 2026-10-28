// VFPU support for recompiled code: decodes a VFPU instruction word and runs the matching
// handler of PPSSPP's VFPU interpreter (src/psp/ppsspp) directly on the guest context.
// Encodings follow tools/recomp/allegrex.py, which decodes 100% of the game's code.
#include <cstdarg>
#include <cstdio>

#include "ppsspp/ppsspp_compat.h"
#include "psp/memory.h"

using namespace MIPSInt;

// ---- definitions the PPSSPP sources expect (values from PPSSPP Core/MIPS/MIPS.cpp) ----------
u8 voffset[128];
u8 fromvoffset[128];
const float vfpuPrefixConstants[8] = {0.f, 1.f, 2.f, 0.5f, 3.f, 1.f / 3.f, 0.25f, 1.f / 6.f};
const float cst_constants[32] = {
    0,
    std::numeric_limits<float>::max(),
    sqrtf(2.0f),
    sqrtf(0.5f),
    2.0f / sqrtf(3.14159265358979323846f),
    2.0f / 3.14159265358979323846f,
    1.0f / 3.14159265358979323846f,
    3.14159265358979323846f / 4,
    3.14159265358979323846f / 2,
    3.14159265358979323846f,
    2.71828182845904523536f,
    1.44269504088896340736f,
    0.434294481903251827651f,
    0.693147180559945309417f,
    2.30258509299404568402f,
    2 * 3.14159265358979323846f,
    3.14159265358979323846f / 6,
    log10f(2.0f),
    logf(10.0f) / logf(2.0f),
    sqrtf(3.0f) / 2.0f,
};

static struct VoffsetInit {
    VoffsetInit() {
        int i = 0;
        for (int m = 0; m < 8; m++)
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) voffset[m * 4 + x * 32 + y] = (u8)i++;
        for (int k = 0; k < 128; k++) fromvoffset[voffset[k]] = (u8)k;
    }
} s_voffsetInit;

void ppsspp_log(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::fprintf(stderr, "[VFPU] ");
    std::vfprintf(stderr, fmt, a);
    std::fprintf(stderr, "\n");
    va_end(a);
}

std::string StringFromFormat(const char* fmt, ...) {
    char buf[1024];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    return buf;
}

std::string MIPSDisasmAt(u32 pc) { return StringFromFormat("%08x", pc); }

void Core_MemoryException(u32 address, u32 size, u32 pc, MemoryExceptionType, const char* desc) {
    ppsspp_log("invalid %s access of %u bytes at %08x", desc, size, address);
    (void)pc;
}

// ---- guest memory view --------------------------------------------------------------------
bool GuestAddressSpace::IsValid4AlignedAddress(u32 addr) const { return (addr & 3) == 0 && psp::validAddress(addr); }
u32 GuestAddressSpace::ReadUnchecked_U32(u32 addr) const { return psp::read32(addr); }
float GuestAddressSpace::ReadUnchecked_Float(u32 addr) const {
    u32 v = psp::read32(addr);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
void GuestAddressSpace::WriteUnchecked_U32(u32 v, u32 addr) const { psp::write32(addr, v); }
void GuestAddressSpace::WriteUnchecked_Float(float v, u32 addr) const {
    u32 u;
    std::memcpy(&u, &v, 4);
    psp::write32(addr, u);
}
u8* GuestAddressSpace::GetPointerUnchecked(u32 addr) const { return psp::hostPtr(addr); }
const GuestAddressSpace* GuestSpace() {
    static GuestAddressSpace space;
    return &space;
}

// ---- dispatch ---------------------------------------------------------------------------
extern "C" void vfpu_exec(Ctx* c, uint32_t w) {
    MIPSState* m = static_cast<MIPSState*>(c);
    MIPSOpcode op(w);
    const u32 major = w >> 26;
    switch (major) {
    case 0x12: Int_Mftv(m, op); return;               // mfv/mtv/mfvc/mtvc
    case 0x18:                                         // VFPU0
        if (((w >> 23) & 7) == 2) Int_Vsbn(m, op); else Int_VecDo3(m, op);
        return;
    case 0x19:                                         // VFPU1
        switch ((w >> 23) & 7) {
        case 0: Int_VecDo3(m, op); return;             // vmul
        case 1: Int_VDot(m, op); return;
        case 2: Int_VScl(m, op); return;
        case 4: Int_VHdp(m, op); return;
        case 5: Int_Vcrs(m, op); return;
        case 6: Int_Vdet(m, op); return;
        }
        break;
    case 0x1B:                                         // VFPU3
        switch ((w >> 23) & 7) {
        case 0: Int_Vcmp(m, op); return;
        case 2: case 3: Int_Vminmax(m, op); return;
        case 5: Int_Vscmp(m, op); return;
        case 6: Int_Vsge(m, op); return;
        case 7: Int_Vslt(m, op); return;
        }
        break;
    case 0x32: case 0x3A: Int_SV(m, op); return;       // lv.s / sv.s
    case 0x35: case 0x36: case 0x3D: case 0x3E: Int_SVQ(m, op); return;
    case 0x34: {                                       // VFPU4
        const u32 sub = (w >> 21) & 0x1F, k = (w >> 16) & 0x1F;
        if (sub == 0) {
            if (k == 3) { Int_Vidt(m, op); return; }
            if (k == 6 || k == 7) { Int_VVectorInit(m, op); return; }
            Int_VV2Op(m, op);
            return;
        }
        if (sub == 1) {
            if (k == 0) { Int_Vrnds(m, op); return; }
            if (k >= 1 && k <= 3) { Int_VrndX(m, op); return; }
            if (k == 0x12) { Int_Vf2h(m, op); return; }
            if (k == 0x13) { Int_Vh2f(m, op); return; }
            if (k == 0x16) { Int_Vsbz(m, op); return; }
            if (k == 0x17) { Int_Vlgb(m, op); return; }
            if (k >= 0x18 && k <= 0x1B) { Int_Vx2i(m, op); return; }
            if (k >= 0x1C) { Int_Vi2x(m, op); return; }
            break;
        }
        if (sub == 2) {
            switch (k) {
            case 0: Int_Vsrt1(m, op); return;
            case 1: Int_Vsrt2(m, op); return;
            case 2: case 3: Int_Vbfy(m, op); return;
            case 4: Int_Vocp(m, op); return;
            case 5: Int_Vsocp(m, op); return;
            case 6: Int_Vfad(m, op); return;
            case 7: Int_Vavg(m, op); return;
            case 8: Int_Vsrt3(m, op); return;
            case 9: Int_Vsrt4(m, op); return;
            case 0xA: Int_Vsgn(m, op); return;
            case 0x10: Int_Vmfvc(m, op); return;
            case 0x11: Int_Vmtvc(m, op); return;
            case 0x19: case 0x1A: case 0x1B: Int_ColorConv(m, op); return;
            }
            break;
        }
        if (sub == 3) { Int_Vcst(m, op); return; }
        if (sub >= 0x10 && sub <= 0x13) { Int_Vf2i(m, op); return; }
        if (sub == 0x14) { Int_Vi2f(m, op); return; }
        if (sub == 0x15) { Int_Vcmov(m, op); return; }
        if (sub >= 0x18 && sub <= 0x1B) { Int_Vwbn(m, op); return; }
        break;
    }
    case 0x37:                                         // VFPU5
        if (((w >> 24) & 3) == 3) Int_Viim(m, op); else Int_VPFX(m, op);
        return;
    case 0x3C:                                         // VFPU6
        switch ((w >> 23) & 7) {
        case 0: Int_Vmmul(m, op); return;
        case 1: case 2: case 3: Int_Vtfm(m, op); return;
        case 4: Int_Vmscl(m, op); return;
        case 5: Int_CrossQuat(m, op); return;
        case 7:
            if (((w >> 21) & 3) == 0) {
                if (((w >> 16) & 7) == 0) Int_Vmmov(m, op); else Int_VMatrixInit(m, op);
                return;
            }
            if (((w >> 21) & 3) == 1) { Int_Vrot(m, op); return; }
            break;
        }
        break;
    case 0x3F:                                         // vnop / vsync / vflush
        if (w == 0xFFFF040D) Int_Vflush(m, op);
        return;
    }
    ppsspp_log("unhandled VFPU instruction %08x", w);
}
