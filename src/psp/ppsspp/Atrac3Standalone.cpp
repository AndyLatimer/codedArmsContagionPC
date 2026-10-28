// Modified for the Coded Arms PC port: PPSSPP includes replaced, CreateAudioDecoder/AudioClose/
// GetCodecName (from Core/HW/SimpleAudioDec.cpp) appended. Original: Core/HW/Atrac3Standalone.cpp,
// github.com/hrydgard/ppsspp (GPL-2.0-or-later; Copyright (c) 2013- PPSSPP Project).

#include <algorithm>

#include "SimpleAudioDec.h"
#include "at3_standalone/at3_decoders.h"

class AudioDecoder;
AudioDecoder *CreateAtrac3Audio(int channels, size_t blockAlign, const uint8_t *extraData, size_t extraDataSize);
AudioDecoder *CreateAtrac3PlusAudio(int channels, size_t blockAlign);

inline int16_t clamp16(float f) {
	if (f >= 1.0f)
		return 32767;
	else if (f <= -1.0f)
		return -32767;
	else
		return (int)(f * 32767);
}

// Uses our standalone AT3/AT3+ decoder derived from FFMPEG
// Test case for ATRAC3: Mega Man Maverick Hunter X, PSP menu sound
class Atrac3Audio : public AudioDecoder {
public:
	Atrac3Audio(PSPAudioType audioType, int channels, size_t blockAlign, const uint8_t *extraData, size_t extraDataSize)
		: audioType_(audioType), channels_(channels) {
		blockAlign_ = (int)blockAlign;
		if (audioType_ == PSP_CODEC_AT3) {
			at3Ctx_ = atrac3_alloc(channels, &blockAlign_, extraData, (int)extraDataSize);
			if (at3Ctx_) {
				codecOpen_ = true;
			} else {
				ERROR_LOG(Log::ME, "Failed to open atrac3 context! !channels=%d blockAlign=%d ed=%d)", channels, (int)blockAlign, (int)extraDataSize);
				codecFailed_ = true;
			}
		}
		for (int i = 0; i < 2; i++) {
			buffers_[i] = new float[4096];
		}
	}
	~Atrac3Audio() override {
		if (at3Ctx_) {
			atrac3_free(at3Ctx_);
		}
		if (at3pCtx_) {
			atrac3p_free(at3pCtx_);
		}
		for (int i = 0; i < 2; i++) {
			delete[] buffers_[i];
		}
	}

	bool IsOK() const override {
		return codecOpen_;
	}

	int LastError() const override {
		return lastError_;
	}

	void FlushBuffers() override {
		if (at3Ctx_) {
			atrac3_flush_buffers(at3Ctx_);
		}
		if (at3pCtx_) {
			atrac3p_flush_buffers(at3pCtx_);
		}
	}

	bool Decode(const uint8_t *inbuf, int inbytes, int *inbytesConsumed, int outputChannels, int16_t *outbuf, int *outSamples) override {
		if (outSamples)
			*outSamples = 0;
		if (inbytesConsumed)
			*inbytesConsumed = 0;
		if (!codecOpen_) {
			// We delay the codecOpen until the first decode, so the setChannels call from MediaEngine::getAudioSamples
			// can take effect. Note, we don't do this with Atrac3, just Atrac3+.
			if (codecFailed_) {
				return false;
			}
			if (audioType_ == PSP_CODEC_AT3PLUS) {
				at3pCtx_ = atrac3p_alloc(channels_, &blockAlign_);
				if (at3pCtx_) {
					codecOpen_ = true;
				} else {
					ERROR_LOG(Log::ME, "Failed to open atrac3+ context! (channels=%d blockAlign=%d)", channels_, (int)blockAlign_);
					codecFailed_ = true;
				}
			}
			if (!codecOpen_) {
				WARN_LOG_N_TIMES(codecNotOpen, 5, Log::ME, "Atrac3Audio:Decode: Codec not open, not decoding");
				return false;
			}
		}
		if (inbytes != blockAlign_ && blockAlign_ != 0) {
			WARN_LOG(Log::ME, "Atrac3Audio::Decode: inbytes not matching expected blockalign. Updating blockAlign_. Got %d bytes, expected %d. (%s)", inbytes, blockAlign_, at3pCtx_ ? "Atrac3+" : "Atrac3");
		}

		blockAlign_ = inbytes;
		// We just call the decode function directly without going through the whole packet machinery.
		int result;
		int nb_samples = 0;
		if (audioType_ == PSP_CODEC_AT3PLUS) {
			result = atrac3p_decode_frame(at3pCtx_, buffers_, &nb_samples, inbuf, inbytes);
		} else {
			result = atrac3_decode_frame(at3Ctx_, buffers_, &nb_samples, inbuf, inbytes);
		}
		lastError_ = result < 0 ? result : 0;
		if (result < 0) {
			// NOTE: Here, to recover from single bad packets, we update inBytesConsumed/outSamples with the regular packet size.
			// Otherwise we might try to decode the same packet over and over.
			// This is seen in some unofficial game mods, mainly.
			if (inbytesConsumed) {
				*inbytesConsumed = inbytes;
			}
			if (outSamples) {
				if (*outSamples != 0) {
					nb_samples = std::min(*outSamples, nb_samples);
				}
				*outSamples = nb_samples;
			}
			return false;
		}
		if (inbytesConsumed) {
			*inbytesConsumed = result;
		}
		if (outSamples) {
			// Allow capping the output samples by setting *outSamples to non-zero.
			if (*outSamples != 0) {
				nb_samples = std::min(*outSamples, nb_samples);
			}
			*outSamples = nb_samples;
		}
		if (nb_samples > 0) {
			if (outSamples) {
				*outSamples = nb_samples;
			}
			if (outbuf) {
				
				const float *left = buffers_[0];
				if (outputChannels == 2) {
					// Stereo output, standard.
					const float *right = channels_ == 2 ? buffers_[1] : buffers_[0];
					for (int i = 0; i < nb_samples; i++) {
						outbuf[i * 2] = clamp16(left[i]);
						outbuf[i * 2 + 1] = clamp16(right[i]);
					}
				} else {
					// Mono output, just take the left channel.
					for (int i = 0; i < nb_samples; i++) {
						outbuf[i] = clamp16(left[i]);
					}
				}
			}
		}
		return true;
	}

	void SetChannels(int channels) override {
		_dbg_assert_(audioType_ == PSPAudioType::PSP_CODEC_AT3PLUS);
		if (audioType_ == PSPAudioType::PSP_CODEC_AT3PLUS) {
			if (!at3pCtx_) {
				// Codec shouldn't be open yet.
				_dbg_assert_(!codecOpen_);
				channels_ = 1;
			} else {
				// This will come every packet, just ignore it.
			}
		}
	}

	PSPAudioType GetAudioType() const override { return audioType_; }

private:
	ATRAC3PContext *at3pCtx_ = nullptr;
	ATRAC3Context *at3Ctx_ = nullptr;

	int channels_ = 0;
	int blockAlign_ = 0;

	float *buffers_[2]{};

	bool codecOpen_ = false;
	bool codecFailed_ = false;
	int lastError_ = 0;

	PSPAudioType audioType_;
};

AudioDecoder *CreateAtrac3Audio(int channels, size_t blockAlign, const uint8_t *extraData, size_t extraDataSize) {
	return new Atrac3Audio(PSP_CODEC_AT3, channels, blockAlign, extraData, extraDataSize);
}
AudioDecoder *CreateAtrac3PlusAudio(int channels, size_t blockAlign) {
	return new Atrac3Audio(PSP_CODEC_AT3PLUS, channels, blockAlign, nullptr, 0);
}

// ---- from Core/HW/SimpleAudioDec.cpp (ATRAC only) ----
AudioDecoder *CreateAudioDecoder(PSPAudioType audioType, int sampleRateHz, int channels, size_t blockAlign, const uint8_t *extraData, size_t extraDataSize) {
	switch (audioType) {
	case PSP_CODEC_AT3:
		return CreateAtrac3Audio(channels, blockAlign, extraData, extraDataSize);
	case PSP_CODEC_AT3PLUS:
		return CreateAtrac3PlusAudio(channels, blockAlign);
	default:
		ERROR_LOG(Log::ME, "Audio codec %s not available in this port", GetCodecName(audioType));
		return nullptr;
	}
}

void AudioClose(AudioDecoder **ctx) {
	delete *ctx;
	*ctx = 0;
}

static const char *const codecNames[4] = {
	"AT3+", "AT3", "MP3", "AAC",
};

const char *GetCodecName(int codec) {
	if (codec >= PSP_CODEC_AT3PLUS && codec <= PSP_CODEC_AAC) {
		return codecNames[codec - PSP_CODEC_AT3PLUS];
	} else {
		return "(unk)";
	}
}
