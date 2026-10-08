# GalaxyQuest on the Steam Frame

[GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) plays Super Mario Galaxy in VR as a native port of the game, built on the [Petari](https://github.com/SMGCommunity/Petari) decompilation. Upstream ships it as a Meta Quest app. This fork builds the same code as a native Linux arm64 program for Valve's Steam Frame, running on the Frame's own SteamVR: no Android container (Lepton), no APK, no adb. The game, the VR presentation and every VR panel (the settings panel, the HUD, the giant screen, the diorama, the setup screen) are the upstream code unchanged; only the Android glue has a Linux counterpart. The `steam-frame` branch is the one to use.

Built and run on a Steam Frame with SteamOS in October 2026. Not affiliated with Valve, Nintendo, the Petari team or the GalaxyQuest author.

## AI disclosure

The port, its scripts and this document were written by an AI: Anthropic's Claude (Claude Opus 5.5 and Claude Fable 5.1, through Claude Code) running on the Steam Frame itself. The human owner of this repository decided what to attempt, supplied the disc, launched the game in the headset and reported what they saw; the AI read the logs and changed the code. Upstream GalaxyQuest is itself an AI-written project, as its README says. Nothing here has been reviewed by the upstream author or the Petari team. Read the code before trusting it and expect rough edges. "How this was made" at the end has more detail.

## Status

Checked on the Frame, in the headset:

- The game starts through SteamVR, reaches the title screen on the giant screen, and runs at a steady 60 game frames per second at the display's 120 Hz, each frame shown for exactly two refreshes.
- SteamVR binds the Steam Frame controllers through their own interaction profile (table below).
- Sound comes out through SDL2 (PipeWire).
- Quitting from Steam ends the session cleanly.

Not yet confirmed in play: levels, the diorama, haptics, the VR settings panel's every switch, long sessions. Report what you find in the issues, with `~/.local/share/GalaxyQuest/petari_log.txt` attached.

Does not work on the Frame, by the runtime's doing:

- **SpaceWarp** (Meta's frame extrapolation): SteamVR offers no motion vector or depth swapchain formats to OpenGL ES applications. Not needed: at 120 Hz the game paces itself without it.
- **Passthrough**: SteamVR's OpenXR runtime has no `XR_FB_passthrough`. The *Passthrough* switch in the VR settings does nothing.

## Credit

The game is Nintendo's. The source is the **Petari** team's decompilation (CC0), and the port of it to a modern GPU and to VR is **GalaxyQuest by bigmak94** (Unlicense). This fork adds the Linux target and the Steam Frame controller layout, a few hundred lines, and fixes what did not build or run against glibc and libstdc++. Disc extraction uses [nod](https://github.com/encounter/nod) by encounter.

## What the branch changes

1. **Launcher** (`platform/linux/launcher.c`): the game code assumes the Wii's 32-bit address space, so the launcher reserves 0x80000000-0xE0010000 and loads `libgame.so` at 0x98000000. Android has `android_dlopen_ext` for that; here the library is linked with that address as its image base, glibc's loader honours it, and the launcher checks where it landed.
2. **GL context and OpenXR session** (`platform/src/xr/xr_app.cpp`): EGL on Mesa's surfaceless platform (zink on Turnip, OpenGL ES 3.2) instead of Android's, and the session is created through `XR_MNDX_egl_enable` instead of `XR_KHR_android_create_instance`. The OpenXR loader is SteamVR's own (`/opt/steamvr/bin/linuxarm64/libopenxr_loader.so`).
3. **Steam Frame controllers**: bound through `XR_VALVE_frame_controller_interaction`, so SteamVR does not have to translate the Frame's controllers into Quest Touch ones. A and B are the Wii Remote's A and B, as the game's own prompts name them; the left controller's D-pad is the Wii Remote's D-pad.
4. **Sound** (`platform/src/audio/aaudio_out.cpp`): the same ring buffer, drained by SDL2 instead of AAudio.
5. **Storage**: everything lives in `~/.local/share/GalaxyQuest` (`game/` the converted disc, `nand/` the saves, `petari_vr.ini` the VR settings, `petari_log.txt` the log, `shaders.bin` the shader cache). The setup screen's "all files access" step does not exist on Linux.
6. **Clean exit**: SIGTERM (Steam's stop button) asks the runtime to end the session and leaves once it has.
7. **Builds and runs against glibc and libstdc++**: includes spelled as the files are on disk (upstream builds on Windows); the decomp's own `sprintf` and wide-string functions kept in place of glibc's (libstdc++ had un-done the renames, and glibc's `sprintf` printing `(null)` crashed the layout loader at boot); `__fabs`/`__fabsf` renamed; queues shared between game threads and host threads (disc reads, DSP mail, GL deletes) kept on the host heap, since a game-heap block freed on a host thread took the heap's mutex without an OS thread and crashed.

Everything under `decomp/` and `platform/src/xr/` other than `xr_app.cpp` is unchanged from upstream apart from the include spellings and one renamed local `lerp`.

## Install

You need a Steam Frame with SteamVR, and your own Super Mario Galaxy disc as an ISO, RVZ or WBFS image (the US disc RMGE01 is the one tested here; upstream also tested the European RMGP01). Nothing of the game is in this repository or its releases.

1. On the Frame, in a terminal (SSH, or Konsole from desktop mode):

   ```sh
   git clone -b steam-frame https://github.com/simplysanatori/GalaxyQuest-steam-frame.git
   cd GalaxyQuest-steam-frame
   ```

2. Download `GalaxyQuest-steam-frame-*.tar.gz` from the Releases page (or build it, below) and install it. Steam must be running:

   ```sh
   steam-frame/install.sh ~/Downloads/GalaxyQuest-steam-frame-v0.1.8-frame.1.tar.gz
   ```

   This puts the program in `~/Games/GalaxyQuest/app`, adds **Super Mario Galaxy VR** to your Steam library and sets it to 120 Hz in SteamVR.

3. Convert your disc, once (a few minutes, 3.3 GB; `--no-movies` saves 2.3 GB and plays the prologue and ending movies as black):

   ```sh
   steam-frame/prepare-game.sh ~/Documents/Roms/wii/SuperMarioGalaxy.rvz
   ```

4. Put the headset on and launch **Super Mario Galaxy VR** from the library. It starts on a giant screen; the VR settings (below) switch it to the diorama.

## Controls

| Steam Frame | Wii | In the game |
|---|---|---|
| Left stick | Nunchuk stick | Move |
| A | A | Jump, talk, confirm. Hold to float as Boo Mario or to skip a cutscene or dialogue |
| B | B | Shoot star bits, cancel, back out of menus |
| Right trigger | B | The same as B, if you prefer the trigger for shooting |
| Y, right bumper, or a flick of either controller | Shake | Spin |
| Left trigger | Z | Crouch, ground pound, long and back flip jumps |
| Left grip or left bumper | C | Put the game camera behind Mario |
| Menu, X or View | + / − | Pause menu, with the VR settings panel beside it |
| D-pad (left controller) | D-pad | The Wii D-pad. Left and right turn the diorama in steps, as the right stick does |
| Right stick left / right | D-pad left / right | Turn the diorama round Mario, or the game camera on the giant screen |
| Right stick up | D-pad up | First-person look |
| Right controller aim | Pointer | Collect star bits, grab Pull Stars, point at menus |

**The VR settings panel.** Press Menu (or X, or View) to open the pause menu. The VR settings panel appears to the right of its buttons. Aim the right controller at it and press A or pull the trigger; its four tabs hold every setting of `petari_vr.ini`. The *Screen* tab has *Giant screen*, the switch between the screen and the diorama. [docs/CONTROLS.md](docs/CONTROLS.md) explains every setting.

**Recentring.** Upstream recentres on the Meta button, which the Frame does not have. Use SteamVR's own recentre (hold the system button, or the dashboard): the game listens for the runtime moving its reference space and puts the screen or diorama in front of you again.

## Settings that matter on the Frame

- **120 Hz.** `install.sh` sets the shortcut's SteamVR refresh rate to 120 Hz. At 72 or 90 Hz the game still runs, but its 60 fps no longer divides the display's rate and motion is uneven. The game's own *Refresh rate* setting cannot change SteamVR's rate; the per-app SteamVR setting is what counts.
- **Resolution.** The game renders each eye at the size SteamVR recommends times its own dynamic scale (0.8 to 1.6, in `petari_vr.ini`), so SteamVR's resolution slider is the one to raise or lower.
- **Sound** goes to the default PipeWire output, so the Frame's speakers or whatever SteamVR has selected.

## Known limitations

- SpaceWarp and passthrough, see Status.
- No controller models are drawn (upstream draws none either; the laser shows while pointing at the settings panel).
- Steam's overlay does not inject into the game (it is a plain GLES program; the `LD_PRELOAD` warnings in `launch.log` are that and are harmless).
- Only the US disc has been tried on the Frame.

## Building

All on the Frame, with the tools SteamOS has: clang, lld, CMake, Ninja, the EGL/GLES and SDL2 headers, Python 3, and SteamVR installed.

```sh
./build_linux.sh          # build-linux/galaxyquest, libgame.so, petari_headless (about 35 minutes)
steam-frame/install.sh    # installs that build (stripped) and adds the shortcut
steam-frame/build.sh v0.1.8-frame.2   # build and package steam-frame/release/*.tar.gz
```

The first build fetches the Khronos OpenXR headers (`tools/fetch_openxr.sh`, the Android loader package, only its headers are used). `compat.h` and `ppc_intrinsics.h` are included in every file, so a change to them rebuilds everything.

**Headless test**, without the headset: boots the game, saves a snapshot every 5 s.

```sh
mkdir -p /tmp/gq/nand && EGL_PLATFORM=surfaceless SDL_AUDIODRIVER=dummy PETARI_SHOT_MS=5000 \
  build-linux/petari_headless ~/.local/share/GalaxyQuest/game /tmp/gq/nand 45     # -> /tmp/gq/shots/*.png
```

`steam-frame/dev/fix_include_case.py <repo> --apply` rewrites `#include` lines to the files' real spelling after merging upstream.

## Runtime facts, for anyone digging further

- SteamVR's Linux arm64 OpenXR runtime advertises `XR_KHR_opengl_es_enable`, `XR_MNDX_egl_enable`, `XR_FB_display_refresh_rate` (it lists only the current rate; requesting another returns `XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB`), `XR_META_recommended_layer_resolution`, `XR_META_performance_metrics` (only the app GPU counter answers), `XR_FB_space_warp` (no GLES formats for it), `XR_VALVE_frame_controller_interaction`, and no `XR_FB_passthrough`.
- `/usr/lib/libopenxr_loader.a` on SteamOS is an empty stub; the working loader is SteamVR's.
- A session may hold 16 swapchains; the game makes 9.
- The game's shutdown with `exit()` hung on a static destructor while game threads still ran; the Linux build leaves with `_exit()` after the session has ended.
- `ptrace` attach is not permitted for user processes here; debug with `gdb --args` from the start, or `/proc/<pid>/task/*/wchan`.

## How this was made

I did not write the code by hand. The investigation, the port, the scripts and this document were produced by Anthropic's Claude (Claude Opus 5.5 and Claude Fable 5.1, through Claude Code) working on my Steam Frame, with me supplying the disc and launching builds in the headset. The work went roughly like this: reading GalaxyQuest's platform layer to see how much was Android-specific (little: the launcher, the OpenXR session binding, the sound output, storage paths), checking that SteamVR's native runtime offers GLES through EGL, replacing those pieces, then four rounds of full builds against glibc and libstdc++ for things that compile differently from Android's bionic and libc++, and two boot crashes traced with gdb (a `(null)` path from glibc's `sprintf`, and a disc-read queue freed on the wrong heap). Everything in Status comes from launching the result on the headset, not from reasoning about the code.

## License

GalaxyQuest is released into the public domain under [The Unlicense](LICENSE), and so are this fork's changes. The decompilation in `decomp/` is the Petari team's work under CC0 1.0 (`decomp/LICENSE-Petari.txt`). Third-party parts keep their own licenses, see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). No Nintendo game data is included.
