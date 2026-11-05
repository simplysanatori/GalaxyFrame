#!/bin/bash
# Removes GalaxyFrame's Steam shortcut and program folder. The data folder (your game files, saves,
# settings and logs in ~/.local/share/GalaxyFrame) stays unless --purge.
#   steam-frame/uninstall.sh [--purge]
# Environment: GALAXYFRAME_APP, GALAXYFRAME_HOME and GALAXYFRAME_SKIP_STEAM as in install.sh.
# If Steam is not running the shortcut stays; run this again with Steam up, or remove it in Steam.
# SteamVR's per-app settings for the game are left alone.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DEST=${GALAXYFRAME_APP:-$HOME/Games/GalaxyFrame/app}
DATA=${GALAXYFRAME_HOME:-$HOME/.local/share/GalaxyFrame}
NAME="Super Mario Galaxy VR"
PURGE=0
case "$1" in --purge) PURGE=1 ;; "") ;; *) echo "usage: $0 [--purge]" >&2; exit 2 ;; esac
if pgrep -f "^$DEST/galaxyframe" > /dev/null 2>&1; then echo "the game is running; quit it first" >&2; exit 1; fi

if [ -n "$GALAXYFRAME_SKIP_STEAM" ]; then
  echo "GALAXYFRAME_SKIP_STEAM set: Steam left alone"
elif curl -s --max-time 3 127.0.0.1:8080/json > /dev/null; then
  CEF="python3 $HERE/steam-cef.py"
  ID=$($CEF "(() => { for (const a of (appStore.allApps || [])) { if (a.display_name === '$NAME' && a.app_type === 1073741824) return a.appid; } return 0; })()" 2> /dev/null || echo 0)
  if [ -n "$ID" ] && [ "$ID" != 0 ]; then
    $CEF "(() => { SteamClient.Apps.RemoveShortcut($ID); return 1; })()" > /dev/null
    echo "removed the '$NAME' shortcut (appid $ID)"
  else
    echo "Steam has no '$NAME' shortcut"
  fi
else
  echo "Steam is not reachable, so the '$NAME' shortcut stays; remove it in Steam, or run this again with Steam up."
fi

if [ -d "$DEST" ]; then rm -rf "$DEST"; echo "removed $DEST"; fi
if [ "$PURGE" = 1 ]; then
  echo "--purge: removing $DATA (game files, saves, settings, logs)"
  rm -rf "$DATA"
else
  echo "kept your data in $DATA; --purge removes it"
fi
