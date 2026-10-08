# GalaxyQuest for the Steam Frame

**Super Mario Galaxy in VR, running natively on Valve's Steam Frame.**

This is a fork of [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest), a native VR port of Super Mario Galaxy built on the [Petari](https://github.com/SMGCommunity/Petari) decompilation. Upstream targets the Meta Quest. This fork builds the same game as a Linux arm64 program for the Steam Frame, running on the headset's own SteamVR: no Android layer (Lepton), no APK, no adb, no PC. You get the upstream port as it is, the diorama and the giant screen, its VR settings panel, and a control layout made for the Frame's controllers.

You need your own Super Mario Galaxy disc. Nothing of the game is in this repository or its releases.

> **AI disclosure.** This port, its scripts and its documentation were written by an AI: Anthropic's Claude (Claude Opus 5.5 and Claude Fable 5.1, through Claude Code), running on the Steam Frame itself. The human owner of this repository decided what to attempt, supplied the disc, played every build in the headset and reported what they saw; the AI read the logs and changed the code. Upstream GalaxyQuest is itself an AI-written project, as its own README says. Nothing here has been reviewed by the upstream author or the Petari team. It works on one Steam Frame as of October 2026; read the code before trusting it, and expect rough edges. [STEAM_FRAME.md](STEAM_FRAME.md) has the full account of what was done and what was tried and rejected.

## Status

Played on a Steam Frame, in the headset: the game runs at its 60 frames per second on the 120 Hz display, both eyes complete and in step, through the Comet Observatory and into levels in the diorama. It took several rounds to get there (the first builds jittered, ghosted, or stuttered in one eye; the causes and fixes are in [STEAM_FRAME.md](STEAM_FRAME.md)).

Known:

- A hitch of about a second when a new area loads, while its textures upload and its shaders compile. The shader cache fills as you play, and `steam-frame/warm-shaders.sh` fills it ahead of time (below).
- Not available on the Frame: SpaceWarp (Meta's frame extrapolation; not needed at 120 Hz) and passthrough. The *Smooth motion* and *Passthrough* switches in the VR settings do nothing.
- Not yet checked: haptics, long sessions, every switch of the VR settings panel.

## What you need

- A **Steam Frame** with SteamVR, about 4 GB free.
- Your **Super Mario Galaxy disc** as an ISO, RVZ or WBFS image. The US disc (RMGE01) is the one tested here; upstream also tested the European one (RMGP01).
- A way to type a few commands on the Frame: SSH, or Konsole from the desktop (SteamVR dashboard → Launch a program → Desktop). Steam must be running.

## Install

1. Get the repository:

   ```sh
   git clone -b steam-frame https://github.com/simplysanatori/GalaxyQuest-steam-frame.git
   cd GalaxyQuest-steam-frame
   ```

2. Download `GalaxyQuest-steam-frame-*.tar.gz` from the [Releases](https://github.com/simplysanatori/GalaxyQuest-steam-frame/releases) page and install it:

   ```sh
   steam-frame/install.sh ~/Downloads/GalaxyQuest-steam-frame-v0.1.8-frame.2.tar.gz
   ```

   This puts the program in `~/Games/GalaxyQuest/app`, adds **Super Mario Galaxy VR** to your Steam library and sets SteamVR up for it (120 Hz, no throttling, no motion smoothing). Library artwork appears after Steam's next restart.

3. Convert your disc, once (a few minutes, 3.3 GB; `--no-movies` saves 2.3 GB and plays the prologue and ending movies as black):

   ```sh
   steam-frame/prepare-game.sh ~/Documents/Roms/wii/SuperMarioGalaxy.rvz
   ```

4. Put the headset on and launch **Super Mario Galaxy VR** from your Steam library. It starts on a giant virtual screen; the VR settings panel (below) switches it to the diorama.

5. Optional, after you have made a save: compile the galaxies' shaders ahead so new areas do not hitch (about 45 minutes, no headset needed, the game not running):

   ```sh
   steam-frame/warm-shaders.sh
   ```

Game files, saves, settings and the log live in `~/.local/share/GalaxyQuest`. To build the program yourself instead of downloading it, see [Building](STEAM_FRAME.md#building) (it builds on the Frame in about 35 minutes).

## Controls

The Steam Frame's controllers are bound directly (through SteamVR's Frame controller profile), not translated from Quest controls.

| Steam Frame | Wii | In the game |
|---|---|---|
| Left stick | Nunchuk stick | Move |
| **A** | A | Jump, talk, confirm. Hold to float as Boo Mario, or to skip a cutscene or a dialogue |
| **B** | B | Shoot star bits, cancel, back out of menus |
| Right trigger | B | The same as B, if you prefer the trigger for shooting |
| **X** or **Y**, right bumper, or a flick of either controller | Shake | Spin |
| Left trigger | Z | Crouch, ground pound, long jump, backflip |
| Left grip or left bumper | C | Put the game camera behind Mario |
| **Menu** (right) or **View** (left) | + / − | Pause menu, with the VR settings panel beside it |
| D-pad (left controller) | D-pad | The Wii D-pad; left and right turn the diorama in steps |
| Right stick left / right | D-pad left / right | Turn the diorama round Mario (or the game camera on the giant screen) |
| Right stick up | D-pad up | First-person look |
| Right controller aim | Pointer | Collect star bits, grab Pull Stars, point at menus |

**The VR settings panel.** Press Menu (or View). The pause menu opens on the panel in front of you, and GalaxyQuest's VR settings panel sits to the right of its buttons. Aim the right controller at it and press A or pull the trigger. Its four tabs hold every setting: *Giant screen* (the switch between the TV-style screen and the diorama) is on the *Screen* tab. [docs/CONTROLS.md](docs/CONTROLS.md) explains every setting.

**Recentring.** Use SteamVR's own recentre (hold the system button, or the dashboard); the game follows it.

## Settings worth knowing

- **Resolution** is held at 0.8 of what SteamVR recommends (`min_resolution` and `resolution` in `~/.local/share/GalaxyQuest/petari_vr.ini`, or the *Picture* tab). The GPU cost hardly changes with the size, but at full size in heavy areas the game and SteamVR's compositor together filled the GPU and the picture ghosted on head movement. Raise it if you like; keep the two values equal.
- **120 Hz** is set per app by the installer and re-applied at every launch; the game cannot be paced on 72 or 90 Hz (its 60 fps does not divide them).
- **Lower latency, more hitches:** `PETARI_RETRACE_STEP=2` in `~/.local/share/GalaxyQuest/petari_debug.env` moves the game's frame timing a refresh later (8 ms less latency); the game then has 8 ms for a frame, and its occasional slow frames are shown twice.

## How it works, in short

The Android glue of upstream's platform layer has a Linux counterpart: a launcher that loads the game at the fixed address the decompiled code assumes, EGL through Mesa's surfaceless platform (zink on Turnip), an OpenXR session through SteamVR's `XR_MNDX_egl_enable`, SDL2 for sound, plain folders for storage. Everything else (the game, the renderer, the VR presentation and panels) is upstream's code. The frame pacing was reworked for SteamVR, which reads an eye image the instant the game releases it and shares the GPU with the game: both eyes are rendered from one head pose, kept until the GPU has finished them, and handed over together. Details, the dead ends, and the runtime facts are in [STEAM_FRAME.md](STEAM_FRAME.md).

## Credit

The game is Nintendo's. The source is the **Petari** team's decompilation (CC0); the port to a modern GPU and to VR is **GalaxyQuest by bigmak94** (Unlicense), whose README is kept here as [README.upstream.md](README.upstream.md). Disc extraction uses [nod](https://github.com/encounter/nod) by encounter. This fork adds the Linux target, the Steam Frame controls and the SteamVR pacing. Super Mario Galaxy, Mario and their logos are trademarks of Nintendo; this is an unofficial fan project, not affiliated with or endorsed by Nintendo, Valve, the Petari team or the GalaxyQuest author.

## License

GalaxyQuest and this fork's changes are in the public domain under [The Unlicense](LICENSE). The decompilation in `decomp/` is the Petari team's work under CC0 1.0 (`decomp/LICENSE-Petari.txt`). Third-party parts keep their own licenses: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). No Nintendo game data is included.
