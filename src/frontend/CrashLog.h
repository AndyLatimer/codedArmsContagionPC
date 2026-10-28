#pragma once
// Log file and crash reports for the PC front end.
//
// On Windows the program has no console, so stderr (the runtime's log) goes to CodedArmsContagion.log
// next to the executable. If the program dies (an access violation or other exception, abort,
// a fatal signal), the reason, the host code address and the game's current thread and last
// system call are appended to the log and the player is told where to find it.
#include <string>

namespace ca {

// `dir` ends with a path separator (SDL_GetBasePath()).
void initCrashLog(const std::string& dir);

} // namespace ca
