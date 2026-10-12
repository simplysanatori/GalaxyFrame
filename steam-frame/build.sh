#!/bin/bash
# Builds the Steam Frame (Linux arm64) target and packages it for the
# Releases page: steam-frame/release/galaxyframe-<version>-linux-arm64.tar.gz
# with the launcher, the stripped game library, the shader library of the
# Vulkan backend (libgq_glsl.so, left out when glslang was not available to
# build it; the game then uses GLES), the launch wrapper and, if you give one,
# a Vulkan shader cache seed.  SHA256SUMS is written next to the tarball.
#   steam-frame/build.sh [version [spirv.bin]]
# The version defaults to 1.0.0.  spirv.bin is optional: the SPIR-V cache of
# a played session (~/.local/share/GalaxyFrame/spirv.bin).  It goes into the
# tarball as it is, and install.sh puts it into the data folder only when
# there is no spirv.bin there yet, so a first launch compiles fewer shaders.
# GALAXYFRAME_SPIRV_SEED=FILE names it as well.
# With GALAXYFRAME_NO_BUILD=1 (or the old GALAXYQUEST_NO_BUILD=1) it packages
# the build in build-linux/ as it is.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE/.."
VERSION=${1:-1.0.0}
SEED=${2:-$GALAXYFRAME_SPIRV_SEED}
[ -z "$SEED" ] || [ -f "$SEED" ] || { echo "no such shader cache seed: $SEED" >&2; exit 1; }

[ -n "$GALAXYFRAME_NO_BUILD$GALAXYQUEST_NO_BUILD" ] || ./build_linux.sh   # GALAXYFRAME_NO_BUILD=1: package the existing build-linux/
mkdir -p steam-frame/release
STAGE=$(mktemp -d)
cp build-linux/galaxyframe build-linux/petari_headless "$STAGE/"
strip --strip-unneeded -o "$STAGE/libgame.so" build-linux/libgame.so
FILES="galaxyframe petari_headless libgame.so galaxyframe.sh"
if [ -f build-linux/libgq_glsl.so ]; then
  strip --strip-unneeded -o "$STAGE/libgq_glsl.so" build-linux/libgq_glsl.so
  FILES="$FILES libgq_glsl.so"
else
  echo "note: build-linux/libgq_glsl.so is missing: the release has no Vulkan shader library (GLES only)" >&2
fi
cp steam-frame/galaxyframe.sh tools/vrtiming.py "$STAGE/"
FILES="$FILES vrtiming.py"
if [ -n "$SEED" ]; then
  cp "$SEED" "$STAGE/spirv.bin"
  FILES="$FILES spirv.bin"
fi
NAME=galaxyframe-$VERSION-linux-arm64.tar.gz
OUT=steam-frame/release/$NAME
tar -czf "$OUT" -C "$STAGE" $FILES
rm -rf "$STAGE"
(cd steam-frame/release && sha256sum "$NAME" | tee SHA256SUMS)
