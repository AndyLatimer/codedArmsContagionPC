#!/usr/bin/env bash
# Cross-compiles the Windows build with MinGW-w64 and packages dist/CodedArmsContagion-win64.zip.
# Usage: scripts/build_windows.sh /path/to/your/Coded.Arms.Contagion.iso
# Requires: x86_64-w64-mingw32-g++ and third_party/SDL2-2.30.9 (scripts/fetch_deps.sh).
# The package contains code recompiled from your own game image: it is for your own use only and
# must not be distributed (dist/ is not committed).
set -euo pipefail
iso=$(realpath "${1:?usage: $0 GAME.iso}")
cd "$(dirname "$0")/.."
cmake -S . -B build/win64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release -DCA_GAME_ISO="$iso"
cmake --build build/win64 -j"$(nproc)" --target CodedArmsContagion ca_psprun
out=dist/CodedArmsContagion-win64
rm -rf "$out" && mkdir -p "$out/game"
cp build/win64/CodedArmsContagion.exe build/win64/ca_psprun.exe "$out/"
cp third_party/SDL2-2.30.9/x86_64-w64-mingw32/bin/SDL2.dll "$out/"
cp README.md PROGRESS.md TECHNICAL_NOTES.md LICENSE "$out/"
cp docs/LEEME_WINDOWS.txt "$out/LEEME.txt"
echo "Copia aqui tu ISO de Coded Arms: Contagion con el nombre CodedArmsContagion.iso" > "$out/game/PON_AQUI_TU_ISO.txt"
(cd dist && rm -f CodedArmsContagion-win64.zip && zip -qr CodedArmsContagion-win64.zip CodedArmsContagion-win64)
echo "Package: dist/CodedArmsContagion-win64.zip"
