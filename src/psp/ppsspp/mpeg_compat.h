// Coded Arms PC port: minimal stand-ins for the PPSSPP headers used by its MPEG code
// (MediaEngine.cpp, MpegDemux.cpp, BufferQueue.h, Atrac3Standalone.cpp, GPL-2.0-or-later),
// mapped onto this port's runtime (guest memory in psp/memory.h, log in psp/kernel.h).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

#include "psp/memory.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
// The PSP and every host this port targets are little endian.
typedef uint16_t u16_le;
typedef uint32_t u32_le;
typedef int32_t s32_le;
typedef int64_t s64_le;
inline u32 bswap32(u32 v) { return __builtin_bswap32(v); }
inline u16 bswap16(u16 v) { return __builtin_bswap16(v); }
struct u16_be {
    u16 raw;
    operator u16() const { return bswap16(raw); }
};
struct s32_be {
    u32 raw;
    operator s32() const { return (s32)bswap32(raw); }
};

namespace psp::mpeg {
void logf(const char* fmt, ...);
bool verbose();
}
namespace Log { enum { ME = 1, Mpeg = 2, HLE = 3 }; }
#define ERROR_LOG(cat, ...) psp::mpeg::logf(__VA_ARGS__)
#define WARN_LOG(cat, ...) psp::mpeg::logf(__VA_ARGS__)
#define ERROR_LOG_REPORT(cat, ...) psp::mpeg::logf(__VA_ARGS__)
#define WARN_LOG_REPORT(cat, ...) psp::mpeg::logf(__VA_ARGS__)
#define INFO_LOG(cat, ...) psp::mpeg::logf(__VA_ARGS__)
#define DEBUG_LOG(cat, ...) do { if (psp::mpeg::verbose()) psp::mpeg::logf(__VA_ARGS__); } while (0)
#define VERBOSE_LOG(cat, ...) do {} while (0)
#define ONCE_LOG_(id, ...) do { static bool once_##id = false; if (!once_##id) { once_##id = true; psp::mpeg::logf(__VA_ARGS__); } } while (0)
#define ERROR_LOG_REPORT_ONCE(id, cat, ...) ONCE_LOG_(id, __VA_ARGS__)
#define WARN_LOG_REPORT_ONCE(id, cat, ...) ONCE_LOG_(id, __VA_ARGS__)
#define WARN_LOG_ONCE(id, cat, ...) ONCE_LOG_(id, __VA_ARGS__)
#define WARN_LOG_N_TIMES(id, n, cat, ...) do { static int cnt_##id = 0; if (cnt_##id++ < (n)) psp::mpeg::logf(__VA_ARGS__); } while (0)
#define _assert_(cond) do { if (!(cond)) psp::mpeg::logf("assert failed: %s (%s:%d)", #cond, __FILE__, __LINE__); } while (0)
#define _dbg_assert_(cond) _assert_(cond)

// GE pixel formats (PPSSPP GPU/ge_constants.h).
enum GEBufferFormat {
    GE_CMODE_16BIT_BGR5650 = 0,
    GE_CMODE_16BIT_ABGR5551 = 1,
    GE_CMODE_16BIT_ABGR4444 = 2,
    GE_CMODE_32BIT_ABGR8888 = 3,
};

// Core/MemMap.h subset over the port's guest memory.
namespace Memory {
inline bool IsValidAddress(u32 addr) { return psp::validAddress(addr); }
inline bool IsValidRange(u32 addr, u32 size) {
    return psp::validAddress(addr) && (size == 0 || psp::validAddress(addr + size - 1));
}
inline bool IsVRAMAddress(u32 addr) { return ((addr & 0x3F800000) == 0x04000000); }
inline u8* GetPointerWriteUnchecked(u32 addr) { return psp::hostPtr(addr); }
inline u8* GetPointerWriteRangeOrException(u32 addr, u32 size) {
    return IsValidRange(addr, size) ? psp::hostPtr(addr) : nullptr;
}
} // namespace Memory
