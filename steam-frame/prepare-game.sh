#!/bin/bash
# Converts your own Super Mario Galaxy disc into the files the game reads,
# on the Frame itself: ~/.local/share/GalaxyQuest/game (about 3.3 GB).
#   steam-frame/prepare-game.sh <disc image (.rvz/.iso/.wbfs) or extracted folder> [--no-movies]
# Disc images are extracted with nodtool (https://github.com/encounter/nod),
# downloaded once into ~/.cache/galaxyquest and checked against its sha256.
# The extraction (3.2 GB) is deleted afterwards.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$1
[ -n "$SRC" ] && [ -e "$SRC" ] || { echo "usage: $0 <disc image or extracted folder> [--no-movies]" >&2; exit 1; }
MOVIES=--with-movies
[ "$2" = "--no-movies" ] && MOVIES=
OUT=${GALAXYQUEST_HOME:-$HOME/.local/share/GalaxyQuest}/game
CACHE=$HOME/.cache/galaxyquest

NOD_VERSION=v2.0.0-alpha.12
NOD_SHA256=33e801c0e4aee7e714ad84e89521f50e2f79005571b9d4779b695e44ea21ff99
NODTOOL=$CACHE/nodtool-$NOD_VERSION

extracted=
cleanup() { [ -n "$extracted" ] && rm -rf "$extracted"; }
trap cleanup EXIT

if [ -d "$SRC" ]; then
  DATA=$SRC
else
  mkdir -p "$CACHE"
  if [ ! -x "$NODTOOL" ]; then
    echo "fetching nodtool $NOD_VERSION"
    curl -fsSL -o "$NODTOOL.tmp" "https://github.com/encounter/nod/releases/download/$NOD_VERSION/nodtool-linux-aarch64"
    echo "$NOD_SHA256  $NODTOOL.tmp" | sha256sum -c - > /dev/null
    chmod +x "$NODTOOL.tmp" && mv "$NODTOOL.tmp" "$NODTOOL"
  fi
  "$NODTOOL" info "$SRC" | grep -E '^(Title|Game ID):' | head -2
  extracted=$CACHE/extracted.$$
  echo "extracting the disc to $extracted"
  "$NODTOOL" extract -q "$SRC" "$extracted"
  DATA=$extracted
fi

echo "converting to $OUT"
mkdir -p "$(dirname "$OUT")"
python3 "$HERE/../tools/cook/cook.py" "$DATA" "$OUT" $MOVIES
echo "done: the game files are in $OUT"
