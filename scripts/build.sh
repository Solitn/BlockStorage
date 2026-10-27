#!/usr/bin/env sh
# Build BlockStorage on Linux / macOS.
# Requires: cmake (>= 3.16), a C++17 compiler, OpenSSL development headers
#   Debian/Ubuntu: sudo apt install build-essential cmake libssl-dev
#   macOS:         brew install cmake openssl
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build="$root/build"

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --parallel

echo "built: $build/BlockStorage"
