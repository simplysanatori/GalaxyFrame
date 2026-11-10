#!/bin/bash
# Builds the Steam Frame target so that it runs on every SteamOS build (glibc 2.39 and newer), and packs it:
#   steam-frame/tools/build-portable.sh [version [spirv.bin]]
# - compiles and links against a glibc 2.39 + GCC 15 libstdc++ sysroot (SYSROOT), so no symbol newer than
#   GLIBC_2.39 is needed; SteamOS's own EGL, GLESv2 and SDL2 are only linked (they are on every build);
# - builds the Khronos OpenXR loader (OPENXR_SDK_SRC, release-1.1.63 or newer) as libopenxr_loader.so.1 with a
#   proper soname, which libgame.so loads from its own folder instead of SteamVR's soname-less copy;
# - runs steam-frame/build.sh, which checks the result with tools/check-bundle.sh and writes the tarball.
# SYSROOT: a directory with glibc 2.39's usr/include and usr/lib and GCC 15's libstdc++ (headers and
# libstdc++.so.6, libgcc_s.so.1, crt files, lib/gcc/<triple>/<ver>), laid out like an ordinary root.
# Needs clang, lld, CMake, Ninja, glslc, and the static glslang libraries (GQ_GLSLANG_LIB_DIR, /usr/lib).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SYSROOT=${SYSROOT:?set SYSROOT to the glibc 2.39 sysroot}
OPENXR_SDK_SRC=${OPENXR_SDK_SRC:?set OPENXR_SDK_SRC to an unpacked OpenXR-SDK source tree}
BUILD=$ROOT/build-portable
STAGE=$BUILD/host
mkdir -p "$STAGE/inc" "$STAGE/lib" "$STAGE/pc"
# Only the headers and libraries that are used, not the host's whole /usr (its glibc is newer).
for d in EGL GLES2 GLES3 KHR SDL2 vulkan vk_video glslang X11 xcb GL; do ln -sfn "/usr/include/$d" "$STAGE/inc/$d"; done
for f in /usr/include/wayland-*.h; do ln -sfn "$f" "$STAGE/inc/"; done
for l in libEGL.so.1 libGLESv2.so.2 libSDL2-2.0.so.0 libGLdispatch.so.0; do
  [ -e "/usr/lib/$l" ] && ln -sfn "$(readlink -f "/usr/lib/$l")" "$STAGE/lib/$l"
done
ln -sfn libEGL.so.1 "$STAGE/lib/libEGL.so"; ln -sfn libGLESv2.so.2 "$STAGE/lib/libGLESv2.so"; ln -sfn libSDL2-2.0.so.0 "$STAGE/lib/libSDL2.so"
GQ_GLSLANG_LIB_DIR=${GQ_GLSLANG_LIB_DIR:-/usr/lib}
for a in libGenericCodeGen libglslang libglslang-default-resource-limits libMachineIndependent libOSDependent libSPIRV; do
  ln -sfn "$GQ_GLSLANG_LIB_DIR/$a.a" "$STAGE/lib/$a.a"
done
cat > "$STAGE/pc/sdl2.pc" << PC
Name: sdl2
Description: sdl2
Version: 2.0
Cflags: -I$STAGE/inc/SDL2
Libs: -L$STAGE/lib -lSDL2
PC
cat > "$STAGE/toolchain.cmake" << TC
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_SYSROOT $SYSROOT)
set(CMAKE_C_FLAGS_INIT "-isystem $STAGE/inc")
set(CMAKE_CXX_FLAGS_INIT "-isystem $STAGE/inc")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -L$STAGE/lib -Wl,--allow-shlib-undefined")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld -L$STAGE/lib -Wl,--allow-shlib-undefined")
TC

[ -f "$ROOT/third_party/openxr/prefab/modules/headers/include/openxr/openxr.h" ] || bash "$ROOT/tools/fetch_openxr.sh"

cmake -S "$OPENXR_SDK_SRC" -B "$BUILD/openxr" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$STAGE/toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DBUILD_API_LAYERS=OFF -DBUILD_TESTS=OFF \
  -DBUILD_CONFORMANCE_TESTS=OFF -DDYNAMIC_LOADER=ON -DBUILD_WITH_SYSTEM_JSONCPP=OFF > "$BUILD/openxr-configure.log"
ninja -C "$BUILD/openxr" openxr_loader
cp -L "$BUILD/openxr/src/loader/libopenxr_loader.so.1" "$BUILD/libopenxr_loader.so.1"
cp "$BUILD/libopenxr_loader.so.1" "$STAGE/lib/libopenxr_loader.so.1"

L=""; command -v ccache > /dev/null && L="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
PKG_CONFIG_LIBDIR="$STAGE/pc" cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$STAGE/toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOPENXR_LOADER="$BUILD/libopenxr_loader.so.1" -DGQ_OPENXR_SONAME=libopenxr_loader.so.1 \
  -DGQ_GLSLANG_LIB_DIR="$STAGE/lib" $L > "$BUILD/configure.log"
nice -n 5 ninja -j"${JOBS:-5}" -C "$BUILD"

GALAXYFRAME_NO_BUILD=1 GALAXYFRAME_BUILD_DIR=build-portable exec "$ROOT/steam-frame/build.sh" "$@"
