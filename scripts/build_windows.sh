#!/usr/bin/env bash
# Cross-compiles the Windows build with MinGW-w64 and packages dist/CodedArmsPort-win64.zip.
# Requires: x86_64-w64-mingw32-g++ and third_party/SDL2-2.30.9 (scripts/fetch_deps.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build/win64 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/win64 -j"$(nproc)"
out=dist/CodedArmsPort-win64
rm -rf "$out" && mkdir -p "$out/game"
cp build/win64/CodedArms.exe build/win64/ca_assetcheck.exe "$out/"
cp third_party/SDL2-2.30.9/x86_64-w64-mingw32/bin/SDL2.dll "$out/"
cp README.md PROGRESS.md TECHNICAL_NOTES.md "$out/"
cp docs/LEEME_WINDOWS.txt "$out/LEEME.txt"
echo "Copia aqui tu ISO de Coded Arms con el nombre CodedArms.iso" > "$out/game/PON_AQUI_TU_ISO.txt"
(cd dist && rm -f CodedArmsPort-win64.zip && zip -qr CodedArmsPort-win64.zip CodedArmsPort-win64)
echo "Package: dist/CodedArmsPort-win64.zip"
