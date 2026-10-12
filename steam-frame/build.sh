#!/bin/bash
# Builds the Steam Frame (Linux arm64) target and packages it for the
# Releases page: steam-frame/release/GalaxyQuest-steam-frame-<version>.tar.gz
# with the launcher, the stripped game library, the shader library of the
# Vulkan backend (libgq_glsl.so, left out when glslang was not available to
# build it; the game then uses GLES) and the launch wrapper.
#   steam-frame/build.sh [version]
# With GALAXYQUEST_NO_BUILD=1 it packages the build in build-linux/ as it is.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE/.."
VERSION=${1:-$(git describe --tags --always 2> /dev/null || echo dev)}

[ -n "$GALAXYQUEST_NO_BUILD" ] || ./build_linux.sh   # GALAXYQUEST_NO_BUILD=1: package the existing build-linux/
mkdir -p steam-frame/release
STAGE=$(mktemp -d)
cp build-linux/galaxyquest build-linux/petari_headless "$STAGE/"
strip --strip-unneeded -o "$STAGE/libgame.so" build-linux/libgame.so
FILES="galaxyquest petari_headless libgame.so galaxyquest.sh"
if [ -f build-linux/libgq_glsl.so ]; then
  strip --strip-unneeded -o "$STAGE/libgq_glsl.so" build-linux/libgq_glsl.so
  FILES="$FILES libgq_glsl.so"
else
  echo "note: build-linux/libgq_glsl.so is missing: the release has no Vulkan shader library (GLES only)" >&2
fi
cp steam-frame/galaxyquest.sh "$STAGE/"
OUT=steam-frame/release/GalaxyQuest-steam-frame-$VERSION.tar.gz
tar -czf "$OUT" -C "$STAGE" $FILES
rm -rf "$STAGE"
sha256sum "$OUT"
