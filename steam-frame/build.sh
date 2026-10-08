#!/bin/bash
# Builds the Steam Frame (Linux arm64) target and packages it for the
# Releases page: steam-frame/release/GalaxyQuest-steam-frame-<version>.tar.gz
# with the launcher, the stripped game library and the launch wrapper.
#   steam-frame/build.sh [version]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE/.."
VERSION=${1:-$(git describe --tags --always 2> /dev/null || echo dev)}

./build_linux.sh
mkdir -p steam-frame/release
STAGE=$(mktemp -d)
cp build-linux/galaxyquest build-linux/petari_headless "$STAGE/"
strip --strip-unneeded -o "$STAGE/libgame.so" build-linux/libgame.so
cp steam-frame/galaxyquest.sh "$STAGE/"
OUT=steam-frame/release/GalaxyQuest-steam-frame-$VERSION.tar.gz
tar -czf "$OUT" -C "$STAGE" galaxyquest petari_headless libgame.so galaxyquest.sh
rm -rf "$STAGE"
sha256sum "$OUT"
