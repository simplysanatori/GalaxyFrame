#!/bin/bash
# Builds the Steam Frame (Linux arm64) target and packages it for the
# Releases page: steam-frame/release/galaxyframe-<version>-linux-arm64.tar.gz
# with the launcher, the stripped game library, the shader library of the
# Vulkan backend (libgq_glsl.so, left out when glslang was not available to
# build it; the game then uses GLES), the launch wrapper and, if you give one,
# a Vulkan shader cache seed, the OpenXR loader (libopenxr_loader.so.1, when the
# build has one), and install.sh, steam-cef.py and uninstall.sh at
# the top level (the unpacked folder installs itself).  SHA256SUMS is written next to the tarball.
#   steam-frame/build.sh [version [spirv.bin]]
# The version defaults to 1.1.1.  spirv.bin is optional: the SPIR-V cache of
# a played session (~/.local/share/GalaxyFrame/spirv.bin).  It goes into the
# tarball as it is, and install.sh puts it into the data folder only when
# there is no spirv.bin there yet, so a first launch compiles fewer shaders.
# GALAXYFRAME_SPIRV_SEED=FILE names it as well.
# With GALAXYFRAME_NO_BUILD=1 (or the old GALAXYQUEST_NO_BUILD=1) it packages
# the build in build-linux/ as it is.  GALAXYFRAME_BUILD_DIR names another build
# folder; steam-frame/tools/build-portable.sh uses it (build-portable/).
# The folder is checked with steam-frame/tools/check-bundle.sh before it is
# packed, and nothing is packed if the check fails: the release must run on
# every SteamOS build (glibc 2.39 and the libraries SteamOS has).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE/.."
VERSION=${1:-1.1.1}
SEED=${2:-$GALAXYFRAME_SPIRV_SEED}
[ -z "$SEED" ] || [ -f "$SEED" ] || { echo "no such shader cache seed: $SEED" >&2; exit 1; }

B=${GALAXYFRAME_BUILD_DIR:-build-linux}
[ -n "$GALAXYFRAME_NO_BUILD$GALAXYQUEST_NO_BUILD" ] || ./build_linux.sh   # GALAXYFRAME_NO_BUILD=1: package the existing build-linux/
mkdir -p steam-frame/release
STAGE=$(mktemp -d)
cp $B/galaxyframe $B/petari_headless "$STAGE/"
strip --strip-unneeded -o "$STAGE/libgame.so" $B/libgame.so
FILES="galaxyframe petari_headless libgame.so galaxyframe.sh"
if [ -f $B/libopenxr_loader.so.1 ]; then
  strip --strip-unneeded -o "$STAGE/libopenxr_loader.so.1" $B/libopenxr_loader.so.1
  FILES="$FILES libopenxr_loader.so.1"
fi
if [ -f $B/libgq_glsl.so ]; then
  strip --strip-unneeded -o "$STAGE/libgq_glsl.so" $B/libgq_glsl.so
  FILES="$FILES libgq_glsl.so"
else
  echo "note: $B/libgq_glsl.so is missing: the release has no Vulkan shader library (GLES only)" >&2
fi
cp steam-frame/galaxyframe.sh tools/vrtiming.py "$STAGE/"
FILES="$FILES vrtiming.py"
# The installer, its helper and the uninstaller: a tool that unpacks the tarball (Frametanium) runs
# install.sh from the unpacked folder.
cp steam-frame/install.sh steam-frame/steam-cef.py steam-frame/uninstall.sh "$STAGE/"
FILES="$FILES install.sh steam-cef.py uninstall.sh"
if [ -n "$SEED" ]; then
  cp "$SEED" "$STAGE/spirv.bin"
  FILES="$FILES spirv.bin"
fi
steam-frame/tools/check-bundle.sh "$STAGE" || { echo "build.sh: the bundle check failed, nothing was packed" >&2; rm -rf "$STAGE"; exit 1; }
NAME=galaxyframe-$VERSION-linux-arm64.tar.gz
OUT=steam-frame/release/$NAME
tar -czf "$OUT" -C "$STAGE" $FILES
rm -rf "$STAGE"
(cd steam-frame/release && sha256sum "$NAME" | tee SHA256SUMS)
