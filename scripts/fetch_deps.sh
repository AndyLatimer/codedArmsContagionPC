#!/usr/bin/env bash
# Downloads third-party dependencies that are not committed (SDL2 MinGW dev package).
set -euo pipefail
cd "$(dirname "$0")/../third_party"
if [ ! -d SDL2-2.30.9 ]; then
  curl -sSL -o sdl.tgz https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-mingw.tar.gz
  tar xzf sdl.tgz && rm sdl.tgz
fi
