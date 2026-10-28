// Coded Arms PC port: stand-in for PPSSPP's Common/MemoryUtil.h (aligned allocation only).
#pragma once
#include <cstddef>
#include <cstdlib>
#ifdef _WIN32
#include <malloc.h>
inline void* AllocateAlignedMemory(size_t size, size_t alignment) { return _aligned_malloc(size ? size : 1, alignment); }
inline void FreeAlignedMemory(void* p) { _aligned_free(p); }
#else
inline void* AllocateAlignedMemory(size_t size, size_t alignment) {
    void* p = nullptr;
    return posix_memalign(&p, alignment, size ? size : 1) == 0 ? p : nullptr;
}
inline void FreeAlignedMemory(void* p) { std::free(p); }
#endif
