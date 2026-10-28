#include "core/Log.h"
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace ca::log {

static FILE* g_file = nullptr;

void init(const std::string& filePath) {
    if (!filePath.empty()) g_file = std::fopen(filePath.c_str(), "w");
}

void shutdown() {
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

static void write(const char* level, const char* fmt, va_list args) {
    char msg[2048];
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    std::fprintf(stdout, "[%s] %s\n", level, msg);
    std::fflush(stdout);
    if (g_file) {
        std::fprintf(g_file, "[%s] %s\n", level, msg);
        std::fflush(g_file);
    }
}

void info(const char* fmt, ...) { va_list a; va_start(a, fmt); write("INFO", fmt, a); va_end(a); }
void warn(const char* fmt, ...) { va_list a; va_start(a, fmt); write("WARN", fmt, a); va_end(a); }
void error(const char* fmt, ...) { va_list a; va_start(a, fmt); write("ERROR", fmt, a); va_end(a); }

} // namespace ca::log
