ATRAC3 / ATRAC3+ decoders from FFmpeg, made standalone by PPSSPP (ext/at3_standalone,
https://github.com/hrydgard/ppsspp). Copied unmodified (LGPL-2.1-or-later, see the file
headers); aac_defines.h, which nothing here uses, was left out. shim/ holds minimal
stand-ins for the three PPSSPP headers the sources include (Common/Log.h,
Common/MemoryUtil.h, ppsspp_config.h). Used by src/psp/ppsspp/MediaEngine.cpp to decode
the ATRAC3+ audio of PSMF movies, exactly as PPSSPP's MediaEngine does.
