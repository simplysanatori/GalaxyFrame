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

# Test settings for one launch from the Steam library, which passes no
# environment of its own: VAR=value lines in session.env (e.g.
# DISABLE_VULKAN_FDM_INJECTION_LAYER=1), deleted by whoever wrote it.
if [ -f "$HOME_DIR/session.env" ]; then
  echo "session.env:"; cat "$HOME_DIR/session.env"
  set -a; . "$HOME_DIR/session.env"; set +a
fi

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

# Session logging, so a session can be read afterwards (tools/session-report.py):
#  - vrtiming.log: SteamVR's compositor timing, a line a second, for the whole
#    session (read-only; the last session's is kept as vrtiming.log.prev).
#    GQ_VRTIMING=0 turns it off.
#  - perfcriteria.txt: Steam's own performance log, copied next to the game's
#    log when the game exits, if Steam wrote one during the session.
# Neither can stop the game from starting: a missing python, missing OpenVR
# library or an unwritable folder only means no log.
VRT_PID=
touch "$HOME_DIR/.session-start" 2> /dev/null
if [ "${GQ_VRTIMING:-1}" != 0 ] && command -v python3 > /dev/null 2>&1; then
  for f in "$HERE/vrtiming.py" "$HERE/../src/tools/vrtiming.py" "$HERE/../tools/vrtiming.py"; do
    [ -f "$f" ] && { VRT=$f; break; }
  done
  if [ -n "$VRT" ] && [ -f /opt/steamvr/bin/linuxarm64/libopenvr_api.so ]; then
    [ -f "$HOME_DIR/vrtiming.log" ] && mv -f "$HOME_DIR/vrtiming.log" "$HOME_DIR/vrtiming.log.prev" 2> /dev/null
    env -u LD_LIBRARY_PATH python3 -I "$VRT" 86400 "$HOME_DIR/vrtiming.log" > /dev/null 2>&1 &
    VRT_PID=$!
  fi
fi

# Not exec: the wrapper stays to stop the logger and collect the log.  A signal
# for the wrapper (Steam stopping the app) goes on to the game.
"$HERE/galaxyquest" "$@" &
GAME_PID=$!
trap 'kill -TERM "$GAME_PID" 2> /dev/null' TERM INT HUP
rc=0
while kill -0 "$GAME_PID" 2> /dev/null; do  # a signal interrupts wait before the game is gone
  wait "$GAME_PID"
  rc=$?
done
trap - TERM INT HUP

[ -n "$VRT_PID" ] && kill "$VRT_PID" 2> /dev/null
PERF=${GQ_PERFCRITERIA:-$HOME/.local/share/Steam/logs/perfcriteria.txt}
if [ -f "$PERF" ] && [ "$PERF" -nt "$HOME_DIR/.session-start" ]; then
  [ -f "$HOME_DIR/perfcriteria.txt" ] && mv -f "$HOME_DIR/perfcriteria.txt" "$HOME_DIR/perfcriteria.txt.prev" 2> /dev/null
  cp -f "$PERF" "$HOME_DIR/perfcriteria.txt" 2> /dev/null
fi
exit $rc
