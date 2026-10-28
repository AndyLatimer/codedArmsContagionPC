#pragma once
// High-level emulation (HLE) of the PSP kernel for recompiled code: module loading,
// threads and scheduling, synchronisation objects and the import dispatch table.
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "psp/recomp.h"

namespace psp {
class IsoReader;
}

namespace psp::kernel {

// PSP kernel error codes used by the HLE functions.
enum : uint32_t {
    ERR_ILLEGAL_PRIORITY = 0x80020193,
    ERR_UNKNOWN_THID = 0x80020198,
    ERR_UNKNOWN_SEMID = 0x80020199,
    ERR_UNKNOWN_EVFID = 0x8002019A,
    ERR_UNKNOWN_CBID = 0x80020065,
    ERR_WAIT_TIMEOUT = 0x800201A8,
    ERR_WAIT_CANCEL = 0x800201A9,
    ERR_WAIT_DELETE = 0x800201B5,
    ERR_SEMA_ZERO = 0x800201AD,
    ERR_SEMA_OVF = 0x800201AE,
    ERR_EVF_COND = 0x800201AF,
    ERR_EVF_MULTI = 0x800201B0,
    ERR_EVF_ILPAT = 0x800201B1,
    ERR_NO_MEMORY = 0x80020190,
    ERR_DORMANT = 0x800201A4,
    ERR_NOT_DORMANT = 0x800201A5,
};

struct BootOptions {
    std::string isoPath;
    bool traceHle = false;     // log every import call
};

// Loads BOOT.BIN from the image, relocates it, creates the main thread and runs the guest
// until it exits. Returns the guest exit status.
int boot(const BootOptions& opt);
void requestExit();

// Import dispatch -------------------------------------------------------------------
using HleFn = std::function<void(Ctx*)>;
void registerHle(const char* name, HleFn fn);
bool callImportByAddress(Ctx* c, uint32_t addr);

// Scheduling helpers for HLE modules (call with no kernel lock held) -----------------
uint64_t nowUs();                         // microseconds since boot (stops while paused)
// Pauses the game (front end menus): the guest clock stops and guest threads stop at their
// next system call until it is resumed.
void setPaused(bool paused);
int currentThreadId();
// Crash reports: the current guest thread and its last system call.
void describeCrash(FILE* out);
// Blocks the calling guest thread until `ready()` returns true or `timeoutUs` (0 = none)
// elapses. `ready` is evaluated under the kernel lock. Returns false on timeout.
bool waitUntil(const std::function<bool()>& ready, uint64_t timeoutUs, bool processCallbacks = false);
void delayCurrentThread(uint64_t us, bool processCallbacks = false);
// Wakes threads blocked in waitUntil so they re-check their condition (e.g. after an
// object changed state, or from a host thread such as audio/vblank).
void notifyStateChanged();
// HLE helper threads (PPSSPP HLEHelperThread): scheduled like guest threads, run host code.
int startHostThread(const std::string& name, int priority, std::function<void()> body);
bool threadStopped(int id);
void terminateThread(int id);
int currentThreadPriority();
void changeCurrentThreadPriority(int priority); // also reschedules (back of the new queue)
void eatUs(uint64_t us);                          // keep the CPU busy for `us`
void yieldIfHigherPriorityReady();

// Interrupts: guest handlers run on the calling thread at HLE boundaries.
void registerSubIntr(int intno, int subno, uint32_t handler, uint32_t arg);
void enableSubIntr(int intno, int subno, bool enable);
void raiseInterrupt(int intno, uint32_t arg2 = 0); // queue a guest interrupt
// Queues a call of guest function `addr`(a0, a1, a2) to run at the next HLE boundary.
void queueGuestCall(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2);
void runGuestFunction(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t* v0 = nullptr);

// Callbacks (sceKernelCreateCallback)
void notifyCallback(int cbid, uint32_t arg1, uint32_t arg2);
// Runs the calling thread's notified callbacks (the "CB" waits do this). Returns how many ran.
int runPendingCallbacks();

[[noreturn]] void fatal(Ctx* c, const char* fmt, ...);
void log(const char* fmt, ...);

IsoReader& iso();

// Argument helpers for HLE functions ---------------------------------------------------
inline uint32_t A(Ctx* c, int i) { return c->r[4 + i]; } // a0-a3, t0-t3
inline void ret(Ctx* c, uint32_t v) { c->r[2] = v; }
inline void ret64(Ctx* c, uint64_t v) { c->r[2] = (uint32_t)v; c->r[3] = (uint32_t)(v >> 32); }

} // namespace psp::kernel
