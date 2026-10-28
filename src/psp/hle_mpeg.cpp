// sceMpeg HLE: PSMF movie playback (H.264 video through FFmpeg, ATRAC3+ audio through the
// standalone decoder) for the recompiled game.
//
// Ported from PPSSPP Core/HLE/sceMpeg.cpp (github.com/hrydgard/ppsspp), modified for the
// Coded Arms PC port: PPSSPP's HLE wrappers, savestates, the PMP/YCbCr/scePsmf paths and the
// functions the game does not import were removed; hleDelayResult maps to
// delayCurrentThread, and the ringbuffer put callback (PPSSPP: hleEnqueueCall + PostPutAction)
// runs synchronously on the calling guest thread's stack.
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
//
// This code is part shamelessly "inspired" from JPSCP.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "psp/gpu/gpu.h"
#include "psp/kernel.h"
#include "psp/memory.h"
#include "ppsspp/MediaEngine.h"
#include "ppsspp/sceMpeg.h"

namespace psp::mpeg {

static bool g_verbose = std::getenv("CA_MPEG_VERBOSE") != nullptr;
bool verbose() { return g_verbose; }
void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kernel::log("[MPEG] %s", buf);
}

} // namespace psp::mpeg

namespace {

using namespace psp;
using psp::kernel::A;
using psp::kernel::ret;

// Error codes (PPSSPP Core/HLE/ErrorCodes.h)
constexpr u32 SCE_KERNEL_ERROR_ILLEGAL_ADDRESS = 0x8002006a;
constexpr u32 SCE_MPEG_ERROR_NO_DATA = 0x80618001;
constexpr u32 SCE_MPEG_ERROR_BAD_VERSION = 0x80610002;
constexpr u32 SCE_MPEG_ERROR_NOT_YET_INIT = 0x80618009;
constexpr u32 SCE_MPEG_ERROR_NO_MEMORY = 0x80610022;
constexpr u32 SCE_MPEG_ERROR_INVALID_VALUE = 0x806101fe;
constexpr u32 SCE_MPEG_ERROR_AVC_DECODE_FATAL = 0x80628002;

// MPEG AVC elementary stream.
const int MPEG_AVC_ES_SIZE = 2048;          // MPEG packet size.

// MPEG ATRAC elementary stream.
const int MPEG_ATRAC_ES_SIZE = 2112;
const int MPEG_ATRAC_ES_OUTPUT_SIZE = 8192;

// MPEG analysis results.
const int MPEG_VERSION_0012 = 0;
const int MPEG_VERSION_0013 = 1;
const int MPEG_VERSION_0014 = 2;
const int MPEG_VERSION_0015 = 3;

// PSMF analysis results.
const int PSMF_VERSION_0012 = 0x32313030;
const int PSMF_VERSION_0013 = 0x33313030;
const int PSMF_VERSION_0014 = 0x34313030;
const int PSMF_VERSION_0015 = 0x35313030;

// MPEG streams.
const int MPEG_AVC_STREAM = 0;
const int MPEG_ATRAC_STREAM = 1;
const int MPEG_PCM_STREAM = 2;
const int MPEG_DATA_STREAM = 3;      // Arbitrary user defined type. Can represent audio or video.
const int MPEG_AUDIO_STREAM = 15;
const u32 MPEG_MEMSIZE_0104 = 0x0b3DB;
const u32 MPEG_MEMSIZE_0105 = 0x10000;     // 64k.
const int MPEG_AVC_DECODE_SUCCESS = 1;     // Internal value.

// "Ms" in PPSSPP's names, but these are microseconds (hleDelayResult takes usec).
const int atracDecodeDelayMs = 3000;
const int avcFirstDelayMs = 3600;
const int avcDecodeDelayMs = 5400;         // Varies between 4700 and 6000.
const int avcEmptyDelayMs = 320;
const int mpegDecodeErrorDelayMs = 100;
const int videoTimestampStep = 3003;       // Value based on pmfplayer (mpegTimestampPerSecond / 29.970 (fps)).
const s64 UNKNOWN_TIMESTAMP = -1;

// Compat flag MpegAvcWarmUp: not set for Coded Arms.
const bool kMpegAvcWarmUp = false;

// Calculate the number of total packets added to the ringbuffer by calling the sceMpegRingbufferPut() once.
int ringbufferPutPacketsAdded = 0;
const bool useRingbufferPutCallbackMulti = true;

const u8 defaultMpegheader[2048] = {
	0x50,0x53,0x4d,0x46,0x30,0x30,0x31,0x35,0x00,0x00,0x08,0x00,0x00,
	0x10,0xc8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4e,0x00,
	0x00,0x00,0x01,0x5f,0x90,0x00,0x00,0x00,0x0d,0xbe,0xca,0x00,0x00,0x61,0xa8,0x00,0x01,0x5f,
	0x90,0x02,0x01,0x00,0x00,0x00,0x34,0x00,0x00,0x00,0x01,0x5f,0x90,0x00,0x00,0x00,0x0d,0xbe,
	0xca,0x00,0x01,0x00,0x00,0x00,0x22,0x00,0x02,0xe0,0x00,0x20,0xfb,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x1e,0x11,0x00,0x00,0xbd,0x00,0x20,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x02,0x02
};

bool isMpegInit;
// Set from the module info of the sceMpeg_library PRX the game loads (mpegNotifyModuleLoaded);
// PPSSPP's default when none was seen.
int mpegLibVersion = 0x010A;
u32 streamIdGen = 1;
std::map<u32, MpegContext *> g_mpegCtxs;

// ---- guest structure access -----------------------------------------------------------
// PSPPointer<SceMpegRingBuffer> equivalent: the struct lives in guest memory.
SceMpegRingBuffer* ringPtr(u32 addr) {
	if (!Memory::IsValidRange(addr, sizeof(SceMpegRingBuffer)))
		return nullptr;
	return (SceMpegRingBuffer*)hostPtr(addr);
}

// Debug: dump decoded frames (CA_MPEG_DUMP=dir, CA_MPEG_DUMP_EVERY=n frames, default 30).
const char* g_dumpDir = std::getenv("CA_MPEG_DUMP");
int g_dumpEvery = std::getenv("CA_MPEG_DUMP_EVERY") ? std::atoi(std::getenv("CA_MPEG_DUMP_EVERY")) : 30;
int g_dumpMovie = 0;

void dumpFrame(u32 buffer, int frameWidth, int pixelMode, int width, int height, int frameNo) {
	char path[512];
	std::snprintf(path, sizeof(path), "%s/movie%02d_frame%05d.ppm", g_dumpDir, g_dumpMovie, frameNo);
	FILE* f = std::fopen(path, "wb");
	if (!f)
		return;
	std::fprintf(f, "P6\n%d %d\n255\n", width, height);
	std::vector<u8> row(width * 3);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			u8 r, g, b;
			if (pixelMode == GE_CMODE_32BIT_ABGR8888) {
				u32 p = read32(buffer + (y * frameWidth + x) * 4);
				r = p & 0xFF; g = (p >> 8) & 0xFF; b = (p >> 16) & 0xFF;
			} else {
				u16 p = read16(buffer + (y * frameWidth + x) * 2);
				if (pixelMode == GE_CMODE_16BIT_BGR5650) {
					r = (p & 0x1F) << 3; g = ((p >> 5) & 0x3F) << 2; b = ((p >> 11) & 0x1F) << 3;
				} else if (pixelMode == GE_CMODE_16BIT_ABGR5551) {
					r = (p & 0x1F) << 3; g = ((p >> 5) & 0x1F) << 3; b = ((p >> 10) & 0x1F) << 3;
				} else {
					r = (p & 0xF) << 4; g = ((p >> 4) & 0xF) << 4; b = ((p >> 8) & 0xF) << 4;
				}
			}
			row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = b;
		}
		std::fwrite(row.data(), 1, row.size(), f);
	}
	std::fclose(f);
	psp::mpeg::logf("dumped %s", path);
}

// Runs guest function `fn`(a0, a1, a2) on the calling guest thread, below its stack pointer,
// and returns v0. (PPSSPP queues such calls to run right after the HLE function returns, on
// the same thread; the callee-saved state the guest relies on is preserved either way.)
u32 callGuest(Ctx* c, u32 fn, u32 a0, u32 a1, u32 a2) {
	Ctx sub = *c;
	sub.r[4] = a0;
	sub.r[5] = a1;
	sub.r[6] = a2;
	sub.r[29] = (c->r[29] - 0x40) & ~0xFu;
	call_indirect(&sub, fn);
	return sub.r[2];
}

// hleDelayResult(result, ..., usec)
void delayResult(Ctx* c, u32 result, int usec) {
	ret(c, result);
	kernel::delayCurrentThread((uint64_t)usec);
}

} // namespace

void SceMpegAu::read(u32 addr) {
	std::memcpy(this, hostPtr(addr), sizeof(*this));
	pts = (pts & 0xFFFFFFFFULL) << 32 | (((u64)pts) >> 32);
	dts = (dts & 0xFFFFFFFFULL) << 32 | (((u64)dts) >> 32);
}

void SceMpegAu::write(u32 addr) {
	pts = (pts & 0xFFFFFFFFULL) << 32 | (((u64)pts) >> 32);
	dts = (dts & 0xFFFFFFFFULL) << 32 | (((u64)dts) >> 32);
	std::memcpy(hostPtr(addr), this, sizeof(*this));
}

MpegContext::MpegContext() {
	memcpy(mpegheader, defaultMpegheader, 2048);
}
MpegContext::~MpegContext() {
	delete mediaengine;
}

namespace {

MpegContext *getMpegCtx(u32 mpegAddr) {
	if (!Memory::IsValidAddress(mpegAddr))
		return nullptr;

	u32 mpeg = read32(mpegAddr);
	auto found = g_mpegCtxs.find(mpeg);
	if (found == g_mpegCtxs.end())
		return nullptr;
	return found->second;
}

u32 convertTimestampToDate(u32 ts) {
	return ts;  // TODO
}

u32 getMpegVersion(u32 mpegRawVersion) {
	switch (mpegRawVersion) {
		case PSMF_VERSION_0012: return MPEG_VERSION_0012;
		case PSMF_VERSION_0013: return MPEG_VERSION_0013;
		case PSMF_VERSION_0014: return MPEG_VERSION_0014;
		case PSMF_VERSION_0015: return MPEG_VERSION_0015;
		default: return -1;
	}
}

void AnalyzeMpeg(const u8 *buffer, u32 validSize, MpegContext *ctx) {
	ctx->mpegMagic = *(u32_le*)buffer;
	ctx->mpegRawVersion = *(u32_le*)(buffer + PSMF_STREAM_VERSION_OFFSET);
	ctx->mpegVersion = getMpegVersion(ctx->mpegRawVersion);
	ctx->mpegOffset = bswap32(*(u32_le*)(buffer + PSMF_STREAM_OFFSET_OFFSET));
	ctx->mpegStreamSize = bswap32(*(u32_le*)(buffer + PSMF_STREAM_SIZE_OFFSET));
	ctx->mpegFirstTimestamp = getMpegTimeStamp(buffer + PSMF_FIRST_TIMESTAMP_OFFSET);
	ctx->mpegLastTimestamp = getMpegTimeStamp(buffer + PSMF_LAST_TIMESTAMP_OFFSET);
	ctx->mpegFirstDate = convertTimestampToDate(ctx->mpegFirstTimestamp);
	ctx->mpegLastDate = convertTimestampToDate(ctx->mpegLastTimestamp);
	ctx->mpegwarmUp = 0;
	ctx->avc.avcDetailFrameWidth = (*(u8*)(buffer + 142)) * 0x10;
	ctx->avc.avcDetailFrameHeight = (*(u8*)(buffer + 143)) * 0x10;
	ctx->avc.avcDecodeResult = MPEG_AVC_DECODE_SUCCESS;
	ctx->avc.avcFrameStatus = 0;
	ctx->videoFrameCount = 0;
	ctx->audioFrameCount = 0;
	ctx->endOfAudioReached = false;
	ctx->endOfVideoReached = false;

	// Sanity Check ctx->mpegFirstTimestamp
	if (ctx->mpegFirstTimestamp != 90000) {
		WARN_LOG_REPORT(Log::Mpeg, "Unexpected mpeg first timestamp: %llx / %lld", (long long)ctx->mpegFirstTimestamp, (long long)ctx->mpegFirstTimestamp);
	}

	if (ctx->mpegMagic != PSMF_MAGIC || ctx->mpegVersion < 0 ||
		(ctx->mpegOffset & 2047) != 0 || ctx->mpegOffset == 0) {
		// mpeg header is invalid!
		return;
	}

	if (!ctx->isAnalyzed && ctx->mediaengine && ctx->mpegStreamSize > 0 && validSize >= ctx->mpegOffset) {
		// init mediaEngine
		SceMpegRingBuffer *ringbuffer = ringPtr(ctx->mpegRingbufferAddr);
		if (ringbuffer) {
			ctx->mediaengine->loadStream(buffer, ctx->mpegOffset, ringbuffer->packets * ringbuffer->packetSize);
		} else {
			// TODO: Does this make any sense?
			ctx->mediaengine->loadStream(buffer, ctx->mpegOffset, 0);
		}

		// When used with scePsmf, some applications attempt to use sceMpegQueryStreamOffset
		// and sceMpegQueryStreamSize, which forces a packet overwrite in the Media Engine and in
		// the MPEG ringbuffer.
		// Mark the current MPEG as analyzed to filter this, and restore it at sceMpegFinish.
		ctx->isAnalyzed = true;
	}

	// copy header struct to mpeg header.
	memcpy(ctx->mpegheader, buffer, validSize >= 2048 ? 2048 : validSize);
	*(u32_le*)(ctx->mpegheader + PSMF_STREAM_OFFSET_OFFSET) = 0x80000;

	INFO_LOG(Log::Mpeg, "Stream offset: %d, Stream size: 0x%X", ctx->mpegOffset, ctx->mpegStreamSize);
	INFO_LOG(Log::Mpeg, "First timestamp: %lld, Last timestamp: %lld", (long long)ctx->mpegFirstTimestamp, (long long)ctx->mpegLastTimestamp);
}

// Memory::ClampValidSizeAt
u32 clampValidSizeAt(u32 addr, u32 size) {
	while (size > 0 && !Memory::IsValidRange(addr, size))
		size = size > 2048 ? size - 2048 : 0;
	return size;
}

u32 sceMpegInit(Ctx* c) {
	if (isMpegInit) {
		WARN_LOG(Log::Mpeg, "sceMpegInit(): already initialized");
	} else {
		INFO_LOG(Log::Mpeg, "sceMpegInit(), mpegLibVersion 0x%0x", mpegLibVersion);
	}
	isMpegInit = true;
	delayResult(c, 0, 750);
	return 0;
}

u32 __MpegRingbufferQueryMemSize(int packets) {
	return packets * (104 + 2048);
}

u32 sceMpegRingbufferConstruct(u32 gp, u32 ringbufferAddr, u32 numPackets, u32 data, u32 size, u32 callbackAddr, u32 callbackArg) {
	if (!Memory::IsValidAddress(ringbufferAddr)) {
		ERROR_LOG_REPORT(Log::Mpeg, "sceMpegRingbufferConstruct(%08x, %i, %08x, %08x, %08x, %08x): bad ringbuffer, should crash", ringbufferAddr, numPackets, data, size, callbackAddr, callbackArg);
		return SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	}

	if ((int)size < 0) {
		ERROR_LOG_REPORT(Log::Mpeg, "sceMpegRingbufferConstruct(%08x, %i, %08x, %08x, %08x, %08x): invalid size", ringbufferAddr, numPackets, data, size, callbackAddr, callbackArg);
		return SCE_MPEG_ERROR_NO_MEMORY;
	}

	if (__MpegRingbufferQueryMemSize(numPackets) > size) {
		if (numPackets < 0x00100000) {
			ERROR_LOG_REPORT(Log::Mpeg, "sceMpegRingbufferConstruct(%08x, %i, %08x, %08x, %08x, %08x): too many packets for buffer", ringbufferAddr, numPackets, data, size, callbackAddr, callbackArg);
			return SCE_MPEG_ERROR_NO_MEMORY;
		} else {
			// The PSP's firmware allows some cases here, due to a bug in its validation.
			ERROR_LOG_REPORT(Log::Mpeg, "sceMpegRingbufferConstruct(%08x, %i, %08x, %08x, %08x, %08x): too many packets for buffer, bogus size", ringbufferAddr, numPackets, data, size, callbackAddr, callbackArg);
		}
	}

	SceMpegRingBuffer *ring = ringPtr(ringbufferAddr);

	ring->packets = numPackets;
	ring->packetsRead = 0;
	ring->packetsWritePos = 0;
	ring->packetsAvail = 0;
	ring->packetSize = 2048;
	ring->data = data;
	ring->callback_addr = callbackAddr;
	ring->callback_args = callbackArg;
	ring->dataUpperBound = data + numPackets * 2048;
	if (ring->semaID != 0) {
		// I'm starting to think this is just padding or something.
		// It's not written by this function.
		WARN_LOG(Log::Mpeg, "Detected 'semaID' (might not be) %d (%08x)", ring->semaID, ring->semaID);
	}
	ring->mpeg = 0;
	// This isn't in ver 0104, but it is in 0105.
	if (mpegLibVersion >= 0x0105)
		ring->gp = gp; // PPSSPP: __KernelGetModuleGP(__KernelGetCurThreadModuleId())
	return 0;
}

u32 MpegRequiredMem() {
	if (mpegLibVersion < 0x0105) {
		return MPEG_MEMSIZE_0104;
	}
	return MPEG_MEMSIZE_0105;
}

// ddrTop is currently ignored.
void sceMpegCreate(Ctx* c, u32 mpegAddr, u32 dataPtr, u32 size, u32 ringbufferAddr, u32 frameWidth, u32 mode, u32 ddrTop) {
	if (!Memory::IsValidAddress(mpegAddr)) {
		WARN_LOG(Log::Mpeg, "sceMpegCreate: invalid addresses");
		ret(c, (u32)-1);
		return;
	}

	if (size < MpegRequiredMem()) {
		WARN_LOG(Log::Mpeg, "sceMpegCreate: no memory (%08x)", size);
		ret(c, SCE_MPEG_ERROR_NO_MEMORY);
		return;
	}

	SceMpegRingBuffer *ringbuffer = ringPtr(ringbufferAddr);
	if (ringbuffer) {
		if (ringbuffer->packetSize == 0) {
			ringbuffer->packetsAvail = 0;
		} else {
			ringbuffer->packetsAvail = ringbuffer->packets - (ringbuffer->dataUpperBound - ringbuffer->data) / ringbuffer->packetSize;
		}
		ringbuffer->mpeg = mpegAddr;
	}

	// Generate, and write mpeg handle into mpeg data, for some reason
	int mpegHandle = dataPtr + 0x30;
	write32(mpegAddr, mpegHandle);

	// Initialize fake mpeg struct.
	memcpy(hostPtr(mpegHandle), "LIBMPEG\0", 8);
	memcpy(hostPtr(mpegHandle + 8), "001\0", 4);
	write32(mpegHandle + 12, -1);
	if (ringbuffer) {
		write32(mpegHandle + 16, ringbufferAddr);
		write32(mpegHandle + 20, ringbuffer->dataUpperBound);
	}
	MpegContext *ctx = new MpegContext();
	if (g_mpegCtxs.find(mpegHandle) != g_mpegCtxs.end()) {
		WARN_LOG_REPORT(Log::HLE, "Replacing existing mpeg context at %08x", mpegAddr);
		// Otherwise, it would leak.
		delete g_mpegCtxs[mpegHandle];
	}
	g_mpegCtxs[mpegHandle] = ctx;

	// Initialize mpeg values.
	ctx->mpegRingbufferAddr = ringbufferAddr;
	ctx->videoFrameCount = 0;
	ctx->audioFrameCount = 0;
	ctx->videoPixelMode = GE_CMODE_32BIT_ABGR8888; // TODO: What's the actual default?
	ctx->avcRegistered = false;
	ctx->atracRegistered = false;
	ctx->pcmRegistered = false;
	ctx->dataRegistered = false;
	ctx->ignoreAtrac = false;
	ctx->ignorePcm = false;
	ctx->ignoreAvc = false;
	ctx->defaultFrameWidth = frameWidth;
	ctx->mpegStreamSize = 0;
	ctx->mpegOffset = 0;
	for (int i = 0; i < MPEG_DATA_ES_BUFFERS; i++) {
		ctx->esBuffers[i] = false;
	}

	// Detailed "analysis" is done later in Query* for some reason.
	ctx->isAnalyzed = false;
	ctx->mediaengine = new MediaEngine();
	++g_dumpMovie;

	INFO_LOG(Log::Mpeg, "sceMpegCreate(%08x, %08x, %08x, %08x, %d, %d, %08x) -> handle %08x", mpegAddr, dataPtr, size, ringbufferAddr, frameWidth, mode, ddrTop, mpegHandle);
	delayResult(c, 0, 29000);
}

void sceMpegDelete(Ctx* c, u32 mpeg) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) {
		WARN_LOG(Log::Mpeg, "sceMpegDelete: bad mpeg handle");
		ret(c, (u32)-1);
		return;
	}

	delete ctx;
	g_mpegCtxs.erase(read32(mpeg));

	delayResult(c, 0, 40000);
}

int sceMpegAvcDecodeMode(u32 mpeg, u32 modeAddr) {
	if (!Memory::IsValidRange(modeAddr, 8)) {
		return -1;
	}

	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) {
		return -1;
	}

	int mode = read32(modeAddr);
	int pixelMode = read32(modeAddr + 4);
	if (pixelMode >= GE_CMODE_16BIT_BGR5650 && pixelMode <= GE_CMODE_32BIT_ABGR8888) {
		ctx->videoPixelMode = pixelMode;
	} else {
		ERROR_LOG(Log::Mpeg, "sceMpegAvcDecodeMode(%i, %i): unknown pixelMode ", mode, pixelMode);
	}
	INFO_LOG(Log::Mpeg, "sceMpegAvcDecodeMode(%08x, mode=%d, pixelMode=%d)", mpeg, mode, pixelMode);
	return 0;
}

int sceMpegQueryStreamOffset(u32 mpeg, u32 bufferAddr, u32 offsetAddr) {
	if (!Memory::IsValidAddress(bufferAddr) || !Memory::IsValidAddress(offsetAddr)) {
		return -1;
	}

	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) {
		return -1;
	}

	// Kinda destructive, no? Shouldn't this just do what sceMpegQueryStreamSize does?
	AnalyzeMpeg(hostPtr(bufferAddr), clampValidSizeAt(bufferAddr, 32768), ctx);

	if (ctx->mpegMagic != PSMF_MAGIC) {
		write32(offsetAddr, 0);
		ERROR_LOG(Log::Mpeg, "sceMpegQueryStreamOffset: Bad PSMF magic");
		return SCE_MPEG_ERROR_INVALID_VALUE;
	} else if (ctx->mpegVersion < 0) {
		write32(offsetAddr, 0);
		ERROR_LOG(Log::Mpeg, "sceMpegQueryStreamOffset: Bad version");
		return SCE_MPEG_ERROR_BAD_VERSION;
	} else if ((ctx->mpegOffset & 2047) != 0 || ctx->mpegOffset == 0) {
		write32(offsetAddr, 0);
		ERROR_LOG(Log::Mpeg, "sceMpegQueryStreamOffset: Bad offset");
		return SCE_MPEG_ERROR_INVALID_VALUE;
	}

	write32(offsetAddr, ctx->mpegOffset);
	return 0;
}

u32 sceMpegQueryStreamSize(u32 bufferAddr, u32 sizeAddr) {
	if (!Memory::IsValidAddress(bufferAddr) || !Memory::IsValidAddress(sizeAddr)) {
		return -1;
	}

	MpegContext ctx;
	ctx.mediaengine = nullptr;  // makes sure we don't actually load the stream.
	ctx.isAnalyzed = false;

	AnalyzeMpeg(hostPtr(bufferAddr), clampValidSizeAt(bufferAddr, 32768), &ctx);

	if (ctx.mpegMagic != PSMF_MAGIC) {
		write32(sizeAddr, 0);
		return SCE_MPEG_ERROR_INVALID_VALUE;
	} else if ((ctx.mpegOffset & 2047) != 0 ) {
		write32(sizeAddr, 0);
		return SCE_MPEG_ERROR_INVALID_VALUE;
	}

	write32(sizeAddr, ctx.mpegStreamSize);
	return 0;
}

int sceMpegRegistStream(u32 mpeg, u32 streamType, u32 streamNum) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) {
		return -1;
	}

	switch (streamType) {
	case MPEG_AVC_STREAM:
		ctx->avcRegistered = true;
		ctx->mediaengine->addVideoStream(streamNum);
		// TODO: Probably incorrect?
		ctx->mediaengine->setVideoStream(streamNum);
		break;
	case MPEG_AUDIO_STREAM:
	case MPEG_ATRAC_STREAM:
		ctx->atracRegistered = true;
		// TODO: Probably incorrect?
		ctx->mediaengine->setAudioStream(streamNum);
		break;
	case MPEG_PCM_STREAM:
		ctx->pcmRegistered = true;
		break;
	case MPEG_DATA_STREAM:
		ctx->dataRegistered = true;
		break;
	default :
		DEBUG_LOG(Log::Mpeg, "sceMpegRegistStream(%i) : unknown stream type", streamType);
		break;
	}
	// ...
	u32 sid = streamIdGen++;
	StreamInfo info;
	info.type = streamType;
	info.num = streamNum;
	info.sid = sid;
	info.needsReset = true;
	ctx->streamMap[sid] = info;
	ctx->streamIgnoreMap[sid] = false;
	INFO_LOG(Log::Mpeg, "sceMpegRegistStream(%08x, type=%d, num=%d) -> %d", mpeg, streamType, streamNum, sid);
	return sid;
}

int sceMpegMallocAvcEsBuf(u32 mpeg) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) {
		return -1;
	}

	// Doesn't actually malloc, just keeps track of a couple of flags
	for (int i = 0; i < MPEG_DATA_ES_BUFFERS; i++) {
		if (!ctx->esBuffers[i]) {
			ctx->esBuffers[i] = true;
			return i + 1;
		}
	}

	// No es buffer
	return 0;
}

int sceMpegFreeAvcEsBuf(u32 mpeg, int esBuf) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) {
		return -1;
	}

	if (esBuf == 0) {
		return SCE_MPEG_ERROR_INVALID_VALUE;
	}

	if (esBuf >= 1 && esBuf <= MPEG_DATA_ES_BUFFERS) {
		// TODO: Check if it's already been free'd?
		ctx->esBuffers[esBuf - 1] = false;
	}
	return 0;
}

void sceMpegAvcDecode(Ctx* c, u32 mpeg, u32 auAddr, u32 frameWidth, u32 bufferAddr, u32 initAddr) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) {
		WARN_LOG(Log::Mpeg, "sceMpegAvcDecode: bad mpeg handle");
		ret(c, (u32)-1);
		return;
	}

	if (frameWidth == 0) {  // wtf, go sudoku passes in 0xcccccccc
		if (!ctx->defaultFrameWidth) {
			frameWidth = ctx->avc.avcDetailFrameWidth;
		} else {
			frameWidth = ctx->defaultFrameWidth;
		}
	}

	SceMpegAu avcAu;
	avcAu.read(auAddr);

	SceMpegRingBuffer *ringbuffer = ringPtr(ctx->mpegRingbufferAddr);
	if (!ringbuffer) {
		ERROR_LOG(Log::Mpeg, "sceMpegAvcDecode: Bogus mpegringbufferaddr");
		ret(c, (u32)-1);
		return;
	}

	if (!Memory::IsValidRange(bufferAddr, 4) || !Memory::IsValidRange(initAddr, 4)) {
		ERROR_LOG(Log::Mpeg, "sceMpegAvcDecode: invalid addresses");
		ret(c, (u32)-1);
		return;
	}

	u32 buffer = read32(bufferAddr);
	u32 init = read32(initAddr);
	DEBUG_LOG(Log::Mpeg, "video: bufferAddr = %08x, *buffer = %08x, *init = %08x", bufferAddr, buffer, init);

	if (ringbuffer->packetsRead == 0 || ctx->mediaengine->IsVideoEnd()) {
		WARN_LOG(Log::Mpeg, "sceMpegAvcDecode: mpeg buffer empty");
		delayResult(c, SCE_MPEG_ERROR_AVC_DECODE_FATAL, avcEmptyDelayMs);
		return;
	}

	s32 beforeAvail = ringbuffer->packets - ctx->mediaengine->getRemainSize() / 2048;

	// We stored the video stream id here in sceMpegGetAvcAu().
	ctx->mediaengine->setVideoStream(avcAu.esBuffer);

	int accumDelay = 0;

	if (ctx->mediaengine->stepVideo(ctx->videoPixelMode)) {
		int written = ctx->mediaengine->writeVideoImage(buffer, frameWidth, ctx->videoPixelMode);
		// PPSSPP: gpu->PerformWriteFormattedFromMemory(): framebuffers in that memory are refreshed.
		psp::gpu::notifyMemoryWrite(buffer, (uint32_t)std::max(written, 0));
		ctx->avc.avcFrameStatus = 1;
		ctx->videoFrameCount++;
		if (g_dumpDir && g_dumpEvery > 0 && (ctx->videoFrameCount % g_dumpEvery) == 1)
			dumpFrame(buffer, frameWidth, ctx->videoPixelMode, std::min<int>(ctx->mediaengine->VideoWidth(), frameWidth),
			          ctx->mediaengine->VideoHeight(), ctx->videoFrameCount);
	} else {
		ctx->avc.avcFrameStatus = 0;
	}
	s32 afterAvail = ringbuffer->packets - ctx->mediaengine->getRemainSize() / 2048;
	// Don't actually reset avail, we only change it by what was decoded.
	// Garbage frames can cause this to be incorrect, but some games expect that.
	if (mpegLibVersion <= 0x0103) {
		ringbuffer->packetsAvail += afterAvail - beforeAvail;
	} else {
		ringbuffer->packetsAvail = afterAvail;
	}

	avcAu.pts = ctx->mediaengine->getVideoTimeStamp() + ctx->mpegFirstTimestamp;

	// Flush structs back to memory
	avcAu.write(auAddr);

	if (mpegLibVersion >= 0x0105 && mpegLibVersion < 0x010a) {
		//Killzone - Liberation expect , issue #16727
		write32(initAddr, 1);
	}
	else {
		// Save the current frame's status to initAddr
		write32(initAddr, ctx->avc.avcFrameStatus);
	}
	ctx->avc.avcDecodeResult = MPEG_AVC_DECODE_SUCCESS;

	DEBUG_LOG(Log::Mpeg, "sceMpegAvcDecode: frame %d status %d pts %lld avail %d", ctx->videoFrameCount, ctx->avc.avcFrameStatus, (long long)avcAu.pts, (int)ringbuffer->packetsAvail);
	if (ctx->videoFrameCount <= 1) {
		delayResult(c, 0, accumDelay + avcFirstDelayMs);
	} else {
		delayResult(c, 0, accumDelay + avcDecodeDelayMs);
	}
}

u32 sceMpegAvcDecodeStop(u32 mpeg, u32 frameWidth, u32 bufferAddr, u32 statusAddr) {
	if (!Memory::IsValidAddress(bufferAddr) || !Memory::IsValidAddress(statusAddr)) {
		return -1;
	}

	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) {
		return -1;
	}

	// No last frame generated
	write32(statusAddr, 0);
	return 0;
}

u32 sceMpegUnRegistStream(u32 mpeg, int streamUid) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) {
		return -1;
	}

	StreamInfo info = {0};

	switch (info.type) {
	case MPEG_AVC_STREAM:
		ctx->avcRegistered = false;
		break;
	case MPEG_AUDIO_STREAM:
	case MPEG_ATRAC_STREAM:
		ctx->atracRegistered = false;
		break;
	case MPEG_PCM_STREAM:
		ctx->pcmRegistered = false;
		break;
	case MPEG_DATA_STREAM:
		ctx->dataRegistered = false;
		break;
	default :
		DEBUG_LOG(Log::Mpeg, "sceMpegUnRegistStream(%i) : unknown streamID ", streamUid);
		break;
	}
	ctx->streamMap[streamUid] = info;
	info.type = -1;
	info.sid = -1 ;
	info.needsReset = true;
	ctx->isAnalyzed = false;
	return 0;
}

int sceMpegInitAu(u32 mpeg, u32 bufferAddr, u32 auPointer) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) {
		return -1;
	}

	SceMpegAu sceAu;
	sceAu.read(auPointer);

	if (bufferAddr >= 1 && bufferAddr <= (u32)MPEG_DATA_ES_BUFFERS && ctx->esBuffers[bufferAddr - 1]) {
		// This esbuffer has been allocated for Avc.
		// Default to 0, since we stuff the stream id in here.  Technically, we shouldn't.
		// TODO: Do something better to track the AU data.  This used to be bufferAddr.
		sceAu.esBuffer = 0;
		sceAu.esSize = MPEG_AVC_ES_SIZE;
		sceAu.dts = 0;
		sceAu.pts = 0;

		sceAu.write(auPointer);
	} else {
		// This esbuffer has been left as Atrac.
		// Default to 0, since we stuff the stream id in here.  Technically, we shouldn't.
		// TODO: Do something better to track the AU data.  This used to be bufferAddr.
		sceAu.esBuffer = 0;
		sceAu.esSize = MPEG_ATRAC_ES_SIZE;
		sceAu.pts = 0;
		sceAu.dts = UNKNOWN_TIMESTAMP;

		sceAu.write(auPointer);
	}
	return 0;
}

int sceMpegQueryAtracEsSize(u32 mpeg, u32 esSizeAddr, u32 outSizeAddr) {
	if (!Memory::IsValidAddress(esSizeAddr) || (esSizeAddr & 3) || !Memory::IsValidAddress(outSizeAddr) || (outSizeAddr & 3)) {
		return -1;
	}

	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) {
		return -1;
	}

	write32(esSizeAddr, MPEG_ATRAC_ES_SIZE);
	write32(outSizeAddr, MPEG_ATRAC_ES_OUTPUT_SIZE);
	return 0;
}

int sceMpegRingbufferAvailableSize(u32 ringbufferAddr) {
	SceMpegRingBuffer *ringbuffer = ringPtr(ringbufferAddr);
	if (!ringbuffer) {
		ERROR_LOG(Log::Mpeg, "sceMpegRingbufferAvailableSize: invalid ringbuffer, should crash");
		return SCE_KERNEL_ERROR_ILLEGAL_ADDRESS;
	}

	MpegContext *ctx = getMpegCtx(ringbuffer->mpeg);
	if (!ctx) {
		ERROR_LOG(Log::Mpeg, "sceMpegRingbufferAvailableSize: bad mpeg handle");
		return SCE_MPEG_ERROR_NOT_YET_INIT;
	}

	ctx->mpegRingbufferAddr = ringbufferAddr;

	// PPSSPP doesn't have a fully asynchronous Media Engine thread to constantly drain the buffer.
	// Games like Ridge Racer 2 rely on this function to implicitly update the available size.
	// However, we must NOT overwrite packetsAvail if the stream hasn't been initialized/analyzed yet.
	// This protects manual struct modifications (like in the 'avail' test) prior to stream playback.
	if (ctx->mediaengine && ctx->isAnalyzed) {
		ringbuffer->packetsAvail = ringbuffer->packets - ctx->mediaengine->getRemainSize() / 2048;
	}

	// PPSSPP: hleEatCycles(2020); hleReSchedule(). The dispatcher's afterHle() reschedules to
	// any higher-priority ready thread; real time passes on its own here.
	return ringbuffer->packets - ringbuffer->packetsAvail;
}

// PostPutAction::run: called after each round of the put callback with the callback's v0.
// Returns the value sceMpegRingbufferPut returns; *again is set when the callback must be
// called again (with *nextArgs) to deliver the remaining packets.
u32 postPut(u32 ringAddr, int packetsAddedThisRound, s32 remainingPackets, bool *again, u32 nextArgs[3], s32 *nextRemaining) {
	*again = false;
	SceMpegRingBuffer *ringbuffer = ringPtr(ringAddr);

	MpegContext *ctx = getMpegCtx(ringbuffer->mpeg);
	int writeOffset = ringbuffer->packetsWritePos % (s32)ringbuffer->packets;

	if (packetsAddedThisRound > 0) {
		ringbufferPutPacketsAdded += packetsAddedThisRound;
	}

	if (!ctx) {
		ERROR_LOG(Log::Mpeg, "sceMpegRingbufferPut: bad mpeg handle %08x", (u32)ringbuffer->mpeg);
		return packetsAddedThisRound; // PPSSPP leaves v0 (the callback's result) in place.
	}

	// It seems validation is done only by older mpeg libs.
	if (mpegLibVersion < 0x0105 && packetsAddedThisRound > 0) {
		// TODO: Faster / less wasteful validation.
		auto demuxer = std::make_unique<MpegDemux>(packetsAddedThisRound * 2048, 0);
		int readOffset = ringbuffer->packetsRead % (s32)ringbuffer->packets;
		uint32_t bufSize = clampValidSizeAt(ringbuffer->data + readOffset * 2048, packetsAddedThisRound * 2048);
		const u8 *buf = hostPtr(ringbuffer->data + readOffset * 2048);
		bool invalid = false;
		for (uint32_t i = 0; i < bufSize / 2048; ++i) {
			demuxer->addStreamData(buf, 2048);
			buf += 2048;

			if (!demuxer->demux(0xFFFF)) {
				invalid = true;
			}
		}
		if (invalid) {
			// Bail out early - don't accept any of the packets, even the good ones.
			// This happens during legit playback at the Burnout Legends menu!
			ERROR_LOG(Log::Mpeg, "sceMpegRingbufferPut(): invalid mpeg data");

			if (mpegLibVersion <= 0x0103) {
				// Act like they were actually added, but don't increment read pos.
				ringbuffer->packetsWritePos += packetsAddedThisRound;
				ringbuffer->packetsAvail += packetsAddedThisRound;
			}
			return SCE_MPEG_ERROR_INVALID_VALUE;
		}
	}

	if (ringbuffer->packetsRead == 0 && ctx->mediaengine && packetsAddedThisRound > 0) {
		// init mediaEngine
		AnalyzeMpeg(ctx->mpegheader, 2048, ctx);
		ctx->mediaengine->loadStream(ctx->mpegheader, 2048, ringbuffer->packets * ringbuffer->packetSize);
	}
	if (packetsAddedThisRound > 0) {
		if (packetsAddedThisRound > ringbuffer->packets - ringbuffer->packetsAvail) {
			WARN_LOG(Log::Mpeg, "sceMpegRingbufferPut clamping packetsAdded old=%i new=%i", packetsAddedThisRound, ringbuffer->packets - ringbuffer->packetsAvail);
			packetsAddedThisRound = ringbuffer->packets - ringbuffer->packetsAvail;
		}
		const u8 *data = hostPtr(ringbuffer->data + writeOffset * 2048);
		uint32_t dataSize = clampValidSizeAt(ringbuffer->data + writeOffset * 2048, packetsAddedThisRound * 2048);
		int actuallyAdded = ctx->mediaengine == NULL ? 8 : ctx->mediaengine->addStreamData(data, dataSize) / 2048;
		if (actuallyAdded != packetsAddedThisRound) {
			WARN_LOG(Log::Mpeg, "sceMpegRingbufferPut(): unable to enqueue all added packets, going to overwrite some frames (%d != %d)", actuallyAdded, packetsAddedThisRound);
		}
		ringbuffer->packetsRead += packetsAddedThisRound;
		ringbuffer->packetsWritePos += packetsAddedThisRound;
		ringbuffer->packetsAvail += packetsAddedThisRound;

		if (remainingPackets > 0) {
			if (packetsAddedThisRound > remainingPackets) {
				WARN_LOG(Log::Mpeg, "Wrote more packets than expected (added %d, remaining %d).", packetsAddedThisRound, remainingPackets);
			}
			else if (packetsAddedThisRound < remainingPackets) {
				// Call the callback again to get more packets.
				writeOffset = ringbuffer->packetsWritePos % (s32)ringbuffer->packets;
				u32 desiredPacketsThisRound = std::min(remainingPackets, (s32)ringbuffer->packets - writeOffset);

				*nextRemaining = remainingPackets - desiredPacketsThisRound;
				nextArgs[0] = (u32)ringbuffer->data + (u32)writeOffset * 2048;
				nextArgs[1] = desiredPacketsThisRound;
				nextArgs[2] = (u32)ringbuffer->callback_args;
				*again = true;
			}
		}
	}
	DEBUG_LOG(Log::Mpeg, "packetAdded: %i packetsRead: %i packetsTotal: %i", packetsAddedThisRound, (int)ringbuffer->packetsRead, (int)ringbuffer->packets);

	if (packetsAddedThisRound < 0 && ringbufferPutPacketsAdded == 0) {
		// Return an error.
		return packetsAddedThisRound;
	} else {
		return ringbufferPutPacketsAdded;
	}
}

// Program signals that it has written data to the ringbuffer and gets a callback ?
u32 sceMpegRingbufferPut(Ctx* c, u32 ringbufferAddr, int numPackets, int available) {
	SceMpegRingBuffer *ringbuffer = ringPtr(ringbufferAddr);
	if (!ringbuffer) {
		// Would have crashed before, TODO test behavior.
		ERROR_LOG(Log::Mpeg, "sceMpegRingbufferPut: invalid ringbuffer address");
		return -1;
	}

	numPackets = std::min(numPackets, available);
	// Generally, program will call sceMpegRingbufferAvailableSize() before this func.
	// Seems still need to check actual available, Patapon 3 for example.
	numPackets = std::min(numPackets, ringbuffer->packets - ringbuffer->packetsAvail);
	if (numPackets <= 0) {
		DEBUG_LOG(Log::Mpeg, "sceMpegRingbufferPut: no packets to enqueue");
		return 0;
	}

	MpegContext *ctx = getMpegCtx(ringbuffer->mpeg);
	if (!ctx) {
		WARN_LOG(Log::Mpeg, "sceMpegRingbufferPut: bad mpeg handle %08x", (u32)ringbuffer->mpeg);
		return -1;
	}
	ringbufferPutPacketsAdded = 0;
	// Execute callback function as a direct MipsCall, no blocking here so no messing around with wait states etc
	if (ringbuffer->callback_addr != 0) {
		DEBUG_LOG(Log::Mpeg, "sceMpegRingbufferPut(%08x, %i, %i)", ringbufferAddr, numPackets, available);

		// Call this multiple times until we get numPackets.
		// Normally this would be if it did not read enough, but also if available > packets.
		// Should ultimately return the TOTAL number of returned packets.
		int writeOffset = ringbuffer->packetsWritePos % (s32)ringbuffer->packets;
		u32 desiredPacketsThisRound = std::min(numPackets, (s32)ringbuffer->packets - writeOffset); //min(How many avail, how many will fit)
		// Old savestate don't use this feature (useRingbufferPutCallbackMulti), just for compatibility.
		s32 remaining = useRingbufferPutCallbackMulti ? numPackets - desiredPacketsThisRound : 0;
		u32 args[3] = { (u32)ringbuffer->data + (u32)writeOffset * 2048, desiredPacketsThisRound, (u32)ringbuffer->callback_args };
		u32 callbackAddr = ringbuffer->callback_addr;
		for (;;) {
			u32 v0 = callGuest(c, callbackAddr, args[0], args[1], args[2]);
			bool again = false;
			s32 nextRemaining = 0;
			u32 result = postPut(ringbufferAddr, (int)v0, remaining, &again, args, &nextRemaining);
			if (!again)
				return result;
			remaining = nextRemaining;
		}
	} else {
		ERROR_LOG_REPORT(Log::Mpeg, "sceMpegRingbufferPut: callback_addr zero");
	}
	return 0;
}

void sceMpegGetAvcAu(Ctx* c, u32 mpeg, u32 streamId, u32 auAddr, u32 attrAddr) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) {
		ERROR_LOG(Log::Mpeg, "sceMpegGetAvcAu: bad mpeg handle");
		ret(c, (u32)-1);
		return;
	}

	SceMpegRingBuffer *ringbuffer = ringPtr(ctx->mpegRingbufferAddr);
	if (!ringbuffer) {
		// Would have crashed before, TODO test behavior.
		ERROR_LOG(Log::Mpeg, "sceMpegGetAvcAu: invalid ringbuffer address");
		ret(c, (u32)-1);
		return;
	}

	if (kMpegAvcWarmUp) {
		if (ctx->mpegwarmUp == 0) {
			ctx->mpegwarmUp++;
			ret(c, SCE_MPEG_ERROR_NO_DATA);
			return;
		}
	}

	SceMpegAu avcAu;
	avcAu.read(auAddr);

	if (ringbuffer->packetsRead == 0 || ringbuffer->packetsAvail == 0) {
		avcAu.pts = 0;
		avcAu.dts = 0;
		avcAu.write(auAddr);
		// TODO: Does this really reschedule?
		delayResult(c, SCE_MPEG_ERROR_NO_DATA, 2000);
		return;
	}

	auto streamInfo = ctx->streamMap.find(streamId);
	if (streamInfo == ctx->streamMap.end())	{
		WARN_LOG(Log::Mpeg, "sceMpegGetAvcAu: invalid video stream %08x", streamId);
		ret(c, (u32)-1);
		return;
	}

	if (ctx->streamIgnoreMap[streamId]) {
		avcAu.pts = ctx->mediaengine->getVideoTimeStamp() + ctx->mpegFirstTimestamp;
		avcAu.dts = avcAu.pts - videoTimestampStep;
		avcAu.esBuffer = streamInfo->second.num;
		avcAu.write(auAddr);
		if (Memory::IsValidAddress(attrAddr) && (attrAddr & 3) == 0) {
			write32(attrAddr, 1);
		}
		delayResult(c, 0, 100);
		return;
	}

	if (streamInfo->second.needsReset) {
		avcAu.pts = 0;
		streamInfo->second.needsReset = false;
	}

	// esBuffer is the memory where this au data goes.  We don't write the data to memory.
	// Instead, let's abuse it to keep track of the stream number.
	avcAu.esBuffer = streamInfo->second.num;

	int result = 0;

	avcAu.pts = ctx->mediaengine->getVideoTimeStamp() + ctx->mpegFirstTimestamp;
	avcAu.dts = avcAu.pts - videoTimestampStep;

	if (ctx->mediaengine->IsVideoEnd()) {
		INFO_LOG(Log::Mpeg, "video end reach. pts: %i dts: %i", (int)avcAu.pts, (int)ctx->mediaengine->getLastTimeStamp());
		avcAu.dts = -1;
		result = SCE_MPEG_ERROR_NO_DATA;
	}

	// The avcau struct may have been modified by mediaengine, write it back.
	avcAu.write(auAddr);

	if (result == 0) {
		// Jeanne d'Arc return 00000000 as attrAddr here and cause WriteMemoryOrRaiseException error
		if (Memory::IsValidAddress(attrAddr)) {
			write32(attrAddr, 1);
		}
	}

	// TODO: sceMpegGetAvcAu seems to modify esSize, and delay when it's > 1000 or something.
	// There's definitely more to it, but ultimately it seems games should expect it to delay randomly.
	delayResult(c, result, 100);
}

void sceMpegFinish(Ctx* c) {
	if (!isMpegInit) {
		WARN_LOG(Log::Mpeg, "sceMpegFinish(...): not initialized");
	} else {
		INFO_LOG(Log::Mpeg, "sceMpegFinish()");
	}
	isMpegInit = false;
	delayResult(c, 0, 250);
}

void sceMpegGetAtracAu(Ctx* c, u32 mpeg, u32 streamId, u32 auAddr, u32 attrAddr) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) {
		WARN_LOG(Log::Mpeg, "sceMpegGetAtracAu: bad mpeg handle");
		ret(c, (u32)-1);
		return;
	}

	SceMpegRingBuffer *ringbuffer = ringPtr(ctx->mpegRingbufferAddr);
	if (!ringbuffer) {
		// Would have crashed before, TODO test behavior.
		WARN_LOG(Log::Mpeg, "sceMpegGetAtracAu: invalid ringbuffer address");
		ret(c, (u32)-1);
		return;
	}

	SceMpegAu atracAu;
	atracAu.read(auAddr);

	auto streamInfo = ctx->streamMap.find(streamId);
	if (streamInfo != ctx->streamMap.end() && streamInfo->second.needsReset) {
		atracAu.pts = 0;
		streamInfo->second.needsReset = false;
	}
	if (streamInfo == ctx->streamMap.end()) {
		WARN_LOG_REPORT(Log::Mpeg, "sceMpegGetAtracAu: invalid audio stream %08x", streamId);
		// TODO: Why was this changed to not return an error?
	}

	if (streamInfo != ctx->streamMap.end() && ctx->streamIgnoreMap[streamId]) {
		atracAu.pts = ctx->mediaengine->getAudioTimeStamp() + ctx->mpegFirstTimestamp;
		atracAu.dts = atracAu.pts;
		atracAu.esBuffer = streamInfo->second.num;
		atracAu.write(auAddr);
		if (Memory::IsValidAddress(attrAddr) && (attrAddr & 3) == 0) {
			write32(attrAddr, 0);
		}
		delayResult(c, 0, 100);
		return;
	}

	// The audio can end earlier than the video does.
	if (ringbuffer->packetsAvail == 0) {
		atracAu.pts = 0;
		atracAu.dts = 0;
		atracAu.write(auAddr);
		// TODO: Does this really delay?
		delayResult(c, SCE_MPEG_ERROR_NO_DATA, mpegDecodeErrorDelayMs);
		return;
	}

	// esBuffer is the memory where this au data goes.  We don't write the data to memory.
	// Instead, let's abuse it to keep track of the stream number.
	if (streamInfo != ctx->streamMap.end()) {
		atracAu.esBuffer = streamInfo->second.num;
		ctx->mediaengine->setAudioStream(streamInfo->second.num);
	}

	int result = 0;
	atracAu.pts = ctx->mediaengine->getAudioTimeStamp() + ctx->mpegFirstTimestamp;
	atracAu.dts = atracAu.pts;

	if (ctx->mediaengine->IsNoAudioData()) {
		atracAu.dts = -1;
		result = SCE_MPEG_ERROR_NO_DATA;
	}

	if (ctx->atracRegistered && ctx->mediaengine->IsNoAudioData() && !ctx->endOfAudioReached) {
		WARN_LOG(Log::Mpeg, "Audio end reach. pts: %i dts: %i", (int)atracAu.pts, (int)ctx->mediaengine->getLastTimeStamp());
		ctx->endOfAudioReached = true;
	}

	atracAu.write(auAddr);

	if (result == 0) {
		// 3rd birthday return 00000000 as attrAddr here and cause WriteMemoryOrRaiseException error
		if (Memory::IsValidAddress(attrAddr) && (attrAddr & 3) == 0) {
			write32(attrAddr, 0);
		}
	}

	// TODO: Not clear on exactly when this delays.
	delayResult(c, result, 100);
}

u32 sceMpegFlushAllStream(u32 mpeg) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) {
		return -1;
	}

	ctx->isAnalyzed = false;

	SceMpegRingBuffer *ringbuffer = ringPtr(ctx->mpegRingbufferAddr);
	if (ringbuffer) {
		ringbuffer->packetsAvail = 0;
		ringbuffer->packetsRead = 0;
		ringbuffer->packetsWritePos = 0;
	}

	INFO_LOG(Log::Mpeg, "sceMpegFlushAllStream(%08x): %d video frames decoded", mpeg, ctx->videoFrameCount);
	return 0;
}

void sceMpegAtracDecode(Ctx* c, u32 mpeg, u32 auAddr, u32 bufferAddr, int init) {
	MpegContext *ctx = getMpegCtx(mpeg);
	if (!ctx) {
		WARN_LOG(Log::Mpeg, "sceMpegAtracDecode: bad mpeg handle");
		ret(c, (u32)-1);
		return;
	}

	if (!Memory::IsValidAddress(bufferAddr)) {
		WARN_LOG(Log::Mpeg, "sceMpegAtracDecode: invalid addresses");
		ret(c, (u32)-1);
		return;
	}

	SceMpegAu atracAu;
	atracAu.read(auAddr);

	// We kept track of the stream number here in sceMpegGetAtracAu().
	ctx->mediaengine->setAudioStream(atracAu.esBuffer);

	if (Memory::IsValidRange(bufferAddr, MPEG_ATRAC_ES_OUTPUT_SIZE))
		memset(hostPtr(bufferAddr), 0, MPEG_ATRAC_ES_OUTPUT_SIZE);
	ctx->mediaengine->getAudioSamples(bufferAddr);
	if (g_dumpDir && Memory::IsValidRange(bufferAddr, MPEG_ATRAC_ES_OUTPUT_SIZE)) {
		// Raw 44.1 kHz stereo s16le, as handed to the game.
		char path[512];
		std::snprintf(path, sizeof(path), "%s/movie%02d_audio.raw", g_dumpDir, g_dumpMovie);
		if (FILE* f = std::fopen(path, "ab")) {
			std::fwrite(hostPtr(bufferAddr), 1, MPEG_ATRAC_ES_OUTPUT_SIZE, f);
			std::fclose(f);
		}
	}
	atracAu.pts = ctx->mediaengine->getAudioTimeStamp() + ctx->mpegFirstTimestamp;

	atracAu.write(auAddr);

	SceMpegRingBuffer *ringbuffer = ringPtr(ctx->mpegRingbufferAddr);
	if (ringbuffer) {
		ringbuffer->packetsAvail = ringbuffer->packets - ctx->mediaengine->getRemainSize() / 2048;
	}

	delayResult(c, 0, atracDecodeDelayMs);
}

u32 sceMpegRingbufferDestruct(u32 ringbufferAddr) {
	// Apparently, does nothing.
	return 0;
}

// HLE_CLEAR_STACK_BYTES: the firmware leaves this many zeroed bytes below the caller's sp.
void clearStack(Ctx* c, u32 bytes) {
	u32 sp = c->r[29];
	if (Memory::IsValidRange(sp - bytes, bytes))
		memset(hostPtr(sp - bytes), 0, bytes);
}

} // namespace

namespace psp::kernel {

// Called by sceKernelLoadModule with the module info of the loaded PRX (PPSSPP
// __MpegLoadModule): the sceMpeg_library version selects version-specific behaviour.
void mpegNotifyModuleLoaded(const std::string& name, int version) {
	if (name == "sceMpeg_library") {
		mpegLibVersion = version;
		INFO_LOG(Log::Mpeg, "sceMpeg_library version %04x", version);
	}
}

void registerMpegHle() {
	registerHle("sceMpegInit", [](Ctx* c) { clearStack(c, 0x48); sceMpegInit(c); });
	registerHle("sceMpegFinish", [](Ctx* c) { clearStack(c, 0x18); sceMpegFinish(c); });
	registerHle("sceMpegQueryMemSize", [](Ctx* c) { clearStack(c, 0x18); ret(c, MpegRequiredMem()); });
	registerHle("sceMpegCreate", [](Ctx* c) {
		clearStack(c, 0xA8);
		sceMpegCreate(c, A(c, 0), A(c, 1), A(c, 2), A(c, 3), A(c, 4), A(c, 5), A(c, 6));
	});
	registerHle("sceMpegDelete", [](Ctx* c) { clearStack(c, 0x18); sceMpegDelete(c, A(c, 0)); });
	registerHle("sceMpegQueryStreamOffset", [](Ctx* c) { clearStack(c, 0x18); ret(c, (u32)sceMpegQueryStreamOffset(A(c, 0), A(c, 1), A(c, 2))); });
	registerHle("sceMpegQueryStreamSize", [](Ctx* c) { clearStack(c, 0x8); ret(c, sceMpegQueryStreamSize(A(c, 0), A(c, 1))); });
	registerHle("sceMpegRegistStream", [](Ctx* c) { clearStack(c, 0x48); ret(c, (u32)sceMpegRegistStream(A(c, 0), A(c, 1), A(c, 2))); });
	registerHle("sceMpegUnRegistStream", [](Ctx* c) { clearStack(c, 0x18); ret(c, sceMpegUnRegistStream(A(c, 0), (int)A(c, 1))); });
	registerHle("sceMpegMallocAvcEsBuf", [](Ctx* c) { ret(c, (u32)sceMpegMallocAvcEsBuf(A(c, 0))); });
	registerHle("sceMpegFreeAvcEsBuf", [](Ctx* c) { ret(c, (u32)sceMpegFreeAvcEsBuf(A(c, 0), (int)A(c, 1))); });
	registerHle("sceMpegQueryAtracEsSize", [](Ctx* c) { ret(c, (u32)sceMpegQueryAtracEsSize(A(c, 0), A(c, 1), A(c, 2))); });
	registerHle("sceMpegInitAu", [](Ctx* c) { ret(c, (u32)sceMpegInitAu(A(c, 0), A(c, 1), A(c, 2))); });
	registerHle("sceMpegGetAvcAu", [](Ctx* c) { sceMpegGetAvcAu(c, A(c, 0), A(c, 1), A(c, 2), A(c, 3)); });
	registerHle("sceMpegGetAtracAu", [](Ctx* c) { sceMpegGetAtracAu(c, A(c, 0), A(c, 1), A(c, 2), A(c, 3)); });
	registerHle("sceMpegFlushAllStream", [](Ctx* c) { ret(c, sceMpegFlushAllStream(A(c, 0))); });
	registerHle("sceMpegAvcDecode", [](Ctx* c) { sceMpegAvcDecode(c, A(c, 0), A(c, 1), A(c, 2), A(c, 3), A(c, 4)); });
	registerHle("sceMpegAvcDecodeMode", [](Ctx* c) { ret(c, (u32)sceMpegAvcDecodeMode(A(c, 0), A(c, 1))); });
	registerHle("sceMpegAvcDecodeStop", [](Ctx* c) { ret(c, sceMpegAvcDecodeStop(A(c, 0), A(c, 1), A(c, 2), A(c, 3))); });
	registerHle("sceMpegAtracDecode", [](Ctx* c) { sceMpegAtracDecode(c, A(c, 0), A(c, 1), A(c, 2), (int)A(c, 3)); });
	registerHle("sceMpegRingbufferQueryMemSize", [](Ctx* c) { ret(c, __MpegRingbufferQueryMemSize((int)A(c, 0))); });
	registerHle("sceMpegRingbufferConstruct", [](Ctx* c) {
		ret(c, sceMpegRingbufferConstruct(c->r[28], A(c, 0), A(c, 1), A(c, 2), A(c, 3), A(c, 4), A(c, 5)));
	});
	registerHle("sceMpegRingbufferDestruct", [](Ctx* c) { ret(c, sceMpegRingbufferDestruct(A(c, 0))); });
	registerHle("sceMpegRingbufferPut", [](Ctx* c) { ret(c, sceMpegRingbufferPut(c, A(c, 0), (int)A(c, 1), (int)A(c, 2))); });
	registerHle("sceMpegRingbufferAvailableSize", [](Ctx* c) { ret(c, (u32)sceMpegRingbufferAvailableSize(A(c, 0))); });
}

} // namespace psp::kernel
