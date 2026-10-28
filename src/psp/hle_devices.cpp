// Device HLE: display, controller, GE front end, audio, UMD, power, impose, RTC. Network
// (ad hoc multiplayer) is reported unavailable for now; MPEG video is in hle_mpeg.cpp and the
// utility dialogs in hle_utility.cpp.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "psp/ge.h"
#include "psp/gpu/gpu.h"
#include "psp/host.h"
#include "psp/kernel.h"
#include "psp/memory.h"

namespace psp::host {
Controller g_ctrl;
Display g_display;
AudioChannel g_audio[kAudioChannels];
std::mutex g_audioMutex;
std::atomic<bool> g_audioConsumer{false};
size_t audioQueuedFrames(int ch) { return g_audio[ch].queue.size() / 2; }
} // namespace psp::host

namespace psp::kernel {

namespace {

constexpr double kVblankUs = 1001000000.0 / 60000.0; // 59.94 Hz
uint64_t vcount() { return (uint64_t)(nowUs() / kVblankUs); }
void waitVblank(bool cb) {
    if (cb) runPendingCallbacks();
    // Contagion updates once per vblank wait with a frame-time based logic: 60 fps at most.
    // A 30 fps cap (host::g_targetFps) waits for every other vblank instead.
    uint64_t target = vcount() + 1;
    if (host::g_targetFps.load() == 30) target += target & 1;
    uint64_t next = (uint64_t)std::ceil((double)target * kVblankUs) + 1;
    uint64_t now = nowUs();
    delayCurrentThread(next > now ? next - now : 1, cb);
    if (cb) runPendingCallbacks();
    static const bool perf = std::getenv("CA_PERF") != nullptr;
    if (perf) { // time the calling threads spend between vblank waits (frame work)
        static uint64_t lastReport = 0, busy = 0, waits = 0, maxBusy = 0;
        static thread_local uint64_t lastReturn = 0;
        uint64_t t = nowUs();
        if (lastReturn) { busy += now - lastReturn; maxBusy = std::max(maxBusy, now - lastReturn); }
        lastReturn = t;
        ++waits;
        if (t - lastReport >= 1000000) {
            std::fprintf(stderr, "[PERF] vblank waits=%llu work=%llums max=%.1fms\n", (unsigned long long)waits,
                         (unsigned long long)(busy / 1000), maxBusy / 1000.0);
            lastReport = t; busy = waits = maxBusy = 0;
        }
    }
}

int g_umdCallback = 0;
// PSP_UMD_* bits (PPSSPP sceUmd.h): PRESENT 0x02, READY 0x10, READABLE 0x20. The activation
// notification carries PRESENT|READABLE (READY only for modules with a compiled SDK version,
// which this game does not declare).
constexpr uint32_t kUmdPresent = 0x02, kUmdReady = 0x10, kUmdReadable = 0x20;

// Audio pacing. With a host consumer (the front end's audio device), a blocking output waits
// until the channel's queue has drained below a threshold, like PPSSPP: the game's audio
// threads then run on the sound card's clock, so the queue neither underruns (gaps, crackle)
// nor grows. Without a consumer (headless runs) a blocking output returns when the previous
// buffer would have finished playing on the wall clock, like the hardware DMA.
uint64_t g_chanBusyUntil[host::kAudioChannels];

size_t queueLimitFrames(uint32_t n) { return std::max<size_t>(2048, n); }

uint32_t audioOutput(int ch, int volL, int volR, uint32_t buf, bool blocking) {
    using namespace host;
    if (ch < 0 || ch >= kAudioChannels || !g_audio[ch].reserved) return 0x80260003;
    uint32_t n = g_audio[ch].sampleCount;
    const bool consumer = g_audioConsumer.load();
    if (blocking && consumer) {
        // Time out so a stalled device (e.g. unplugged) cannot hang the game.
        waitUntil([ch, n] {
            std::lock_guard<std::mutex> lk(g_audioMutex);
            return g_audio[ch].queue.size() / 2 <= queueLimitFrames(n);
        }, 250000);
    } else if (blocking) {
        uint64_t now = nowUs();
        if (g_chanBusyUntil[ch] > now) delayCurrentThread(g_chanBusyUntil[ch] - now);
    }
    {
        std::lock_guard<std::mutex> lk(g_audioMutex);
        AudioChannel& a = g_audio[ch];
        bool mono = a.format == 0x10;
        if (a.queue.size() < 44100 * 2) { // drop if the host is not consuming
            for (uint32_t i = 0; i < n; ++i) {
                int16_t l = (int16_t)read16(buf + (mono ? i * 2 : i * 4));
                int16_t r = mono ? l : (int16_t)read16(buf + i * 4 + 2);
                a.queue.push_back((int16_t)((l * volL) >> 15));
                a.queue.push_back((int16_t)((r * volR) >> 15));
            }
        }
    }
    uint64_t now = nowUs();
    uint64_t dur = (uint64_t)n * 1000000ull / 44100ull;
    g_chanBusyUntil[ch] = std::max(g_chanBusyUntil[ch], now) + dur;
    return n;
}

} // namespace

void registerDeviceHle() {
    using namespace host;
    // ---- sceDisplay
    registerHle("sceDisplaySetMode", [](Ctx* c) { ret(c, 0); });
    registerHle("sceDisplaySetFrameBuf", [](Ctx* c) {
        g_display.topAddr = A(c, 0);
        g_display.bufferWidth = A(c, 1);
        g_display.pixelFormat = A(c, 2);
        g_display.frameCount++;
        ret(c, 0);
        if (gpu::installed() && A(c, 0)) gpu::setDisplay(A(c, 0), A(c, 1), A(c, 2));
    });
    registerHle("sceDisplayGetVcount", [](Ctx* c) { ret(c, (uint32_t)vcount()); });
    registerHle("sceDisplayWaitVblankStart", [](Ctx* c) { waitVblank(false); ret(c, 0); });
    registerHle("sceDisplayWaitVblankStartCB", [](Ctx* c) { waitVblank(true); ret(c, 0); });
    registerHle("sceDisplayGetFramePerSec", [](Ctx* c) { c->f[0] = 59.9400599f; ret(c, 0); });

    // ---- sceCtrl
    registerHle("sceCtrlSetSamplingCycle", [](Ctx* c) { ret(c, 0); });
    registerHle("sceCtrlSetSamplingMode", [](Ctx* c) { ret(c, 1); });
    registerHle("sceCtrlPeekBufferPositive", [](Ctx* c) {
        uint32_t p = A(c, 0);
        int n = (int)A(c, 1);
        for (int i = 0; i < n; ++i, p += 16) {
            write32(p, (uint32_t)nowUs());
            write32(p + 4, g_ctrl.buttons.load());
            write8(p + 8, g_ctrl.lx.load());
            write8(p + 9, g_ctrl.ly.load());
            for (int k = 10; k < 16; ++k) write8(p + k, 0);
        }
        ret(c, (uint32_t)n);
    });

    // ---- sceGe_user
    registerHle("sceGeEdramGetAddr", [](Ctx* c) { ret(c, 0x04000000); });
    registerHle("sceGeGetCmd", [](Ctx* c) { ret(c, ge::reg((int)A(c, 0))); });
    registerHle("sceGeListEnQueue", [](Ctx* c) { ret(c, (uint32_t)ge::enqueue(A(c, 0), A(c, 1), (int)A(c, 2), false)); });
    registerHle("sceGeListEnQueueHead", [](Ctx* c) { ret(c, (uint32_t)ge::enqueue(A(c, 0), A(c, 1), (int)A(c, 2), true)); });
    registerHle("sceGeListUpdateStallAddr", [](Ctx* c) { ret(c, (uint32_t)ge::updateStall((int)A(c, 0), A(c, 1))); });
    registerHle("sceGeListSync", [](Ctx* c) {
        int id = (int)A(c, 0);
        if (A(c, 1) == 1) { ret(c, ge::listDone(id) ? 0 : 2); return; } // peek
        waitUntil([id] { return ge::listDone(id); }, 0);
        ret(c, 0);
    });
    registerHle("sceGeDrawSync", [](Ctx* c) {
        if (A(c, 0) == 1) { ret(c, ge::allDone() ? 0 : 2); return; }
        waitUntil([] { return ge::allDone(); }, 0);
        ret(c, 0);
    });
    registerHle("sceGeBreak", [](Ctx* c) { ret(c, 0); });
    registerHle("sceGeContinue", [](Ctx* c) { ret(c, 0); });
    registerHle("sceGeSetCallback", [](Ctx* c) {
        uint32_t p = A(c, 0);
        ret(c, (uint32_t)ge::setCallback(read32(p), read32(p + 4), read32(p + 8), read32(p + 12)));
    });
    registerHle("sceGeUnsetCallback", [](Ctx* c) { ge::unsetCallback((int)A(c, 0)); ret(c, 0); });

    // ---- sceAudio
    registerHle("sceAudioChReserve", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_audioMutex);
        int ch = (int)A(c, 0);
        if (ch < 0) {
            for (int i = kSrcChannel - 1; i >= 0; --i)
                if (!g_audio[i].reserved) { ch = i; break; }
        }
        if (ch < 0 || ch >= kSrcChannel || g_audio[ch].reserved) { ret(c, 0x80260002); return; }
        g_audio[ch].reserved = true;
        g_audio[ch].sampleCount = A(c, 1);
        g_audio[ch].format = A(c, 2);
        g_audio[ch].queue.clear();
        ret(c, (uint32_t)ch);
    });
    registerHle("sceAudioChRelease", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_audioMutex);
        int ch = (int)A(c, 0);
        if (ch >= 0 && ch < kAudioChannels) g_audio[ch].reserved = false;
        ret(c, 0);
    });
    registerHle("sceAudioOutputBlocking", [](Ctx* c) {
        int v = (int)A(c, 1);
        ret(c, audioOutput((int)A(c, 0), v, v, A(c, 2), true));
    });
    registerHle("sceAudioOutputPanned", [](Ctx* c) { ret(c, audioOutput((int)A(c, 0), (int)A(c, 1), (int)A(c, 2), A(c, 3), false)); });
    registerHle("sceAudioOutputPannedBlocking", [](Ctx* c) { ret(c, audioOutput((int)A(c, 0), (int)A(c, 1), (int)A(c, 2), A(c, 3), true)); });
    registerHle("sceAudioGetChannelRestLength", [](Ctx* c) {
        int ch = (int)A(c, 0);
        if (ch < 0 || ch >= kAudioChannels) { ret(c, 0x80260003); return; }
        if (g_audioConsumer.load()) {
            std::lock_guard<std::mutex> lk(g_audioMutex);
            ret(c, (uint32_t)(g_audio[ch].queue.size() / 2));
            return;
        }
        uint64_t now = nowUs();
        uint64_t left = g_chanBusyUntil[ch] > now ? g_chanBusyUntil[ch] - now : 0;
        ret(c, (uint32_t)(left * 44100 / 1000000));
    });
    registerHle("sceAudioSetChannelDataLen", [](Ctx* c) {
        int ch = (int)A(c, 0);
        if (ch >= 0 && ch < kAudioChannels) g_audio[ch].sampleCount = A(c, 1);
        ret(c, 0);
    });
    registerHle("sceAudioChangeChannelConfig", [](Ctx* c) {
        int ch = (int)A(c, 0);
        if (ch >= 0 && ch < kAudioChannels) g_audio[ch].format = A(c, 1);
        ret(c, 0);
    });
    registerHle("sceAudioChangeChannelVolume", [](Ctx* c) { ret(c, 0); });
    // sceAudioOutput2 (the "SRC" output used by Contagion): one stereo channel of a1 samples.
    registerHle("sceAudioOutput2Reserve", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_audioMutex);
        AudioChannel& a = g_audio[kSrcChannel];
        if (a.reserved) { ret(c, 0x80268002); return; }
        a.reserved = true;
        a.sampleCount = A(c, 0);
        a.format = 0;
        a.queue.clear();
        ret(c, 0);
    });
    registerHle("sceAudioOutput2Release", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_audioMutex);
        g_audio[kSrcChannel].reserved = false;
        ret(c, 0);
    });
    registerHle("sceAudioOutput2ChangeLength", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_audioMutex);
        g_audio[kSrcChannel].sampleCount = A(c, 0);
        ret(c, 0);
    });
    registerHle("sceAudioOutput2OutputBlocking", [](Ctx* c) {
        int v = (int)A(c, 0);
        ret(c, audioOutput(kSrcChannel, v, v, A(c, 1), true));
    });
    registerHle("sceAudioOutput2GetRestSample", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_audioMutex);
        ret(c, (uint32_t)(g_audio[kSrcChannel].queue.size() / 2));
    });

    // ---- Added for Coded Arms: Contagion
    registerHle("sceDisplayGetFrameBuf", [](Ctx* c) {
        if (A(c, 0)) write32(A(c, 0), g_display.topAddr);
        if (A(c, 1)) write32(A(c, 1), g_display.bufferWidth);
        if (A(c, 2)) write32(A(c, 2), g_display.pixelFormat);
        ret(c, 0);
    });
    registerHle("sceCtrlGetSamplingMode", [](Ctx* c) { if (A(c, 0)) write32(A(c, 0), 1); ret(c, 0); });
    registerHle("sceRtcGetAccumulativeTime", [](Ctx* c) {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(now).count() + 62135596800000000ull;
        c->r[2] = (uint32_t)us;
        c->r[3] = (uint32_t)(us >> 32);
    });
    registerHle("sceUmdWaitDriveStatCB", [](Ctx* c) { runPendingCallbacks(); ret(c, 0); });
    registerHle("sceUmdCancelWaitDriveStat", [](Ctx* c) { ret(c, 0); });

    // ---- sceUtility: see hle_utility.cpp

    // ---- sceUmdUser
    registerHle("sceUmdCheckMedium", [](Ctx* c) { ret(c, 1); });
    registerHle("sceUmdActivate", [](Ctx* c) {
        if (g_umdCallback) notifyCallback(g_umdCallback, 1, kUmdPresent | kUmdReadable);
        ret(c, 0);
    });
    registerHle("sceUmdWaitDriveStat", [](Ctx* c) { ret(c, 0); });
    registerHle("sceUmdGetDriveStat", [](Ctx* c) { ret(c, kUmdPresent | kUmdReady | kUmdReadable); });
    registerHle("sceUmdRegisterUMDCallBack", [](Ctx* c) { g_umdCallback = (int)A(c, 0); ret(c, 0); });
    registerHle("sceUmdUnRegisterUMDCallBack", [](Ctx* c) { g_umdCallback = 0; ret(c, 0); });

    // ---- scePower / sceImpose / sceRtc
    registerHle("scePowerRegisterCallback", [](Ctx* c) { ret(c, 0); });
    registerHle("sceImposeSetLanguageMode", [](Ctx* c) { ret(c, 0); });
    registerHle("sceImposeGetLanguageMode", [](Ctx* c) {
        if (A(c, 0)) write32(A(c, 0), 1);
        if (A(c, 1)) write32(A(c, 1), 1);
        ret(c, 0);
    });
    registerHle("sceRtcGetCurrentTick", [](Ctx* c) {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(now).count() + 62135596800000000ull;
        write32(A(c, 0), (uint32_t)us);
        write32(A(c, 0) + 4, (uint32_t)(us >> 32));
        ret(c, 0);
    });
    registerHle("sceRtcGetCurrentClockLocalTime", [](Ctx* c) {
        std::time_t t = std::time(nullptr);
        std::tm lt = *std::localtime(&t);
        uint32_t p = A(c, 0);
        write16(p, (uint16_t)(lt.tm_year + 1900));
        write16(p + 2, (uint16_t)(lt.tm_mon + 1));
        write16(p + 4, (uint16_t)lt.tm_mday);
        write16(p + 6, (uint16_t)lt.tm_hour);
        write16(p + 8, (uint16_t)lt.tm_min);
        write16(p + 10, (uint16_t)lt.tm_sec);
        write32(p + 12, 0);
        ret(c, 0);
    });

    // ---- network (ad hoc multiplayer): not available yet
    for (const char* n : {"sceNetInit", "sceNetTerm", "sceNetAdhocInit", "sceNetAdhocTerm", "sceNetAdhocctlInit",
                          "sceNetAdhocctlTerm"})
        registerHle(n, [](Ctx* c) { ret(c, 0); });
    for (const char* n : {"sceNetAdhocctlConnect", "sceNetAdhocctlScan", "sceNetAdhocPdpCreate", "sceNetAdhocPtpOpen",
                          "sceNetAdhocPtpListen", "sceNetAdhocPtpConnect"})
        registerHle(n, [](Ctx* c) { ret(c, 0x80410B80); }); // generic ad hoc error
}

} // namespace psp::kernel
