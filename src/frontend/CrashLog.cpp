#include "frontend/CrashLog.h"

#include <SDL.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include "psp/kernel.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace ca {

namespace {

std::string g_logPath;

void report(const char* what) {
    std::fprintf(stderr, "[CRASH] %s\n", what);
    psp::kernel::describeCrash(stderr);
    std::fflush(stderr);
    const std::string msg = "Coded Arms se ha cerrado por un error.\n\nLos detalles estan en:\n" +
                            (g_logPath.empty() ? std::string("la salida de error") : g_logPath);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Coded Arms: Contagion", msg.c_str(), nullptr);
}

#ifdef _WIN32
LONG WINAPI onException(EXCEPTION_POINTERS* e) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_EXECUTE_HANDLER;
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    HMODULE module = GetModuleHandleW(nullptr);
    char b[256];
    std::snprintf(b, sizeof b, "exception %08lx at %p (CodedArmsContagion.exe+%#llx)%s", (unsigned long)r->ExceptionCode,
                  r->ExceptionAddress, (unsigned long long)((const char*)r->ExceptionAddress - (const char*)module),
                  r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2
                      ? (r->ExceptionInformation[0] ? ", writing" : ", reading")
                      : "");
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        std::fprintf(stderr, "[CRASH] invalid address %p\n", (void*)r->ExceptionInformation[1]);
    report(b);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void onSignal(int sig) {
    static volatile std::sig_atomic_t once = 0;
    if (once) std::_Exit(128 + sig);
    once = 1;
    char b[64];
    std::snprintf(b, sizeof b, "signal %d", sig);
    report(b);
    std::_Exit(128 + sig);
}

void onTerminate() {
    report("unhandled C++ exception (std::terminate)");
    std::_Exit(3);
}

} // namespace

void initCrashLog(const std::string& dir) {
#ifdef _WIN32
    g_logPath = dir + "CodedArmsContagion.log";
    if (std::freopen(g_logPath.c_str(), "w", stderr)) std::setvbuf(stderr, nullptr, _IONBF, 0);
    else g_logPath.clear();
    SetUnhandledExceptionFilter(onException);
#else
    (void)dir;
    std::signal(SIGSEGV, onSignal);
    std::signal(SIGBUS, onSignal);
    std::signal(SIGILL, onSignal);
    std::signal(SIGFPE, onSignal);
#endif
    std::signal(SIGABRT, onSignal);
    std::set_terminate(onTerminate);
}

} // namespace ca
