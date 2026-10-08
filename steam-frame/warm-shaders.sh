#!/bin/bash
# Fills the shader cache ahead of play: boots the game headless (no headset)
# into one galaxy after another, with the VR presentation simulated, so the
# shaders each galaxy's materials need are compiled now and saved to
# ~/.local/share/GalaxyQuest/shaders.bin, not during play (a new area's
# shaders cost about a second of dropped frames the first time).
#   steam-frame/warm-shaders.sh [Galaxy[:scenario] ...]
# Without arguments, upstream's tour list (20 galaxies, about 130 s each).
# Needs the converted game files and a save file (the title and file select
# are played through by a scripted controller); the saves are copied first
# and left as they are.  Not while the game is running.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
HOME_DIR=${GALAXYQUEST_HOME:-$HOME/.local/share/GalaxyQuest}
GAME=${GALAXYQUEST_GAME:-$HOME_DIR/game}
APP=$HOME/Games/GalaxyQuest/app
if [ -x "$APP/petari_headless" ]; then
  RUNNER=$APP/petari_headless
elif [ -x "$HERE/../build-linux/petari_headless" ]; then
  RUNNER=$HERE/../build-linux/petari_headless
else
  echo "petari_headless not found (install a release that has it, or run ./build_linux.sh)" >&2
  exit 1
fi
[ -d "$GAME/files" ] || { echo "no game files at $GAME (steam-frame/prepare-game.sh first)" >&2; exit 1; }
[ -d "$HOME_DIR/nand/title" ] || { echo "no save files at $HOME_DIR/nand: start the game once and make a save" >&2; exit 1; }
pgrep -x galaxyquest > /dev/null && { echo "the game is running; quit it first" >&2; exit 1; }

GALAXIES=("$@")
if [ ${#GALAXIES[@]} -eq 0 ]; then
  GALAXIES=(AstroGalaxy HeavensDoorGalaxy EggStarGalaxy HoneyBeeKingdomGalaxy CosmosGardenGalaxy BattleShipGalaxy
            TriLegLv1Galaxy HeavenlyBeachGalaxy PhantomGalaxy IceVolcanoGalaxy SandClockGalaxy ReverseKingdomGalaxy
            OceanRingGalaxy FactoryGalaxy HellProminenceGalaxy KoopaBattleVs1Galaxy DarkRoomGalaxy OceanFloaterLandGalaxy
            SurfingLv1Galaxy CannonFleetGalaxy)
fi

# The scripted controller, as in tools/galaxy_tour.sh: through the title
# screen and the file select (save slot 1), then A now and then for the
# dialogues and cutscenes on the way into the galaxy.
PRESSES=$(for t in $(seq 44 3 77); do printf ",%s-%s.2:A" $t $t; done)
LATE=$(for t in $(seq 95 4 125); do printf ",%s-%s.2:A" $t $t; done)
MENU="28-28.4:A|B,30-36:PX=-0.25|PY=0.27,33-33.2:A,36-42:PX=0.52|PY=0.78,38-38.2:A"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/galaxyquest-warm.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp -r "$HOME_DIR/nand" "$WORK/nand"
mkdir -p "$WORK/shots"
LOG=$HOME_DIR/warm-shaders.log
: > "$LOG"

count() { grep -ao '[0-9]* programs loaded' "$1" | tail -1 | cut -d' ' -f1; }
echo "warming the shader cache: ${#GALAXIES[@]} galaxies, about $(( ${#GALAXIES[@]} * 140 / 60 )) minutes; log in $LOG"
i=0
for g in "${GALAXIES[@]}"; do
  i=$((i + 1))
  printf '%2d/%d %-28s ' "$i" "${#GALAXIES[@]}" "$g"
  EGL_PLATFORM=surfaceless SDL_AUDIODRIVER=dummy \
    PETARI_SHADER_CACHE="$HOME_DIR/shaders.bin" PETARI_VRINI="$HOME_DIR/petari_vr.ini" \
    PETARI_STAGE="$g" PETARI_INPUT="$MENU$PRESSES$LATE" PETARI_SHOT_MS=15000 PETARI_XRSIM=1 PETARI_XRSIM_STEPS=20 \
    timeout 170 "$RUNNER" "$GAME" "$WORK/nand" 130 > "$WORK/run.log" 2>&1 || true
  cat "$WORK/run.log" >> "$LOG"
  before=$(count "$WORK/run.log")
  draws=$(grep -a 'headless: frame' "$WORK/run.log" | tail -1 | sed -E 's/.*\(([0-9]+) draws.*/\1/')
  if grep -aq 'exiting' "$WORK/run.log"; then
    echo "ok (${draws:-?} draws in the last frame; cache had ${before:-?} programs at start)"
  else
    echo "did not finish (see $LOG)"
  fi
  rm -f "$WORK"/shots/*
done
echo "done: $(grep -ao '[0-9]* programs loaded' "$LOG" | tail -1 | cut -d' ' -f1) programs were in the cache at the last start; new ones were added as compiled"
