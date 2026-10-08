#!/bin/bash
# Steam shortcut target for GalaxyQuest on the Steam Frame.  Game files,
# saves, settings and the game's own log (petari_log.txt) are in
# ~/.local/share/GalaxyQuest; this wrapper's own output goes to launch.log
# there.  An argument names the game files' folder (default: game/ in that
# directory, or the folder picked on the setup screen).
HERE=$(dirname "$(readlink -f "$0")")
HOME_DIR=${GALAXYQUEST_HOME:-$HOME/.local/share/GalaxyQuest}
mkdir -p "$HOME_DIR"
exec > "$HOME_DIR/launch.log" 2>&1

# SteamVR's per-app settings, every launch: SteamVR has dropped them from its
# settings file before (a dashboard change or a restart rewrites the file),
# and without them the display ran at 72 Hz with throttling and motion
# smoothing, which the game's pacing cannot work with.  SteamAppId is set by
# Steam when launched from the library.
VRCMD=/opt/steamvr/bin/linuxarm64/vrcmd
if [ -n "$SteamAppId" ] && [ -x "$VRCMD" ]; then
  for kv in preferredRefreshRate=120 framesToThrottle=0 additionalFramesToPredict=0 motionSmoothingOverride=2; do
    "$VRCMD" --set-settings-int "steam.app.$SteamAppId.${kv%=*}" "${kv#*=}" > /dev/null 2>&1 || echo "could not set SteamVR's ${kv%=*}"
  done
fi
exec "$HERE/galaxyquest" "$@"
