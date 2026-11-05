# GalaxyFrame user guide

The full detail for playing GalaxyFrame on the Steam Frame. The short version is in the [README](../README.md). Technical detail for developers is in [AGENTS.md](../AGENTS.md).

## What it does

- Plays Super Mario Galaxy as a diorama in front of you, or on a giant virtual screen.
- Renders both eyes on every refresh of the 120 Hz display, from your current head position, at 0.9 of SteamVR's recommended resolution.
- Uses the Steam Frame controllers through SteamVR's own controller profile, with rumble.
- Turns the diorama smoothly with the stick, and shoots Star Bits faster, with a slider for each.
- Spins when you flick a controller, like a Wii Remote.
- Keeps the game's own cutscenes in the diorama, following their camera, with a switch per kind.
- Darkens the edges of the view while it moves, to help with motion sickness.

GalaxyQuest is a Meta Quest app. GalaxyFrame is the same game built as a Linux Arm64 program with its own Vulkan renderer, running in the Frame's SteamVR. You do not need Lepton, an APK, adb or a PC.

You need your own Super Mario Galaxy disc. No game data is in this repository or its releases.

## Known issues

Version 1.1.0 is the current release. It has been played on one Steam Frame (SteamOS 0.4.5, SteamVR 2.18) with the US disc.

- Mario and the world move at the game's 60 frames a second. On the 120 Hz display an object your eye follows can look doubled along its movement. Head movement is not affected.
- A new area still hitches briefly while its textures upload. Compiling the shaders ahead of play (Install, step 5 in the README) removes the shader part of it.

## Install details

**Step 2, `install.sh`.** The installer copies the program to `~/Games/GalaxyFrame/app`. It adds **Super Mario Galaxy VR** to your Steam library as a non-Steam shortcut, or updates the entry if it is there. It sets SteamVR's per-app settings for that shortcut. The script talks to Steam through its local debugging port, which SteamOS has on by default.

**Step 3, `prepare-game.sh`.** This takes a few minutes and 3.3 GB. With `--no-movies` it takes 2.3 GB less, and the game skips the prologue and ending movies. The script downloads [nodtool](https://github.com/encounter/nod) once, checks its sha256, extracts the disc and converts it to `~/.local/share/GalaxyFrame/game`. The extracted copy is deleted afterwards.

**Step 5, `warm-shaders.sh`.** It boots the game into 20 galaxies one after another and fills the shader cache. Your saves are copied first and left alone. It runs headless; the game must not be running.

**Updating.** To update, download the new tarball and run `install.sh` again. Your game files, saves and settings stay where they are.

**Moving over from the old name.** If you ran this port under its old name GalaxyQuest, `install.sh` moves `~/.local/share/GalaxyQuest` to `~/.local/share/GalaxyFrame` and `~/Games/GalaxyQuest/app` to `~/Games/GalaxyFrame/app` once, removes the old `galaxyquest` program files and points the Steam entry at the new ones. It tells you what it moved.

The installer changes nothing outside the folders listed under [Files](#files), the Steam shortcut and SteamVR's settings for that shortcut.

### Install with Frametanium

If you use [Frametanium](https://github.com/Cogential/frametanium), add `simplysanatori/GalaxyFrame` there. It downloads the release, runs its `install.sh` and updates it when I publish a new version. Your game files still come from your own disc: clone this repository and run `steam-frame/prepare-game.sh` once (step 3 in the README).

### Install with FrameDrop

[FrameDrop](https://framedropvr.com) can install this too: [Install with FrameDrop](https://framedropvr.com/install?manifest=https%3A%2F%2Fgithub.com%2Fsimplysanatori%2FGalaxyFrame%2Freleases%2Flatest%2Fdownload%2Fgalaxyframe.framedrop.json). The release has a `galaxyframe-<version>-framedrop-linux-arm64.zip` and a `galaxyframe.framedrop.json` for it. FrameDrop unpacks the zip on the Frame and adds its own library entry, which runs the `run.sh` in the zip. The first time it runs, and again after an update, `run.sh` runs `install.sh` with the Steam shortcut step turned off, so you get FrameDrop's entry and no second one from the installer, and then starts the game through its launch wrapper. Your game files still come from your own disc: clone this repository and run `steam-frame/prepare-game.sh` once.

### Uninstall

`steam-frame/uninstall.sh` removes the Steam shortcut and the program folder. Your data folder (`~/.local/share/GalaxyFrame`: game files, saves, settings and logs) stays unless you add `--purge`. Quit the game first. If Steam is not running the shortcut stays; run it again with Steam up, or remove it in Steam. SteamVR's per-app settings for the game are left alone.

## Controls

The game sees a Wii Remote with a Nunchuk. The Frame controllers are mapped onto them like this.

| Steam Frame | Wii | In the game |
|---|---|---|
| Left stick | Nunchuk stick | Move |
| **A** | A | Jump, talk, confirm. Hold to float as Boo Mario. Hold to skip a cutscene or a dialogue |
| **B** or right trigger | B | Shoot Star Bits (3x as fast as on the Wii by default; *Star Bit speed* on the Controls tab, 1x to 5x), cancel, back out of menus |
| **X**, **Y** or right bumper | Shake the Remote | Spin |
| Flick the right controller | Shake the Remote | Spin (*Shake to spin* on the Controls tab) |
| Flick the left controller | Shake the Nunchuk | Spin (*Shake to spin* on the Controls tab) |
| Left trigger | Z | Crouch, ground pound, long jump and backflip (with A) |
| Left bumper | C | Put the game camera behind Mario |
| **Menu** or **View** | Plus, Minus | Pause menu, with the VR settings panel beside it |
| D-pad | D-pad | The Wii D-pad. In the diorama, left and right turn the view round Mario |
| Right stick left / right | D-pad left / right | In the diorama, turn the view round Mario, faster the further you push. On the giant screen, turn the game camera where the level allows it |
| Right controller aim | Pointer | Collect Star Bits, grab Pull Stars, point at menus and the settings panel |
| Right controller tilt | Remote tilt | Steer the Star Ball and the Ray |

With *Smooth turning* off, the stick turns the diorama 45 degrees at a time behind a short blink, as the D-pad does.

Shake to spin: flick a controller as you would a Wii Remote. *Shake to spin* on the Controls tab turns it off, and *Shake strength* sets how hard the flick must be. Shakes are ignored while the pause menu or the settings panel is open.

Rumble: when the game rumbles the Wii Remote, both controllers buzz. A light tick marks the pointer reaching a new target on the settings panel.

Skipping: hold A during a cutscene or a dialogue. A ring fills at the lower right of your view, and the scene is skipped when it is full.

Recentring: use SteamVR's own recentre. The game puts the screen or the diorama in front of you again.

More on the controls (tilt, pointing, the diorama's height and size, left-handed play, playing as Luigi, pausing) is in [CONTROLS.md](CONTROLS.md). That file comes from GalaxyQuest and was written for the Quest Touch Plus controllers, with notes for the Steam Frame.

## Settings

Press Menu or View in the game. The pause menu opens, with the VR settings panel to its right. Aim the right controller at it and press A. The panel has six tabs: Diorama, Screen, Cutscenes, Controls, Comfort and Display. The Cutscenes tab has a switch per kind of scene (Power Stars, bosses and enemies, the Comet Observatory, the storybook, Lumas and friends, Mario's changes, other scenes): on, the scene plays in the diorama following its own camera; off, on the flat screen. Lumas and friends is off by default. Launch Star flights, pipes and arrivals are shown on the flat screen. The Controls tab has *Shake to spin* (on by default) and *Shake strength* (20 % to 160 %, 100 % by default; lower needs a gentler flick). Each setting has a short description under it, and everything on the panel is also in `~/.local/share/GalaxyFrame/petari_vr.ini`, with a comment on each line.

Two settings matter most:

- **Resolution** (Display tab), 0.90 by default. 1.00 is the eye size SteamVR's own resolution setting gives the game (1728 pixels per eye at SteamVR's 100 % on the Frame); the panel shows the pixels. At 0.90 the game renders both eyes on every refresh with some headroom. If the GPU falls behind in a heavy scene, it renders new eyes every second refresh until the scene is lighter again, and the log says when.
- **Comfort vignette** (Comfort tab). It darkens the edges of the view whenever it moves: a clear circle around where you look, closing in with speed. The two sliders set how dark it gets at the slowest and at the fastest movement; the fast value is never below the slow one. 0 turns it off.

Every setting, tab by tab, and every line of `petari_vr.ini` is described in [CONTROLS.md](CONTROLS.md#vr-settings).

## Performance

Numbers from my Frame, from the game's own log and SteamVR's compositor counters, at 120 Hz. The eyes are rendered at resolution 1.0 (1728 by 1728 pixels each) unless stated.

| | GalaxyFrame |
|---|---|
| New eye images | 120 a second, one pair per refresh, from the head pose of that refresh |
| GPU for both eyes with the sharpening pass | 5.3 ms on average, 6.5 ms in the heaviest scenes, against 8.3 ms per refresh |
| GPU for both eyes at resolution 0.8 | about 4.5 ms |
| Game frames shown late | 0.4 % |
| Frames the compositor had to reuse | 0.6 % |
| Missed refreshes | about one every 2 to 3 seconds, mostly at area loads |
| The two eyes' poses | from one head locate per refresh; they agree to 0.1 mm |
| Shaders at launch with a warm cache | about 1000 Vulkan pipelines ready in 0.1 s |
| Slowest game frame at an area load | about 20 ms (it was 35 ms before the load work) |
| Memory | 310 to 430 MB resident |

Where the time went and what was done about it is in [AGENTS.md](../AGENTS.md#performance-internals).

## Files

| What | Where |
|---|---|
| Program | `~/Games/GalaxyFrame/app` |
| Game files, saves, settings, shader caches | `~/.local/share/GalaxyFrame` (`game/`, `nand/`, `petari_vr.ini`, `spirv.bin`, `pipelines.bin`) |
| Logs | `~/.local/share/GalaxyFrame/petari_log.txt` (the game), `launch.log` (the launch wrapper), `vrtiming.log` (SteamVR's compositor timing) |
| Downloaded tools | `~/.cache/galaxyframe` (nodtool, from `prepare-game.sh`) |
| Optional test settings | `~/.local/share/GalaxyFrame/petari_debug.env`, `NAME=value` lines read at start |

## Troubleshooting

- **`install.sh` says Steam's debugging port is not reachable.** Start Steam and run it again.
- **The headset shows a setup screen.** The game files are missing. Run `steam-frame/prepare-game.sh` with your disc image.
- **Motion is uneven, or the picture ghosts when you turn your head.** Check that SteamVR runs the game at 120 Hz with motion smoothing off. `launch.log` shows the refresh rate asked for at each start, and `petari_log.txt` shows the pacing every 5 seconds.
- **The game drops to new eyes every second refresh for long stretches.** Lower the resolution to 0.8 on the Display tab.
- **New areas hitch.** Run `steam-frame/warm-shaders.sh` once after your first save.
- **The Steam overlay does not appear in the game.** Steam's overlay does not inject into this program. The `LD_PRELOAD` warnings in `launch.log` come from that and are harmless.
- **No vibration.** Check that `haptics` is not 0 in `petari_vr.ini`, and look for `haptics: first pulse result` in `petari_log.txt`.

## Reporting a problem

Open an [issue](https://github.com/simplysanatori/GalaxyFrame/issues) with what you did, what you expected and what happened, your disc's region, and `petari_log.txt` and `launch.log` from `~/.local/share/GalaxyFrame`. If the problem was in the session before your last one, attach `petari_log.txt.prev` instead.

These logs hold file paths under your home folder, the Steam shortcut's app id and the SteamVR settings. They hold no account names, network addresses or headset serial numbers. SteamVR's own logs can contain device serial numbers, so check them before you attach them.
