// sceAtrac3plus HLE: the ATRAC3/ATRAC3+ player library Contagion's FMOD uses for its music
// streams (USRDIR/SOUND/CD/*.AT3).
//
// Ported from PPSSPP Core/HLE/sceAtrac.cpp (github.com/hrydgard/ppsspp, see
// ppsspp/atrac/PPSSPP_COMMIT), modified for the Coded Arms: Contagion PC port: only the
// functions the game imports, contexts are always the host-side Atrac class (AtracCtx.cpp; the
// library is HLE, so there is no BSS for AtracCtx2), hleDelayResult maps to delayCurrentThread
// and decoded samples are written at once instead of when the delay ends. The player itself
// (ppsspp/atrac/Core/HLE/AtracCtx.cpp, Core/Util/AtracTrack.cpp) is PPSSPP's.
//
// Copyright (c) 2012- PPSSPP Project.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.
//
// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string>

#include "Core/HLE/AtracCtx.h"
#include "Core/HLE/AtracCtx2.h"
#include "Core/HLE/sceAtrac.h"
#include "Core/HLE/ErrorCodes.h"
#include "Core/HLE/sceAudiocodec.h"
#include "Core/Util/AtracTrack.h"
#include "psp/kernel.h"

AtracKernelMemoryStub kernelMemory;

const char* AtracStatusToString(AtracStatus status) {
    switch (status) {
    case ATRAC_STATUS_NO_DATA: return "NO_DATA";
    case ATRAC_STATUS_ALL_DATA_LOADED: return "ALL_DATA_LOADED";
    case ATRAC_STATUS_HALFWAY_BUFFER: return "HALFWAY_BUFFER";
    case ATRAC_STATUS_STREAMED_WITHOUT_LOOP: return "STREAMED_WITHOUT_LOOP";
    case ATRAC_STATUS_STREAMED_LOOP_FROM_END: return "STREAMED_LOOP_FROM_END";
    case ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER: return "STREAMED_LOOP_WITH_TRAILER";
    case ATRAC_STATUS_LOW_LEVEL: return "LOW_LEVEL";
    case ATRAC_STATUS_FOR_SCESAS: return "FOR_SCESAS";
    default: return "(unknown!)";
    }
}

// From PPSSPP Core/HLE/sceAudiocodec.cpp: the Atrac3 layouts libatrac3plus.prx picks.
namespace {
struct At3Param {
    u8 param;
    u16 bytes;
    u8 channels;
    u8 jointStereo;
};
const At3Param at3Params[] = {
    {0x04, 0x0180, 2, 0}, {0x06, 0x0130, 2, 0}, {0x0B, 0x00C0, 2, 1}, {0x0E, 0x00C0, 1, 0}, {0x0F, 0x0098, 1, 0},
};
} // namespace

bool Atrac3DecoderChannels(int bytesPerFrame, bool jointStereo, int* channels) {
    for (int i = ARRAY_SIZE(at3Params) - 1; i >= 0; i--) {
        if (at3Params[i].bytes == bytesPerFrame && (at3Params[i].jointStereo != 0) == jointStereo) {
            *channels = at3Params[i].channels;
            return true;
        }
    }
    return false;
}

bool IsAtrac3StreamJointStereo(int codecType, int bytesPerFrame, int channels) {
    if (codecType != PSP_CODEC_AT3) return false;
    for (const At3Param& p : at3Params)
        if (p.bytes == bytesPerFrame && p.channels == channels) return p.jointStereo != 0;
    return false;
}

// Media Engine time at the default clock (PPSSPP sceAudiocodec.cpp).
int AudioCodecInitUs(int codec, bool monoAt3Plus) {
    switch (codec) {
    case PSP_CODEC_AT3PLUS: return monoAt3Plus ? 524 : 646;
    case PSP_CODEC_AT3: return 210;
    case PSP_CODEC_MP3: return 517;
    default: return 230;
    }
}

int AudioCodecDecodeUs(int codec, int channels, int frameBytes) {
    switch (codec) {
    case PSP_CODEC_AT3PLUS: return channels == 1 ? 1500 + frameBytes * 84 / 100 : 1815 + frameBytes * 158 / 100;
    case PSP_CODEC_AT3: return channels == 1 ? 685 : 970 + frameBytes * 44 / 100;
    default: return 0;
    }
}

namespace psp::kernel {

namespace {

constexpr int kMaxIds = PSP_MAX_ATRAC_IDS;
std::mutex g_atracMutex; // the FMOD streamer and decoder threads both use the contexts
AtracBase* g_contexts[kMaxIds] = {};
u32 g_contextTypes[kMaxIds] = {PSP_CODEC_AT3PLUS, PSP_CODEC_AT3PLUS, PSP_CODEC_AT3, PSP_CODEC_AT3, 0, 0};
const int g_maxContexts = 6;
bool g_verbose = std::getenv("CA_ATRAC_VERBOSE") != nullptr;

// The contexts (SceAtracContext, 256 bytes each) live in guest memory, like the library's BSS:
// AtracCtx2 decodes straight from the game's buffers. (The older AtracCtx copies stream data
// when it is added, and FMOD adds data before its asynchronous read into that space ends.)
u32 g_bss = 0;

AtracBase* getAtrac(int id) {
    if (id < 0 || id >= kMaxIds) return nullptr;
    AtracBase* a = g_contexts[id];
    if (a) a->UpdateContextFromPSPMem();
    return a;
}

int allocAtrac(int codecType) {
    if (!g_bss) g_bss = allocHleMemory(kMaxIds * sizeof(SceAtracContext), "sceAtrac contexts");
    for (int i = 0; i < g_maxContexts; ++i) {
        if (g_contextTypes[i] == (u32)codecType && !g_contexts[i]) {
            if (g_bss) g_contexts[i] = new Atrac2(g_bss + i * (u32)sizeof(SceAtracContext), codecType);
            else g_contexts[i] = new Atrac(i, codecType);
            return i;
        }
    }
    return SCE_ERROR_ATRAC_NO_ATRACID;
}

int releaseAtrac(int id) {
    if (id >= 0 && id < kMaxIds && g_contexts[id]) {
        delete g_contexts[id];
        g_contexts[id] = nullptr;
        return 0;
    }
    return SCE_ERROR_ATRAC_BAD_ATRACID;
}

int validateData(const AtracBase* a) {
    if (!a) return SCE_ERROR_ATRAC_BAD_ATRACID;
    if (a->BufferState() == ATRAC_STATUS_NO_DATA) return SCE_ERROR_ATRAC_NO_DATA;
    return 0;
}

int validateManaged(const AtracBase* a) {
    if (int e = validateData(a)) return e;
    if (a->BufferState() == ATRAC_STATUS_LOW_LEVEL) return SCE_ERROR_ATRAC_IS_LOW_LEVEL;
    if (a->BufferState() == ATRAC_STATUS_FOR_SCESAS) return SCE_ERROR_ATRAC_IS_FOR_SCESAS;
    return 0;
}

int frameUs(const AtracBase* a) { return AudioCodecDecodeUs(a->CodecType(), a->Channels(), a->BytesPerFrame()); }

uint64_t setDataDelay(const AtracBase* a) {
    return meScheduleJob((uint64_t)(AudioCodecInitUs(a->CodecType(), a->Channels() == 1) + a->SkippedFramesOnSetData() * frameUs(a)));
}

// Returns `v` to the guest after `us` (PPSSPP hleDelayResult).
void delayedRet(Ctx* c, uint32_t v, uint64_t us) {
    ret(c, v);
    delayCurrentThread(us ? us : 1);
}

void writeIf(u32 addr, u32 v) {
    if (Memory::IsValidAddress(addr)) psp::write32(addr, v);
}

int setDataError(Ctx* c, int r, const Track& track) {
    if (r != (int)SCE_ERROR_ATRAC_API_FAIL) return r;
    // The codec was set up and the first frame tried on the ME before failing.
    const int us = AudioCodecInitUs(track.codecType, track.channels == 1) + (track.codecType == PSP_CODEC_AT3PLUS ? 214 : 169);
    delayCurrentThread(meScheduleJob((uint64_t)us));
    return r;
}

void trace(const char* fn, Ctx* c) {
    if (g_verbose) log("%s(%08x, %08x, %08x, %08x, %08x) -> %08x", fn, c->r[4], c->r[5], c->r[6], c->r[7], c->r[8], c->r[2]);
}

} // namespace

void registerAtracHle() {
    registerHle("sceAtracSetDataAndGetID", [](Ctx* c) {
        u32 buffer = A(c, 0);
        int size = (int)A(c, 1);
        if (size < 0) size = 0x10000000;
        Track track;
        std::string error;
        std::unique_lock<std::mutex> lk(g_atracMutex);
        int r = AnalyzeAtracTrack(buffer ? Memory::GetPointer(buffer) : nullptr, (u32)size, &track, &error);
        if (r < 0) { log("sceAtracSetDataAndGetID: %s (%08x)", error.c_str(), r); ret(c, (u32)r); return; }
        int id = allocAtrac(track.codecType);
        if (id < 0) { ret(c, (u32)id); return; }
        r = g_contexts[id]->SetData(track, buffer, (u32)size, (u32)size, 0, 2, false);
        if (r < 0) {
            releaseAtrac(id);
            lk.unlock();
            ret(c, (u32)setDataError(c, r, track));
            return;
        }
        uint64_t d = setDataDelay(g_contexts[id]);
        log("sceAtracSetDataAndGetID(%08x, %d) -> id %d (%s, %d ch, %d B/frame)", buffer, size, id,
            track.codecType == PSP_CODEC_AT3PLUS ? "ATRAC3+" : "ATRAC3", track.channels, track.bytesPerFrame);
        lk.unlock();
        delayedRet(c, (u32)id, d);
    });
    registerHle("sceAtracSetData", [](Ctx* c) {
        int id = (int)A(c, 0);
        u32 buffer = A(c, 1), size = A(c, 2);
        std::unique_lock<std::mutex> lk(g_atracMutex);
        AtracBase* a = getAtrac(id);
        if (!a) { ret(c, SCE_ERROR_ATRAC_BAD_ATRACID); return; }
        Track track;
        std::string error;
        int r = AnalyzeAtracTrack(Memory::GetPointer(buffer), size, &track, &error);
        if (r < 0) { ret(c, (u32)r); return; }
        if ((u32)track.codecType != g_contextTypes[id]) { ret(c, SCE_ERROR_ATRAC_WRONG_CODECTYPE); return; }
        r = a->SetData(track, buffer, size, size, 0, 2, false);
        if (r < 0) { lk.unlock(); ret(c, (u32)setDataError(c, r, track)); return; }
        uint64_t d = setDataDelay(a);
        lk.unlock();
        delayedRet(c, (u32)r, d);
    });
    registerHle("sceAtracReleaseAtracID", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_atracMutex);
        ret(c, (u32)releaseAtrac((int)A(c, 0)));
    });
    registerHle("sceAtracAddStreamData", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_atracMutex);
        AtracBase* a = getAtrac((int)A(c, 0));
        if (int e = validateManaged(a)) { ret(c, (u32)e); return; }
        if (a->BufferState() == ATRAC_STATUS_ALL_DATA_LOADED) { ret(c, SCE_ERROR_ATRAC_ALL_DATA_LOADED); return; }
        ret(c, (u32)a->AddStreamData(A(c, 1)));
        trace("sceAtracAddStreamData", c);
    });
    registerHle("sceAtracDecodeData", [](Ctx* c) {
        int id = (int)A(c, 0);
        u32 outAddr = A(c, 1), numSamplesAddr = A(c, 2), finishAddr = A(c, 3), remainAddr = A(c, 4);
        std::unique_lock<std::mutex> lk(g_atracMutex);
        AtracBase* a = getAtrac(id);
        if (int e = validateData(a)) { ret(c, (u32)e); return; }
        if (outAddr & 1) { ret(c, SCE_ERROR_ATRAC_BAD_ALIGNMENT); return; }
        if (outAddr && !Memory::IsValidAddress(outAddr)) { ret(c, SCE_ERROR_ATRAC_SIZE_TOO_SMALL); return; }
        u8* out = outAddr ? Memory::GetPointerWrite(outAddr) : nullptr;
        int samples = 0, finish = 0, remains = 0;
        int r = (int)a->DecodeData(out, outAddr, &samples, &finish, &remains);
        if (r != (int)SCE_ERROR_ATRAC_BAD_ATRACID && r != (int)SCE_ERROR_ATRAC_NO_DATA) {
            writeIf(numSamplesAddr, (u32)samples);
            writeIf(finishAddr, (u32)finish);
            if (r == 0) writeIf(remainAddr, (u32)remains);
        }
        if (g_verbose) log("sceAtracDecodeData(%d, %08x) -> %08x samples=%d finish=%d remain=%d", id, outAddr, r, samples, finish, remains);
        if (r == 0 || r == (int)SCE_ERROR_ATRAC_API_FAIL) {
            uint64_t d = meScheduleJob((uint64_t)frameUs(a));
            lk.unlock();
            delayedRet(c, (u32)r, d);
            return;
        }
        ret(c, (u32)r);
    });
    registerHle("sceAtracGetStreamDataInfo", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_atracMutex);
        AtracBase* a = getAtrac((int)A(c, 0));
        if (int e = validateManaged(a)) { ret(c, (u32)e); return; }
        u32 writePtr = 0, writable = 0, readOffset = 0;
        a->GetStreamDataInfo(&writePtr, &writable, &readOffset);
        writeIf(A(c, 1), writePtr);
        writeIf(A(c, 2), writable);
        writeIf(A(c, 3), readOffset);
        ret(c, 0);
        if (g_verbose) log("sceAtracGetStreamDataInfo(%d) -> write %08x, %u bytes, file offset %u", (int)A(c, 0), writePtr, writable, readOffset);
    });
    registerHle("sceAtracGetSecondBufferInfo", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_atracMutex);
        AtracBase* a = getAtrac((int)A(c, 0));
        if (int e = validateManaged(a)) { ret(c, (u32)e); return; }
        if (!Memory::IsValidAddress(A(c, 1)) || !Memory::IsValidAddress(A(c, 2))) { ret(c, SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
        u32 off = 0, size = 0;
        int r = a->GetSecondBufferInfo(&off, &size);
        psp::write32(A(c, 1), off);
        psp::write32(A(c, 2), size);
        ret(c, (u32)r);
    });
    registerHle("sceAtracSetSecondBuffer", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_atracMutex);
        AtracBase* a = getAtrac((int)A(c, 0));
        if (int e = validateManaged(a)) { ret(c, (u32)e); return; }
        ret(c, (u32)a->SetSecondBuffer(A(c, 1), A(c, 2)));
    });
    registerHle("sceAtracGetChannel", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_atracMutex);
        AtracBase* a = getAtrac((int)A(c, 0));
        if (int e = validateData(a)) { ret(c, (u32)e); return; }
        writeIf(A(c, 1), (u32)a->Channels());
        ret(c, 0);
    });
    registerHle("sceAtracGetBitrate", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_atracMutex);
        AtracBase* a = getAtrac((int)A(c, 0));
        if (int e = validateData(a)) { ret(c, (u32)e); return; }
        writeIf(A(c, 1), (u32)a->Bitrate());
        ret(c, 0);
    });
    registerHle("sceAtracGetSoundSample", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_atracMutex);
        AtracBase* a = getAtrac((int)A(c, 0));
        if (int e = validateManaged(a)) { ret(c, (u32)e); return; }
        int end = -1, loopStart = -1, loopEnd = -1;
        int r = a->GetSoundSample(&end, &loopStart, &loopEnd);
        if (r < 0) { ret(c, (u32)r); return; }
        writeIf(A(c, 1), (u32)end);
        writeIf(A(c, 2), (u32)loopStart);
        writeIf(A(c, 3), (u32)loopEnd);
        ret(c, (u32)r);
    });
    registerHle("sceAtracSetLoopNum", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_atracMutex);
        AtracBase* a = getAtrac((int)A(c, 0));
        if (int e = validateData(a)) { ret(c, (u32)e); return; }
        ret(c, (u32)a->SetLoopNum((int)A(c, 1)));
    });
    registerHle("sceAtracGetRemainFrame", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_atracMutex);
        AtracBase* a = getAtrac((int)A(c, 0));
        if (int e = validateManaged(a)) { ret(c, (u32)e); return; }
        if (!Memory::IsValidAddress(A(c, 1))) { ret(c, SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
        psp::write32(A(c, 1), (u32)a->RemainingFrames());
        ret(c, 0);
    });
    registerHle("sceAtracGetInternalErrorInfo", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_atracMutex);
        AtracBase* a = getAtrac((int)A(c, 0));
        if (int e = validateData(a)) { ret(c, (u32)e); return; }
        writeIf(A(c, 1), a->GetInternalCodecError());
        ret(c, 0);
    });
}

} // namespace psp::kernel
