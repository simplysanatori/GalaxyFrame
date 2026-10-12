# GalaxyFrame

Super Mario Galaxy in VR, running natively on the Valve Steam Frame.

GalaxyFrame is a port of [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) by bigmak94, the VR version of Super Mario Galaxy built on the [Petari](https://github.com/SMGCommunity/Petari) decompilation. GalaxyQuest is a Meta Quest app. GalaxyFrame is the same game built as a Linux Arm64 program with its own Vulkan renderer, running in the Frame's SteamVR. You do not need Lepton, an APK, adb or a PC.

You need your own Super Mario Galaxy disc. No game data is in this repository or its releases.

What it does:

- Plays Super Mario Galaxy as a diorama in front of you, or on a giant virtual screen.
- Renders both eyes on every refresh of the 120 Hz display, from your current head position, at SteamVR's full recommended resolution.
- Uses the Steam Frame controllers through SteamVR's own controller profile, with rumble.
- Turns the diorama smoothly with the stick, tilts it, and lets you set its height and size while you play.
- Keeps flights between planets and the game's cutscenes in the diorama, following the game's own camera.
- Has comfort settings for motion sickness: a vignette that closes in as the view moves, and a choice of how the world turns on small planets.

## What this adds to GalaxyQuest

GalaxyQuest draws with OpenGL ES and runs on the Quest's own runtime. On the Frame that would mean a translation layer and pacing that SteamVR does not like. GalaxyFrame changes the platform side and leaves the game itself as it is:

- A Linux Arm64 build: a launcher, an OpenXR session in SteamVR, SDL2 sound and plain folders for the game files, saves and settings.
- A Vulkan renderer beside the OpenGL ES one. It replays the game's recorded frames with Vulkan directly, with the shaders compiled to SPIR-V at run time and cached. OpenGL ES stays as the fallback.
- Frame pacing written for SteamVR. Each refresh locates the head once, renders both eyes from that pose and hands them over only after the GPU has finished them. If the GPU cannot keep up, it drops to new eyes every second refresh and returns on its own.
- A control layout for the Frame controllers, smooth turning and tilting, height and size on the left grip and D-pad, first person on the D-pad, a left-handed option and haptics on both controllers.
- The diorama for flights, arrivals and in-engine cutscenes, following their camera. The game's videos stay on the flat screen.
- A comfort vignette measured in viewing angle, with its strength set for slow and fast movement, and a Planet turns setting (smooth, in steps behind a blink, or off).
- Full-view fades: the death screen and the game's other fades darken the whole view, not just the screen.
- The laser, the pointer and the skip ring drawn over the flat screen, and the VR settings panel placed clear of the game's pause menu.
- The VR settings panel reorganised into six tabs, with the switches that do nothing on the Frame hidden.
- Area loads with less of a hitch: textures are decoded off the game thread, the frame memory is mapped ahead, and the game's threads run at a raised priority.
- SteamVR per-app settings for the game (120 Hz, no throttling, no motion smoothing), set at install and again at every launch.
- Scripts to install the game, add it to Steam, convert your disc and compile the shaders ahead of play.

## Status

Version 1.0.0 is the first release. It has been played on one Steam Frame (SteamOS 0.4.5, SteamVR 2.18) with the US disc.

Seen working in the headset: the diorama and the giant screen, 120 Hz with new eyes every refresh at resolution 1.0, the controls and rumble, the VR settings panel, flights and cutscenes in the diorama, the comfort vignette, the full-view fades, saving and loading. Tested only headless or by automated checks: the European disc, discs other than the US one, long sessions, the OpenGL ES fallback in the headset.

Known:

- Mario and the world move at the game's 60 frames a second. On the 120 Hz display an object your eye follows can look doubled along its movement. Head movement is not affected.
- Eye-tracked foveated rendering is built in but off. On this GPU it saves little and blurs much of the view.
- A new area still hitches briefly while its textures upload. Compiling the shaders ahead of play (Install, step 5) removes the shader part of it.

## Requirements

- A Steam Frame with SteamOS and SteamVR, and about 4 GB free.
- Your Super Mario Galaxy disc as an ISO, RVZ or WBFS image. The US disc (RMGE01) is the one tested.
- A terminal on the Frame: SSH, or Konsole from the desktop (SteamVR dashboard, Launch a program, Desktop).
- Steam running on the Frame while you install, so the installer can add the library entry.

## Install

1. Get the repository:

   ```sh
   git clone https://github.com/simplysanatori/GalaxyQuest-steam-frame.git galaxyframe
   cd galaxyframe
   ```

2. Download `galaxyframe-1.0.0-linux-arm64.tar.gz` and `SHA256SUMS` from the [Releases](https://github.com/simplysanatori/GalaxyQuest-steam-frame/releases) page. Check the download and install it:

   ```sh
   sha256sum -c --ignore-missing SHA256SUMS
   steam-frame/install.sh ~/Downloads/galaxyframe-1.0.0-linux-arm64.tar.gz
   ```

   The installer copies the program to `~/Games/GalaxyQuest/app`. It adds **Super Mario Galaxy VR** to your Steam library as a non-Steam shortcut, or updates the entry if it is there. It sets SteamVR's per-app settings for that shortcut. The script talks to Steam through its local debugging port, which SteamOS has on by default.

3. Convert your disc, once. This takes a few minutes and 3.3 GB. With `--no-movies` it takes 2.3 GB less, and the prologue and ending movies play as black.

   ```sh
   steam-frame/prepare-game.sh /path/to/SuperMarioGalaxy.rvz
   ```

   The script downloads [nodtool](https://github.com/encounter/nod) once, checks its sha256, extracts the disc and converts it to `~/.local/share/GalaxyQuest/game`. The extracted copy is deleted afterwards.

4. Put the headset on and launch **Super Mario Galaxy VR** from your Steam library.

5. Optional, after your first save: compile the galaxies' shaders ahead of play, so new areas hitch less. It takes about 45 minutes, headless; the game must not be running.

   ```sh
   steam-frame/warm-shaders.sh
   ```

   It boots the game into 20 galaxies one after another and fills the shader cache. Your saves are copied first and left alone.

To update, download the new tarball and run `install.sh` again. Your game files, saves and settings stay where they are.

The installer changes nothing outside the folders listed under [Files](#files), the Steam shortcut and SteamVR's settings for that shortcut.

## First run

The game starts on a giant virtual screen, as on a TV. To play in the diorama, press Menu, aim at the VR settings panel beside the pause menu, and turn off *Giant screen* on the *Screen* tab.

If the headset shows a setup screen asking for the game files, the game did not find them. Run `prepare-game.sh` (Install, step 3).

## Controls

The game sees a Wii Remote with a Nunchuk. The Frame controllers are mapped onto them like this.

| Steam Frame | Wii | In the game |
|---|---|---|
| Left stick | Nunchuk stick | Move |
| **A** | A | Jump, talk, confirm. Hold to float as Boo Mario. Hold to skip a cutscene or a dialogue |
| **B** or right trigger | B | Shoot Star Bits, cancel, back out of menus |
| **X**, **Y** or right bumper | Shake the Remote | Spin |
| Flick the right controller | Shake the Remote | Spin |
| Flick the left controller | Shake the Nunchuk | Spin |
| Left trigger | Z | Crouch, ground pound, long jump and backflip (with A) |
| Left bumper | C | Put the game camera behind Mario |
| Left grip | C | The same, except in the diorama, where it is the modifier below |
| **Menu** or **View** | Plus, Minus | Pause menu, with the VR settings panel beside it |
| D-pad | D-pad | The Wii D-pad. In the diorama, left and right turn the view round Mario |
| D-pad up | D-pad up | First-person view |
| Right stick left / right | D-pad left / right | In the diorama, turn the view round Mario, faster the further you push. On the giant screen, turn the game camera where the level allows it |
| Right stick up / down | | In the diorama, tilt the view: up sees Mario from higher. In lists, page up and down |
| Right stick click | | Level the tilt. With *Planet turns* on Steps or Off, also level the world |
| Left grip + D-pad up / down | | Raise or lower the diorama |
| Left grip + D-pad left / right | | Make the diorama smaller or bigger |
| Right controller aim | Pointer | Collect Star Bits, grab Pull Stars, point at menus and the settings panel |
| Right controller tilt | Remote tilt | Steer the Star Ball and the Ray |

With *Smooth turning* off, the stick turns the diorama 45 degrees at a time behind a short blink, as the D-pad does, and stick up is first person.

*Left-handed* (Controls tab) points with the left controller and swaps the hands' sticks, triggers, grips and bumpers. The buttons, Menu, View and the D-pad stay where they are, and height and size stay on the left grip and D-pad.

Rumble: when the game rumbles the Wii Remote, both controllers buzz. A light tick marks the pointer reaching a new target on the settings panel.

Skipping: hold A during a cutscene or a dialogue. A ring fills at the lower right of your view, and the scene is skipped when it is full.

Recentring: use SteamVR's own recentre. The game puts the screen or the diorama in front of you again.

## Settings

Press Menu or View in the game. The pause menu opens, with the VR settings panel to its right. Aim the right controller at it and press A. The panel has six tabs:

| Tab | Settings |
|---|---|
| Diorama | Distance, height, world size, follow smoothing, see-through scenery, turn with the camera |
| Screen | Giant screen, screen distance, stereoscopic 3D and its depth and resolution |
| Cutscenes | Diorama in flights, and a switch for each kind of cutscene: Power Stars, bosses and enemies, the Comet Observatory, the storybook, Lumas and friends, Mario's changes, other scenes. Each shows in the diorama or on the flat screen. Hold A to skip |
| Controls | Smooth turning, turn speed, tilt speed, Star Bit speed, invert camera, left-handed |
| Comfort | Planet turns (smooth, steps or off), turn smoothing, comfort vignette, vignette at slow and at fast movement |
| Display | Resolution, sharpening and its strength, language |

Everything on the panel is also in `~/.local/share/GalaxyQuest/petari_vr.ini`, with a comment on each line.

Two settings matter most:

- **Resolution.** 1.00 is the eye size SteamVR's own resolution setting gives the game (1728 pixels per eye at SteamVR's 100 % on the Frame); the panel shows the pixels. The game renders both eyes on every refresh at 1.00. If the GPU falls behind in a heavy scene, it renders new eyes every second refresh until the scene is lighter again, and the log says when. 0.80 holds every refresh with room to spare.
- **Planet turns.** On a small planet the world turns so that Mario's up stays up. *Smooth* turns it continuously, which some people find uncomfortable. *Steps* keeps the world still and re-levels it behind a short blink once it has drifted 35 degrees. *Off* never turns it on its own; the right stick click levels it.

The comfort vignette darkens the edges of the view whenever it moves by your input or on its own: turning, tilting, resizing, planet turns and flights. It is a clear circle around where you look; the two sliders set how dark and how tight it gets at the slowest and at the fastest movement. 0 turns it off.

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

Where the time went, and what was done about it:

- **The renderer.** The game records each frame as a list of GX commands, and the renderer replays it. Replaying with Vulkan instead of OpenGL ES over a translation layer made each eye about a third cheaper. On top of that, the per-pixel drawing state is sent once per run of draws instead of being read per pixel (26 % off each eye), draws whose output nothing reads are skipped, the game's channel-only copies are plain blits, the HUD is drawn once per game frame, and the GPU renders directly to memory instead of in tiles (another 7 %). Both eyes at 1.0 went from 8.2 ms to 6.1 ms in the headset simulation.
- **Pacing.** SteamVR shows an image the moment the game releases it, finished or not. So each refresh records both eyes into one command buffer, submits it, and the previous refresh's pair is released only once its GPU work is complete. Eight command buffers are kept in flight so the CPU never waits while recording. A refresh counts as late only if its pair really missed its display slot.
- **Area loads.** Textures are decoded on two helper threads instead of the game thread, the frame buffers are allocated and mapped ahead of use, and the game's threads get a raised priority through RealtimeKit. The slowest frame at a load dropped by 43 %.
- **Shaders.** The game writes its shaders as GLSL at run time. They are compiled to SPIR-V through glslang in a separate library and kept in a cache, and every pipeline in the cache is created in the background at launch. A warmed cache from the OpenGL ES build converts in about two seconds.

What did not help, so you know: SteamVR on the Frame accepts both frame synthesis extensions (`XR_EXT_frame_synthesis` and `XR_FB_space_warp`) but does not use them, which a standalone probe in `tools/xrprobe` showed. Eye-tracked foveation works but saves almost nothing, because this game's cost is in draws, not pixels, and it only takes effect in tiled rendering, which is slower here. Both are left out of the default.

## Files

| What | Where |
|---|---|
| Program | `~/Games/GalaxyQuest/app` |
| Game files, saves, settings, shader caches | `~/.local/share/GalaxyQuest` (`game/`, `nand/`, `petari_vr.ini`, `spirv.bin`, `pipelines.bin`) |
| Logs | `~/.local/share/GalaxyQuest/petari_log.txt` (the game), `launch.log` (the launch wrapper), `vrtiming.log` (SteamVR's compositor timing) |
| Optional test settings | `~/.local/share/GalaxyQuest/petari_debug.env`, `NAME=value` lines read at start |

## Troubleshooting

- **`install.sh` says Steam's debugging port is not reachable.** Start Steam and run it again.
- **The headset shows a setup screen.** The game files are missing. Run `steam-frame/prepare-game.sh` with your disc image.
- **Motion is uneven, or the picture ghosts when you turn your head.** Check that SteamVR runs the game at 120 Hz with motion smoothing off. `launch.log` shows the refresh rate asked for at each start, and `petari_log.txt` shows the pacing every 5 seconds.
- **The game drops to new eyes every second refresh for long stretches.** Lower the resolution to 0.8 on the Display tab.
- **New areas hitch.** Run `steam-frame/warm-shaders.sh` once after your first save.
- **The 3D looks wrong or the game will not start on Vulkan.** `renderer = gles` in `petari_vr.ini` runs the game on OpenGL ES. Please report it with the logs.
- **The Steam overlay does not appear in the game.** Steam's overlay does not inject into this program. The `LD_PRELOAD` warnings in `launch.log` come from that and are harmless.
- **No vibration.** Check that `haptics` is not 0 in `petari_vr.ini`, and look for `haptics: first pulse result` in `petari_log.txt`.

## Reporting a problem

Open an [issue](https://github.com/simplysanatori/GalaxyQuest-steam-frame/issues) with what you did, what you expected and what happened, your disc's region, and `petari_log.txt` and `launch.log` from `~/.local/share/GalaxyQuest`. If the problem was in the session before your last one, attach `petari_log.txt.prev` instead.

These logs hold file paths under your home folder, the Steam shortcut's app id and the SteamVR settings. They hold no account names, network addresses or headset serial numbers. SteamVR's own logs can contain device serial numbers, so check them before you attach them.

## Build from source

See [STEAM_FRAME.md](STEAM_FRAME.md). The build runs on the Frame itself with the tools SteamOS has, in about 35 minutes the first time. `tools/privacy-check.sh` checks the tree, the history and a release tarball for personal data before publishing.

## Support

GalaxyFrame exists because of bigmak94's [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) and the Petari decompilation. If you want to support someone, support them first: star and follow GalaxyQuest, report what you find there, and send improvements to the game upstream.

If you would like more Steam Frame ports, you can also [buy me a coffee](https://ko-fi.com/sana748653). It pays for the Claude API credits these ports are built with, spent as carefully as I can.

## Credits

- Built by [simplysanatori](https://github.com/simplysanatori).
- [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) by bigmak94: the VR port of Super Mario Galaxy that this is built on, including the diorama, the giant screen, the VR settings panel, the GX recorder and the OpenGL ES renderer that the Vulkan renderer mirrors. Released under the Unlicense.
- [Petari](https://github.com/SMGCommunity/Petari) by the SMGCommunity: the decompilation of Super Mario Galaxy that makes a native port possible at all. Released under CC0.
- [nod](https://github.com/encounter/nod) by encounter, which extracts the disc.
- [OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) from Khronos, [glslang](https://github.com/KhronosGroup/glslang) for the shader compiler, AMD FidelityFX CAS for the sharpening, and SDL2 for the sound.

[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) has the full list with licences.

Super Mario Galaxy, Mario and their logos are trademarks of Nintendo. This is an unofficial fan project. It is not affiliated with or endorsed by Nintendo, Valve, the Petari team or the GalaxyQuest author.

## Licence

GalaxyQuest and GalaxyFrame's changes are in the public domain under [The Unlicense](LICENSE). The decompilation in `decomp/` is the Petari team's work under CC0 1.0 (`decomp/LICENSE-Petari.txt`). Third-party parts keep their own licences: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The probe in `tools/xrprobe` is GPLv3 and is not part of the game. No Nintendo game data is included.

## How this was made

I wanted Super Mario Galaxy on my Frame without Lepton, so I started from GalaxyQuest and had it built natively for SteamVR. The first version drew through a translation layer and its pacing fought SteamVR; one eye jittered and the GPU ran out in heavy areas. The second step was a Vulkan renderer of its own, pacing written for how SteamVR actually behaves, and then the comfort and control work that makes the diorama pleasant for a longer session. It started as a project for my own headset. It ended up good enough to share.

This is vibe coded, with manual review throughout. I wrote it with Claude Code, using Anthropic's Fable 5.1 and Opus 5.5 models running on the Frame itself. I chose what to build, reviewed the code and the scripts as they were written, tested each build in the headset and reported what I saw; Claude read the logs and changed the code. GalaxyQuest is also AI-written, as its README says. Neither the GalaxyQuest author nor the Petari team has reviewed this port. If that is not for you, no worries. Read the code before trusting it; it is a starting point for anyone who wants Super Mario Galaxy, or another GalaxyQuest-style port, on the Frame.

Developer notes, the probe results and what was learned about SteamVR on the Frame are in [STEAM_FRAME.md](STEAM_FRAME.md).
