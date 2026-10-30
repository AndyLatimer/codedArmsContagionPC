// sceSasCore: the Media Engine software mixer the game's sound driver uses for all sound
// effects and music voices. Argument validation, error codes and timing follow PPSSPP's
// Core/HLE/sceSas.cpp (GPL-2.0-or-later); the mixer itself is PPSSPP's SasAudio/SasReverb
// (src/psp/ppsspp/). Mixing runs synchronously on the calling thread, which then waits for
// as long as the Media Engine would take to mix the grain.
#include <algorithm>
#include <mutex>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "psp/kernel.h"
#include "psp/memory.h"
#include "psp/ppsspp/SasAudio.h"

void ppsspp_audio_log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    psp::kernel::log("[SAS] %s", buf);
}

namespace psp::kernel {

namespace {

enum : uint32_t {
    SAS_ERROR_INVALID_GRAIN = 0x80420001,
    SAS_ERROR_INVALID_MAX_VOICES = 0x80420002,
    SAS_ERROR_INVALID_OUTPUT_MODE = 0x80420003,
    SAS_ERROR_INVALID_SAMPLE_RATE = 0x80420004,
    SAS_ERROR_BAD_ADDRESS = 0x80420005,
    SAS_ERROR_INVALID_VOICE = 0x80420010,
    SAS_ERROR_INVALID_NOISE_FREQ = 0x80420011,
    SAS_ERROR_INVALID_PITCH = 0x80420012,
    SAS_ERROR_INVALID_ADSR_CURVE_MODE = 0x80420013,
    SAS_ERROR_INVALID_PARAMETER = 0x80420014,
    SAS_ERROR_INVALID_LOOP_POS = 0x80420015,
    SAS_ERROR_VOICE_PAUSED = 0x80420016,
    SAS_ERROR_INVALID_VOLUME = 0x80420018,
    SAS_ERROR_INVALID_ADSR_RATE = 0x80420019,
    SAS_ERROR_REV_INVALID_TYPE = 0x80420020,
    SAS_ERROR_REV_INVALID_FEEDBACK = 0x80420021,
    SAS_ERROR_REV_INVALID_DELAY = 0x80420022,
    SAS_ERROR_REV_INVALID_VOLUME = 0x80420023,
};

SasInstance* g_sas = nullptr;
uint64_t g_meBusyUntilUs = 0;


bool badVoice(int v) { return v < 0 || v >= PSP_SAS_VOICES_MAX; }

// Returns `result` to the guest once the mix job would have finished. A grain takes the Media
// Engine at most a few milliseconds; the estimate is bounded so a bad value cannot park the
// game's sound thread (and the game waiting for it).
void delaySasResult(Ctx* c, uint32_t result) {
    const int est = g_sas->EstimateMixUs();
    uint64_t us = meScheduleJob((uint64_t)std::clamp(est, 0, 20000));
    ret(c, result);
    delayCurrentThread(us);
}

} // namespace

// PPSSPP MEScheduleJob: the Media Engine runs one job after another (SAS mixing, ATRAC
// decoding). Returns how long from now until a job of `us` queued now has finished.
uint64_t meScheduleJob(uint64_t us) {
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    uint64_t now = nowUs();
    us = std::min<uint64_t>(us, 50000);
    // Never more than 100 ms of queued work: the queue cannot run away from real time.
    if (g_meBusyUntilUs > now + 100000) g_meBusyUntilUs = now;
    uint64_t start = std::max(now, g_meBusyUntilUs);
    g_meBusyUntilUs = start + us;
    return g_meBusyUntilUs - now;
}

uint64_t meBusyUntilUs() { return g_meBusyUntilUs; }

void registerSasHle() {
    g_sas = new SasInstance();

    registerHle("__sceSasInit", [](Ctx* c) {
        uint32_t core = A(c, 0), grain = A(c, 1), maxVoices = A(c, 2), outputMode = A(c, 3), sampleRate = A(c, 4);
        log("__sceSasInit(%08x, %u, %u, %u, %u)", core, grain, maxVoices, outputMode, sampleRate);
        if (!validAddress(core) || (core & 0x3F) != 0) { ret(c, SAS_ERROR_BAD_ADDRESS); return; }
        if (maxVoices == 0 || maxVoices > PSP_SAS_VOICES_MAX) { ret(c, SAS_ERROR_INVALID_MAX_VOICES); return; }
        if (grain < 0x40 || grain > 0x800 || (grain & 0x1F) != 0) { ret(c, SAS_ERROR_INVALID_GRAIN); return; }
        if (outputMode != 0 && outputMode != 1) { ret(c, SAS_ERROR_INVALID_OUTPUT_MODE); return; }
        if (sampleRate != 44100) { ret(c, SAS_ERROR_INVALID_SAMPLE_RATE); return; }
        g_sas->SetGrainSize((int)grain);
        // The maxVoices parameter is effectively ignored (PPSSPP).
        g_sas->maxVoices = PSP_SAS_VOICES_MAX;
        g_sas->outputMode = (int)outputMode;
        for (int i = 0; i < g_sas->maxVoices; i++) {
            g_sas->voices[i].sampleRate = (int)sampleRate;
            g_sas->voices[i].playing = false;
            g_sas->voices[i].loop = false;
        }
        ret(c, 0);
    });
    registerHle("__sceSasCore", [](Ctx* c) {
        uint32_t out = A(c, 1);
        if (!validAddress(out)) { ret(c, SAS_ERROR_INVALID_PARAMETER); return; }
        g_sas->Mix(out, 0, 0, 0, false);
        delaySasResult(c, 0);
    });
    registerHle("__sceSasCoreWithMix", [](Ctx* c) {
        uint32_t io = A(c, 1);
        if (!validAddress(io)) { ret(c, SAS_ERROR_INVALID_PARAMETER); return; }
        if (g_sas->outputMode == 1) { ret(c, 0x80000107); return; } // raw output: unsupported (PPSSPP)
        g_sas->Mix(io, io, (int)A(c, 2), (int)A(c, 3), false);
        delaySasResult(c, 0);
    });
    registerHle("__sceSasGetGrain", [](Ctx* c) { ret(c, (uint32_t)g_sas->GetGrainSize()); });
    registerHle("__sceSasSetGrain", [](Ctx* c) { g_sas->SetGrainSize((int)A(c, 1)); ret(c, 0); });
    registerHle("__sceSasGetOutputmode", [](Ctx* c) { ret(c, (uint32_t)g_sas->outputMode); });
    registerHle("__sceSasSetOutputmode", [](Ctx* c) {
        if (A(c, 1) > 1) { ret(c, SAS_ERROR_INVALID_OUTPUT_MODE); return; }
        g_sas->outputMode = (int)A(c, 1);
        ret(c, 0);
    });
    registerHle("__sceSasGetPauseFlag", [](Ctx* c) {
        uint32_t f = 0;
        for (int i = 0; i < PSP_SAS_VOICES_MAX; ++i) if (g_sas->voices[i].paused) f |= 1u << i;
        ret(c, f);
    });
    registerHle("__sceSasSetPause", [](Ctx* c) {
        uint32_t mask = A(c, 1);
        for (int i = 0; i < PSP_SAS_VOICES_MAX; ++i) if (mask & (1u << i)) g_sas->voices[i].paused = A(c, 2) != 0;
        ret(c, 0);
    });
    registerHle("__sceSasGetEndFlag", [](Ctx* c) {
        uint32_t flags = 0;
        for (int i = 0; i < g_sas->maxVoices; i++)
            if (!g_sas->voices[i].playing) flags |= 1u << i;
        ret(c, flags);
    });
    registerHle("__sceSasSetVoice", [](Ctx* c) {
        int v = (int)A(c, 1);
        uint32_t vagAddr = A(c, 2);
        int size = (int)A(c, 3), loop = (int)A(c, 4);
        if (badVoice(v)) { ret(c, SAS_ERROR_INVALID_VOICE); return; }
        if (size == 0 || ((uint32_t)size & 0xF) != 0) { ret(c, SAS_ERROR_INVALID_PARAMETER); return; }
        if (loop != 0 && loop != 1) { ret(c, SAS_ERROR_INVALID_LOOP_POS); return; }
        if (!validAddress(vagAddr)) { ret(c, 0); return; } // ignored, returns OK
        SasVoice& sv = g_sas->voices[v];
        if (size < 0) size = 0; // PPSSPP: negative sizes return OK but play nothing
        bool reset = sv.type != VOICETYPE_VAG || sv.vagAddr != vagAddr || sv.vagSize != (uint32_t)size || sv.loop != (loop != 0);
        sv.type = VOICETYPE_VAG;
        sv.vagAddr = vagAddr;
        sv.vagSize = (uint32_t)size;
        sv.loop = loop != 0;
        if (sv.on) sv.playing = true;
        if (reset) sv.vag.Start(vagAddr, (uint32_t)size, loop != 0);
        ret(c, 0);
    });
    registerHle("__sceSasSetVolume", [](Ctx* c) {
        int v = (int)A(c, 1);
        int l = (int)A(c, 2), r = (int)A(c, 3), el = (int)A(c, 4), er = (int)A(c, 5);
        if (badVoice(v)) { ret(c, SAS_ERROR_INVALID_VOICE); return; }
        if (std::abs(l) > PSP_SAS_VOL_MAX || std::abs(r) > PSP_SAS_VOL_MAX || std::abs(el) > PSP_SAS_VOL_MAX ||
            std::abs(er) > PSP_SAS_VOL_MAX) { ret(c, SAS_ERROR_INVALID_VOLUME); return; }
        SasVoice& sv = g_sas->voices[v];
        sv.volumeLeft = l;
        sv.volumeRight = r;
        sv.effectLeft = el;
        sv.effectRight = er;
        ret(c, 0);
    });
    registerHle("__sceSasSetPitch", [](Ctx* c) {
        int v = (int)A(c, 1), pitch = (int)A(c, 2);
        if (badVoice(v)) { ret(c, SAS_ERROR_INVALID_VOICE); return; }
        if (pitch < PSP_SAS_PITCH_MIN || pitch > PSP_SAS_PITCH_MAX) { ret(c, SAS_ERROR_INVALID_PITCH); return; }
        g_sas->voices[v].pitch = pitch;
        ret(c, 0);
    });
    registerHle("__sceSasSetKeyOn", [](Ctx* c) {
        int v = (int)A(c, 1);
        if (badVoice(v)) { ret(c, SAS_ERROR_INVALID_VOICE); return; }
        if (g_sas->voices[v].paused || g_sas->voices[v].on) { ret(c, SAS_ERROR_VOICE_PAUSED); return; }
        g_sas->voices[v].KeyOn();
        ret(c, 0);
    });
    registerHle("__sceSasSetKeyOff", [](Ctx* c) {
        int v = (int)A(c, 1);
        if (badVoice(v)) { ret(c, SAS_ERROR_INVALID_VOICE); return; }
        if (g_sas->voices[v].paused || !g_sas->voices[v].on) { ret(c, SAS_ERROR_VOICE_PAUSED); return; }
        g_sas->voices[v].KeyOff();
        ret(c, 0);
    });
    registerHle("__sceSasSetNoise", [](Ctx* c) {
        int v = (int)A(c, 1), freq = (int)A(c, 2);
        if (badVoice(v)) { ret(c, SAS_ERROR_INVALID_VOICE); return; }
        if (freq < 0 || freq >= 64) { ret(c, SAS_ERROR_INVALID_NOISE_FREQ); return; }
        g_sas->voices[v].type = VOICETYPE_NOISE;
        g_sas->voices[v].noiseFreq = freq;
        ret(c, 0);
    });
    registerHle("__sceSasSetSL", [](Ctx* c) {
        int v = (int)A(c, 1);
        if (badVoice(v)) { ret(c, SAS_ERROR_INVALID_VOICE); return; }
        g_sas->voices[v].envelope.SetSustainLevel((int)A(c, 2));
        ret(c, 0);
    });
    registerHle("__sceSasSetADSR", [](Ctx* c) {
        int v = (int)A(c, 1), flag = (int)A(c, 2);
        int a = (int)A(c, 3), d = (int)A(c, 4), s = (int)A(c, 5), r = (int)A(c, 6);
        if (badVoice(v)) { ret(c, SAS_ERROR_INVALID_VOICE); return; }
        int invalid = (a < 0 ? 0x1 : 0) | (d < 0 ? 0x2 : 0) | (s < 0 ? 0x4 : 0) | (r < 0 ? 0x8 : 0);
        if (invalid & flag) { ret(c, SAS_ERROR_INVALID_ADSR_RATE); return; }
        g_sas->voices[v].envelope.SetRate(flag, a, d, s, r);
        ret(c, 0);
    });
    registerHle("__sceSasSetADSRmode", [](Ctx* c) {
        int v = (int)A(c, 1), flag = (int)A(c, 2);
        // The PSP ignores the top bit of these values.
        int a = (int)(A(c, 3) & 0x7FFFFFFF), d = (int)(A(c, 4) & 0x7FFFFFFF);
        int s = (int)(A(c, 5) & 0x7FFFFFFF), r = (int)(A(c, 6) & 0x7FFFFFFF);
        if (badVoice(v)) { ret(c, SAS_ERROR_INVALID_VOICE); return; }
        int invalid = 0;
        if (a > 5 || (a & 1) != 0) invalid |= 0x1;
        if (d > 5 || (d & 1) != 1) invalid |= 0x2;
        if (s > 5) invalid |= 0x4;
        if (r > 5 || (r & 1) != 1) invalid |= 0x8;
        if (invalid & flag) { ret(c, SAS_ERROR_INVALID_ADSR_CURVE_MODE); return; }
        g_sas->voices[v].envelope.SetEnvelope(flag, a, d, s, r);
        ret(c, 0);
    });
    registerHle("__sceSasSetSimpleADSR", [](Ctx* c) {
        int v = (int)A(c, 1);
        uint32_t env1 = A(c, 2), env2 = A(c, 3);
        if (badVoice(v)) { ret(c, SAS_ERROR_INVALID_VOICE); return; }
        if ((env2 >> 13) & 1) { ret(c, SAS_ERROR_INVALID_ADSR_CURVE_MODE); return; }
        g_sas->voices[v].envelope.SetSimpleEnvelope(env1 & 0xFFFF, env2 & 0xFFFF);
        ret(c, 0);
    });
    registerHle("__sceSasGetEnvelopeHeight", [](Ctx* c) {
        int v = (int)A(c, 1);
        if (badVoice(v)) { ret(c, SAS_ERROR_INVALID_VOICE); return; }
        ret(c, (uint32_t)g_sas->voices[v].envelope.GetHeight());
    });
    registerHle("__sceSasRevType", [](Ctx* c) {
        int type = (int)A(c, 1);
        if (type < PSP_SAS_EFFECT_TYPE_OFF || type > PSP_SAS_EFFECT_TYPE_MAX) { ret(c, SAS_ERROR_REV_INVALID_TYPE); return; }
        g_sas->SetWaveformEffectType(type);
        ret(c, 0);
    });
    registerHle("__sceSasRevParam", [](Ctx* c) {
        int delay = (int)A(c, 1), feedback = (int)A(c, 2);
        if (delay < 0 || delay >= 128) { ret(c, SAS_ERROR_REV_INVALID_DELAY); return; }
        if (feedback < 0 || feedback >= 128) { ret(c, SAS_ERROR_REV_INVALID_FEEDBACK); return; }
        g_sas->SetWaveformEffectParams(delay, feedback);
        ret(c, 0);
    });
    registerHle("__sceSasRevEVOL", [](Ctx* c) {
        uint32_t lv = A(c, 1), rv = A(c, 2);
        if (lv > 0x1000 || rv > 0x1000) { ret(c, SAS_ERROR_REV_INVALID_VOLUME); return; }
        g_sas->waveformEffect.leftVol = (int)lv;
        g_sas->waveformEffect.rightVol = (int)rv;
        ret(c, 0);
    });
    registerHle("__sceSasRevVON", [](Ctx* c) {
        g_sas->waveformEffect.isDryOn = A(c, 1) != 0;
        g_sas->waveformEffect.isWetOn = A(c, 2) != 0;
        ret(c, 0);
    });
}

} // namespace psp::kernel
