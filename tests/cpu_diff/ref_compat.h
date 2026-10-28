// Stand-ins for the PPSSPP headers used by ref_interp.cpp (reference interpreter for the
// differential CPU test). Memory goes through the port's guest memory (psp::g_mem).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "psp/memory.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

struct MIPSOpcode {
    u32 encoding;
    MIPSOpcode(u32 v = 0) : encoding(v) {}
    operator u32() const { return encoding; }
};

enum class MemoryExceptionType { NONE, UNKNOWN, READ_WORD, WRITE_WORD, READ_BLOCK, WRITE_BLOCK, ALIGNMENT };
enum class ExecExceptionType { JUMP, THREAD };
extern int g_refExceptions;
inline void Core_MemoryException(u32, u32, u32, MemoryExceptionType, const char* = "") { g_refExceptions++; }
inline void Core_ExecException(u32, u32, ExecExceptionType) { g_refExceptions++; }
inline void Core_BreakException(u32) { g_refExceptions++; }

#define _dbg_assert_msg_(c, ...) do {} while (0)
#define _dbg_assert_(c) do {} while (0)
#define ERROR_LOG(...) do {} while (0)
#define WARN_LOG(...) do {} while (0)
#define ERROR_LOG_REPORT(...) do {} while (0)
#define ERROR_LOG_REPORT_ONCE(...) do {} while (0)
#define DEBUG_LOG(...) do {} while (0)
#define INFO_LOG(...) do {} while (0)

struct RefSpace {
    bool IsValidAddress(u32 a) const { return psp::validAddress(a); }
    bool IsValid4AlignedAddress(u32 a) const { return (a & 3) == 0 && psp::validAddress(a); }
    bool IsValid2AlignedAddress(u32 a) const { return (a & 1) == 0 && psp::validAddress(a); }
    bool IsValidRange(u32 a, u32 n) const { return psp::validAddress(a) && psp::validAddress(a + n - 1); }
    u8 ReadUnchecked_U8(u32 a) const { return psp::read8(a); }
    u16 ReadUnchecked_U16(u32 a) const { return psp::read16(a); }
    u32 ReadUnchecked_U32(u32 a) const { return psp::read32(a); }
    float ReadUnchecked_Float(u32 a) const { u32 v = psp::read32(a); float f; std::memcpy(&f, &v, 4); return f; }
    void WriteUnchecked_U8(u8 v, u32 a) const { psp::write8(a, v); }
    void WriteUnchecked_U16(u16 v, u32 a) const { psp::write16(a, v); }
    void WriteUnchecked_U32(u32 v, u32 a) const { psp::write32(a, v); }
    void WriteUnchecked_Float(float v, u32 a) const { u32 u; std::memcpy(&u, &v, 4); psp::write32(a, u); }
    u8* GetPointerUnchecked(u32 a) const { return psp::hostPtr(a); }
};

namespace Log { enum { CPU = 0 }; }
#define WARN_LOG_REPORT(...) do {} while (0)
enum { FCR31_FLAG_DIV_BY_ZERO = 1 << 5, FCR31_ENABLE_DIV_BY_ZERO = 1 << 10, FCR31_CAUSE_DIV_BY_ZERO = 1 << 15 };
enum class FPUExceptionType { DIVIDE_BY_ZERO };
inline void Core_FPUException(u32, FPUExceptionType) { g_refExceptions++; }
struct RefConfig { bool bEnableFPUExceptionTraps = false; };
extern RefConfig g_Config;
struct MIPSState;
inline void ApplyHostRoundingMode(const MIPSState*) {}
inline u32 __rotr(u32 x, int s) { s &= 31; return s ? (x >> s) | (x << (32 - s)) : x; }
inline u32 ReverseBits32(u32 x) { u32 r = 0; for (int i = 0; i < 32; ++i) { r = (r << 1) | (x & 1); x >>= 1; } return r; }
// from PPSSPP Core/MIPS/MIPSCodeUtils.h
inline int AllegrexBitOp(MIPSOpcode op) {
    int sa = (op >> 6) & 31;
    if (!(sa & 0x10)) return 2 | (sa & 1);
    if (sa & 4) return 20;
    return (sa & 8) ? 24 : 16;
}
struct JitStub { void UpdateFCR31() {} };

struct MIPSState {
    static const u32 FCR0_VALUE = 0x00003351;
    JitStub* jit = nullptr;
    u32 r[32];
    union { float f[32]; u32 fi[32]; int fs[32]; };
    u32 vfpuCtrl[16];
    u32 pc, lo, hi, fcr31, fpcond;
    u32 nextPC = 0;
    bool inDelaySlot = false;
    int llBit = 0;
    int downcount = 0;
    u32 cop0[32];
    const RefSpace* space;
    bool IsMediaEngine() const { return false; }
    void InvalidateICacheRangeImmediate(u32, u32) {}
};

namespace Memory {
inline bool IsMMIOAccess(u32) { return false; }
inline bool MayRunInKernelMode(u32) { return false; }
}
inline u32 ReadMMIO(MIPSState*, u32, int) { return 0; }
inline void WriteMMIO(MIPSState*, u32, int, u32) {}

// from PPSSPP Common/Math/math_util.h and Common/Data/Convert/SmallDataConvert.h
inline bool my_isnan(float f) { return std::isnan(f); }
inline bool my_isinf(float f) { return std::isinf(f); }
inline bool my_isnanorinf(float f) { return !std::isfinite(f); }
inline u32 swap32(u32 x) { return __builtin_bswap32(x); }
inline s32 SignExtend8ToS32(u32 v) { return (s8)(v & 0xFF); }
inline u32 SignExtend8ToU32(u32 v) { return (u32)SignExtend8ToS32(v); }
inline s32 SignExtend16ToS32(u32 v) { return (s16)(v & 0xFFFF); }
inline u32 SignExtend16ToU32(u32 v) { return (u32)SignExtend16ToS32(v); }
inline int32_t SaturatedFloatToInt(double d) {
    if (d >= 2147483648.0) return 0x7FFFFFFF;
    if (d <= -2147483648.0) return (int32_t)0x80000000;
    if (d != d) return 0x7FFFFFFF;
    return (int32_t)d;
}
inline int is_even(float d) { float ip; modff(d / 2.0f, &ip); return 2.0f * ip == d; }
inline double round_ieee_754(double d) {
    float i = (float)floor(d); d -= i;
    if (d < 0.5f) return i;
    if (d > 0.5f) return i + 1.0f;
    if (is_even(i)) return i;
    return i + 1.0f;
}
inline int clz32(u32 v) { return v ? __builtin_clz(v) : 32; }
inline int clz32_nonzero(u32 v) { return __builtin_clz(v); }
inline float FlushForConversion(const MIPSState*, float f) { return f; }
inline u32 __builtin_bitreverse32_compat(u32 x) { u32 r = 0; for (int i = 0; i < 32; ++i) { r = (r << 1) | (x & 1); x >>= 1; } return r; }

namespace MIPSInt {
#define DECL(n) void n(MIPSState* mips, MIPSOpcode op);
DECL(Int_IType) DECL(Int_StoreSync) DECL(Int_RType3) DECL(Int_ITypeMem) DECL(Int_FPULS) DECL(Int_mxc1)
DECL(Int_RType2) DECL(Int_MulDivType) DECL(Int_ShiftType) DECL(Int_Allegrex) DECL(Int_Allegrex2)
DECL(Int_Special3) DECL(Int_FPU2op) DECL(Int_FPUComp) DECL(Int_FPU3op)
#undef DECL
}
