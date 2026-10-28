#!/bin/sh
# Builds the minimal static FFmpeg the sceMpeg HLE (src/psp/hle_mpeg.cpp, MediaEngine) needs:
# the H.264 decoder + parser, the MPEG-PS demuxer and swscale. Nothing else (ATRAC3+ audio is
# decoded by PPSSPP's standalone decoder in src/psp/ppsspp/at3_standalone).
#
# Uses the same FFmpeg release PPSSPP bundles (3.0.2, its "ffmpeg" submodule:
# https://github.com/hrydgard/ppsspp-ffmpeg) so movie decoding matches PPSSPP.
#
#   tools/build_ffmpeg.sh <ffmpeg-source-dir> <linux-x86_64|mingw-x86_64> [jobs]
#
# The build is out of tree (the source directory is only read). Results are installed to
# third_party/ffmpeg/<target>/{include,lib}, which CMakeLists.txt picks up.
set -e

SRC=$(cd "$1" && pwd)
TARGET=$2
JOBS=${3:-4}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "$SRC" ] || [ -z "$TARGET" ] || [ ! -x "$SRC/configure" ]; then
    echo "usage: $0 <ffmpeg-3.0.2-source-dir> <linux-x86_64|mingw-x86_64> [jobs]" >&2
    exit 2
fi
if [ -f "$SRC/config.h" ]; then
    echo "error: $SRC has been configured in-tree; out-of-tree builds need a clean source tree" >&2
    exit 1
fi

PREFIX="$ROOT/third_party/ffmpeg/$TARGET"
BUILD=${FFMPEG_BUILD_DIR:-"$ROOT/build/ffmpeg-$TARGET"}
mkdir -p "$BUILD"

case "$TARGET" in
linux-x86_64)
    PLATFORM="--arch=x86_64 --enable-pic"
    ;;
mingw-x86_64)
    command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "x86_64-w64-mingw32-gcc not found" >&2; exit 1; }
    # Native Win32 threads (no winpthreads dependency), like PPSSPP's Windows builds.
    PLATFORM="--arch=x86_64 --target-os=mingw32 --cross-prefix=x86_64-w64-mingw32- --enable-cross-compile
              --enable-w32threads --disable-pthreads"
    ;;
*)
    echo "unknown target $TARGET" >&2
    exit 2
    ;;
esac

cd "$BUILD"
# FFmpeg 3.0 predates GCC 10's -fno-common default and newer warnings-as-errors; the flags
# below keep it building with current compilers without changing the generated code paths.
"$SRC/configure" \
    --prefix="$PREFIX" \
    $PLATFORM \
    --disable-shared --enable-static \
    --disable-everything \
    --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-postproc \
    --disable-swresample --disable-network --disable-iconv --disable-zlib --disable-bzlib \
    --disable-lzma --disable-sdl --disable-xlib --disable-vaapi --disable-vdpau \
    --disable-videotoolbox --disable-vda --disable-dxva2 --disable-d3d11va \
    --disable-yasm --disable-debug --disable-stripping \
    --enable-decoder=h264 --enable-parser=h264 --enable-demuxer=mpegps \
    --extra-cflags="-D__STDC_CONSTANT_MACROS -O3 -fcommon -Wno-incompatible-pointer-types -Wno-int-conversion"

make -j"$JOBS"
make install
# Keep only what the port links against.
rm -rf "$PREFIX/lib/pkgconfig" "$PREFIX/share"
echo "FFmpeg installed to $PREFIX"
