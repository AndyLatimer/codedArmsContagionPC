// Coded Arms: Contagion PC port: stand-ins for the PPSSPP headers its ATRAC code
// (atrac/Core/HLE/AtracCtx.cpp, atrac/Core/Util/AtracTrack.cpp, GPL-2.0-or-later) includes,
// mapped onto this port's runtime. The PPSSPP files are unmodified copies; each header they
// include is a file under atrac/ that includes this one. Savestates are not supported, so
// PointerWrap and Do() are no-ops.
#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mpeg_compat.h"
#include "SimpleAudioDec.h"

namespace Log { enum { Atrac = 10, SaveState = 11 }; }

// Common/Swap.h
typedef uint16_t u16_le;
typedef int16_t s16_le;

// Common/StringUtils.h (renamed: vfpu.cpp has its own StringFromFormat)
#define StringFromFormat AtracStringFromFormat
inline std::string AtracStringFromFormat(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

// Core/MemMap.h (the parts mpeg_compat.h lacks)
namespace Memory {
inline u8* GetPointerWrite(u32 addr) { return psp::validAddress(addr) ? psp::hostPtr(addr) : nullptr; }
inline const u8* GetPointer(u32 addr) { return GetPointerWrite(addr); }
inline u8* GetPointerWriteOrException(u32 addr) { return GetPointerWrite(addr); }
inline const u8* GetPointerRange(u32 addr, u32 size) { return IsValidRange(addr, size) ? psp::hostPtr(addr) : nullptr; }
inline u32 Read_U32(u32 a) { return psp::read32(a); }
inline u16 Read_U16(u32 a) { return psp::read16(a); }
inline u8 Read_U8(u32 a) { return psp::read8(a); }
inline void Write_U32(u32 v, u32 a) { psp::write32(a, v); }
inline void Write_U16(u16 v, u32 a) { psp::write16(a, v); }
inline void Write_U8(u8 v, u32 a) { psp::write8(a, v); }
inline void Memcpy(u32 dst, const void* src, u32 size, const char* = nullptr, size_t = 0) { std::memcpy(psp::hostPtr(dst), src, size); }
inline void Memcpy(void* dst, u32 src, u32 size, const char* = nullptr, size_t = 0) { std::memcpy(dst, psp::hostPtr(src), size); }
inline void Memcpy(u32 dst, u32 src, u32 size, const char* = nullptr, size_t = 0) { std::memmove(psp::hostPtr(dst), psp::hostPtr(src), size); }
inline void Memset(u32 dst, u8 v, u32 size, const char* = nullptr, size_t = 0) { std::memset(psp::hostPtr(dst), v, size); }
// Bytes from addr to the end of its memory region.
inline u32 MaxSizeAtAddress(u32 addr) {
    u32 a = addr & psp::kAddrMask;
    if (a >= psp::kKernelRam && a < psp::kRamEnd) return psp::kRamEnd - a;
    if (a >= psp::kVram && a < psp::kVram + psp::kVramSize * 2) return psp::kVram + psp::kVramSize * 2 - a;
    if (a >= psp::kScratchpad && a < psp::kScratchpad + psp::kScratchpadSize) return psp::kScratchpad + psp::kScratchpadSize - a;
    return 0;
}
inline const u8* GetPointerOrException(u32 addr) { return GetPointerWrite(addr); }
inline const u8* GetPointerUnchecked(u32 addr) { return psp::hostPtr(addr); }
inline const u8* GetPointerOrNull(u32 addr) { return GetPointerWrite(addr); }
inline void WriteUnchecked_U32(u32 v, u32 a) { psp::write32(a, v); }
inline u32 ClampValidSizeAt(u32 addr, u32 size) { u32 m = MaxSizeAtAddress(addr); return size < m ? size : m; }
inline u32 GetAddressFromHostPointer(const void* p) { return (u32)((const u8*)p - psp::g_mem); }
} // namespace Memory

// Core/MemMapHelpers.h / Core/Debugger/MemBlockInfo.h
enum class MemBlockFlags { ALLOC = 1, SUB_ALLOC = 2, WRITE = 4, TEXTURE = 8, READ = 16, FREE = 32, SUB_FREE = 64, SKIP_MEMCHECK = 128 };
inline void NotifyMemInfo(MemBlockFlags, u32, u32, const char*, size_t = 0) {}

template <class T>
struct PSPPointer {
    u32 ptr = 0;
    operator T*() const { return ptr ? reinterpret_cast<T*>(psp::hostPtr(ptr)) : nullptr; }
    PSPPointer& operator=(u32 p) { ptr = p; return *this; }
    T* operator->() const { return reinterpret_cast<T*>(psp::hostPtr(ptr)); }
    T& operator*() const { return *reinterpret_cast<T*>(psp::hostPtr(ptr)); }
    bool IsValid() const { return ptr != 0 && psp::validAddress(ptr); }
    static PSPPointer<T> Create(u32 p) { PSPPointer<T> r; r.ptr = p; return r; }
};

// Common/Serialize: no savestates in this port.
class PointerWrap {
public:
    enum Mode { MODE_READ = 1, MODE_WRITE, MODE_MEASURE, MODE_VERIFY, MODE_NOOP };
    Mode mode = MODE_NOOP;
    int SError = 0;
    bool error = false;
    struct Section { explicit operator bool() const { return false; } int operator<(int) const { return 0; } };
    Section Section_(const char*, int, int) { return {}; }
};
template <class... T> inline void Do(PointerWrap&, T&...) {}
template <class T> inline void DoArray(PointerWrap&, T*, int) {}
template <class T> inline void DoClass(PointerWrap&, T*&) {}
struct PointerWrapSection { int v = 0; explicit operator bool() const { return false; } bool operator<(int) const { return true; } };
inline PointerWrapSection PointerWrapSectionStub(PointerWrap&, const char*, int, int = 1) { return {}; }

// Core/HLE/sceKernelMemory.h: contexts in guest memory are only used by AtracCtx2 / the
// _sceAtracGetContextAddress call, which Contagion does not import.
struct AtracKernelMemoryStub {
    u32 Alloc(u32&, bool, const char*) { return 0; }
    void Free(u32) {}
};
extern AtracKernelMemoryStub kernelMemory;

// Core/HW/Atrac3Standalone.h
AudioDecoder* CreateAtrac3Audio(int channels, size_t blockAlign, const uint8_t* extraData, size_t extraDataSize);
AudioDecoder* CreateAtrac3PlusAudio(int channels, size_t blockAlign);

// Common/File/FileUtil.h / Core/Util dumping: never enabled in this port.
enum class DumpFileType { EBOOT, PRX, Atrac3 };
inline void DumpFileIfEnabled(const u8*, u32, const std::string&, DumpFileType) {}

// Core/Reporting.h
namespace Reporting { inline bool ShouldLogNTimes(const char*, int) { return false; } }
#define ERROR_LOG_REPORT_ONCE(id, cat, ...) ONCE_LOG_(id, __VA_ARGS__)

// Core/System.h: no per-game compatibility flags.
struct AtracCompatFlags { bool AtracLoopHack = false; };
struct AtracCompat { AtracCompatFlags f; const AtracCompatFlags& flags() const { return f; } };
struct AtracCoreParameter { AtracCompat compat; };
inline AtracCoreParameter& PSP_CoreParameter() { static AtracCoreParameter p; return p; }
inline bool MemBlockInfoDetailed() { return false; }
inline size_t FormatMemWriteTagAt(char* buf, size_t, const char*, u32, u32) { buf[0] = 0; return 0; }
inline void DataToHexString(const u8* data, size_t size, std::string* out, bool = true) {
    char b[4];
    out->clear();
    for (size_t i = 0; i < size; ++i) { std::snprintf(b, sizeof b, "%02x ", data[i]); *out += b; }
}
#define _dbg_assert_msg_(cond, ...) do {} while (0)
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
