#!/bin/bash
# Builds the mock OpenXR runtime: tools/xrmock/build.sh [out dir]
# Writes libxrmock.so and runtime.json (for XR_RUNTIME_JSON) into the out dir
# (default ~/.cache/gq-xrmock).  Standalone: clang and the OpenXR headers of
# third_party/openxr (tools/fetch_openxr.sh), nothing of the game's build.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=${1:-$HOME/.cache/gq-xrmock}
INC=${OPENXR_INCLUDE:-$REPO/third_party/openxr/prefab/modules/headers/include}
[ -f "$INC/openxr/openxr.h" ] || INC=$(cd "$REPO/../../src/third_party/openxr/prefab/modules/headers/include" 2>/dev/null && pwd) || { echo "no OpenXR headers: tools/fetch_openxr.sh" >&2; exit 1; }
mkdir -p "$OUT"
clang -O1 -g -Wall -Wextra -shared -fPIC -o "$OUT/libxrmock.so" "$HERE/xrmock.c" "$HERE/xrmock_gles.c" "$HERE/xrmock_vk.c" -I "$INC" -I "$HERE" -lGLESv2 -lEGL -lm
sed "s#@LIB@#$OUT/libxrmock.so#" "$HERE/runtime.json.in" > "$OUT/runtime.json"
echo "$OUT/runtime.json"
