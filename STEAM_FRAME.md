# GalaxyQuest on the Steam Frame: technical notes

[GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) plays Super Mario Galaxy in VR as a native port of the game, built on the [Petari](https://github.com/SMGCommunity/Petari) decompilation. Upstream ships it as a Meta Quest app. This fork builds the same code as a Linux arm64 program for Valve's Steam Frame. It runs on the Frame's own SteamVR, without Lepton, an APK or adb. Use the `steam-frame` branch.

Install, first run, controls, settings, troubleshooting and how to report problems are in the [README](README.md). This document lists what the branch changes and why, how to build it, and what was learned about SteamVR on the Frame.

Built and run on a Steam Frame with SteamOS in October 2026. Not affiliated with Valve, Nintendo, the Petari team or the GalaxyQuest author.

## AI disclosure

The port, its scripts and this document were written with Anthropic's Claude (Claude Opus 5.5 and Claude Fable 5.1, through Claude Code), running on the Steam Frame. The owner of this repository chose what to attempt, supplied the disc, launched each build in the headset and reported what they saw. Claude read the logs and changed the code. Upstream GalaxyQuest is also AI-written, as its README says. The upstream author and the Petari team have not reviewed this fork. Read the code before trusting it. "How this was made" at the end has more detail.

## Status

Checked on the Frame, in the headset:

- The game starts through SteamVR and reaches the title screen on the giant screen.
- It runs at a steady 60 game frames per second on the 120 Hz display. Each frame is shown for two refreshes.
- It has been played in the diorama through the Comet Observatory and into levels, with 2 to 10 missed refreshes per 10 seconds and both eyes complete. This took several rounds of pacing work. The earlier builds jittered, ghosted or stuttered in one eye.
- SteamVR binds the Steam Frame controllers through their own interaction profile.
- Sound plays through SDL2 (PipeWire).
- Quitting from Steam ends the session cleanly.

Known: a hitch of about a second when a new area loads (texture uploads and shader compiles). One refractive crystal once showed streaks in one eye. Not yet confirmed in the headset: haptics (frame.3), long sessions.

Two upstream features do not work on the Frame, because SteamVR does not provide them:

- **SpaceWarp** (Meta's frame extrapolation). SteamVR offers no motion vector or depth swapchain formats to OpenGL ES apps. At 120 Hz the game paces itself without it.
- **Passthrough.** SteamVR's OpenXR runtime has no `XR_FB_passthrough`. The *Passthrough* switch in the VR settings has no effect.

The same holds for *Super resolution* (no `XR_FB_composition_layer_settings`), *High clocks* (no `XR_EXT_performance_settings`) and *Refresh rate* (the runtime lists only the current rate and refuses a request for another). The README has the table.

## What the branch changes

1. **Launcher** (`platform/linux/launcher.c`). The game code assumes the Wii's 32-bit address space. The launcher reserves 0x80000000 to 0xE0010000 and loads `libgame.so` at 0x98000000. Android has `android_dlopen_ext` for that. Here the library is linked with that address as its image base, glibc's loader honours it, and the launcher checks where it landed.
2. **GL context and OpenXR session** (`platform/src/xr/xr_app.cpp`). EGL runs on Mesa's surfaceless platform (zink on Turnip, OpenGL ES 3.2). The session is created through `XR_MNDX_egl_enable` in place of `XR_KHR_android_create_instance`. The OpenXR loader is SteamVR's own (`/opt/steamvr/bin/linuxarm64/libopenxr_loader.so`).
3. **Steam Frame controllers** (`suggestFrame()` and `updateInput()` in `xr_app.cpp`). They are bound through `XR_VALVE_frame_controller_interaction`, so SteamVR does not translate them into Quest Touch controllers. A and B are the Wii Remote's A and B, as the game's own prompts name them. X, Y, the right bumper or a flick spin. The left controller's D-pad is the Wii Remote's D-pad, and View is minus. The full table is in the README. Rumble is routed to the haptic action bound on both hands' `output/haptic`; see "Haptics" below.
4. **Sound** (`platform/src/audio/aaudio_out.cpp`). The same ring buffer, drained by SDL2 in place of AAudio.
5. **Storage.** Everything lives in `~/.local/share/GalaxyQuest` (or `$GALAXYQUEST_HOME`): `game/` the converted disc, `nand/` the saves, `petari_vr.ini` the VR settings, `petari_log.txt` the log, `shaders.bin` the shader cache. The setup screen's "all files access" step does not exist on Linux.
6. **Clean exit.** SIGTERM (Steam's stop button) asks the runtime to end the session, and the program leaves once it has.
7. **Frame pacing for SteamVR.** SteamVR on the Frame reads a released OpenGL ES swapchain image at once, finished or not. Quest's runtime waits for the GPU. SteamVR's compositor also shares the GPU with the game, so the game's eye images took up to 12 ms to finish after being queued. Upstream releases the right eye the moment its draws are queued. On the Frame that eye showed half-drawn and jittered, while the left eye, held for a refresh, was fine. Now both eyes are rendered from one head pose (left on one refresh, right on the next) and kept acquired. They are released two refreshes after the right eye, once a fence confirms the GPU is done. Three sets of eye images rotate, so none is drawn into while on display. This adds one refresh of latency compared with upstream.
   Tried and dropped on the way: a CPU wait before release (the loop missed refreshes and both eyes ghosted); both eyes in one refresh (too much CPU work in one step); handing over a refresh earlier (the right eye alone stuttered).
   `PETARI_RETRACE_STEP=2` in `petari_debug.env` moves the game's own frame timing a refresh later to win that latency back. The cost is a repeated frame whenever the game takes over 8 ms on one, which happens as areas load.
8. **SteamVR per-app settings, re-applied at launch** (`steam-frame/install.sh`, `steam-frame/galaxyquest.sh`). Through `vrcmd`, the shortcut gets `preferredRefreshRate=120`, `framesToThrottle=0`, `additionalFramesToPredict=0` and `motionSmoothingOverride=2` (motion smoothing off). SteamVR throttles an app that has missed frames. With the pacing above, that halved the game's speed while motion smoothing synthesized the frames in between, and the picture ghosted on head movement. SteamVR also dropped the whole per-app block from its settings file once, which put the display at 72 Hz. The pacing detects a throttled loop and falls back to one game frame per loop frame.
9. **Steady resolution** (`platform/src/xr/vr_game.cpp`). The eyes cost the GPU about the same at any size. The cost is per-frame overhead from the game's scene copies, and pixels matter little. Each step of the dynamic resolution was a visible change in sharpness. So the Frame build holds it at 0.8 of SteamVR's recommended size (`min_resolution = resolution = 0.8`). Render targets of sizes no longer in use are kept and reused, so changing the size no longer stalls the render thread.
10. **Thread priority** (`platform/src/os/os_thread.cpp`). The game threads and the frame loop run at nice -8, the lowest the system allows. Without it the game thread sometimes lost the CPU for 10 ms or more, which showed as a repeated frame.
11. **Builds and runs against glibc and libstdc++.** Includes are spelled as the files are on disk (upstream builds on Windows). The decomp's own `sprintf` and wide-string functions are kept in place of glibc's: libstdc++ had undone the renames, and glibc's `sprintf` printing `(null)` crashed the layout loader at boot. `__fabs` and `__fabsf` are renamed. Queues shared between game threads and host threads (disc reads, DSP mail, GL deletes) are kept on the host heap. A game-heap block freed on a host thread had taken the heap's mutex without an OS thread and crashed.
12. **Scripts** (`steam-frame/`). `install.sh` installs a release tarball or a local build and adds or updates the Steam shortcut through Steam's local CEF debugging port (`steam-cef.py`). `prepare-game.sh` extracts and converts your disc with nodtool. `warm-shaders.sh` fills the shader cache headless. `build.sh` builds and packages a release.

Under `decomp/`, the only changes are include spellings, one added `<cfloat>` include and one renamed local `lerp`. Under `platform/`, the changes are those listed above.

## frame.3: structure for the Vulkan backend

frame.3 prepares the code for a native Vulkan renderer beside the GLES one. The game's behaviour does not change: GLES stays the default and the fallback, and the replay and xrsim outputs of the GLES path are byte for byte what they were before the split.

- **Renderer interface** (`platform/src/gx/gpu_renderer.h`). `gpu::Renderer` is an abstract class with the public methods the game and the XR layer already used. Textures are `TexHandle` (a GL name under GLES, an image slot under Vulkan) and render targets are `Target {fbo, color, depth, w, h}`. The preparation of a recorded frame (`Builder`, the item list, the copy-texture keys) moved to `gx/gpu_prepare.*` and is shared by both backends. The GLES backend is `gl_renderer.cpp`. `gpu::selectRenderer` picks the backend at startup: `PETARI_GFX=vk|gles` in the environment, else `renderer=vk|gles` in `petari_vr.ini`, else GLES. A Vulkan request that cannot be met logs why and uses GLES.
- **Shader library** (`tools/glsl`, `libgq_glsl.so`). The game generates its shaders as GLSL at run time. For Vulkan they go through glslang (linked statically into this separate library, which keeps `libgame.so`'s own flags and fixed address untouched) to SPIR-V. `libgq_glsl.so` needs only libstdc++, libm, libgcc_s and libc and is loaded with `dlopen` from the directory of `libgame.so`. It is stripped and shipped beside it in the release. If it is missing the Vulkan backend reports that and the game runs on GLES. `PETARI_GQ_GLSL=<path>` names another copy. The front end (`gx/vk_spirv.*`) keeps the compiled SPIR-V in `spirv.bin` and can seed it from an existing `shaders.bin`.
- **XR graphics split** (`xr/xr_gfx*`, `xr/vr_draw*`). `xr_gfx` is the graphics half of the OpenXR session (binding, swapchains, submit and fences, depth and motion vector swapchains); `vr_draw` is what the presentation draws itself (the composite of the diorama into the eye image, panels, laser, skip ring, UI layer images, motion passes, GPU timers). `xr_app.cpp`, `vr_game.cpp`, `vr_settings.cpp` and `vr_setup.cpp` work through them and no longer call a graphics API. The GLES implementations are `xr_gfx_gl.cpp` and `vr_draw_gl.cpp`. The frame loop stays in `xr_app.cpp`.
- **Probes** (`tools/xrprobe`). A standalone OpenXR + Vulkan program that tests how SteamVR treats Vulkan apps (pacing, frame synthesis, depth). It is GPLv3, not part of the game build and not shipped in the release (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). Results are at the end of this document.
- **Packaging.** The release tarball now holds `libgq_glsl.so` too. `install.sh` copies it into the app folder and removes an older copy when a release has none. `tools/privacy-check.sh` checks the tree, the history of the current branch or a tarball for personal data (the personal strings come from a file outside the repository).

### Haptics

The game drives the Wii Remote's rumble motor with `WPADControlMotor` (`platform/src/input/wpad.cpp`), which `updateInput()` reads through `port_input_rumble(0)` every frame. Both controllers buzz. While the motor is on each gets a 120 ms pulse at amplitude 0.5, renewed every 80 ms; when the motor goes off the haptic is stopped at once. Short pulses mean a runtime that ignores the duration cannot leave a controller running. A 15 ms tick at amplitude 0.25 marks the pointer reaching a new target or control of the settings panel. The haptic action is bound on `/user/hand/left/output/haptic` and `/user/hand/right/output/haptic` for the Frame profile (`/interaction_profiles/valve/frame_controller`) and the Touch profiles. `haptics = 0` in `petari_vr.ini` turns both off. The result of the first `xrApplyHapticFeedback` is logged once as `haptics: first pulse result N`.

## Building

All on the Frame, with the tools SteamOS has: clang, lld, CMake 3.24 or newer, Ninja, the EGL/GLES and SDL2 headers, Python 3, and SteamVR installed.

```sh
./build_linux.sh                      # build-linux/galaxyquest, libgame.so, petari_headless (about 35 minutes)
steam-frame/install.sh                # installs that build (stripped) and adds the shortcut
steam-frame/build.sh v0.1.8-frame.3   # builds and packages steam-frame/release/*.tar.gz
```

A release tarball holds five files: `galaxyquest` (the launcher), `libgame.so` (the game, stripped), `libgq_glsl.so` (the Vulkan backend's shader compiler, stripped; the game runs without it, on GLES), `petari_headless` (the headless runner for `warm-shaders.sh`) and `galaxyquest.sh` (the launch wrapper). `steam-frame/release/` is ignored by git.

**Trial install.** `install.sh` can install into a scratch folder without touching Steam or SteamVR: `GALAXYQUEST_APP=DIR GALAXYQUEST_SKIP_STEAM=1 steam-frame/install.sh TARBALL`. Run `tools/privacy-check.sh TARBALL` on a release before publishing it.

The first build fetches the Khronos OpenXR headers (`tools/fetch_openxr.sh`, the Android loader package; only its headers are used). `compat.h` and `ppc_intrinsics.h` are included in every file, so a change to them rebuilds everything.

**Headless test**, without the headset. It boots the game and saves a snapshot every 5 seconds:

```sh
mkdir -p /tmp/gq/nand && EGL_PLATFORM=surfaceless SDL_AUDIODRIVER=dummy PETARI_SHOT_MS=5000 \
  build-linux/petari_headless ~/.local/share/GalaxyQuest/game /tmp/gq/nand 45     # -> /tmp/gq/shots/*.png
```

`steam-frame/dev/fix_include_case.py <repo> --apply` rewrites `#include` lines to the files' real spelling after merging upstream.

## Runtime facts, for anyone digging further

- SteamVR's Linux arm64 OpenXR runtime advertises `XR_KHR_opengl_es_enable`, `XR_MNDX_egl_enable`, `XR_FB_display_refresh_rate`, `XR_META_recommended_layer_resolution`, `XR_META_performance_metrics`, `XR_FB_space_warp` and `XR_VALVE_frame_controller_interaction`. It has no `XR_FB_passthrough`.
- `XR_FB_display_refresh_rate` lists only the current rate. Requesting another returns `XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB`.
- Of `XR_META_performance_metrics`, only the app GPU counter answers. `XR_FB_space_warp` offers no GLES formats.
- `/usr/lib/libopenxr_loader.a` on SteamOS is an empty stub. The working loader is SteamVR's.
- A session may hold 16 swapchains. The game makes 9.
- The game's shutdown with `exit()` hung on a static destructor while game threads still ran. The Linux build leaves with `_exit()` after the session has ended.
- `ptrace` attach is not permitted for user processes here. Debug with `gdb --args` from the start, or read `/proc/<pid>/task/*/wchan`.

## How this was made

I did not write the code by hand. The investigation, the port, the scripts and this document were produced by Anthropic's Claude (Claude Opus 5.5 and Claude Fable 5.1, through Claude Code) working on my Steam Frame. I supplied the disc and launched builds in the headset.

The work went roughly like this. First, reading GalaxyQuest's platform layer to see how much was Android-specific. Little was: the launcher, the OpenXR session binding, the sound output and the storage paths. Then checking that SteamVR's native runtime offers GLES through EGL, and replacing those pieces. Then four rounds of full builds against glibc and libstdc++, for code that compiles differently from Android's bionic and libc++. Two boot crashes were traced with gdb: a `(null)` path from glibc's `sprintf`, and a disc-read queue freed on the wrong heap. After that came several rounds of pacing work, each one tested in the headset. Everything in Status was checked by running the game in the headset.

## Licence

GalaxyQuest is released into the public domain under [The Unlicense](LICENSE), and so are this fork's changes. The decompilation in `decomp/` is the Petari team's work under CC0 1.0 (`decomp/LICENSE-Petari.txt`). Third-party parts keep their own licences: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). No Nintendo game data is included.

## Pacing probes (xrprobe), results pending H1

`tools/xrprobe/` is a small standalone OpenXR + Vulkan program (its own CMake project, not part of the game build) that answers the questions the frame.3 Vulkan backend depends on. `tools/xrprobe/run-h1.sh` runs its five cases in a row and writes every log, plus `vrtiming.py` output when that script is present, into one dated directory under `~/Games/GalaxyQuest/probe/`.

| Case | What it tests | Result |
|---|---|---|
| `--pace` | 14 ms of GPU work per frame, images released right after `vkQueueSubmit`. Does SteamVR show unfinished Vulkan images, as it does for GLES? | pending H1 |
| `--pace=fence` | The same with `vkWaitForFences` before the release (control) | pending H1 |
| `--synth` | `XR_EXT_frame_synthesis`: motion vectors (R16G16B16A16_SFLOAT) and depth at 30 fps with `REQUEST_RELAXED_FRAME_INTERVAL` | pending H1 |
| `--synth --no-mv` | The same frame rate without synthesis data | pending H1 |
| `--depth-only` | `XR_KHR_composition_layer_depth` at the display rate | pending H1 |

Seen in a 3-second smoke run with the headset asleep (no frames shown): SteamVR/OpenXR 2.18.2 accepts Vulkan 1.0 to 1.2, recommends 1728x1728 per eye, gives 3 images per swapchain and listed only 90 Hz. It offers R8G8B8A8_SRGB, B8G8R8A8_SRGB, R32G32B32A32_SFLOAT, R32G32B32_SFLOAT, R16G16B16A16_SFLOAT, A2B10G10R10_UNORM, D32_SFLOAT, D16_UNORM, D24_UNORM_S8_UINT and D32_SFLOAT_S8_UINT to Vulkan apps.
