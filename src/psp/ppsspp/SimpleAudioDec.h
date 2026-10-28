// Modified for the Coded Arms PC port: reduced to the AudioDecoder interface and factory the
// MediaEngine uses (ATRAC3/ATRAC3+ through the standalone decoder; no FFmpeg audio, no AuCtx).
// Original: Core/HW/SimpleAudioDec.h, github.com/hrydgard/ppsspp
// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <cstddef>
#include <cstdint>

#include "mpeg_compat.h"

// From Core/HLE/sceAudiocodec.h
enum PSPAudioType {
	PSP_CODEC_AT3PLUS = 0x00001000,
	PSP_CODEC_AT3 = 0x00001001,
	PSP_CODEC_MP3 = 0x00001002,
	PSP_CODEC_AAC = 0x00001003,
};

// Decodes packet by packet - does NOT demux.

// This basically corresponds to the core of sceAudiocodec.
class AudioDecoder {
public:
	virtual ~AudioDecoder() {}

	virtual PSPAudioType GetAudioType() const = 0;

	// inbytesConsumed can include skipping metadata.
	// outSamples is in stereo samples. So you have to multiply by 4 for 16-bit stereo audio to get bytes.
	// For Atrac3, if *outSamples != 0, it'll cap the number of samples to output. In this case, its value can only shrink.
	virtual bool Decode(const uint8_t *inbuf, int inbytes, int *inbytesConsumed, int outputChannels, int16_t *outbuf, int *outSamples) = 0;
	virtual bool IsOK() const = 0;
	// The decoder's own code for why the last Decode failed, or 0.
	virtual int LastError() const { return 0; }

	// NOTE: This can come late (MediaEngine::getAudioSample)! But it will come before the first Decode.
	virtual void SetChannels(int channels) = 0;
	virtual void FlushBuffers() {}
};

void AudioClose(AudioDecoder **ctx);
const char *GetCodecName(int codec);  // audioType
// Only ATRAC3 and ATRAC3+ are available in this port; other types return nullptr.
AudioDecoder *CreateAudioDecoder(PSPAudioType audioType, int sampleRateHz = 44100, int channels = 2, size_t blockAlign = 0, const uint8_t *extraData = nullptr, size_t extraDataSize = 0);
