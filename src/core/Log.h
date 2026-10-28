#pragma once
#include <string>

namespace ca::log {

void init(const std::string& filePath);
void shutdown();
void info(const char* fmt, ...);
void warn(const char* fmt, ...);
void error(const char* fmt, ...);

} // namespace ca::log
