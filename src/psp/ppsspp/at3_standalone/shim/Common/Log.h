// Coded Arms PC port: stand-in for PPSSPP's Common/Log.h used by the standalone ATRAC3/3+
// decoder (compat.cpp). Errors and warnings go to the runtime log; the rest is dropped.
#pragma once
void ppsspp_log(const char* fmt, ...);
#define ERROR_LOG(cat, ...) ppsspp_log(__VA_ARGS__)
#define WARN_LOG(cat, ...) ppsspp_log(__VA_ARGS__)
#define INFO_LOG(cat, ...) do {} while (0)
#define DEBUG_LOG(cat, ...) do {} while (0)
namespace Log { enum { ME = 1 }; }
