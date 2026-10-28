// Minimal stand-ins for the PPSSPP headers its SAS mixer (SasAudio.cpp, SasReverb.cpp,
// GPL-2.0-or-later) depends on, mapped onto this port's runtime. Kept separate from
// ppsspp_compat.h (VFPU), which brings in the CPU context.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
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

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

namespace PpssppAudioLog { enum { SasMix, sceSas, SaveState }; }
void ppsspp_audio_log(const char* fmt, ...);
#define PPSSPP_AUDIO_LOG(cat, ...) ppsspp_audio_log(__VA_ARGS__)
#ifndef ERROR_LOG
namespace Log = PpssppAudioLog;
#define ERROR_LOG(cat, ...) ppsspp_audio_log(__VA_ARGS__)
#define WARN_LOG(cat, ...) ppsspp_audio_log(__VA_ARGS__)
#define ERROR_LOG_REPORT(cat, ...) ppsspp_audio_log(__VA_ARGS__)
#define WARN_LOG_REPORT(cat, ...) ppsspp_audio_log(__VA_ARGS__)
#define WARN_LOG_REPORT_ONCE(id, cat, ...) do { static bool once_##id = false; if (!once_##id) { once_##id = true; ppsspp_audio_log(__VA_ARGS__); } } while (0)
#define VERBOSE_LOG(cat, ...) do {} while (0)
#define _dbg_assert_(cond) do {} while (0)
#endif

// from PPSSPP Core/Util/AudioFormat.h
static inline s16 clamp_s16(int i) {
    if (i > 32767) return 32767;
    if (i < -32768) return -32768;
    return i;
}

// The game never changes the CPU clock (scePowerSetClockFrequency is not imported), so the
// Media Engine runs at the default 222 MHz the timings were measured at.
inline int PowerScaleFromDefaultClock(int us) { return us; }

namespace Memory {
inline bool IsValidAddress(u32 a) { return psp::validAddress(a); }
inline bool IsValidRange(u32 a, u32 n) { return psp::validAddress(a) && (n == 0 || psp::validAddress(a + n - 1)); }
inline u8* GetPointerUnchecked(u32 a) { return psp::hostPtr(a); }
inline u8* GetPointerWriteRangeOrException(u32 a, u32 n) { return IsValidRange(a, n) ? psp::hostPtr(a) : nullptr; }
inline const u8* GetPointerRangeOrException(u32 a, u32 n) { return IsValidRange(a, n) ? psp::hostPtr(a) : nullptr; }
inline void Memcpy(void* dst, u32 src, u32 n, const char* = nullptr) {
    if (IsValidRange(src, n)) std::memcpy(dst, psp::hostPtr(src), n);
    else std::memset(dst, 0, n);
}
}
