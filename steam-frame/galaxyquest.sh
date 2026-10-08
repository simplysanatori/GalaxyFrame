#!/bin/bash
# Steam shortcut target for GalaxyQuest on the Steam Frame.  Game files,
# saves, settings and the game's own log (petari_log.txt) are in
# ~/.local/share/GalaxyQuest; this wrapper's own output goes to launch.log
# there.  An argument names the game files' folder (default: game/ in that
# directory, or the folder picked on the setup screen).
HERE=$(dirname "$(readlink -f "$0")")
HOME_DIR=${GALAXYQUEST_HOME:-$HOME/.local/share/GalaxyQuest}
mkdir -p "$HOME_DIR"
exec "$HERE/galaxyquest" "$@" > "$HOME_DIR/launch.log" 2>&1
