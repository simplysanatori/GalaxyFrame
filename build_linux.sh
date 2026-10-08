#!/bin/bash
# Configures and builds the Linux arm64 target (the Steam Frame's own SteamVR
# runtime): build-linux/libgame.so (the game) and build-linux/galaxyquest
# (the launcher).
#   ./build_linux.sh [ninja targets...]
# Needs clang, lld, CMake 3.24+, Ninja, the EGL/GLES and SDL2 headers, and
# SteamVR (its OpenXR loader, /opt/steamvr/bin/linuxarm64/libopenxr_loader.so).
# The OpenXR headers come from tools/fetch_openxr.sh, as for Android.
set -e
cd "$(dirname "$0")"

[ -f third_party/openxr/prefab/modules/headers/include/openxr/openxr.h ] || tools/fetch_openxr.sh

BUILD=${BUILD:-build-linux}
if [ ! -f $BUILD/build.ninja ]; then
  cmake -S . -B $BUILD -G Ninja \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=${CONFIG:-RelWithDebInfo}
fi
ninja -C $BUILD "$@"
