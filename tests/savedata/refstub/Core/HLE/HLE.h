// Test-only stand-in for PPSSPP's Core/HLE/HLE.h (only what sceChnnlsv.cpp uses).
#pragma once
#include "ppsspp_utility_compat.h"

template <typename T> inline T hleLogError(int, T v, const char* = nullptr) { return v; }
template <typename T> inline T hleLogDebug(int, T v, const char* = nullptr) { return v; }
template <typename T> inline T hleNoLog(T v) { return v; }

typedef void (*HLEFunc)();
struct HLEFunction {
    u32 ID;
    HLEFunc func;
    const char* name;
    char retmask;
    const char* argmask;
};
inline void RegisterHLEModule(const char*, int, const HLEFunction*) {}
