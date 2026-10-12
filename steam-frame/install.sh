#!/bin/bash
# Installs GalaxyQuest on the Steam Frame: copies the launcher, the game
# library and the launch wrapper to ~/Games/GalaxyQuest/app and adds a
# "Super Mario Galaxy VR" entry to the Steam library, set to 120 Hz in
# SteamVR.  Steam must be running.
#   steam-frame/install.sh [GalaxyQuest-steam-frame-*.tar.gz]
# Without an argument it installs the build output (build-linux/, from
# build_linux.sh) if there is one, else the newest tarball in
# steam-frame/release/.
# The game files are a separate step: steam-frame/prepare-game.sh.
# For a trial install (tests, packaging checks) these environment variables
# redirect it:
#   GALAXYQUEST_APP=DIR        where the program goes (default ~/Games/GalaxyQuest/app)
#   GALAXYQUEST_HOME=DIR       the game's data folder, only named in the closing message
#   GALAXYQUEST_SKIP_STEAM=1   leave Steam's library and SteamVR's settings alone
#                              (Steam need not be running)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DEST=${GALAXYQUEST_APP:-$HOME/Games/GalaxyQuest/app}
DATA=${GALAXYQUEST_HOME:-$HOME/.local/share/GalaxyQuest}
NAME="Super Mario Galaxy VR"
VRCMD=/opt/steamvr/bin/linuxarm64/vrcmd

if [ -z "$GALAXYQUEST_SKIP_STEAM" ]; then
  curl -s --max-time 3 127.0.0.1:8080/json > /dev/null || {
    echo "Steam's debugging port is not reachable. Make sure Steam is running (on SteamOS it is on by default)." >&2
    exit 1
  }
fi

mkdir -p "$DEST"
TARBALL=$1
[ -n "$TARBALL" ] || [ -x "$HERE/../build-linux/galaxyquest" ] || TARBALL=$(ls "$HERE"/release/GalaxyQuest-steam-frame*.tar.gz 2> /dev/null | sort -V | tail -1)
if [ -n "$TARBALL" ] && [ -f "$TARBALL" ]; then
  echo "installing $TARBALL"
  tar -xzf "$TARBALL" -C "$DEST" galaxyquest libgame.so
  tar -xzf "$TARBALL" -C "$DEST" petari_headless 2> /dev/null || true  # releases before frame.2 had none
  # The Vulkan shader library; a release without it runs on GLES, and an old
  # copy must not stay beside a newer game library.
  rm -f "$DEST/libgq_glsl.so"
  tar -xzf "$TARBALL" -C "$DEST" libgq_glsl.so 2> /dev/null || echo "note: this release has no libgq_glsl.so (the game uses GLES)"
elif [ -x "$HERE/../build-linux/galaxyquest" ]; then
  echo "installing the build in build-linux/"
  cp "$HERE/../build-linux/galaxyquest" "$DEST/galaxyquest"
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
  echo "usage: $0 GalaxyQuest-steam-frame-*.tar.gz (from the Releases page), or run ./build_linux.sh first" >&2
  exit 1
fi
cp "$HERE/galaxyquest.sh" "$DEST/galaxyquest.sh"
chmod +x "$DEST/galaxyquest" "$DEST/galaxyquest.sh"

APPID=none
if [ -z "$GALAXYQUEST_SKIP_STEAM" ]; then
  CEF="python3 $HERE/steam-cef.py"
  EXISTING=$($CEF "(() => { for (const a of (appStore.allApps || [])) { if (a.display_name === '$NAME' && a.app_type === 1073741824) return a.appid; } return 0; })()" 2> /dev/null || echo 0)
  if [ -n "$EXISTING" ] && [ "$EXISTING" != "0" ]; then
    APPID=$EXISTING
    echo "Steam already has a '$NAME' shortcut (appid $APPID), updating it"
    $CEF "(() => { SteamClient.Apps.SetShortcutExe($APPID, '\"$DEST/galaxyquest.sh\"'); SteamClient.Apps.SetShortcutStartDir($APPID, '\"$DEST\"'); return 1; })()" > /dev/null
  else
    APPID=$($CEF "(async () => { const id = await SteamClient.Apps.AddShortcut('$NAME', '$DEST/galaxyquest.sh', '$DEST', ''); SteamClient.Apps.SetShortcutName(id, '$NAME'); return id; })()")
  fi
  # 120 Hz: each 60 fps game frame is shown for exactly two refreshes.  No
  # throttling to every other refresh (SteamVR does that to an app that missed
  # frames, and the game's pacing then ran it at half speed) and no motion
  # smoothing (its synthesized frames ghost; the game paces its own 60 fps).
  if [ -x "$VRCMD" ]; then
    for kv in preferredRefreshRate=120 framesToThrottle=0 additionalFramesToPredict=0 motionSmoothingOverride=2; do
      "$VRCMD" --set-settings-int "steam.app.$APPID.${kv%=*}" "${kv#*=}" > /dev/null 2>&1 ||
        echo "warning: could not set SteamVR's ${kv%=*} for the game (is SteamVR running?); see STEAM_FRAME.md"
    done
  fi
fi

cat << MSG

Installed in $DEST.
$([ -z "$GALAXYQUEST_SKIP_STEAM" ] && echo "Steam library entry \"$NAME\" (appid $APPID) runs $DEST/galaxyquest.sh.")
Game files: steam-frame/prepare-game.sh <your Super Mario Galaxy disc image>  (once)
Then launch it from the library with SteamVR running.
Optional, after your first save: steam-frame/warm-shaders.sh compiles the galaxies' shaders ahead (about 45 minutes, no headset needed).
Logs: $DATA/petari_log.txt (the game) and launch.log
MSG
