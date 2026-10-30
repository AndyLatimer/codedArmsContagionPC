// PSP kernel HLE: module loader, threads/scheduler, synchronisation objects, memory,
// interrupts and the import dispatch table.
//
// Threading model: every guest thread is a host thread, but exactly one of them runs
// guest code at a time (g_current). Context switches happen only inside HLE calls, which
// matches the PSP kernel (no time slicing between threads of equal priority; a thread
// runs until it blocks or a higher-priority thread is woken).
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif
#include "psp/kernel.h"
#include "psp/gpu/gpu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "psp/iso.h"
#include "psp/memory.h"

namespace psp::kernel {

// ---- logging --------------------------------------------------------------------------
static bool g_trace = false;

void log(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::fprintf(stderr, "[HLE] ");
    std::vfprintf(stderr, fmt, a);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    va_end(a);
}

void fatal(Ctx* c, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::fprintf(stderr, "[FATAL] ");
    std::vfprintf(stderr, fmt, a);
    std::fprintf(stderr, "\n");
    va_end(a);
    if (c) {
        std::fprintf(stderr, "  ra=%08x sp=%08x v0=%08x a0=%08x a1=%08x a2=%08x a3=%08x\n", c->r[31], c->r[29],
                     c->r[2], c->r[4], c->r[5], c->r[6], c->r[7]);
    }
    std::fflush(stderr);
    std::_Exit(3);
}

// ---- time ------------------------------------------------------------------------------
static std::chrono::steady_clock::time_point g_bootTime;
static uint64_t hostUs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_bootTime).count();
}
// Pause (front end menus): the guest clock stops and every guest thread stops at its next system
// call, so the game sees no time pass.
static std::atomic<uint64_t> g_pausedAt{0};    // host time the pause began (0: running)
static std::atomic<uint64_t> g_pausedTotal{0}; // host time spent paused before
static std::mutex g_pauseMutex;
static std::condition_variable g_pauseCv;
uint64_t nowUs() {
    const uint64_t at = g_pausedAt.load();
    return (at ? at : hostUs()) - g_pausedTotal.load();
}
uint64_t wallClockUs() {
    static const uint64_t epochAtBoot = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::system_clock::now().time_since_epoch()).count() - nowUs();
    return epochAtBoot + nowUs();
}
void setPaused(bool paused) {
    std::lock_guard<std::mutex> lk(g_pauseMutex);
    const uint64_t at = g_pausedAt.load();
    if (paused && !at) g_pausedAt = std::max<uint64_t>(hostUs(), 1);
    if (!paused && at) {
        g_pausedTotal += hostUs() - at;
        g_pausedAt = 0;
        g_pauseCv.notify_all();
    }
}
static void waitWhilePaused() {
    std::unique_lock<std::mutex> lk(g_pauseMutex);
    g_pauseCv.wait(lk, [] { return g_pausedAt.load() == 0; });
}

// ---- threads ---------------------------------------------------------------------------
enum class TState { Dormant, Ready, Running, Waiting, Dead };

struct Thread {
    int id = 0;
    std::string name;
    uint32_t entry = 0, attr = 0, stackSize = 0, stackAddr = 0, gp = 0;
    int stackBlock = 0;
    int priority = 0x20;
    TState state = TState::Dormant;
    uint64_t readySeq = 0;
    Ctx ctx{};
    std::condition_variable cv;
    std::thread host;
    // wait state
    std::function<bool()> waitCond;
    uint64_t deadline = 0; // 0 = none
    bool timedOut = false;
    bool processCallbacks = false;
    int exitStatus = 0;
    int wakeupCount = 0;
    std::function<void()> hostBody; // HLE helper thread: runs host code instead of guest code
    int lastImport = -1;            // debugging (dumpThreads): its last system call
    uint64_t hleCalls = 0, hleCallsDumped = 0; // debugging: system calls made (since the last dump)
    uint32_t lastImportRa = 0;
};

static std::mutex g_km;
static std::condition_variable g_idleCv;
static std::map<int, std::unique_ptr<Thread>> g_threads;
static Thread* g_current = nullptr;
static thread_local Thread* t_self = nullptr;
static int g_nextUid = 0x1000;
static uint64_t g_readySeq = 0;
static std::atomic<bool> g_exit{false};
static int g_exitStatus = 0;
static uint32_t g_gp = 0;
static IsoReader g_iso;
void installTextPatches(IsoReader& iso); // game_hooks.cpp

IsoReader& iso() { return g_iso; }

uint32_t allocHleMemory(uint32_t size, const char* tag) {
    std::lock_guard<std::mutex> lk(g_km);
    int id = 0;
    uint32_t a = g_userPartition.alloc(size, 1, 0, 256, &id);
    log("HLE memory '%s': %u bytes at %08x", tag, size, a);
    if (a) std::memset(hostPtr(a), 0, size);
    return a;
}
int newUid() { return g_nextUid++; }

static void makeReady(Thread* t) {
    t->state = TState::Ready;
    t->readySeq = ++g_readySeq;
}

static Thread* pickNext() {
    Thread* best = nullptr;
    for (auto& [id, t] : g_threads)
        if (t->state == TState::Ready && (!best || t->priority < best->priority ||
                                          (t->priority == best->priority && t->readySeq < best->readySeq)))
            best = t.get();
    return best;
}

// Re-evaluates the conditions of waiting threads and expires timeouts. Caller holds g_km.
static void reevaluateWaits() {
    uint64_t now = nowUs();
    for (auto& [id, t] : g_threads) {
        if (t->state != TState::Waiting) continue;
        if (t->waitCond && t->waitCond()) {
            t->waitCond = nullptr;
            t->timedOut = false;
            makeReady(t.get());
        } else if (t->deadline && now >= t->deadline) {
            t->waitCond = nullptr;
            t->timedOut = true;
            makeReady(t.get());
        }
    }
}

static uint64_t earliestDeadline() {
    uint64_t best = 0;
    for (auto& [id, t] : g_threads)
        if (t->state == TState::Waiting && t->deadline && (!best || t->deadline < best)) best = t->deadline;
    return best;
}

static void deliverInterrupts(std::unique_lock<std::mutex>& lk);

// Waits on g_idleCv until `until` (guest time) or a notification. The game's threads sleep for
// fractions of a millisecond (the sound mixer after each grain), but timed waits end on the
// system timer's ticks: 1 ms with timeBeginPeriod(1) on Windows, 15.6 ms without. Late wakeups
// made the sound mixer fall behind real time and the game wait for it forever on a Windows PC,
// so the last 2 ms of a wait are spent yielding the CPU instead of sleeping.
// CA_SIM_WINTIMER=<tick us> (testing): timed waits end on the next tick, like Windows.
static void idleWaitUntil(std::unique_lock<std::mutex>& lk, uint64_t until) {
    static const uint64_t simTick = std::getenv("CA_SIM_WINTIMER") ? std::strtoull(std::getenv("CA_SIM_WINTIMER"), nullptr, 10) : 0;
    static const bool noSpin = std::getenv("CA_NO_IDLE_SPIN") != nullptr; // testing: the old behaviour
    constexpr uint64_t kSpinUs = 2000;
    const uint64_t now = nowUs();
    if (until <= now) return;
    if (noSpin || until - now > kSpinUs) {
        uint64_t sleepUntil = noSpin ? until : until - kSpinUs;
        if (simTick) { // host time of the next tick at or after the deadline
            const uint64_t h = hostUs() + (sleepUntil - now);
            sleepUntil = now + ((h + simTick - 1) / simTick * simTick - hostUs());
        }
        g_idleCv.wait_until(lk, g_bootTime + std::chrono::microseconds(sleepUntil + (hostUs() - now)));
        return;
    }
    lk.unlock();
    std::this_thread::yield();
    lk.lock();
}

// Gives the CPU away. `self` must already be in its new state (Ready/Waiting/Dead).
// Returns when `self` is scheduled again (or immediately when it is dead).
static void schedule(std::unique_lock<std::mutex>& lk, Thread* self) {
    for (;;) {
        if (g_exit) {
            if (self && self->state != TState::Dead) { // park forever: the process is exiting
                g_current = nullptr;
                g_idleCv.notify_all();
                self->cv.wait(lk, [] { return false; });
            }
            return;
        }
        reevaluateWaits();
        Thread* next = pickNext();
        if (next) {
            g_current = next;
            next->state = TState::Running;
            if (next == self) return;
            next->cv.notify_one();
            if (!self || self->state == TState::Dead) return;
            self->cv.wait(lk, [self] { return (g_current == self && self->state == TState::Running) || g_exit; });
            if (g_exit) continue;
            return;
        }
        // Nothing is ready: idle on this host thread until a deadline or an external event.
        g_current = nullptr;
        deliverInterrupts(lk);
        if (pickNext()) continue;
        uint64_t dl = earliestDeadline();
        uint64_t vbl = nowUs() + 16683; // wake at least once per frame to deliver vblank
        uint64_t until = dl ? std::min(dl, vbl) : vbl;
        idleWaitUntil(lk, until);
    }
}

int currentThreadId() { return t_self ? t_self->id : 0; }
extern "C" int ca_trace_thread_id() { return currentThreadId(); }

bool waitUntil(const std::function<bool()>& ready, uint64_t timeoutUs, bool processCallbacks) {
    std::unique_lock<std::mutex> lk(g_km);
    Thread* self = t_self;
    if (ready && ready()) return true;
    self->waitCond = ready;
    self->deadline = timeoutUs ? nowUs() + timeoutUs : 0;
    self->timedOut = false;
    self->processCallbacks = processCallbacks;
    self->state = TState::Waiting;
    schedule(lk, self);
    return !self->timedOut;
}

void delayCurrentThread(uint64_t us, bool processCallbacks) {
    // A delay is a wait with no condition: only the deadline ends it.
    std::unique_lock<std::mutex> lk(g_km);
    Thread* self = t_self;
    self->waitCond = nullptr;
    self->deadline = nowUs() + std::max<uint64_t>(us, 1);
    self->processCallbacks = processCallbacks;
    self->state = TState::Waiting;
    schedule(lk, self);
}

void notifyStateChanged() {
    std::lock_guard<std::mutex> lk(g_km);
    reevaluateWaits();
    g_idleCv.notify_all();
}

void yieldIfHigherPriorityReady() {
    std::unique_lock<std::mutex> lk(g_km);
    Thread* self = t_self;
    if (!self) return;
    reevaluateWaits();
    Thread* n = pickNext();
    if (n && n->priority < self->priority) {
        makeReady(self);
        schedule(lk, self);
    }
}

// ---- guest calls from the host (interrupts, callbacks) ----------------------------------
static uint32_t g_intrStackTop = 0;

static uint32_t g_intrSp = 0; // next free interrupt-stack address (nested calls go below)

void runGuestFunction(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t* v0) {
    Ctx c{};
    c.mem = g_mem;
    c.vfpuCtrl[0] = 0xe4; c.vfpuCtrl[1] = 0xe4; c.vfpuCtrl[3] = 0x3f;
    c.r[4] = a0; c.r[5] = a1; c.r[6] = a2; c.r[7] = a3;
    c.r[28] = g_gp;
    uint32_t saved = g_intrSp;
    uint32_t sp = saved ? saved : g_intrStackTop;
    c.r[29] = sp - 16;
    g_intrSp = sp - 0x4000; // 16 KB per nesting level
    call_indirect(&c, addr);
    g_intrSp = saved;
    if (v0) *v0 = c.r[2];
}

struct SubIntr {
    uint32_t handler = 0, arg = 0;
    bool enabled = false;
};
static std::map<std::pair<int, int>, SubIntr> g_subIntrs;
static uint64_t g_lastVblank = 0;
static std::deque<std::pair<int, uint32_t>> g_pendingIntr;
struct QueuedCall { uint32_t addr, a0, a1, a2; };
static std::deque<QueuedCall> g_queuedCalls;
static std::atomic<bool> g_inInterrupt{false};

void queueGuestCall(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2) {
    std::lock_guard<std::mutex> lk(g_km);
    g_queuedCalls.push_back({addr, a0, a1, a2});
    g_idleCv.notify_all();
}

void registerSubIntr(int intno, int subno, uint32_t handler, uint32_t arg) {
    std::lock_guard<std::mutex> lk(g_km);
    g_subIntrs[{intno, subno}] = {handler, arg, false};
}
void enableSubIntr(int intno, int subno, bool enable) {
    std::lock_guard<std::mutex> lk(g_km);
    g_subIntrs[{intno, subno}].enabled = enable;
}
void raiseInterrupt(int intno, uint32_t arg2) {
    std::lock_guard<std::mutex> lk(g_km);
    g_pendingIntr.push_back({intno, arg2});
    g_idleCv.notify_all();
}

static constexpr int kIntrVblank = 30;

// Runs pending interrupt handlers. Called with g_km held; releases it while guest code runs.
static void deliverInterrupts(std::unique_lock<std::mutex>& lk) {
    if (g_inInterrupt) return;
    uint64_t vcount = nowUs() * 60000ull / 1001000000ull; // 59.94 Hz
    if (vcount > g_lastVblank) {
        g_lastVblank = vcount;
        g_pendingIntr.push_back({kIntrVblank, 0});
        if (gpu::installed()) {
            lk.unlock();
            gpu::vblank();
            lk.lock();
        }
    }
    while (!g_queuedCalls.empty()) {
        QueuedCall qc = g_queuedCalls.front();
        g_queuedCalls.pop_front();
        Thread* prevCurrent = g_current;
        g_inInterrupt = true;
        lk.unlock();
        runGuestFunction(qc.addr, qc.a0, qc.a1, qc.a2, 0);
        lk.lock();
        g_inInterrupt = false;
        g_current = prevCurrent;
    }
    while (!g_pendingIntr.empty()) {
        auto [intno, arg2] = g_pendingIntr.front();
        g_pendingIntr.pop_front();
        std::vector<SubIntr> handlers;
        for (auto& [key, s] : g_subIntrs)
            if (key.first == intno && s.enabled && s.handler) handlers.push_back(s);
        if (handlers.empty()) continue;
        Thread* prevCurrent = g_current;
        g_inInterrupt = true;
        lk.unlock();
        for (auto& [key, s] : g_subIntrs) {
            if (key.first != intno || !s.enabled || !s.handler) continue;
            runGuestFunction(s.handler, (uint32_t)key.second, s.arg, arg2, 0);
        }
        lk.lock();
        g_inInterrupt = false;
        g_current = prevCurrent;
    }
    reevaluateWaits();
}

// ---- callbacks -------------------------------------------------------------------------
struct Callback {
    std::string name;
    uint32_t func = 0, arg = 0;
    int notifyCount = 0;
    uint32_t notifyArg = 0;
    int ownerThread = 0;
};
static std::map<int, Callback> g_callbacks;

void notifyCallback(int cbid, uint32_t arg1, uint32_t arg2) {
    std::lock_guard<std::mutex> lk(g_km);
    auto it = g_callbacks.find(cbid);
    if (it == g_callbacks.end()) return;
    it->second.notifyCount++;
    it->second.notifyArg = arg2;
    (void)arg1;
}

// Runs notified callbacks owned by the current thread. Returns how many ran.
static int runCallbacks() {
    int ran = 0;
    for (;;) {
        Callback cb;
        int id = 0;
        {
            std::lock_guard<std::mutex> lk(g_km);
            for (auto& [cid, c] : g_callbacks)
                if (c.notifyCount > 0 && c.ownerThread == currentThreadId()) {
                    id = cid;
                    cb = c;
                    c.notifyCount = 0;
                    break;
                }
        }
        if (!id) return ran;
        uint32_t ret = 0;
        if (g_trace) log("callback %d '%s' -> %08x(count=%d, arg=%08x)", id, cb.name.c_str(), cb.func, cb.notifyCount, cb.notifyArg);
        runGuestFunction(cb.func, (uint32_t)cb.notifyCount, cb.notifyArg, cb.arg, 0, &ret);
        ++ran;
        if (ret) {
            std::lock_guard<std::mutex> lk(g_km);
            g_callbacks.erase(id);
        }
    }
}

int runPendingCallbacks() { return runCallbacks(); }

// ---- HLE dispatch ----------------------------------------------------------------------
static std::unordered_map<std::string, HleFn>& registry() {
    static std::unordered_map<std::string, HleFn> r;
    return r;
}
void registerHle(const char* name, HleFn fn) { registry()[name] = std::move(fn); }

struct ImportSlot {
    std::string name;
    HleFn fn;
    bool warned = false;
};
static std::vector<ImportSlot> g_imports;
static std::unordered_map<uint32_t, int> g_importByStub;

const char* nidName(uint32_t nid); // hle_nids.cpp

static void bindImports() {
    g_imports.resize(g_hle_import_count);
    for (unsigned i = 0; i < g_hle_import_count; ++i) {
        const HleImport& im = g_hle_imports[i];
        const char* n = nidName(im.nid);
        g_imports[i].name = n ? n : (std::string(im.lib) + "_" + std::to_string(im.nid));
        auto it = registry().find(g_imports[i].name);
        if (it != registry().end()) g_imports[i].fn = it->second;
        g_importByStub[im.stub] = (int)i;
    }
    int bound = 0;
    for (auto& s : g_imports) bound += s.fn ? 1 : 0;
    log("imports bound: %d / %u", bound, g_hle_import_count);
}

static void afterHle(Ctx* c) {
    (void)c;
    if (g_inInterrupt) return;
    if (g_exit) {
        std::unique_lock<std::mutex> lk(g_km);
        schedule(lk, t_self);
    }
    {
        std::unique_lock<std::mutex> lk(g_km);
        deliverInterrupts(lk);
    }
    yieldIfHigherPriorityReady();
}

// Debugging: CA_HLE_STATS prints the most frequent (import, caller) pairs every second
// (CA_HLE_STATS=thread: (import, thread id) pairs).
static void hleStats(const std::string& name, uint32_t ra) {
    static std::mutex m;
    static std::map<std::pair<std::string, uint32_t>, int> counts;
    static uint64_t last = 0;
    std::lock_guard<std::mutex> lk(m);
    counts[{name, ra}]++;
    uint64_t now = nowUs();
    if (now - last < 1000000) return;
    last = now;
    std::vector<std::pair<int, std::pair<std::string, uint32_t>>> v;
    for (auto& [k, n] : counts) v.push_back({n, k});
    std::sort(v.rbegin(), v.rend());
    std::string out = "[hle]";
    for (size_t i = 0; i < v.size() && i < 12; ++i) {
        char b[96];
        std::snprintf(b, sizeof b, " %s@%08x=%d", v[i].second.first.c_str(), v[i].second.second, v[i].first);
        out += b;
    }
    log("%s", out.c_str());
    counts.clear();
}

// The last system call of each thread, for crash reports (describeCrash).
static thread_local int t_lastImport = -1;
static thread_local uint32_t t_lastImportRa = 0;

void describeCrash(FILE* out) {
    if (t_self) std::fprintf(out, "[CRASH] guest thread %d '%s'\n", t_self->id, t_self->name.c_str());
    else std::fprintf(out, "[CRASH] not a guest thread\n");
    if (t_lastImport >= 0 && (size_t)t_lastImport < g_imports.size())
        std::fprintf(out, "[CRASH] its last system call: %s, returning to %08x\n", g_imports[(size_t)t_lastImport].name.c_str(),
                     t_lastImportRa);
}

extern "C" void hle_call(Ctx* c, int idx) {
    ImportSlot& s = g_imports[(size_t)idx];
    t_lastImport = idx;
    t_lastImportRa = c->r[31];
    if (t_self) { t_self->lastImport = idx; t_self->lastImportRa = c->r[31]; ++t_self->hleCalls; }
    static const bool stats = std::getenv("CA_HLE_STATS") != nullptr;
    static const bool byThread = stats && std::getenv("CA_HLE_STATS")[0] == 't';
    if (stats) hleStats(s.name, byThread ? (uint32_t)currentThreadId() : c->r[31]);
    if (g_pausedAt.load()) waitWhilePaused();
    if (g_trace)
        log("%s(%08x, %08x, %08x, %08x) thread=%d ra=%08x", s.name.c_str(), c->r[4], c->r[5], c->r[6], c->r[7], currentThreadId(), c->r[31]);
    if (s.fn) {
        s.fn(c);
        if (g_trace) log("  %s -> %08x", s.name.c_str(), c->r[2]);
    } else {
        if (!s.warned) {
            log("UNIMPLEMENTED %s (a0=%08x a1=%08x) -> 0", s.name.c_str(), c->r[4], c->r[5]);
            s.warned = true;
        }
        c->r[2] = 0;
    }
    afterHle(c);
}

static void dumpEventFlags(); // caller holds g_km

// Debugging (CA_THREAD_DUMP): every guest thread, its state and last system call.
void dumpThreads() {
    static const char* kState[] = {"dormant", "ready", "running", "waiting", "dead"};
    std::lock_guard<std::mutex> lk(g_km);
    const uint64_t now = nowUs(), me = meBusyUntilUs();
    log("[threads] guest time %.3fs, Media Engine busy for %.3f ms more, current %d", now / 1e6,
        me > now ? (me - now) / 1e3 : 0.0, g_current ? g_current->id : 0);
    for (auto& [id, t] : g_threads) {
        const char* imp = t->lastImport >= 0 && (size_t)t->lastImport < g_imports.size() ? g_imports[(size_t)t->lastImport].name.c_str() : "-";
        char wait[96] = "";
        if (t->state == TState::Waiting)
            std::snprintf(wait, sizeof wait, ", %s, deadline %s%.3f ms", t->waitCond ? "condition" : "no condition",
                          !t->deadline ? "none " : (t->deadline >= now ? "in " : "passed "),
                          !t->deadline ? 0.0 : (t->deadline >= now ? (t->deadline - now) / 1e3 : (now - t->deadline) / 1e3));
        log("[threads] %d '%s' prio %x %s, last call %s (ra %08x), %llu calls since last dump%s", id, t->name.c_str(), t->priority,
            kState[(int)t->state], imp, t->lastImportRa, (unsigned long long)(t->hleCalls - t->hleCallsDumped), wait);
        t->hleCallsDumped = t->hleCalls;
    }
    dumpEventFlags();
}

// ---- preemption -------------------------------------------------------------------------
extern "C" volatile int rt_preempt_flag = 0;

extern "C" void rt_preempt(Ctx* c) {
    rt_preempt_flag = 0;
    if (!t_self || g_inInterrupt) return;
    afterHle(c); // interrupts, then the CPU to a higher-priority ready thread
}

// Host timer: requests preemption of the running guest thread when a waiting thread of higher
// priority can run (its deadline passed or its condition holds), and once per frame so that
// interrupts are delivered to threads that spin without system calls.
static void preemptionTimer() {
    uint64_t lastFrame = 0;
    while (!g_exit) {
#ifdef _WIN32
        // MinGW's sleep_for rounds sub-millisecond sleeps down to no sleep at all: this thread
        // would spin, taking g_km thousands of times per millisecond from the running game.
        // 1 ms is precise with timeBeginPeriod(1) (boot).
        Sleep(1);
#else
        std::this_thread::sleep_for(std::chrono::microseconds(500));
#endif
        std::lock_guard<std::mutex> lk(g_km);
        Thread* cur = g_current;
        if (!cur || cur->state != TState::Running) continue;
        uint64_t now = nowUs();
        bool want = now - lastFrame >= 16683;
        if (want) lastFrame = now;
        // Wait conditions may consume what they wait for (a semaphore count), so they are only
        // evaluated by reevaluateWaits, which readies the thread when one holds.
        reevaluateWaits();
        for (auto& [id, t] : g_threads)
            if (t->state == TState::Ready && t->priority < cur->priority) want = true;
        if (want) rt_preempt_flag = 1;
    }
}

bool callImportByAddress(Ctx* c, uint32_t addr) {
    auto it = g_importByStub.find(addr);
    if (it == g_importByStub.end()) return false;
    hle_call(c, it->second);
    return true;
}

// ---- thread host body -----------------------------------------------------------------
static void exitThread(Thread* t, int status, bool deleteIt);

static void threadMain(Thread* t) {
    t_self = t;
    {
        std::unique_lock<std::mutex> lk(g_km);
        t->cv.wait(lk, [t] { return (g_current == t && t->state == TState::Running) || g_exit; });
        if (g_exit) return;
    }
    if (t->hostBody) {
        t->hostBody();
        exitThread(t, 0, false);
        return;
    }
    call_indirect(&t->ctx, t->entry);
    exitThread(t, (int)t->ctx.r[2], false);
}

static void exitThread(Thread* t, int status, bool deleteIt) {
    std::unique_lock<std::mutex> lk(g_km);
    (void)deleteIt; // the host thread ends either way; the id stays valid for WaitThreadEnd
    t->exitStatus = status;
    t->state = TState::Dead;
    log("thread %d '%s' exited (%d)", t->id, t->name.c_str(), status);
    g_idleCv.notify_all();
    schedule(lk, t);
}

// The calling host thread will never run guest code again (its guest thread has ended).
[[noreturn]] static void parkForever() {
    std::unique_lock<std::mutex> lk(g_km);
    t_self->cv.wait(lk, [] { return false; });
    std::abort();
}


static Thread* findThread(int id) {
    if (id == 0) return t_self;
    auto it = g_threads.find(id);
    return it == g_threads.end() ? nullptr : it->second.get();
}

static Thread* createThread(const std::string& name, uint32_t entry, int prio, uint32_t stackSize, uint32_t attr) {
    auto t = std::make_unique<Thread>();
    t->id = newUid();
    t->name = name;
    t->entry = entry;
    t->priority = prio;
    t->attr = attr;
    t->stackSize = (std::max<uint32_t>(stackSize, 0x200) + 0xFF) & ~0xFFu;
    t->stackAddr = g_userPartition.alloc(t->stackSize, 1, 0, 256, &t->stackBlock);
    if (!t->stackAddr) return nullptr;
    std::memset(hostPtr(t->stackAddr), 0xFF, t->stackSize);
    t->gp = g_gp;
    Thread* raw = t.get();
    g_threads[raw->id] = std::move(t);
    return raw;
}

static void startThread(Thread* t, uint32_t argSize, uint32_t argp) {
    Ctx& c = t->ctx;
    std::memset(&c, 0, sizeof(c));
    c.mem = g_mem;
    c.vfpuCtrl[0] = 0xe4; c.vfpuCtrl[1] = 0xe4; c.vfpuCtrl[2] = 0; c.vfpuCtrl[3] = 0x3f;
    uint32_t sp = t->stackAddr + t->stackSize - 64;
    if (argp && argSize) {
        sp -= (argSize + 15) & ~15u;
        std::memcpy(hostPtr(sp), hostPtr(argp), argSize);
        c.r[5] = sp;
    } else {
        c.r[5] = 0;
    }
    c.r[4] = argSize;
    c.r[28] = t->gp;
    c.r[29] = sp - 64;
    c.r[31] = 0;
    c.thread = t;
    makeReady(t);
    t->host = std::thread(threadMain, t);
    t->host.detach();
}

// ---- HLE helper threads -----------------------------------------------------------------
// Like PPSSPP's HLEHelperThread: a kernel thread (attr KERNEL, so no user-memory stack) that is
// scheduled like any guest thread but runs host code. Started with a normal reschedule.
int startHostThread(const std::string& name, int priority, std::function<void()> body) {
    int id;
    {
        std::lock_guard<std::mutex> lk(g_km);
        auto t = std::make_unique<Thread>();
        t->id = newUid();
        t->name = name;
        t->priority = priority;
        t->attr = 0x00001000;
        t->hostBody = std::move(body);
        Thread* raw = t.get();
        g_threads[raw->id] = std::move(t);
        raw->ctx.mem = g_mem;
        raw->ctx.thread = raw;
        makeReady(raw);
        raw->host = std::thread(threadMain, raw);
        raw->host.detach();
        id = raw->id;
    }
    yieldIfHigherPriorityReady();
    return id;
}

bool threadStopped(int id) {
    std::lock_guard<std::mutex> lk(g_km);
    auto it = g_threads.find(id);
    return it == g_threads.end() || it->second->state == TState::Dead || it->second->state == TState::Dormant;
}

void terminateThread(int id) {
    std::lock_guard<std::mutex> lk(g_km);
    auto it = g_threads.find(id);
    if (it == g_threads.end()) return;
    it->second->state = TState::Dead; // its host thread stays parked, never scheduled again
    it->second->waitCond = nullptr;
    g_idleCv.notify_all();
}

int currentThreadPriority() {
    std::lock_guard<std::mutex> lk(g_km);
    return t_self ? t_self->priority : 0;
}

// sceKernelChangeThreadPriority on the current thread: it goes to the back of its new
// priority's ready queue, so an equal-priority thread that was waiting gets the CPU first.
void changeCurrentThreadPriority(int priority) {
    std::unique_lock<std::mutex> lk(g_km);
    Thread* self = t_self;
    self->priority = priority;
    makeReady(self);
    schedule(lk, self);
}

// hleEatMicro/hleEatCycles: the current thread keeps the CPU for this long.
void eatUs(uint64_t us) {
    uint64_t until = nowUs() + us;
    while (nowUs() < until) std::this_thread::yield();
}

// ---- semaphores / event flags ----------------------------------------------------------
struct Sema {
    std::string name;
    int count = 0, max = 0;
    uint32_t attr = 0;
};
struct EventFlag {
    std::string name;
    uint32_t pattern = 0, attr = 0;
    int waiters = 0;
};
static std::map<int, Sema> g_semas;
static std::map<int, EventFlag> g_evfs;
static void dumpEventFlags() {
    for (auto& [id, e] : g_evfs) log("[evflag] %d (%x) '%s' pattern %08x", id, id, e.name.c_str(), e.pattern);
}

static uint32_t readTimeout(uint32_t ptr) { return ptr ? read32(ptr) : 0; }

static void registerCoreHle();

void mpegNotifyModuleLoaded(const std::string& name, int version); // hle_mpeg.cpp

// Reads the module name and version from an unencrypted PRX on the disc (ELF with the
// SceModuleInfo at the first segment's p_paddr). Returns -1 when it cannot.
static int readModuleInfo(const std::string& path, std::string& name) {
    size_t colon = path.find(":/");
    if (colon == std::string::npos) return -1;
    std::string dev = path.substr(0, colon);
    if (dev != "disc0" && dev != "umd0" && dev != "umd1") return -1;
    IsoEntry e;
    if (!g_iso.lookup(path.substr(colon + 2), e) || e.dir || e.size < 0x80) return -1;
    std::vector<uint8_t> f(e.size);
    if (g_iso.read(e.lba, 0, f.data(), f.size()) != f.size()) return -1;
    auto rd32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &f[o], 4); return v; };
    if (rd32(0) != 0x464C457Fu) return -1; // ELF magic (encrypted "~PSP" modules are not parsed)
    uint32_t phoff = rd32(0x1C);
    if (phoff + 0x20 > f.size()) return -1;
    uint32_t off = rd32(phoff + 0x0C) & 0x7FFFFFFF; // p_paddr: file offset of SceModuleInfo
    if (off + 32 > f.size()) return -1;
    name.assign((const char*)&f[off + 4], strnlen((const char*)&f[off + 4], 28));
    return f[off + 2] | (f[off + 3] << 8);
}

// ---- module loading -------------------------------------------------------------------
static bool loadModule(const std::vector<uint8_t>& elf, uint32_t base, uint32_t& entry, uint32_t& gp, uint32_t& end) {
    auto rd = [&](size_t o) { uint32_t v; std::memcpy(&v, &elf[o], 4); return v; };
    auto rd16 = [&](size_t o) { uint16_t v; std::memcpy(&v, &elf[o], 2); return v; };
    if (elf.size() < 52 || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0 || rd16(16) != 0xFFA0) return false;
    uint32_t phoff = rd(28), shoff = rd(32);
    uint16_t phnum = rd16(44), shnum = rd16(48);
    // PT_LOAD segments at their vaddr (BOOT.BIN has one; PSMF_JK.PRX code + data).
    std::vector<uint32_t> segVaddr;
    uint32_t memEnd = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        size_t ph = phoff + i * 32u;
        if (rd(ph) != 1) continue;
        segVaddr.push_back(rd(ph + 8));
        memEnd = std::max(memEnd, rd(ph + 8) + rd(ph + 20));
    }
    if (segVaddr.empty()) return false;
    std::memset(hostPtr(base), 0, memEnd);
    for (uint16_t i = 0; i < phnum; ++i) {
        size_t ph = phoff + i * 32u;
        if (rd(ph) == 1) std::memcpy(hostPtr(base + rd(ph + 8)), &elf[rd(ph + 4)], rd(ph + 16));
    }
    // Relocations: identical algorithm to tools/recomp/pspelf.py. The info word holds the type,
    // the segment the offset is relative to and the segment whose address is added.
    auto seg = [&](uint32_t k) { return k < segVaddr.size() ? segVaddr[k] : 0u; };
    std::vector<uint32_t> pendingHi;
    for (uint16_t i = 0; i < shnum; ++i) {
        size_t sh = shoff + i * 40u;
        if (rd(sh + 4) != 0x700000A0) continue;
        uint32_t off = rd(sh + 16), size = rd(sh + 20);
        for (uint32_t k = 0; k < size; k += 8) {
            uint32_t at = rd(off + k), info = rd(off + k + 4), type = info & 0xFF;
            uint32_t addr = base + seg((info >> 8) & 0xFF) + at;
            uint32_t rbase = base + seg((info >> 16) & 0xFF);
            uint32_t w = read32(addr);
            switch (type) {
            case 2: write32(addr, w + rbase); break;
            case 4: {
                uint32_t target = ((w & 0x03FFFFFF) << 2) + rbase;
                write32(addr, (w & 0xFC000000) | ((target >> 2) & 0x03FFFFFF));
                break;
            }
            case 5: pendingHi.push_back(addr); break;
            case 6: {
                int32_t slo = (int16_t)(w & 0xFFFF);
                for (uint32_t h : pendingHi) {
                    uint32_t hw = read32(h);
                    uint32_t full = ((hw & 0xFFFF) << 16) + (uint32_t)slo + rbase;
                    uint32_t newhi = ((full - (uint32_t)(int32_t)(int16_t)(full & 0xFFFF)) >> 16) & 0xFFFF;
                    write32(h, (hw & 0xFFFF0000) | newhi);
                }
                pendingHi.clear();
                write32(addr, (w & 0xFFFF0000) | ((w + rbase) & 0xFFFF));
                break;
            }
            default: break;
            }
        }
    }
    entry = rd(24) == 0xFFFFFFFFu ? 0 : base + rd(24);
    // .rodata.sceModuleInfo: u16 attr, u16 ver, char name[28], u32 gp, ...
    gp = 0;
    uint32_t shstr = rd(shoff + rd16(50) * 40u + 16);
    for (uint16_t i = 0; i < shnum; ++i) {
        size_t sh = shoff + i * 40u;
        const char* nm = reinterpret_cast<const char*>(&elf[shstr + rd(sh)]);
        if (std::strcmp(nm, ".rodata.sceModuleInfo") == 0) gp = read32(base + rd(sh + 12) + 32);
    }
    end = base + memEnd;
    return true;
}

// Modules recompiled with BOOT.BIN (tools/recomp/games/<disc id>/modules.tsv): loaded from the
// disc at their fixed base when the game asks for them.
static const ExtraModule* findExtraModule(const std::string& path) {
    auto lower = [](std::string s) { for (char& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; };
    std::string p = lower(path);
    for (unsigned i = 0; i < g_extra_module_count; ++i)
        if (lower(g_extra_modules[i].path) == p) return &g_extra_modules[i];
    return nullptr;
}

static bool loadExtraModule(const ExtraModule& m) {
    IsoEntry e;
    std::string p = m.path;
    if (p.compare(0, 6, "disc0:") == 0) p = p.substr(6);
    if (!g_iso.lookup(p, e)) return false;
    std::vector<uint8_t> elf(e.size);
    g_iso.read(e.lba, 0, elf.data(), e.size);
    uint32_t entry = 0, gp = 0, end = 0;
    if (!loadModule(elf, m.base, entry, gp, end)) return false;
    log("module %s loaded at %08x-%08x (recompiled)", m.path, m.base, end);
    return end - m.base <= m.size;
}

static std::atomic<bool> g_exitRequested{false};
void requestExit() { g_exitRequested = true; }

int boot(const BootOptions& opt) {
    g_trace = opt.traceHle;
    g_bootTime = std::chrono::steady_clock::now();
#ifdef _WIN32
    timeBeginPeriod(1); // 1 ms timer ticks for the threads' timed waits (idleWaitUntil)
#endif
    if (!initMemory()) { log("cannot allocate guest memory"); return 1; }
    if (!g_iso.open(opt.isoPath)) { log("cannot open %s", opt.isoPath.c_str()); return 1; }
    installTextPatches(g_iso);
    IsoEntry e;
    if (!g_iso.lookup("PSP_GAME/SYSDIR/BOOT.BIN", e)) { log("BOOT.BIN not found"); return 1; }
    std::vector<uint8_t> elf(e.size);
    g_iso.read(e.lba, 0, elf.data(), e.size);
    uint32_t entry = 0, end = 0;
    if (!loadModule(elf, g_module_base, entry, g_gp, end)) { log("module load failed"); return 1; }
    if (entry != g_module_entry) {
        log("entry mismatch: loaded %08x, recompiled %08x (wrong game image?)", entry, g_module_entry);
        return 1;
    }
    log("module loaded at %08x-%08x, entry %08x, gp %08x", g_module_base, end, entry, g_gp);
    if (const char* dump = std::getenv("CA_DUMP_IMAGE")) { // debugging: compare with the recompiler's image
        if (FILE* f = std::fopen(dump, "wb")) {
            std::fwrite(hostPtr(g_module_base), 1, end - g_module_base, f);
            std::fclose(f);
        }
    }

    g_userPartition.init(kUserRam, kRamEnd);
    g_userPartition.reserve(kUserRam, 0x4000);              // loader area below the module
    g_userPartition.reserve(g_module_base, end - g_module_base);
    for (unsigned i = 0; i < g_extra_module_count; ++i)
        g_userPartition.reserve(g_extra_modules[i].base, (g_extra_modules[i].size + 0xFFFF) & ~0xFFFFu);
    // Interrupt handler stack in kernel RAM (not visible to the game's allocations).
    g_intrStackTop = kKernelRam + 0x00400000;

    registerCoreHle();
    bindImports();

    // The main thread runs the module entry with the boot path as its argument.
    const char* path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    uint32_t argAddr = kKernelRam + 0x00100000;
    writeString(argAddr, path);
    Thread* mainThread;
    {
        std::lock_guard<std::mutex> lk(g_km);
        mainThread = createThread("user_main", entry, 0x20, 0x40000, 0x80000000);
        startThread(mainThread, (uint32_t)std::strlen(path) + 1, argAddr);
    }
    std::thread(preemptionTimer).detach();
    // This host thread becomes the idle/scheduler loop until the game exits.
    {
        std::unique_lock<std::mutex> lk(g_km);
        schedule(lk, nullptr);
        g_idleCv.wait(lk, [] {
            if (g_exit) return true;
            for (auto& [id, t] : g_threads)
                if (t->state != TState::Dead) return false;
            return true;
        });
        log("game ended: %s (status %d)", g_exit ? "exit requested" : "every guest thread has ended", g_exitStatus);
    }
    dumpThreads();
    return g_exitStatus;
}

// ---- core HLE functions ---------------------------------------------------------------
#define HLE(name) registerHle(#name, [](Ctx* c)


// ---- Kernel objects added for Coded Arms: Contagion -------------------------------------
struct Fpl {
    uint32_t base = 0, blockSize = 0;
    int block = 0;
    std::vector<bool> used;
};
static std::map<int, Fpl> g_fpls;

struct Vpl {
    std::string name;
    uint32_t base = 0, size = 0, attr = 0;
    int block = 0;
    std::map<uint32_t, uint32_t> blocks; // address -> size (8-byte header included like the PSP)
    uint32_t alloc(uint32_t n) {
        n = ((n + 7) & ~7u) + 8;
        uint32_t a = base;
        for (auto& [addr, sz] : blocks) {
            if (addr - 8 - a >= n) break;
            a = addr - 8 + sz;
        }
        if (a + n > base + size) return 0;
        blocks[a + 8] = n;
        return a + 8;
    }
    uint32_t freeBytes() const {
        uint32_t used = 0;
        for (auto& [a, sz] : blocks) used += sz;
        return size - used;
    }
};
static std::map<int, Vpl> g_vpls;

struct VTimer {
    bool running = false;
    uint64_t startUs = 0, baseUs = 0;
    uint64_t now() const { return baseUs + (running ? nowUs() - startUs : 0); }
};
static std::map<int, VTimer> g_vtimers;

// printf-style formatting of a guest format string; integer arguments are taken from the
// argument registers starting at a<first> (a0-a3, t0-t3), strings are guest pointers.
static std::string formatGuest(const std::string& fmt, Ctx* c, int first) {
    std::string out;
    int arg = first;
    auto next = [&]() -> uint32_t { return arg < 8 ? c->r[4 + arg++] : 0; };
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        size_t j = i + 1;
        while (j < fmt.size() && std::strchr("-+ #0123456789.lhz", fmt[j])) ++j;
        if (j >= fmt.size()) break;
        std::string spec = "%" + fmt.substr(i + 1, j - i - 1);
        std::string flags;
        for (char ch : spec) if (!std::strchr("lhz", ch)) flags += ch;
        char conv = fmt[j], buf[512];
        switch (conv) {
        case '%': out += '%'; break;
        case 's': std::snprintf(buf, sizeof buf, (flags + "s").c_str(), readString(next(), 400).c_str()); out += buf; break;
        case 'c': out += (char)next(); break;
        case 'd': case 'i': std::snprintf(buf, sizeof buf, (flags + "d").c_str(), (int)next()); out += buf; break;
        case 'u': case 'x': case 'X': case 'o':
            std::snprintf(buf, sizeof buf, (flags + conv).c_str(), (unsigned)next()); out += buf; break;
        case 'p': std::snprintf(buf, sizeof buf, "%08x", (unsigned)next()); out += buf; break;
        case 'f': case 'g': case 'e': next(); next(); out += "<float>"; break;
        default: out += spec + conv; break;
        }
        i = j;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

static void registerCoreHle() {
    // ThreadManForUser: threads
    HLE(sceKernelCreateThread) {
        std::lock_guard<std::mutex> lk(g_km);
        int prio = (int)A(c, 2);
        if (prio < 0x08 || prio > 0x77) { ret(c, ERR_ILLEGAL_PRIORITY); return; }
        Thread* t = createThread(readString(A(c, 0), 32), A(c, 1), prio, A(c, 3), A(c, 4));
        ret(c, t ? (uint32_t)t->id : ERR_NO_MEMORY);
        if (t) log("thread %d '%s' created entry=%08x prio=%x stack=%x", t->id, t->name.c_str(), t->entry, prio, t->stackSize);
    });
    HLE(sceKernelStartThread) {
        {
            std::lock_guard<std::mutex> lk(g_km);
            Thread* t = findThread((int)A(c, 0));
            if (!t) { ret(c, ERR_UNKNOWN_THID); return; }
            if (t->state != TState::Dormant) { ret(c, ERR_NOT_DORMANT); return; }
            startThread(t, A(c, 1), A(c, 2));
            ret(c, 0);
        }
    });
    HLE(sceKernelExitThread) { exitThread(t_self, (int)A(c, 0), false); parkForever(); });
    HLE(sceKernelExitDeleteThread) { exitThread(t_self, (int)A(c, 0), true); parkForever(); });
    HLE(sceKernelDeleteThread) {
        std::lock_guard<std::mutex> lk(g_km);
        Thread* t = findThread((int)A(c, 0));
        ret(c, t ? 0 : ERR_UNKNOWN_THID);
    });
    auto terminate = [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_km);
        // The calling thread (id 0 or its own) cannot be terminated: the kernel returns
        // SCE_KERNEL_ERROR_ILLEGAL_THID (PPSSPP). The game does this when it leaves a level to
        // the terminal; ending its main thread closed the game.
        if (A(c, 0) == 0 || (t_self && (int)A(c, 0) == t_self->id)) { ret(c, 0x80020197); return; }
        Thread* t = findThread((int)A(c, 0));
        if (!t) { ret(c, ERR_UNKNOWN_THID); return; }
        log("thread %d '%s' terminated by thread %d", t->id, t->name.c_str(), t_self ? t_self->id : 0);
        // The host thread stays blocked forever; it is never scheduled again.
        t->state = TState::Dead;
        t->waitCond = nullptr;
        g_idleCv.notify_all();
        ret(c, 0);
    };
    registerHle("sceKernelTerminateThread", terminate);
    registerHle("sceKernelTerminateDeleteThread", terminate);
    HLE(sceKernelGetThreadId) { ret(c, (uint32_t)currentThreadId()); });
    HLE(sceKernelChangeThreadPriority) {
        {
            std::lock_guard<std::mutex> lk(g_km);
            Thread* t = findThread((int)A(c, 0));
            if (!t) { ret(c, ERR_UNKNOWN_THID); return; }
            int p = (int)A(c, 1);
            if (p == 0) p = t_self->priority;
            t->priority = p;
            ret(c, 0);
        }
    });
    HLE(sceKernelDelayThread) { delayCurrentThread(A(c, 0)); ret(c, 0); });
    HLE(sceKernelDelayThreadCB) { runCallbacks(); delayCurrentThread(A(c, 0), true); runCallbacks(); ret(c, 0); });
    HLE(sceKernelSleepThread) {
        Thread* self = t_self;
        waitUntil([self] {
            if (self->wakeupCount > 0) { self->wakeupCount--; return true; }
            return false;
        }, 0);
        ret(c, 0);
    });
    HLE(sceKernelWakeupThread) {
        {
            std::lock_guard<std::mutex> lk(g_km);
            Thread* t = findThread((int)A(c, 0));
            if (!t) { ret(c, ERR_UNKNOWN_THID); return; }
            t->wakeupCount++;
            reevaluateWaits();
        }
        ret(c, 0);
    });
    HLE(sceKernelSuspendThread) { ret(c, 0); });
    HLE(sceKernelResumeThread) { ret(c, 0); });
    HLE(sceKernelWaitThreadEnd) {
        int id = (int)A(c, 0);
        uint32_t tptr = A(c, 1);
        bool ok = waitUntil([id] {
            Thread* t = findThread(id);
            return !t || t->state == TState::Dead || t->state == TState::Dormant;
        }, readTimeout(tptr));
        if (!ok) { ret(c, ERR_WAIT_TIMEOUT); return; }
        std::lock_guard<std::mutex> lk(g_km);
        Thread* t = findThread(id);
        ret(c, t ? (uint32_t)t->exitStatus : ERR_UNKNOWN_THID);
    });
    HLE(sceKernelGetSystemTimeLow) { ret(c, (uint32_t)nowUs()); });

    // Callbacks
    HLE(sceKernelCreateCallback) {
        std::lock_guard<std::mutex> lk(g_km);
        int id = newUid();
        g_callbacks[id] = {readString(A(c, 0), 32), A(c, 1), A(c, 2), 0, 0, currentThreadId()};
        ret(c, (uint32_t)id);
    });
    HLE(sceKernelDeleteCallback) {
        std::lock_guard<std::mutex> lk(g_km);
        ret(c, g_callbacks.erase((int)A(c, 0)) ? 0 : ERR_UNKNOWN_CBID);
    });
    HLE(sceKernelCheckCallback) { ret(c, (uint32_t)(runCallbacks() > 0)); });

    // Semaphores
    HLE(sceKernelCreateSema) {
        std::lock_guard<std::mutex> lk(g_km);
        int id = newUid();
        g_semas[id] = {readString(A(c, 0), 32), (int)A(c, 2), (int)A(c, 3), A(c, 1)};
        ret(c, (uint32_t)id);
    });
    HLE(sceKernelDeleteSema) {
        std::lock_guard<std::mutex> lk(g_km);
        ret(c, g_semas.erase((int)A(c, 0)) ? 0 : ERR_UNKNOWN_SEMID);
        reevaluateWaits();
    });
    HLE(sceKernelSignalSema) {
        {
            std::lock_guard<std::mutex> lk(g_km);
            auto it = g_semas.find((int)A(c, 0));
            if (it == g_semas.end()) { ret(c, ERR_UNKNOWN_SEMID); return; }
            int n = (int)A(c, 1);
            if (it->second.count + n > it->second.max) { ret(c, ERR_SEMA_OVF); return; }
            it->second.count += n;
            reevaluateWaits();
            ret(c, 0);
        }
    });
    HLE(sceKernelWaitSema) {
        int id = (int)A(c, 0), need = (int)A(c, 1);
        uint32_t tptr = A(c, 2);
        bool exists = true;
        bool ok = waitUntil([id, need, &exists] {
            auto it = g_semas.find(id);
            if (it == g_semas.end()) { exists = false; return true; }
            if (it->second.count >= need) { it->second.count -= need; return true; }
            return false;
        }, readTimeout(tptr));
        if (!exists) { ret(c, ERR_UNKNOWN_SEMID); return; }
        if (!ok) { if (tptr) write32(tptr, 0); ret(c, ERR_WAIT_TIMEOUT); return; }
        ret(c, 0);
    });
    HLE(sceKernelPollSema) {
        std::lock_guard<std::mutex> lk(g_km);
        auto it = g_semas.find((int)A(c, 0));
        if (it == g_semas.end()) { ret(c, ERR_UNKNOWN_SEMID); return; }
        int need = (int)A(c, 1);
        if (it->second.count >= need) { it->second.count -= need; ret(c, 0); }
        else ret(c, ERR_SEMA_ZERO);
    });

    // Event flags
    HLE(sceKernelCreateEventFlag) {
        std::lock_guard<std::mutex> lk(g_km);
        int id = newUid();
        g_evfs[id] = {readString(A(c, 0), 32), A(c, 2), A(c, 1), 0};
        ret(c, (uint32_t)id);
    });
    HLE(sceKernelDeleteEventFlag) {
        std::lock_guard<std::mutex> lk(g_km);
        ret(c, g_evfs.erase((int)A(c, 0)) ? 0 : ERR_UNKNOWN_EVFID);
        reevaluateWaits();
    });
    HLE(sceKernelSetEventFlag) {
        std::lock_guard<std::mutex> lk(g_km);
        auto it = g_evfs.find((int)A(c, 0));
        if (it == g_evfs.end()) { ret(c, ERR_UNKNOWN_EVFID); return; }
        it->second.pattern |= A(c, 1);
        reevaluateWaits();
        ret(c, 0);
    });
    HLE(sceKernelClearEventFlag) {
        std::lock_guard<std::mutex> lk(g_km);
        auto it = g_evfs.find((int)A(c, 0));
        if (it == g_evfs.end()) { ret(c, ERR_UNKNOWN_EVFID); return; }
        it->second.pattern &= A(c, 1);
        ret(c, 0);
    });
    HLE(sceKernelWaitEventFlag) {
        int id = (int)A(c, 0);
        uint32_t bits = A(c, 1), mode = A(c, 2), outp = A(c, 3), tptr = A(c, 4);
        if (bits == 0) { ret(c, ERR_EVF_ILPAT); return; }
        bool exists = true;
        uint32_t matched = 0;
        bool ok = waitUntil([id, bits, mode, &exists, &matched] {
            auto it = g_evfs.find(id);
            if (it == g_evfs.end()) { exists = false; return true; }
            uint32_t p = it->second.pattern;
            bool hit = (mode & 1) ? (p & bits) != 0 : (p & bits) == bits; // bit0: OR wait
            if (!hit) return false;
            matched = p;
            // PSP_EVENT_WAITCLEAR (0x20): clear the waited bits; PSP_EVENT_WAITCLEARALL (0x10): clear all
            if (mode & 0x20) it->second.pattern &= ~bits;
            if (mode & 0x10) it->second.pattern = 0;
            return true;
        }, readTimeout(tptr));
        if (!exists) { ret(c, ERR_UNKNOWN_EVFID); return; }
        if (outp) write32(outp, matched);
        if (!ok) { if (tptr) write32(tptr, 0); ret(c, ERR_WAIT_TIMEOUT); return; }
        ret(c, 0);
    });

    // SysMemUserForUser
    HLE(sceKernelAllocPartitionMemory) {
        std::lock_guard<std::mutex> lk(g_km);
        int id = 0;
        uint32_t type = A(c, 2), size = A(c, 3), addr = A(c, 4);
        uint32_t a = g_userPartition.alloc(size, type == 1 ? 1 : (type == 2 ? 2 : 0), addr, 256, &id);
        log("AllocPartitionMemory('%s', type %u, size %x) -> %08x", readString(A(c, 1), 32).c_str(), type, size, a);
        ret(c, a ? (uint32_t)id : ERR_NO_MEMORY);
    });
    HLE(sceKernelFreePartitionMemory) {
        std::lock_guard<std::mutex> lk(g_km);
        ret(c, g_userPartition.free((int)A(c, 0)) ? 0 : 0x800200CA);
    });
    HLE(sceKernelGetBlockHeadAddr) {
        std::lock_guard<std::mutex> lk(g_km);
        ret(c, g_userPartition.blockAddr((int)A(c, 0)));
    });
    HLE(sceKernelTotalFreeMemSize) { ret(c, g_userPartition.freeTotal()); });

    // Kernel_Library: interrupts are never truly disabled in HLE; return the "enabled" state.
    HLE(sceKernelCpuSuspendIntr) { ret(c, 1); });
    HLE(sceKernelCpuResumeIntr) { ret(c, 0); });

    // InterruptManager
    HLE(sceKernelRegisterSubIntrHandler) { registerSubIntr((int)A(c, 0), (int)A(c, 1), A(c, 2), A(c, 3)); ret(c, 0); });
    HLE(sceKernelReleaseSubIntrHandler) { registerSubIntr((int)A(c, 0), (int)A(c, 1), 0, 0); ret(c, 0); });
    HLE(sceKernelEnableSubIntr) { enableSubIntr((int)A(c, 0), (int)A(c, 1), true); ret(c, 0); });

    // UtilsForUser
    HLE(sceKernelLibcClock) { ret(c, (uint32_t)nowUs()); });
    HLE(sceKernelLibcTime) {
        uint32_t t = (uint32_t)std::time(nullptr);
        if (A(c, 0)) write32(A(c, 0), t);
        ret(c, t);
    });
    HLE(sceKernelLibcGettimeofday) {
        const uint64_t us = wallClockUs();
        if (A(c, 0)) { write32(A(c, 0), (uint32_t)(us / 1000000)); write32(A(c, 0) + 4, (uint32_t)(us % 1000000)); }
        ret(c, 0);
    });
    HLE(sceKernelDcacheWritebackAll) { ret(c, 0); });
    HLE(sceKernelDcacheWritebackRange) { ret(c, 0); });

    // LoadExecForUser / ModuleMgrForUser / StdioForUser / sceSuspendForUser
    HLE(sceKernelExitGame) {
        log("game requested exit");
        {
            std::lock_guard<std::mutex> lk(g_km);
            g_exit = true;
            g_idleCv.notify_all();
            for (auto& [id, t] : g_threads) t->cv.notify_all();
        }
        std::unique_lock<std::mutex> lk(g_km);
        schedule(lk, t_self);
    });
    HLE(sceKernelRegisterExitCallback) { ret(c, 0); });
    HLE(sceKernelLoadModule) {
        // System modules shipped on the disc (mpeg, network, audiocodec...) are provided by
        // the HLE layer itself; report success with a fake module id. Their module info
        // (name, version) is still read, as some HLE modules depend on the library version.
        std::string path = readString(A(c, 0));
        if (const ExtraModule* m = findExtraModule(path)) {
            ret(c, loadExtraModule(*m) ? (uint32_t)newUid() : 0x80020146u); // SCE_KERNEL_ERROR_FILEERR
            return;
        }
        std::string name;
        int version = readModuleInfo(path, name);
        log("LoadModule(%s) -> HLE (%s, version %04x)", path.c_str(), name.c_str(), version);
        if (version >= 0) mpegNotifyModuleLoaded(name, version);
        ret(c, (uint32_t)newUid());
    });
    HLE(sceKernelStartModule) { if (A(c, 3)) write32(A(c, 3), 0); ret(c, 0); });
    HLE(sceKernelUnloadModule) { ret(c, 0); });
    HLE(sceKernelSelfStopUnloadModule) { ret(c, 0); });
    HLE(sceKernelStdin) { ret(c, 0); });
    HLE(sceKernelStdout) { ret(c, 1); });
    HLE(sceKernelStderr) { ret(c, 2); });
    HLE(sceKernelPowerLock) { ret(c, 0); });
    HLE(sceKernelPowerUnlock) { ret(c, 0); });
    HLE(sceKernelPowerTick) { ret(c, 0); });

    // ---- Added for Coded Arms: Contagion -------------------------------------------------
    HLE(sceKernelSetCompiledSdkVersion) { ret(c, 0); });
    HLE(sceKernelSetCompilerVersion) { ret(c, 0); });
    HLE(sceKernelPrintf) { log("[guest] %s", formatGuest(readString(A(c, 0)), c, 1).c_str()); ret(c, 0); });
    HLE(sceKernelGetModuleIdByAddress) { ret(c, 1); });
    HLE(sceKernelGetModuleId) { ret(c, 1); });
    HLE(sceKernelStopModule) { if (A(c, 4)) write32(A(c, 4), 0); ret(c, 0); });
    HLE(sceKernelStopUnloadSelfModuleWithStatus) {
        log("game stopped its own module (status %d): exit", (int)A(c, 0));
        {
            std::lock_guard<std::mutex> lk(g_km);
            g_exit = true;
            g_idleCv.notify_all();
            for (auto& [id, t] : g_threads) t->cv.notify_all();
        }
        std::unique_lock<std::mutex> lk(g_km);
        schedule(lk, t_self);
    });
    // Volatile memory: the 4 MB at 0x08400000 that the system lends to games (PPSSPP sceSuspendForUser).
    HLE(sceKernelVolatileMemTryLock) {
        if (A(c, 1)) write32(A(c, 1), 0x08400000);
        if (A(c, 2)) write32(A(c, 2), 0x00400000);
        ret(c, 0);
    });
    HLE(sceKernelVolatileMemLock) {
        if (A(c, 1)) write32(A(c, 1), 0x08400000);
        if (A(c, 2)) write32(A(c, 2), 0x00400000);
        ret(c, 0);
    });
    HLE(sceKernelVolatileMemUnlock) { ret(c, 0); });
    HLE(sceKernelChangeCurrentThreadAttr) {
        std::lock_guard<std::mutex> lk(g_km);
        t_self->attr = (t_self->attr & ~A(c, 0)) | A(c, 1);
        ret(c, 0);
    });
    HLE(sceKernelGetThreadCurrentPriority) { ret(c, (uint32_t)t_self->priority); });
    HLE(sceKernelReleaseThreadEventHandler) { ret(c, 0); });
    HLE(sceKernelReferThreadStatus) { // SceKernelThreadInfo (0x68 bytes)
        std::lock_guard<std::mutex> lk(g_km);
        Thread* t = A(c, 0) ? findThread((int)A(c, 0)) : t_self;
        uint32_t info = A(c, 1);
        if (!t) { ret(c, ERR_UNKNOWN_THID); return; }
        uint32_t size = read32(info);
        static const uint32_t kStatus[] = {16, 2, 1, 4, 32};
        uint32_t f[26] = {};
        f[0] = size;
        std::strncpy(reinterpret_cast<char*>(&f[1]), t->name.c_str(), 31);
        f[9] = t->attr; f[10] = kStatus[(int)t->state]; f[11] = t->entry; f[12] = t->stackAddr;
        f[13] = t->stackSize; f[14] = t->gp; f[15] = (uint32_t)t->priority; f[16] = (uint32_t)t->priority;
        f[19] = (uint32_t)t->wakeupCount; f[20] = (uint32_t)t->exitStatus;
        for (uint32_t i = 0; i < 26 && i * 4 < size; ++i) write32(info + i * 4, f[i]);
        ret(c, 0);
    });
    HLE(sceKernelWaitSemaCB) {
        int id = (int)A(c, 0), need = (int)A(c, 1);
        uint32_t tptr = A(c, 2);
        bool exists = true;
        runCallbacks();
        bool ok = waitUntil([id, need, &exists] {
            auto it = g_semas.find(id);
            if (it == g_semas.end()) { exists = false; return true; }
            if (it->second.count >= need) { it->second.count -= need; return true; }
            return false;
        }, readTimeout(tptr), true);
        runCallbacks();
        if (!exists) { ret(c, ERR_UNKNOWN_SEMID); return; }
        if (!ok) { if (tptr) write32(tptr, 0); ret(c, ERR_WAIT_TIMEOUT); return; }
        ret(c, 0);
    });

    // Fixed-size memory pools (Fpl): one partition block split into equal blocks.
    HLE(sceKernelCreateFpl) {
        std::lock_guard<std::mutex> lk(g_km);
        uint32_t attr = A(c, 2), bsize = (A(c, 3) + 3) & ~3u, n = A(c, 4);
        int block = 0;
        uint32_t base = g_userPartition.alloc(bsize * n, (attr & 0x4000) ? 1 : 0, 0, 256, &block);
        if (!base) { ret(c, ERR_NO_MEMORY); return; }
        int id = newUid();
        Fpl& f = g_fpls[id];
        f.base = base; f.blockSize = bsize; f.block = block;
        f.used.assign(n, false);
        log("CreateFpl('%s', %u x %u) -> %d @%08x", readString(A(c, 0), 32).c_str(), n, bsize, id, base);
        ret(c, (uint32_t)id);
    });
    HLE(sceKernelDeleteFpl) {
        std::lock_guard<std::mutex> lk(g_km);
        auto it = g_fpls.find((int)A(c, 0));
        if (it == g_fpls.end()) { ret(c, 0x8002019A); return; }
        g_userPartition.free(it->second.block);
        g_fpls.erase(it);
        ret(c, 0);
    });
    HLE(sceKernelTryAllocateFpl) {
        std::lock_guard<std::mutex> lk(g_km);
        auto it = g_fpls.find((int)A(c, 0));
        if (it == g_fpls.end()) { ret(c, 0x8002019A); return; }
        Fpl& f = it->second;
        for (size_t i = 0; i < f.used.size(); ++i)
            if (!f.used[i]) {
                f.used[i] = true;
                write32(A(c, 1), f.base + (uint32_t)i * f.blockSize);
                ret(c, 0);
                return;
            }
        ret(c, 0x800201AF); // SCE_KERNEL_ERROR_NO_MEMORY (pool full)
    });
    HLE(sceKernelFreeFpl) {
        std::lock_guard<std::mutex> lk(g_km);
        auto it = g_fpls.find((int)A(c, 0));
        if (it == g_fpls.end()) { ret(c, 0x8002019A); return; }
        Fpl& f = it->second;
        uint32_t i = (A(c, 1) - f.base) / f.blockSize;
        if (A(c, 1) < f.base || i >= f.used.size() || !f.used[i]) { ret(c, 0x800201B3); return; }
        f.used[i] = false;
        ret(c, 0);
    });

    // Virtual timers: a microsecond counter that runs while started (no handlers yet).
    HLE(sceKernelCreateVTimer) {
        std::lock_guard<std::mutex> lk(g_km);
        int id = newUid();
        g_vtimers[id] = VTimer{};
        ret(c, (uint32_t)id);
    });
    HLE(sceKernelDeleteVTimer) {
        std::lock_guard<std::mutex> lk(g_km);
        ret(c, g_vtimers.erase((int)A(c, 0)) ? 0 : 0x800201C1);
    });
    HLE(sceKernelStartVTimer) {
        std::lock_guard<std::mutex> lk(g_km);
        auto it = g_vtimers.find((int)A(c, 0));
        if (it == g_vtimers.end()) { ret(c, 0x800201C1); return; }
        if (it->second.running) { ret(c, 1); return; }
        it->second.running = true;
        it->second.startUs = nowUs();
        ret(c, 0);
    });
    HLE(sceKernelStopVTimer) {
        std::lock_guard<std::mutex> lk(g_km);
        auto it = g_vtimers.find((int)A(c, 0));
        if (it == g_vtimers.end()) { ret(c, 0x800201C1); return; }
        if (!it->second.running) { ret(c, 0); return; }
        it->second.baseUs += nowUs() - it->second.startUs;
        it->second.running = false;
        ret(c, 1);
    });
    HLE(sceKernelGetVTimerTime) {
        std::lock_guard<std::mutex> lk(g_km);
        auto it = g_vtimers.find((int)A(c, 0));
        if (it == g_vtimers.end()) { ret(c, 0x800201C1); return; }
        uint64_t t = it->second.now();
        write32(A(c, 1), (uint32_t)t);
        write32(A(c, 1) + 4, (uint32_t)(t >> 32));
        ret(c, 0);
    });
    HLE(sceKernelGetVTimerTimeWide) {
        std::lock_guard<std::mutex> lk(g_km);
        auto it = g_vtimers.find((int)A(c, 0));
        uint64_t t = it == g_vtimers.end() ? 0 : it->second.now();
        c->r[2] = (uint32_t)t;
        c->r[3] = (uint32_t)(t >> 32);
    });
    HLE(sceKernelSetVTimerTimeWide) { // a0 = id, a2:a3 = new time (64-bit argument pair)
        std::lock_guard<std::mutex> lk(g_km);
        auto it = g_vtimers.find((int)A(c, 0));
        if (it == g_vtimers.end()) { c->r[2] = c->r[3] = 0; return; }
        uint64_t old = it->second.now();
        it->second.baseUs = (uint64_t)A(c, 2) | ((uint64_t)A(c, 3) << 32);
        it->second.startUs = nowUs();
        c->r[2] = (uint32_t)old;
        c->r[3] = (uint32_t)(old >> 32);
    });

    HLE(sceKernelWaitEventFlagCB) {
        int id = (int)A(c, 0);
        uint32_t bits = A(c, 1), mode = A(c, 2), outp = A(c, 3), tptr = A(c, 4);
        if (bits == 0) { ret(c, ERR_EVF_ILPAT); return; }
        bool exists = true;
        uint32_t matched = 0;
        runCallbacks();
        bool ok = waitUntil([id, bits, mode, &exists, &matched] {
            auto it = g_evfs.find(id);
            if (it == g_evfs.end()) { exists = false; return true; }
            uint32_t p = it->second.pattern;
            bool hit = (mode & 1) ? (p & bits) != 0 : (p & bits) == bits;
            if (!hit) return false;
            matched = p;
            if (mode & 0x20) it->second.pattern &= ~bits;
            if (mode & 0x10) it->second.pattern = 0;
            return true;
        }, readTimeout(tptr), true);
        runCallbacks();
        if (!exists) { ret(c, ERR_UNKNOWN_EVFID); return; }
        if (outp) write32(outp, matched);
        if (!ok) { if (tptr) write32(tptr, 0); ret(c, ERR_WAIT_TIMEOUT); return; }
        ret(c, 0);
    });
    HLE(sceKernelReferEventFlagStatus) { // SceKernelEventFlagInfo: size, name[32], attr, init, current, waiters
        std::lock_guard<std::mutex> lk(g_km);
        auto it = g_evfs.find((int)A(c, 0));
        if (it == g_evfs.end()) { ret(c, ERR_UNKNOWN_EVFID); return; }
        uint32_t info = A(c, 1), size = read32(info);
        uint32_t f[13] = {};
        f[0] = size;
        std::strncpy(reinterpret_cast<char*>(&f[1]), it->second.name.c_str(), 31);
        f[9] = it->second.attr;
        f[10] = it->second.pattern;
        f[11] = it->second.pattern;
        f[12] = (uint32_t)it->second.waiters;
        for (uint32_t i = 0; i < 13 && i * 4 < size; ++i) write32(info + i * 4, f[i]);
        ret(c, 0);
    });
    HLE(sceKernelWaitThreadEndCB) {
        int id = (int)A(c, 0);
        uint32_t tptr = A(c, 1);
        runCallbacks();
        bool ok = waitUntil([id] {
            Thread* t = findThread(id);
            return !t || t->state == TState::Dead || t->state == TState::Dormant;
        }, readTimeout(tptr), true);
        runCallbacks();
        if (!ok) { ret(c, ERR_WAIT_TIMEOUT); return; }
        std::lock_guard<std::mutex> lk(g_km);
        Thread* t = findThread(id);
        ret(c, t ? (uint32_t)t->exitStatus : ERR_UNKNOWN_THID);
    });

    // Variable-size memory pools (Vpl): first-fit blocks inside one partition block.
    HLE(sceKernelCreateVpl) {
        std::lock_guard<std::mutex> lk(g_km);
        uint32_t attr = A(c, 2), size = (A(c, 3) + 7) & ~7u;
        int block = 0;
        uint32_t base = g_userPartition.alloc(size, (attr & 0x4000) ? 1 : 0, 0, 256, &block);
        if (!base) { ret(c, ERR_NO_MEMORY); return; }
        int id = newUid();
        Vpl& v = g_vpls[id];
        v.name = readString(A(c, 0), 32);
        v.base = base; v.size = size; v.block = block; v.attr = attr;
        log("CreateVpl('%s', %u bytes) -> %d @%08x", v.name.c_str(), size, id, base);
        ret(c, (uint32_t)id);
    });
    HLE(sceKernelDeleteVpl) {
        std::lock_guard<std::mutex> lk(g_km);
        auto it = g_vpls.find((int)A(c, 0));
        if (it == g_vpls.end()) { ret(c, 0x8002019C); return; } // SCE_KERNEL_ERROR_UNKNOWN_VPLID
        g_userPartition.free(it->second.block);
        g_vpls.erase(it);
        ret(c, 0);
    });
    HLE(sceKernelTryAllocateVpl) {
        std::lock_guard<std::mutex> lk(g_km);
        auto it = g_vpls.find((int)A(c, 0));
        if (it == g_vpls.end()) { ret(c, 0x8002019C); return; }
        uint32_t a = it->second.alloc(A(c, 1));
        if (!a) { ret(c, 0x800201B0); return; } // SCE_KERNEL_ERROR_NO_MEMORY for pools
        write32(A(c, 2), a);
        ret(c, 0);
    });
    HLE(sceKernelFreeVpl) {
        std::lock_guard<std::mutex> lk(g_km);
        auto it = g_vpls.find((int)A(c, 0));
        if (it == g_vpls.end()) { ret(c, 0x8002019C); return; }
        ret(c, it->second.blocks.erase(A(c, 1)) ? 0 : 0x800201B4); // SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK
    });
    HLE(sceKernelReferVplStatus) { // SceKernelVplInfo: size, name[32], attr, poolSize, freeSize, waiters
        std::lock_guard<std::mutex> lk(g_km);
        auto it = g_vpls.find((int)A(c, 0));
        if (it == g_vpls.end()) { ret(c, 0x8002019C); return; }
        uint32_t info = A(c, 1), size = read32(info);
        uint32_t f[13] = {};
        f[0] = size;
        std::strncpy(reinterpret_cast<char*>(&f[1]), it->second.name.c_str(), 31);
        f[9] = it->second.attr;
        f[10] = it->second.size;
        f[11] = it->second.freeBytes();
        for (uint32_t i = 0; i < 13 && i * 4 < size; ++i) write32(info + i * 4, f[i]);
        ret(c, 0);
    });

    extern void registerIoHle();
    extern void registerDeviceHle();
    extern void registerSasHle();
    extern void registerMpegHle();
    extern void registerUtilityHle();
    extern void registerAtracHle();
    extern void registerPsmfHle();
    registerIoHle();
    registerDeviceHle();
    registerSasHle();
    registerMpegHle();
    registerUtilityHle();
    registerAtracHle();
    registerPsmfHle();
}

} // namespace psp::kernel
