# GalaxyQuest for the Steam Frame

Super Mario Galaxy in VR, running natively on Valve's Steam Frame.

This is a fork of [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) by bigmak94, a VR port of Super Mario Galaxy built on the [Petari](https://github.com/SMGCommunity/Petari) decompilation. Upstream targets the Meta Quest. This fork builds the same game as a Linux arm64 program that runs on the Frame's own SteamVR. You do not need Lepton, an APK, adb or a PC.

You need your own Super Mario Galaxy disc. No game data is in this repository or its releases.

## What this fork adds

- A Linux arm64 build of the game for SteamVR on the Frame (launcher, EGL and OpenXR session, SDL2 sound, plain folders for storage).
- A control layout for the Steam Frame controllers, bound through SteamVR's Frame controller profile.
- Frame pacing reworked for SteamVR. Both eyes are rendered from one head pose and handed over once the GPU has finished them.
- SteamVR per-app settings for the game (120 Hz, no throttling, no motion smoothing), set at install and again at every launch.
- A steady default resolution of 0.8 of SteamVR's recommended size.
- Scripts to install the game, add it to Steam, convert your disc and compile shaders ahead of play.

The game itself, the renderer, the diorama, the giant screen and the VR settings panel are upstream's code. [STEAM_FRAME.md](STEAM_FRAME.md) lists every change and the reasons for it.

## Status

Played on a Steam Frame in the headset, October 2026. The game runs at 60 frames per second on the 120 Hz display with both eyes in step. It has been played through the Comet Observatory and into levels in the diorama.

Known issues:

- A hitch of about a second when a new area loads, while its textures upload and its shaders compile. `steam-frame/warm-shaders.sh` reduces it (see Install, step 5).
- SpaceWarp and passthrough do not work on the Frame. SteamVR does not offer them to this kind of app. The *Smooth motion* and *Passthrough* switches in the VR settings have no effect.
- Not yet checked: haptics, long sessions, every switch of the VR settings panel, discs other than the US one.

## Requirements

- A Steam Frame with SteamVR and about 4 GB free.
- Your Super Mario Galaxy disc as an ISO, RVZ or WBFS image. The US disc (RMGE01) is the one tested on the Frame. Upstream also tested the European disc (RMGP01).
- A terminal on the Frame: SSH, or Konsole from the desktop (SteamVR dashboard, Launch a program, Desktop).
- Steam running while you install.

## Install

1. Get the repository:

   ```sh
   git clone -b steam-frame https://github.com/simplysanatori/GalaxyQuest-steam-frame.git
   cd GalaxyQuest-steam-frame
   ```

2. Download the newest `GalaxyQuest-steam-frame-*.tar.gz` from the [Releases](https://github.com/simplysanatori/GalaxyQuest-steam-frame/releases) page and install it:

   ```sh
   steam-frame/install.sh ~/Downloads/GalaxyQuest-steam-frame-v0.1.8-frame.2.tar.gz
   ```

   This copies the program to `~/Games/GalaxyQuest/app`. It adds **Super Mario Galaxy VR** to your Steam library as a non-Steam shortcut, or updates the entry if it is already there. It also sets the SteamVR per-app settings for that shortcut. The script talks to Steam through its local debugging port (127.0.0.1:8080), which SteamOS has on by default.

3. Convert your disc, once. This takes a few minutes and 3.3 GB. With `--no-movies` it takes 2.3 GB less, and the prologue and ending movies play as black.

   ```sh
   steam-frame/prepare-game.sh /path/to/SuperMarioGalaxy.rvz
   ```

   The script downloads [nodtool](https://github.com/encounter/nod) once to `~/.cache/galaxyquest`, checks its sha256, extracts the disc and converts it to `~/.local/share/GalaxyQuest/game`. The extracted copy is deleted afterwards.

4. Put the headset on and launch **Super Mario Galaxy VR** from your Steam library.

5. Optional, after you have made a save: compile the galaxies' shaders ahead of play so new areas hitch less. It takes about 45 minutes. The headset is not needed and the game must not be running.

   ```sh
   steam-frame/warm-shaders.sh
   ```

   It boots the game headless into 20 galaxies, one after another, and adds their shaders to `~/.local/share/GalaxyQuest/shaders.bin`. Your saves are copied first and left as they are. To do other galaxies, name them: `steam-frame/warm-shaders.sh FlipPanelGalaxy`.

To update, download the new tarball and run `install.sh` again. Your game files, saves and settings stay where they are.

To build the program yourself, see [Building](STEAM_FRAME.md#building). It builds on the Frame in about 35 minutes.

## First run

The game starts on a giant virtual screen, as on a TV. To play in the diorama, open the VR settings panel (below) and turn off *Giant screen* on the *Screen* tab.

If the headset shows a setup screen asking for the game files, the game did not find them in `~/.local/share/GalaxyQuest/game`. Run `prepare-game.sh` (Install, step 3).

## Controls

The game sees a Wii Remote with a Nunchuk. The Frame controllers are mapped onto them as below. The mapping is in `updateInput()` and `suggestFrame()` in `platform/src/xr/xr_app.cpp`.

| Steam Frame | Wii | In the game |
|---|---|---|
| Left stick | Nunchuk stick | Move |
| **A** (right) | A | Jump, talk, confirm. Hold to float as Boo Mario. Hold to skip a cutscene or a dialogue |
| **B** (right) | B | Shoot star bits, cancel, back out of menus |
| Right trigger | B | Same as B |
| **X** or **Y** (right) | Shake the Remote | Spin |
| Right bumper | Shake the Remote | Spin |
| Flick the right controller | Shake the Remote | Spin |
| Flick the left controller | Shake the Nunchuk | Spin |
| Left trigger | Z | Crouch, ground pound, long jump and backflip (with A) |
| Left grip or left bumper | C | Put the game camera behind Mario |
| **Menu** (right) | Plus (+) | Pause menu, with the VR settings panel beside it |
| **View** (left) | Minus (-) | Pause menu, as Menu does |
| D-pad (left) | D-pad | The Wii D-pad. In the diorama, left and right turn it 45 degrees round Mario |
| Right stick left / right | D-pad left / right | In the diorama, turn it 45 degrees round Mario behind a short blink. On the giant screen, turn the game camera where the level allows it |
| Right stick up, or click the right stick | D-pad up | First-person look. Page up in lists |
| Right stick down | D-pad down | Page down in lists |
| Right controller aim | Pointer | Collect star bits, grab Pull Stars, point at menus and the VR settings panel |
| Right controller tilt | Remote tilt | Steer the Star Ball and the Ray |

The right grip and the left stick click are not used.

**Pausing.** One press of Menu or View opens the pause menu. Where the game does not allow pausing for a moment, the menu opens as soon as it can, up to 1.5 seconds after the press. Opening the SteamVR dashboard pauses the game. When you return during play, the pause menu is open.

**Skipping.** Hold A during a cutscene or a dialogue. A ring fills at the lower right of your view, and the cutscene is skipped when it is full. `skip_hold` in `petari_vr.ini` sets how long to hold (1 second by default).

**Tilt.** For the Star Ball, hold the right controller level, tip it down to roll forward and roll it to steer. For the Ray, point the right controller ahead and twist it to turn.

**Recentring.** Use SteamVR's recentre (hold the system button, or use the dashboard). The game puts the screen or the diorama in front of you again.

**The VR settings panel.** Press Menu or View. The pause menu opens, and the VR settings panel appears to the right of its buttons. Aim the right controller at it and press A or pull the right trigger. Its four tabs hold every setting of `petari_vr.ini`. [docs/CONTROLS.md](docs/CONTROLS.md) explains each setting, pointing, tilt and skipping in detail. That page was written for the Quest's Touch Plus controllers. Its button table does not apply to the Frame, and the rest does.

## Settings

Game files, saves, settings and logs are in `~/.local/share/GalaxyQuest`:

| Path | What it is |
|---|---|
| `game/` | The converted disc |
| `nand/` | Your saves |
| `petari_vr.ini` | The VR settings (also changed from the VR settings panel) |
| `shaders.bin` | The shader cache |
| `petari_log.txt` | The game's log for the current or last session |
| `petari_log.txt.prev` | The log of the session before |
| `launch.log` | Output of the launch wrapper and the game |
| `warm-shaders.log` | Output of `warm-shaders.sh` |
| `petari_debug.env` | Optional `NAME=value` lines read at start, for testing |

Settings that matter on the Frame:

- **SteamVR per-app settings.** `install.sh` sets these for the shortcut, and `galaxyquest.sh` sets them again at every launch: refresh rate 120 Hz, no throttling, no extra prediction frames, motion smoothing off. SteamVR has dropped them from its settings file before, which put the display at 72 Hz. The game's 60 fps divides 120 Hz evenly, so each game frame is shown for two refreshes. At 72 or 90 Hz motion is uneven. The game's own *Refresh rate* setting cannot change SteamVR's rate.
- **Resolution.** Held at 0.8 of SteamVR's recommended size (`min_resolution` and `resolution` in `petari_vr.ini`, or the *Picture* tab). At 1.0 in heavy areas, the game and SteamVR's compositor together filled the GPU, and the picture ghosted on head movement. You can raise it. Keep the two values equal, since each step of the dynamic resolution is a visible change in sharpness.
- **Lower latency.** `PETARI_RETRACE_STEP=2` in `petari_debug.env` moves the game's frame timing one refresh later, for about 8 ms less latency. The game then has 8 ms per frame, and a slow frame is shown twice, which happens as areas load.
- **Sound** goes to the default PipeWire output.

## How it works

Upstream's Android glue has a Linux counterpart. A launcher loads the game at the fixed address the decompiled code expects. The GL context is EGL on Mesa's surfaceless platform (zink on Turnip). The OpenXR session is created through SteamVR's `XR_MNDX_egl_enable`. Sound goes through SDL2.

The frame pacing was reworked for SteamVR. SteamVR reads an eye image as soon as the game releases it, and its compositor shares the GPU with the game. So both eyes are rendered from one head pose, kept until a fence shows the GPU has finished them, and handed over together. This adds one refresh of latency compared with upstream. [STEAM_FRAME.md](STEAM_FRAME.md) has the details, the approaches that were tried and dropped, and notes on the SteamVR runtime.

## Troubleshooting

- **`install.sh` says Steam's debugging port is not reachable.** Start Steam and run it again. The script needs Steam's local debugging port to add the shortcut.
- **`install.sh` warns it could not set a SteamVR setting.** SteamVR was probably not running. The launch wrapper sets these settings at every launch, so you can ignore it.
- **The headset shows a setup screen.** The game files are missing. Run `steam-frame/prepare-game.sh` with your disc image.
- **Motion is uneven, or the picture ghosts when you turn your head.** Check that SteamVR runs the game at 120 Hz with motion smoothing off. `petari_log.txt` logs the refresh rate at start and the pacing every 10 seconds. If you raised the resolution, set it back to 0.8.
- **New areas hitch for about a second.** This is known. Run `steam-frame/warm-shaders.sh` once after your first save.
- **`warm-shaders.sh` stops at once.** It needs the game files, a save, and the game closed. The message says which is missing.
- **The Steam overlay does not appear in the game.** Steam's overlay does not inject into this program. The `LD_PRELOAD` warnings in `launch.log` come from that and are harmless.
- **No controller models are shown.** Upstream draws none either. The laser shows while you point at the settings panel.

## Reporting problems

Open an issue on this repository with:

- what you did and what you saw,
- the release you installed and your disc's region,
- `~/.local/share/GalaxyQuest/petari_log.txt` and `launch.log`. If the problem was in the session before your last one, attach `petari_log.txt.prev` instead.

These logs hold file paths under your home folder, the Steam shortcut's app id and the SteamVR settings. They hold no account names, network addresses or headset serial numbers. SteamVR's own logs can contain device serial numbers, so check them before you attach them.

## Credits

- Super Mario Galaxy is Nintendo's game.
- The [Petari](https://github.com/SMGCommunity/Petari) team made the decompilation (CC0).
- [bigmak94](https://github.com/bigmak94/GalaxyQuest) made GalaxyQuest, the port to a modern GPU and to VR (Unlicense). Upstream's README is kept as [README.upstream.md](README.upstream.md).
- [encounter](https://github.com/encounter/nod) made nod, which extracts the disc.
- The Steam Frame port was built by simplysanatori.

The code, scripts and documentation of this fork were written with Anthropic's Claude (Claude Opus 5.5 and Claude Fable 5.1, through Claude Code), running on the Frame. simplysanatori chose what to attempt, supplied the disc, tested every build in the headset and reported the results. Upstream GalaxyQuest is also AI-written, as its README says. The upstream author and the Petari team have not reviewed this fork. Read the code before trusting it.

Super Mario Galaxy, Mario and their logos are trademarks of Nintendo. This is an unofficial fan project. It is not affiliated with or endorsed by Nintendo, Valve, the Petari team or the GalaxyQuest author.

## Licence

GalaxyQuest and this fork's changes are in the public domain under [The Unlicense](LICENSE). The decompilation in `decomp/` is the Petari team's work under CC0 1.0 (`decomp/LICENSE-Petari.txt`). Third-party parts keep their own licences: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). No Nintendo game data is included.
