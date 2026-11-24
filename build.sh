#!/bin/sh
# Linux build + test + package.   Needs: cmake g++ libsqlite3-dev xorg-dev libgl1-mesa-dev
set -e
cd "$(dirname "$0")"
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
echo "Built. Run: build/novapm (GUI)   build/novapm-cli help.   Package: cd build && cpack"
