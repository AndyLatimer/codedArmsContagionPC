# FFmpeg (minimal static build for movie playback)

Used by the sceMpeg HLE (`src/psp/hle_mpeg.cpp`, `src/psp/ppsspp/MediaEngine.cpp`) to
demux PSMF movies (MPEG-PS) and decode their H.264 video, as PPSSPP does. ATRAC3+ audio
does not use FFmpeg (see `src/psp/ppsspp/at3_standalone`).

- Version: FFmpeg 3.0.2, the release PPSSPP bundles (its `ffmpeg` submodule,
  https://github.com/hrydgard/ppsspp-ffmpeg), so decoding matches PPSSPP.
- Configuration: `--disable-everything` plus the h264 decoder and parser, the mpegps
  demuxer, avformat/avcodec/avutil/swscale; static, no asm (`--disable-yasm`), LGPL only
  (no `--enable-gpl`/`--enable-nonfree`). Licence: LGPL-2.1-or-later, `COPYING.LGPLv2.1`.
- Built with `tools/build_ffmpeg.sh <ffmpeg-3.0.2-source> <target>`:
  - `linux-x86_64/`: host GCC.
  - `mingw-x86_64/`: cross-built with `x86_64-w64-mingw32-gcc` (Win32 threads). Picked up
    automatically by CMake for MinGW builds (`-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake`).

TODO: MSVC builds have no FFmpeg here yet (PPSSPP's `windows_x64-build.sh` builds one with
`--toolchain=msvc` under MSYS); set `CA_FFMPEG_DIR` to such a build if needed.
