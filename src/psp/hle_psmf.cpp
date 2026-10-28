// scePsmf HLE: the PSMF header library (Sony's PSMF.PRX, encrypted on the disc). Contagion's
// movie player, Konami's PSMF_JK.PRX ("jkPsmfPlayer"), is recompiled with the game and uses
// it together with sceMpeg (hle_mpeg.cpp). The player's API is Konami's own, not Sony's
// scePsmfPlayer, even though it reuses those NIDs.
//
// Ported from PPSSPP Core/HLE/scePsmf.cpp (github.com/hrydgard/ppsspp), modified for the
// Coded Arms: Contagion PC port: only the scePsmf functions the game's modules import, the
// Psmf class without savestates.
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
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "psp/gpu/gpu.h"
#include "psp/iso.h"
#include "psp/kernel.h"
#include "psp/memory.h"
#include "ppsspp/MediaEngine.h"
#include "ppsspp/sceMpeg.h"

namespace {

using namespace psp;
using psp::kernel::A;
using psp::kernel::ret;

// Error codes (PPSSPP Core/HLE/ErrorCodes.h)
enum : u32 {
    SCE_KERNEL_ERROR_PRIV_REQUIRED = 0x80000023,
    SCE_KERNEL_ERROR_INVALID_POINTER = 0x80000103,
    SCE_KERNEL_ERROR_INVALID_VALUE = 0x800001FE,
    SCE_KERNEL_ERROR_ILLEGAL_ADDRESS = 0x8002006a,
    SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT = 0x800200d2,
    SCE_MPEG_ERROR_ALREADY_INIT = 0x80618005,
    SCE_PSMF_ERROR_NOT_INITIALIZED = 0x80615001,
    SCE_PSMF_ERROR_BAD_VERSION = 0x80615002,
    SCE_PSMF_ERROR_NOT_FOUND = 0x80615025,
    SCE_PSMF_ERROR_INVALID_ID = 0x80615100,
    SCE_PSMF_ERROR_INVALID_TIMESTAMP = 0x80615500,
    SCE_PSMF_ERROR_INVALID_VALUE = 0x806151fe,
    SCE_PSMF_ERROR_INVALID_PSMF = 0x80615501,
    SCE_PSMFPLAYER_ERROR_INVALID_STATUS = 0x80616001,
    SCE_PSMFPLAYER_ERROR_INVALID_STREAM = 0x80616003,
    SCE_PSMFPLAYER_ERROR_BUFFER_SIZE = 0x80616005,
    SCE_PSMFPLAYER_ERROR_INVALID_CONFIG = 0x80616006,
    SCE_PSMFPLAYER_ERROR_INVALID_PARAM = 0x80616008,
    SCE_PSMFPLAYER_ERROR_NO_MORE_DATA = 0x8061600c,
};

const int PSMF_AVC_STREAM = 0;
const int PSMF_ATRAC_STREAM = 1;
const int PSMF_PCM_STREAM = 2;
const int PSMF_AUDIO_STREAM = 15;
const int PSMF_PLAYER_VERSION_FULL = 0;
const int PSMF_PLAYER_VERSION_BASIC = 1;
const int PSMF_PLAYER_CONFIG_LOOP = 0;
const int PSMF_PLAYER_CONFIG_NO_LOOP = 1;
const int PSMF_PLAYER_WARMUP_FRAMES = 3;
const int VIDEO_FRAME_DURATION_TS = 3003;
const int audioSamples = 2048;
const int audioSamplesBytes = audioSamples * 4;

int videoPixelMode = GE_CMODE_32BIT_ABGR8888;
int videoLoopStatus = PSMF_PLAYER_CONFIG_NO_LOOP;

enum PsmfPlayerStatus {
    PSMF_PLAYER_STATUS_NONE = 0x0,
    PSMF_PLAYER_STATUS_INIT = 0x1,
    PSMF_PLAYER_STATUS_STANDBY = 0x2,
    PSMF_PLAYER_STATUS_PLAYING = 0x4,
    PSMF_PLAYER_STATUS_ERROR = 0x100,
    PSMF_PLAYER_STATUS_PLAYING_FINISHED = 0x200,
};

enum PsmfPlayerMode {
    PSMF_PLAYER_MODE_PLAY = 0,
    PSMF_PLAYER_MODE_SLOWMOTION = 1,
    PSMF_PLAYER_MODE_STEPFRAME = 2,
    PSMF_PLAYER_MODE_PAUSE = 3,
    PSMF_PLAYER_MODE_FORWARD = 4,
    PSMF_PLAYER_MODE_REWIND = 5,
};

struct PsmfData {
    u32_le version, headerSize, headerOffset, streamSize, streamOffset, streamNum, unk1, unk2;
};
struct PsmfPlayerCreateData {
    u32_le buffer;
    u32_le bufferSize;
    s32_le threadPriority;
};
struct PsmfPlayerData {
    s32_le videoCodec, videoStreamNum, audioCodec, audioStreamNum, playMode, playSpeed;
};
struct PsmfVideoData {
    s32_le frameWidth;
    u32_le displaybuf;
    u32_le displaypts;
};
struct PsmfEntry {
    int EPPts, EPOffset, EPIndex, EPPicOffset;
};

template <class T> T* guest(u32 addr) { return reinterpret_cast<T*>(hostPtr(addr)); }
u32 readU32BE(const u8* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
u16 readU16BE(const u8* p) { return (u16)((p[0] << 8) | p[1]); }

class PsmfStream;

class Psmf {
public:
    Psmf(const u8* ptr, u32 data);
    ~Psmf();
    bool isValidCurrentStreamNumber() const {
        return currentStreamNum >= 0 && streamMap.find(currentStreamNum) != streamMap.end();
    }
    bool setStreamNum(u32 psmfStruct, int num, bool updateCached = true);
    bool setStreamWithType(u32 psmfStruct, int type, int channel);
    bool setStreamWithTypeNumber(u32 psmfStruct, int type, int n);
    int FindEPWithTimestamp(int pts) const;
    int currentStreamType = -1, currentStreamChannel = -1;

    u32 magic = 0, version = 0, streamOffset = 0, streamSize = 0, headerSize = 0, headerOffset = 0;
    u32 presentationStartTime = 0, presentationEndTime = 0;
    int numStreams = 0, currentStreamNum = 0;
    u32 EPMapOffset = 0, EPMapEntriesNum = 0;
    int videoWidth = 0, videoHeight = 0;
    std::vector<PsmfEntry> EPMap;
    std::map<int, PsmfStream*> streamMap;
};

class PsmfStream {
public:
    enum { USE_PSMF = -2, INVALID = -1 };
    PsmfStream(int type, int channel) : type_(type), channel_(channel) {}

    void readMPEGVideoStreamParams(const u8* addr, const u8* data, Psmf* psmf) {
        psmf->EPMapOffset = readU32BE(&addr[4]);
        psmf->EPMapEntriesNum = readU32BE(&addr[8]);
        videoWidth_ = addr[12] * 16;
        videoHeight_ = addr[13] * 16;
        const u32 stride = 10;
        const u64 bytes = stride * (u64)psmf->EPMapEntriesNum;
        if (psmf->headerOffset != 0) {
            u32 a = psmf->headerOffset + psmf->EPMapOffset;
            if (bytes > 0xFFFFFFFFull - psmf->EPMapOffset || !validAddress(a) || !validAddress(a + (u32)bytes))
                psmf->EPMapEntriesNum = 0;
        } else if (bytes > 0x10000) {
            psmf->EPMapEntriesNum = 0x10000 / stride;
        }
        psmf->EPMap.clear();
        for (u32 i = 0; i < psmf->EPMapEntriesNum; i++) {
            const u8* e = data + psmf->EPMapOffset + stride * i;
            PsmfEntry entry;
            entry.EPIndex = e[0];
            entry.EPPicOffset = e[1];
            entry.EPPts = (int)readU32BE(&e[2]);
            entry.EPOffset = (int)readU32BE(&e[6]);
            psmf->EPMap.push_back(entry);
        }
    }
    bool matchesType(int ty) const {
        if (ty == PSMF_AUDIO_STREAM) return type_ == PSMF_ATRAC_STREAM || type_ == PSMF_PCM_STREAM;
        return type_ == ty;
    }
    void readPrivateAudioStreamParams(const u8* addr) {
        audioChannels_ = addr[14];
        audioFrequency_ = addr[15];
    }

    int type_, channel_;
    int videoWidth_ = INVALID, videoHeight_ = INVALID, audioChannels_ = INVALID, audioFrequency_ = INVALID;
};

Psmf::Psmf(const u8* ptr, u32 data) {
    headerOffset = data;
    std::memcpy(&magic, ptr, 4);
    std::memcpy(&version, ptr + 4, 4);
    streamOffset = readU32BE(&ptr[8]);
    streamSize = readU32BE(&ptr[12]);
    presentationStartTime = (u32)getMpegTimeStamp(ptr + PSMF_FIRST_TIMESTAMP_OFFSET);
    presentationEndTime = (u32)getMpegTimeStamp(ptr + PSMF_LAST_TIMESTAMP_OFFSET);
    numStreams = readU16BE(&ptr[0x80]);
    headerSize = 0x800;
    if (numStreams > 128) numStreams = 0;
    currentStreamNum = -1;
    for (int i = 0; i < numStreams; i++) {
        PsmfStream* stream = nullptr;
        const u8* s = ptr + 0x82 + i * 16;
        int streamId = s[0];
        if ((streamId & PSMF_VIDEO_STREAM_ID) == PSMF_VIDEO_STREAM_ID) {
            stream = new PsmfStream(PSMF_AVC_STREAM, streamId & 0x0F);
            stream->readMPEGVideoStreamParams(s, ptr, this);
        } else if ((streamId & PSMF_AUDIO_STREAM_ID) == PSMF_AUDIO_STREAM_ID) {
            int privateStreamId = s[1];
            stream = new PsmfStream((privateStreamId & 0xF0) ? PSMF_PCM_STREAM : PSMF_ATRAC_STREAM, privateStreamId & 0x0F);
            stream->readPrivateAudioStreamParams(s);
        }
        if (stream) streamMap[++currentStreamNum] = stream;
    }
    currentStreamNum = 0;
}

Psmf::~Psmf() {
    for (auto& [n, s] : streamMap) delete s;
}

std::mutex g_psmfMutex;
std::map<u32, Psmf*> psmfMap;

Psmf* getPsmf(u32 psmf) {
    if (!validAddress(psmf)) return nullptr;
    auto it = psmfMap.find(guest<PsmfData>(psmf)->headerOffset);
    if (it == psmfMap.end()) return nullptr;
    it->second->currentStreamNum = (int)guest<PsmfData>(psmf)->streamNum;
    return it->second;
}

bool Psmf::setStreamNum(u32 psmfStruct, int num, bool updateCached) {
    currentStreamNum = num;
    guest<PsmfData>(psmfStruct)->streamNum = (u32)num;
    if (updateCached) currentStreamType = currentStreamChannel = -1;
    if (!isValidCurrentStreamNumber()) return false;
    auto it = streamMap.find(currentStreamNum);
    if (it == streamMap.end()) return false;
    currentStreamType = it->second->type_;
    currentStreamChannel = it->second->channel_;
    return true;
}

bool Psmf::setStreamWithType(u32 psmfStruct, int type, int channel) {
    for (const auto& [n, st] : streamMap)
        if (st->type_ == type && st->channel_ == channel) return setStreamNum(psmfStruct, n);
    return false;
}

bool Psmf::setStreamWithTypeNumber(u32 psmfStruct, int type, int k) {
    for (const auto& [n, st] : streamMap) {
        if (!st->matchesType(type)) continue;
        if (k != 0) { k--; continue; }
        return setStreamNum(psmfStruct, n);
    }
    return false;
}

int Psmf::FindEPWithTimestamp(int pts) const {
    int best = -1, bestPts = 0;
    for (int i = 0; i < (int)EPMap.size(); ++i) {
        const int m = EPMap[i].EPPts;
        if (m == pts) return i;
        if (m < pts && m >= bestPts) { best = i; bestPts = m; }
    }
    return best;
}

} // namespace

namespace psp::kernel {

void registerPsmfHle() {
    // ---- scePsmf
    registerHle("scePsmfSetPsmf", [](Ctx* c) {
        u32 psmfStruct = A(c, 0), psmfData = A(c, 1);
        if (!validAddress(psmfStruct) || !validAddress(psmfData)) { ret(c, SCE_KERNEL_ERROR_ILLEGAL_ADDRESS); return; }
        std::lock_guard<std::mutex> lk(g_psmfMutex);
        Psmf* psmf = new Psmf(hostPtr(psmfData), psmfData);
        u32 err = psmf->magic != PSMF_MAGIC ? SCE_PSMF_ERROR_INVALID_PSMF
                  : psmf->version == 0      ? SCE_PSMF_ERROR_BAD_VERSION
                  : psmf->streamOffset == 0 ? SCE_PSMF_ERROR_INVALID_VALUE : 0;
        if (err) { delete psmf; ret(c, err); return; }
        PsmfData* d = guest<PsmfData>(psmfStruct);
        std::memset(d, 0, sizeof(PsmfData));
        d->version = psmf->version;
        d->headerSize = 0x800;
        d->streamSize = psmf->streamSize;
        d->streamNum = (u32)psmf->currentStreamNum;
        d->headerOffset = psmf->headerOffset;
        auto it = psmfMap.find(d->headerOffset);
        if (it != psmfMap.end()) delete it->second;
        psmfMap[d->headerOffset] = psmf;
        ret(c, 0);
    });
    registerHle("scePsmfGetVideoInfo", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_psmfMutex);
        Psmf* psmf = getPsmf(A(c, 0));
        u32 info = A(c, 1);
        if (!psmf || !psmf->isValidCurrentStreamNumber()) { ret(c, SCE_PSMF_ERROR_NOT_INITIALIZED); return; }
        if (!validAddress(info)) { ret(c, SCE_KERNEL_ERROR_ILLEGAL_ADDRESS); return; }
        PsmfStream* s = psmf->streamMap[psmf->currentStreamNum];
        if (s->videoWidth_ == PsmfStream::INVALID) { ret(c, SCE_PSMF_ERROR_INVALID_ID); return; }
        write32(info, (u32)(s->videoWidth_ == PsmfStream::USE_PSMF ? psmf->videoWidth : s->videoWidth_));
        write32(info + 4, (u32)(s->videoHeight_ == PsmfStream::USE_PSMF ? psmf->videoHeight : s->videoHeight_));
        ret(c, 0);
    });

    registerHle("scePsmfGetNumberOfSpecificStreams", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_psmfMutex);
        Psmf* psmf = getPsmf(A(c, 0));
        if (!psmf) { ret(c, SCE_PSMF_ERROR_NOT_INITIALIZED); return; }
        int n = 0;
        for (const auto& [k, st] : psmf->streamMap) n += st->matchesType((int)A(c, 1)) ? 1 : 0;
        ret(c, (u32)n);
    });
    registerHle("scePsmfSpecifyStreamWithStreamType", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_psmfMutex);
        Psmf* psmf = getPsmf(A(c, 0));
        if (!psmf) { ret(c, SCE_PSMF_ERROR_NOT_INITIALIZED); return; }
        // An unknown type invalidates the stream number but returns 0 (PPSSPP, tested).
        if (!psmf->setStreamWithType(A(c, 0), (int)A(c, 1), (int)A(c, 2))) psmf->setStreamNum(A(c, 0), (int)SCE_PSMF_ERROR_INVALID_ID, false);
        ret(c, 0);
    });
    registerHle("scePsmfSpecifyStreamWithStreamTypeNumber", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_psmfMutex);
        Psmf* psmf = getPsmf(A(c, 0));
        if (!psmf) { ret(c, SCE_PSMF_ERROR_NOT_INITIALIZED); return; }
        ret(c, psmf->setStreamWithTypeNumber(A(c, 0), (int)A(c, 1), (int)A(c, 2)) ? 0 : SCE_PSMF_ERROR_INVALID_ID);
    });
    registerHle("scePsmfGetCurrentStreamType", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_psmfMutex);
        Psmf* psmf = getPsmf(A(c, 0));
        if (!psmf || psmf->currentStreamNum == (int)SCE_PSMF_ERROR_NOT_INITIALIZED) { ret(c, SCE_PSMF_ERROR_NOT_INITIALIZED); return; }
        if (!validAddress(A(c, 1)) || !validAddress(A(c, 2))) { ret(c, SCE_KERNEL_ERROR_ILLEGAL_ADDRESS); return; }
        if (psmf->currentStreamType != -1) {
            write32(A(c, 1), (u32)psmf->currentStreamType);
            write32(A(c, 2), (u32)psmf->currentStreamChannel);
        }
        ret(c, 0);
    });
    registerHle("scePsmfQueryStreamOffset", [](Ctx* c) {
        if (validAddress(A(c, 1)) && validAddress(A(c, 0))) write32(A(c, 1), readU32BE(hostPtr(A(c, 0) + PSMF_STREAM_OFFSET_OFFSET)));
        ret(c, 0);
    });
    registerHle("scePsmfQueryStreamSize", [](Ctx* c) {
        if (validAddress(A(c, 1)) && validAddress(A(c, 0))) write32(A(c, 1), readU32BE(hostPtr(A(c, 0) + PSMF_STREAM_SIZE_OFFSET)));
        ret(c, 0);
    });
    registerHle("scePsmfVerifyPsmf", [](Ctx* c) {
        u32 a = A(c, 0);
        if (!validAddress(a) || (a & 3)) { ret(c, SCE_PSMF_ERROR_NOT_FOUND); return; }
        if (read32(a) != PSMF_MAGIC || (int)read32(a + PSMF_STREAM_VERSION_OFFSET) < 0) { ret(c, SCE_PSMF_ERROR_NOT_FOUND); return; }
        std::memset(hostPtr(c->r[29] - 0x20), 0, 0x20); // as on hardware (PPSSPP, Kurohyou 2)
        ret(c, 0);
    });
    registerHle("scePsmfGetPresentationStartTime", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_psmfMutex);
        Psmf* psmf = getPsmf(A(c, 0));
        if (!psmf) { ret(c, SCE_PSMF_ERROR_NOT_FOUND); return; }
        if (validAddress(A(c, 1)) && !(A(c, 1) & 3)) write32(A(c, 1), psmf->presentationStartTime);
        ret(c, 0);
    });
    registerHle("scePsmfGetPresentationEndTime", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_psmfMutex);
        Psmf* psmf = getPsmf(A(c, 0));
        if (!psmf) { ret(c, SCE_PSMF_ERROR_NOT_FOUND); return; }
        if (validAddress(A(c, 1)) && !(A(c, 1) & 3)) write32(A(c, 1), psmf->presentationEndTime);
        ret(c, 0);
    });
    registerHle("scePsmfCheckEPmap", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_psmfMutex);
        Psmf* psmf = getPsmf(A(c, 0));
        ret(c, !psmf || psmf->EPMap.empty() ? SCE_PSMF_ERROR_NOT_FOUND : 0);
    });
    registerHle("scePsmfGetEPWithId", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_psmfMutex);
        Psmf* psmf = getPsmf(A(c, 0));
        int epid = (int)A(c, 1);
        if (!psmf) { ret(c, SCE_PSMF_ERROR_NOT_INITIALIZED); return; }
        if (epid < 0 || epid >= (int)psmf->EPMap.size()) { ret(c, SCE_PSMF_ERROR_NOT_FOUND); return; }
        if (validAddress(A(c, 2))) *guest<PsmfEntry>(A(c, 2)) = psmf->EPMap[epid];
        ret(c, 0);
    });
    registerHle("scePsmfGetEPWithTimestamp", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_psmfMutex);
        Psmf* psmf = getPsmf(A(c, 0));
        u32 ts = A(c, 1);
        if (!psmf) { ret(c, SCE_PSMF_ERROR_NOT_INITIALIZED); return; }
        if (ts < psmf->presentationStartTime) { ret(c, SCE_PSMF_ERROR_NOT_FOUND); return; }
        int epid = psmf->FindEPWithTimestamp((int)ts);
        if (epid < 0 || epid >= (int)psmf->EPMap.size()) { ret(c, SCE_PSMF_ERROR_NOT_FOUND); return; }
        if (validAddress(A(c, 2))) *guest<PsmfEntry>(A(c, 2)) = psmf->EPMap[epid];
        ret(c, 0);
    });
    registerHle("scePsmfGetEPidWithTimestamp", [](Ctx* c) {
        std::lock_guard<std::mutex> lk(g_psmfMutex);
        Psmf* psmf = getPsmf(A(c, 0));
        u32 ts = A(c, 1);
        if (!psmf || psmf->EPMap.empty()) { ret(c, SCE_PSMF_ERROR_NOT_FOUND); return; }
        if (ts < psmf->presentationStartTime) { ret(c, SCE_PSMF_ERROR_INVALID_TIMESTAMP); return; }
        int epid = psmf->FindEPWithTimestamp((int)ts);
        ret(c, epid < 0 || epid >= (int)psmf->EPMap.size() ? SCE_PSMF_ERROR_INVALID_ID : (u32)epid);
    });
}

} // namespace psp::kernel
