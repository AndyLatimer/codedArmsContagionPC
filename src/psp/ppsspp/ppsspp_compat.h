// Minimal stand-ins for the PPSSPP headers that its VFPU interpreter (InterpreterVFPU.cpp,
// MIPSVFPUUtils.cpp/.h, GPL-2.0-or-later) depends on, mapped onto this port's runtime.
// Definitions marked "from PPSSPP" are copied from Core/MIPS/MIPS.h/.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "psp/recomp.h"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>
#define PPSSPP_ARCH_SSE2 1
#define PPSSPP_ARCH_AMD64 1
#endif
#define PPSSPP_ARCH(x) (defined(PPSSPP_ARCH_##x) && PPSSPP_ARCH_##x)

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

// ---- logging / assertions -----------------------------------------------------------
namespace Log { enum { CPU = 0, ME = 1 }; }
void ppsspp_log(const char* fmt, ...);
#define ERROR_LOG(cat, ...) ppsspp_log(__VA_ARGS__)
#define WARN_LOG(cat, ...) ppsspp_log(__VA_ARGS__)
#define ERROR_LOG_REPORT(cat, ...) ppsspp_log(__VA_ARGS__)
#define ERROR_LOG_REPORT_ONCE(id, cat, ...) do { static bool once_##id = false; if (!once_##id) { once_##id = true; ppsspp_log(__VA_ARGS__); } } while (0)
#define INFO_LOG(cat, ...) do {} while (0)
#define DEBUG_LOG(cat, ...) do {} while (0)
#define VERBOSE_LOG(cat, ...) do {} while (0)
#define M_LOG(...) do {} while (0)
#define _dbg_assert_msg_(cond, ...) do { if (!(cond)) ppsspp_log(__VA_ARGS__); } while (0)
#define _assert_msg_(cond, ...) do { if (!(cond)) ppsspp_log(__VA_ARGS__); } while (0)
#define _dbg_assert_(cond) do { if (!(cond)) ppsspp_log("assert failed: %s", #cond); } while (0)

enum class MemoryExceptionType { NONE, UNKNOWN, READ_WORD, WRITE_WORD, READ_BLOCK, WRITE_BLOCK, ALIGNMENT };
void Core_MemoryException(u32 address, u32 accessSize, u32 pc, MemoryExceptionType type, const char* desc);
std::string MIPSDisasmAt(u32 pc);

// ---- small helpers (Common/) ----------------------------------------------------------
inline s32 SignExtend16ToS32(u32 v) { return (s32)(s16)(v & 0xFFFF); }
inline bool my_isinf(float f) { return std::isinf(f); }
inline bool my_isnan(float f) { return std::isnan(f); }
inline bool my_isnanorinf(float f) { return !std::isfinite(f); }
inline int clz32_nonzero(u32 v) { return __builtin_clz(v); }
inline int clz32(u32 v) { return v ? __builtin_clz(v) : 32; }
#define NO_INLINE __attribute__((noinline))
std::string StringFromFormat(const char* fmt, ...);
// from PPSSPP Common/Math/math_util.h
inline int is_even(float d) {
    float int_part;
    modff(d / 2.0f, &int_part);
    return 2.0f * int_part == d;
}
inline double round_ieee_754(double d) {
    float i = (float)floor(d);
    d -= i;
    if (d < 0.5f) return i;
    if (d > 0.5f) return i + 1.0f;
    if (is_even(i)) return i;
    return i + 1.0f;
}
template <class T> inline T clamp_value(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---- opcode type ------------------------------------------------------------------------
struct MIPSOpcode {
    u32 encoding;
    MIPSOpcode(u32 v = 0) : encoding(v) {}
    operator u32() const { return encoding; }
};

// ---- from PPSSPP Core/MIPS/MIPS.h ---------------------------------------------------------
enum {
    VFPU_CTRL_SPREFIX, VFPU_CTRL_TPREFIX, VFPU_CTRL_DPREFIX, VFPU_CTRL_CC, VFPU_CTRL_INF4,
    VFPU_CTRL_RSV5, VFPU_CTRL_RSV6, VFPU_CTRL_REV, VFPU_CTRL_RCX0, VFPU_CTRL_RCX1,
    VFPU_CTRL_RCX2, VFPU_CTRL_RCX3, VFPU_CTRL_RCX4, VFPU_CTRL_RCX5, VFPU_CTRL_RCX6,
    VFPU_CTRL_RCX7, VFPU_CTRL_MAX,
};
inline int VFPUPrefixSwizzle(u32 prefix, int i) { return (prefix >> (i * 2)) & 3; }
inline bool VFPUPrefixAbs(u32 prefix, int i) { return (prefix >> (8 + i)) & 1; }
inline bool VFPUPrefixIsConstant(u32 prefix, int i) { return (prefix >> (12 + i)) & 1; }
inline bool VFPUPrefixNegate(u32 prefix, int i) { return (prefix >> (16 + i)) & 1; }
inline int VFPUPrefixSat(u32 prefix, int i) { return (prefix >> (i * 2)) & 3; }
inline u8 VFPUPrefixWriteMask(u32 prefix) { return (prefix >> 8) & 0xF; }
inline bool VFPUPrefixWriteMask(u32 prefix, int i) { return (prefix >> (8 + i)) & 1; }
extern const float vfpuPrefixConstants[8];
inline float VFPUPrefixConstant(u32 prefix, int i) {
    const float c = vfpuPrefixConstants[VFPUPrefixSwizzle(prefix, i) + (VFPUPrefixAbs(prefix, i) << 2)];
    return VFPUPrefixNegate(prefix, i) ? -c : c;
}
enum VCondition { VC_FL, VC_EQ, VC_LT, VC_LE, VC_TR, VC_NE, VC_GE, VC_GT, VC_EZ, VC_EN, VC_EI, VC_ES, VC_NZ, VC_NN, VC_NI, VC_NS };
extern u8 voffset[128];
extern u8 fromvoffset[128];
extern const float cst_constants[32];

// MIPSState: the port's Ctx (same field layout) plus the helper methods PPSSPP's code calls.
// It adds no data members, so a Ctx* can be viewed as a MIPSState*.
struct MIPSState : Ctx {
    u8 VfpuWriteMask() const { return VFPUPrefixWriteMask(vfpuCtrl[VFPU_CTRL_DPREFIX]); }
    bool VfpuWriteMask(int i) const { return VFPUPrefixWriteMask(vfpuCtrl[VFPU_CTRL_DPREFIX], i); }
    bool HasDefaultPrefix() const {
        return vfpuCtrl[VFPU_CTRL_SPREFIX] == 0xe4 && vfpuCtrl[VFPU_CTRL_TPREFIX] == 0xe4 && vfpuCtrl[VFPU_CTRL_DPREFIX] == 0;
    }
    bool IsMediaEngine() const { return false; }
};
inline float FlushForConversion(const MIPSState*, float f) { return f; }

// ---- guest memory as seen by the interpreter ---------------------------------------------
struct GuestAddressSpace {
    bool IsValid4AlignedAddress(u32 addr) const;
    u32 ReadUnchecked_U32(u32 addr) const;
    float ReadUnchecked_Float(u32 addr) const;
    void WriteUnchecked_U32(u32 v, u32 addr) const;
    void WriteUnchecked_Float(float v, u32 addr) const;
    u8* GetPointerUnchecked(u32 addr) const;
};
const GuestAddressSpace* GuestSpace();

// ---- from PPSSPP Core/MIPS/InterpreterVFPU.h -------------------------------------------
void EatPrefixes(MIPSState* mips);
namespace MIPSInt {
#define DECL(n) void n(MIPSState* mips, MIPSOpcode op);
DECL(Int_VPFX) DECL(Int_SVQ) DECL(Int_VMatrixInit) DECL(Int_VVectorInit) DECL(Int_Viim) DECL(Int_Vidt)
DECL(Int_Vmmul) DECL(Int_Vmscl) DECL(Int_Vmmov) DECL(Int_Vflush) DECL(Int_VV2Op) DECL(Int_Vocp)
DECL(Int_Vsocp) DECL(Int_Vsgn) DECL(Int_Vf2i) DECL(Int_Vi2f) DECL(Int_Vh2f) DECL(Int_Vf2h)
DECL(Int_Vx2i) DECL(Int_Vi2x) DECL(Int_ColorConv) DECL(Int_VDot) DECL(Int_VHdp) DECL(Int_Vbfy)
DECL(Int_Vsrt1) DECL(Int_Vsrt2) DECL(Int_Vsrt3) DECL(Int_Vsrt4) DECL(Int_Vcrs) DECL(Int_Vdet)
DECL(Int_Vfad) DECL(Int_Vavg) DECL(Int_VScl) DECL(Int_Vrnds) DECL(Int_VrndX) DECL(Int_Vrot)
DECL(Int_Vtfm) DECL(Int_SV) DECL(Int_Mftv) DECL(Int_Vmfvc) DECL(Int_Vmtvc) DECL(Int_Vcst)
DECL(Int_Vcmp) DECL(Int_Vminmax) DECL(Int_Vscmp) DECL(Int_Vsge) DECL(Int_Vslt) DECL(Int_Vcmov)
DECL(Int_VecDo3) DECL(Int_CrossQuat) DECL(Int_Vlgb) DECL(Int_Vwbn) DECL(Int_Vsbn) DECL(Int_Vsbz)
#undef DECL
}  // namespace MIPSInt
