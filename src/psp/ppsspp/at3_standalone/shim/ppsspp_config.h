// Coded Arms PC port: stand-in for PPSSPP's ppsspp_config.h (architecture macros only).
#pragma once
#if defined(__x86_64__) || defined(_M_X64)
#define PPSSPP_ARCH_AMD64 1
#define PPSSPP_ARCH_SSE2 1
#elif defined(__i386__) || defined(_M_IX86)
#define PPSSPP_ARCH_X86 1
#define PPSSPP_ARCH_SSE2 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define PPSSPP_ARCH_ARM64 1
#define PPSSPP_ARCH_ARM_NEON 1
#endif
#define PPSSPP_ARCH(x) (defined(PPSSPP_ARCH_##x) && PPSSPP_ARCH_##x)
