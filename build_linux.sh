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

# Share the machine-wide build lock and ccache with ~/Games/GalaxyQuest/gqbuild.sh: one build
# at a time (RAM and 5 CPUs), worktrees under ~/Games/GalaxyQuest share cache hits.
# GQ_NO_LOCK=1 skips the lock (only for a build that is already inside gqbuild.sh).
GQROOT=${GQROOT:-$HOME/Games/GalaxyQuest}
if [ -z "$GQ_NO_LOCK" ] && [ -d "$GQROOT" ]; then
  exec 9>"$GQROOT/.build.lock"
  echo "build_linux: waiting for $GQROOT/.build.lock"; flock 9
fi
export CCACHE_BASEDIR=${CCACHE_BASEDIR:-$GQROOT} CCACHE_DIR=${CCACHE_DIR:-$HOME/.cache/ccache-gq}
L=""
command -v ccache >/dev/null && L="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
if [ ! -f $BUILD/build.ninja ] || { [ -n "$L" ] && ! grep -q 'CXX_COMPILER_LAUNCHER:.*ccache' $BUILD/CMakeCache.txt; }; then
  cmake -S . -B $BUILD -G Ninja \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=${CONFIG:-RelWithDebInfo} $L
fi
nice -n 5 ninja -j${JOBS:-5} -C $BUILD "$@"
