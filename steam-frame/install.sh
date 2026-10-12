#!/bin/bash
# Installs GalaxyFrame on the Steam Frame: copies the launcher, the game
# library and the launch wrapper to ~/Games/GalaxyFrame/app and adds a
# "Super Mario Galaxy VR" entry to the Steam library, set to 120 Hz in
# SteamVR.  Steam must be running.
#   steam-frame/install.sh [galaxyframe-*-linux-arm64.tar.gz]
# Without an argument it installs the build output (build-linux/, from
# build_linux.sh) if there is one, else the newest tarball in
# steam-frame/release/.
# The game files are a separate step: steam-frame/prepare-game.sh.
#
# An install made under the old name (GalaxyQuest) is moved over once: the
# data folder ~/.local/share/GalaxyQuest becomes ~/.local/share/GalaxyFrame
# and the program folder ~/Games/GalaxyQuest/app becomes
# ~/Games/GalaxyFrame/app (each only if the new folder does not exist yet),
# the old galaxyquest and galaxyquest.sh are removed, and the Steam entry is
# pointed at the new wrapper.
#
# For a trial install (tests, packaging checks) these environment variables
# redirect it:
#   GALAXYFRAME_APP=DIR        where the program goes (default ~/Games/GalaxyFrame/app)
#   GALAXYFRAME_HOME=DIR       the game's data folder (default ~/.local/share/GalaxyFrame)
#   GALAXYFRAME_SKIP_STEAM=1   leave Steam's library and SteamVR's settings alone
#                              (Steam need not be running)
#   GALAXYFRAME_OLD_APP=DIR    where an old-name program folder is looked for
#   GALAXYFRAME_OLD_HOME=DIR   where an old-name data folder is looked for
# The old-name folders are moved only when the new one is not redirected
# (GALAXYFRAME_APP / GALAXYFRAME_HOME unset) or the matching GALAXYFRAME_OLD_*
# is given, so a trial install never touches a real one.  The old variable
# names GALAXYQUEST_APP, GALAXYQUEST_HOME and GALAXYQUEST_SKIP_STEAM still work.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DEST=${GALAXYFRAME_APP:-${GALAXYQUEST_APP:-$HOME/Games/GalaxyFrame/app}}
DATA=${GALAXYFRAME_HOME:-${GALAXYQUEST_HOME:-$HOME/.local/share/GalaxyFrame}}
SKIP_STEAM=${GALAXYFRAME_SKIP_STEAM:-$GALAXYQUEST_SKIP_STEAM}
OLD_APP=${GALAXYFRAME_OLD_APP:-}
OLD_DATA=${GALAXYFRAME_OLD_HOME:-}
[ -n "$OLD_APP" ] || [ -n "$GALAXYFRAME_APP$GALAXYQUEST_APP" ] || OLD_APP=$HOME/Games/GalaxyQuest/app
[ -n "$OLD_DATA" ] || [ -n "$GALAXYFRAME_HOME$GALAXYQUEST_HOME" ] || OLD_DATA=$HOME/.local/share/GalaxyQuest
NAME="Super Mario Galaxy VR"
VRCMD=/opt/steamvr/bin/linuxarm64/vrcmd

if [ -z "$SKIP_STEAM" ]; then
  curl -s --max-time 3 127.0.0.1:8080/json > /dev/null || {
    echo "Steam's debugging port is not reachable. Make sure Steam is running (on SteamOS it is on by default)." >&2
    exit 1
  }
fi

# Move an install made under the old name, once.
MIGRATED=
if [ -n "$OLD_APP" ] && [ -d "$OLD_APP" ] && pgrep -f "^$OLD_APP/galaxyquest" > /dev/null 2>&1; then
  echo "The game is running from the old GalaxyQuest folder. Quit it and run install.sh again." >&2
  exit 1
fi
if [ -n "$OLD_DATA" ] && [ -d "$OLD_DATA" ] && [ ! -e "$DATA" ]; then
  mkdir -p "$(dirname "$DATA")"
  mv "$OLD_DATA" "$DATA"
  echo "moved the data folder $OLD_DATA to $DATA"
  MIGRATED="$MIGRATED the data folder,"
fi
if [ -n "$OLD_APP" ] && [ -d "$OLD_APP" ] && [ ! -e "$DEST" ]; then
  mkdir -p "$(dirname "$DEST")"
  mv "$OLD_APP" "$DEST"
  echo "moved the program folder $OLD_APP to $DEST"
  MIGRATED="$MIGRATED the program folder,"
fi
for pair in "$OLD_DATA:$DATA" "$OLD_APP:$DEST"; do
  old=${pair%%:*}; new=${pair#*:}
  [ -n "$old" ] && [ -d "$old" ] && [ -e "$new" ] && [ "$old" != "$new" ] && echo "note: $old and $new both exist; $old was left where it is"
done

mkdir -p "$DEST"
TARBALL=$1
[ -n "$TARBALL" ] || [ -x "$HERE/../build-linux/galaxyframe" ] || TARBALL=$(ls "$HERE"/release/galaxyframe-*-linux-arm64.tar.gz 2> /dev/null | sort -V | tail -1)
if [ -n "$TARBALL" ] && [ -f "$TARBALL" ]; then
  echo "installing $TARBALL"
  tar -xzf "$TARBALL" -C "$DEST" galaxyframe libgame.so
  tar -xzf "$TARBALL" -C "$DEST" petari_headless 2> /dev/null || true
  # The Vulkan shader library; a release without it runs on GLES, and an old
  # copy must not stay beside a newer game library.
  rm -f "$DEST/libgq_glsl.so"
  tar -xzf "$TARBALL" -C "$DEST" libgq_glsl.so 2> /dev/null || echo "note: this release has no libgq_glsl.so (the game uses GLES)"
elif [ -x "$HERE/../build-linux/galaxyframe" ]; then
  echo "installing the build in build-linux/"
  cp "$HERE/../build-linux/galaxyframe" "$DEST/galaxyframe"
  cp "$HERE/../build-linux/petari_headless" "$DEST/petari_headless"
  # The game may be running: the library is replaced in one step.
  strip --strip-unneeded -o "$DEST/libgame.so.new" "$HERE/../build-linux/libgame.so"
  mv "$DEST/libgame.so.new" "$DEST/libgame.so"
  if [ -f "$HERE/../build-linux/libgq_glsl.so" ]; then
    strip --strip-unneeded -o "$DEST/libgq_glsl.so.new" "$HERE/../build-linux/libgq_glsl.so"
    mv "$DEST/libgq_glsl.so.new" "$DEST/libgq_glsl.so"
  else
    rm -f "$DEST/libgq_glsl.so"
  fi
else
  echo "usage: $0 galaxyframe-*-linux-arm64.tar.gz (from the Releases page), or run ./build_linux.sh first" >&2
  exit 1
fi
cp "$HERE/galaxyframe.sh" "$DEST/galaxyframe.sh"
# The compositor timing logger galaxyframe.sh runs beside the game.
if [ -f "$HERE/../tools/vrtiming.py" ]; then
  cp "$HERE/../tools/vrtiming.py" "$DEST/vrtiming.py"
elif [ -n "$TARBALL" ]; then
  tar -xzf "$TARBALL" -C "$DEST" vrtiming.py 2> /dev/null || true
fi
chmod +x "$DEST/galaxyframe" "$DEST/galaxyframe.sh"
# The old names, from an install made before the rename.
rm -f "$DEST/galaxyquest" "$DEST/galaxyquest.sh"

# A shader cache seed shipped in the tarball (spirv.bin) goes into the data
# folder, unless there is a spirv.bin there already.
if [ -n "$TARBALL" ] && [ -f "$TARBALL" ] && [ ! -e "$DATA/spirv.bin" ] && tar -tzf "$TARBALL" spirv.bin > /dev/null 2>&1; then
  mkdir -p "$DATA"
  tar -xzf "$TARBALL" -C "$DATA" spirv.bin && echo "placed the shader cache seed in $DATA/spirv.bin"
fi

APPID=none
if [ -z "$SKIP_STEAM" ]; then
  CEF="python3 $HERE/steam-cef.py"
  EXISTING=$($CEF "(() => { for (const a of (appStore.allApps || [])) { if (a.display_name === '$NAME' && a.app_type === 1073741824) return a.appid; } return 0; })()" 2> /dev/null || echo 0)
  if [ -n "$EXISTING" ] && [ "$EXISTING" != "0" ]; then
    APPID=$EXISTING
    echo "Steam already has a '$NAME' shortcut (appid $APPID), updating it"
    $CEF "(() => { SteamClient.Apps.SetShortcutExe($APPID, '\"$DEST/galaxyframe.sh\"'); SteamClient.Apps.SetShortcutStartDir($APPID, '\"$DEST\"'); return 1; })()" > /dev/null
  else
    APPID=$($CEF "(async () => { const id = await SteamClient.Apps.AddShortcut('$NAME', '$DEST/galaxyframe.sh', '$DEST', ''); SteamClient.Apps.SetShortcutName(id, '$NAME'); return id; })()")
  fi
  # 120 Hz: each 60 fps game frame is shown for exactly two refreshes.  No
  # throttling to every other refresh (SteamVR does that to an app that missed
  # frames, and the game's pacing then ran it at half speed) and no motion
  # smoothing (its synthesized frames ghost; the game paces its own 60 fps).
  if [ -x "$VRCMD" ]; then
    for kv in preferredRefreshRate=120 framesToThrottle=0 additionalFramesToPredict=0 motionSmoothingOverride=2; do
      "$VRCMD" --set-settings-int "steam.app.$APPID.${kv%=*}" "${kv#*=}" > /dev/null 2>&1 ||
        echo "warning: could not set SteamVR's ${kv%=*} for the game (is SteamVR running?); see docs/TECHNICAL_NOTES.md"
    done
  fi
fi

cat << MSG

Installed in $DEST.
$([ -n "$MIGRATED" ] && echo "An install made under the old name GalaxyQuest was moved over:${MIGRATED%,}.")
$([ -z "$SKIP_STEAM" ] && echo "Steam library entry \"$NAME\" (appid $APPID) runs $DEST/galaxyframe.sh.")
Game files: steam-frame/prepare-game.sh <your Super Mario Galaxy disc image>  (once)
Then launch it from the library with SteamVR running.
Optional, after your first save: steam-frame/warm-shaders.sh compiles the galaxies' shaders ahead (about 45 minutes, no headset needed).
Logs: $DATA/petari_log.txt (the game) and launch.log
MSG
