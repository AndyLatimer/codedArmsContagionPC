#!/usr/bin/env bash
# Builds the Linux binaries and runs the unit tests.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux -j"$(nproc)"
ctest --test-dir build/linux --output-on-failure
