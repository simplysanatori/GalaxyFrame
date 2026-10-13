# GalaxyFrame: technical notes

GalaxyFrame is a port of [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) to the Valve Steam Frame. GalaxyQuest plays Super Mario Galaxy in VR as a native port of the game, built on the [Petari](https://github.com/SMGCommunity/Petari) decompilation, and ships as a Meta Quest app. GalaxyFrame builds the same code as a Linux Arm64 program that runs in the Frame's own SteamVR, without Lepton, an APK or adb. This document is for version 1.0.0.

Install, first run, controls, settings, troubleshooting and how to report problems are in the [README](../README.md). This one says how the port works, how to build it, and what I learned about SteamVR on the Frame. The setting-by-setting description of the VR panel and the ini file is in [CONTROLS.md](CONTROLS.md); the environment switches for testing are in [TECHNICAL.md](TECHNICAL.md), which is GalaxyQuest's own technical document and still describes the shared code.

Not affiliated with Valve, Nintendo, the Petari team or the GalaxyQuest author.

## AI disclosure

I did not write this code by hand. The port, its scripts and these notes were written with Anthropic's Claude (Claude Opus 5.5 and Claude Fable 5.1, through Claude Code), running on my Steam Frame. I chose what to attempt, supplied the disc, reviewed the code and the scripts as they were written, ran each build in the headset and reported what I saw. Claude read the logs and changed the code. GalaxyQuest is also AI-written, as its README says. The GalaxyQuest author and the Petari team have not reviewed this port. Read the code before trusting it. "How this was made" at the end has more.

## Status

Checked in the headset, on one Steam Frame (SteamOS 0.4.5, SteamVR 2.18) with the US disc:

- The game starts through SteamVR and plays on the giant screen or in the diorama.
- The game runs at 60 frames a second on the 120 Hz display, and both eyes are rendered on every refresh from that refresh's head pose, at resolution 1.0.
- Flights and in-engine cutscenes show in the diorama and follow the game's camera.
- The Steam Frame controllers work through their own interaction profile, with rumble on both.
- The VR settings panel, the comfort vignette, the full-view fades, saving and loading, sound through SDL2 (PipeWire), and quitting from Steam all work.

Only tested headless or by automated checks: the European disc and other discs than the US one, long sessions, and the OpenGL ES fallback in the headset.

Known: Mario and the world move at the game's 60 frames a second, so on the 120 Hz display an object your eye follows can look doubled along its movement; head movement is not affected. A new area still hitches briefly while its textures upload. Eye-tracked foveated rendering is built in (`foveation` in the ini) but off: on this GPU it saves little and blurs much of the view.

Some GalaxyQuest features do nothing on the Frame, because SteamVR does not provide what they need:

- **SpaceWarp** (Meta's frame extrapolation). SteamVR's runtime accepts the extension but the compositor does not use the motion vectors (see the probe results below). At 120 Hz the game paces itself without it.
- **Passthrough.** SteamVR's OpenXR runtime has no `XR_FB_passthrough`.
- **Super resolution** (no `XR_FB_composition_layer_settings`), **High clocks** (no `XR_EXT_performance_settings`) and **Refresh rate** as a request to the runtime (it lists only the current rate and refuses another). These switches are hidden on the Frame. The wrapper asks SteamVR for the rate through its per-app settings instead.

## How it works

### Launcher and process

The game code assumes the Wii's 32-bit address space. `platform/linux/launcher.c` reserves 0x80000000 to 0xE0010000 and loads `libgame.so` at 0x98000000. Android has `android_dlopen_ext` for that. Here the library is linked with that address as its image base, glibc's loader honours it, and the launcher checks where it landed. Then it hands over to the game with the data folder, `$GALAXYFRAME_HOME` if set (the old `$GALAXYQUEST_HOME` still works), else `~/.local/share/GalaxyFrame`.

SIGTERM (Steam's stop button) asks the runtime to end the session, and the program leaves once it has. The Linux build then leaves with `_exit()`: the game's shutdown with `exit()` hung on a static destructor while game threads still ran.

The program is built against glibc and libstdc++. Includes are spelled as the files are on disk, since the decompilation is written on Windows. The decomp's own `sprintf` and wide-string functions are kept in place of glibc's, because glibc's `sprintf` printing `(null)` crashed the layout loader at boot. `__fabs` and `__fabsf` are renamed. Queues shared between game threads and host threads (disc reads, DSP mail, GL deletes) are kept on the host heap: a game-heap block freed on a host thread had taken the heap's mutex without an OS thread and crashed. Under `decomp/` the only changes are include spellings, one added `<cfloat>` include and one renamed local `lerp`.

### Storage and sound

Everything the app keeps is in `~/.local/share/GalaxyFrame`: `game/` the converted disc, `nand/` the saves, `petari_vr.ini` the VR settings, `petari_log.txt` the log (the last session's is `petari_log.txt.prev`), `launch.log` the launch wrapper's output, `vrtiming.log` SteamVR's compositor timing, and the shader caches. `petari_debug.env` (`NAME=value` lines, read at start) sets test options; `session.env` does the same for one launch from the Steam library. The setup screen's "all files access" step of the Quest build does not exist on Linux.

Sound is the same ring buffer GalaxyQuest uses, drained by SDL2 in place of AAudio.

### OpenXR session

The session is created through `XR_MNDX_egl_enable` for the OpenGL ES path and `XR_KHR_vulkan_enable2` for the Vulkan one, in place of `XR_KHR_android_create_instance`. The OpenXR loader is SteamVR's own (`/opt/steamvr/bin/linuxarm64/libopenxr_loader.so`); `/usr/lib/libopenxr_loader.a` on SteamOS is an empty stub. The OpenGL ES path runs on Mesa's surfaceless EGL platform (zink on Turnip, OpenGL ES 3.2).

### Renderer

The game records each frame as a list of GX commands (GalaxyQuest's recorder) and a renderer replays it. `platform/src/gx/gpu_renderer.h` is the interface: `gpu::Renderer` with the methods the game and the XR layer use, `TexHandle` for textures (a GL name under GLES, an image slot under Vulkan) and `Target` for render targets. Preparing a recorded frame (`gx/gpu_prepare.*`) is shared. The backends are the Vulkan renderer (`vk_*.cpp`) and the OpenGL ES one (`gl_renderer.cpp`).

On the Frame the Vulkan renderer is the default. `PETARI_GFX=vk|gles` in the environment wins, then `renderer = vk|gles` in `petari_vr.ini`. If Vulkan cannot be set up, or its shader library is missing, the log says why and the game runs on OpenGL ES. Replaying with Vulkan directly instead of OpenGL ES over a translation layer made each eye about a third cheaper. Further savings: per-pixel drawing state is sent once per run of draws, draws whose output nothing reads are skipped, copies are blits, and the HUD is redrawn once per game frame. The switches to turn each of these off are listed in TECHNICAL.md.

The XR half is split the same way. `xr_gfx*` is the graphics part of the session (binding, swapchains, submit and fences, depth swapchains); `vr_draw*` is what the presentation draws itself: the diorama composite into the eye image, panels, laser, skip ring, the sharpening pass, UI layers and GPU timers. `xr_app.cpp`, `vr_game.cpp`, `vr_settings.cpp` and `vr_setup.cpp` work through them and call no graphics API directly. The frame loop is in `xr_app.cpp`.

### Shader library and caches

The game generates its shaders as GLSL at run time. For Vulkan they go through glslang to SPIR-V. glslang is linked statically into a separate library, `libgq_glsl.so`, which keeps `libgame.so`'s own flags and fixed address untouched. It needs only libstdc++, libm, libgcc_s and libc, and is loaded with `dlopen` from the directory of `libgame.so`; `PETARI_GQ_GLSL=<path>` names another copy.

Three caches live in the data folder:

- `spirv.bin`: the compiled SPIR-V of the generated shaders. It does not depend on the GPU or driver, so it can be shared. `gx/vk_spirv.*` can also seed it from an OpenGL ES `shaders.bin` (about two seconds for a warmed cache).
- `pipelines.bin`: Vulkan pipeline cache data, for this device and driver only. It is saved once the background compiles have settled.
- `shaders.bin`: the OpenGL ES fallback's program binaries.

At launch every pipeline in the cache is created in the background. With a warm cache about 1000 pipelines are ready in 0.1 s. `steam-frame/warm-shaders.sh` fills the caches ahead of play by booting the game headless into 20 galaxies one after another.

A release can carry a `spirv.bin` as a seed (see Building). `install.sh` places it in the data folder only if there is none there yet, so a first launch compiles fewer shaders and an existing cache is never replaced.

### Frame pacing

SteamVR on the Frame shows a released swapchain image at once, finished or not. Quest's runtime waits for the GPU. So an image must be released only after the GPU has finished it, and the compositor shares the GPU with the game. The Vulkan loop works like this:

- SteamVR's `xrWaitFrame` returns about one refresh before the display time, too little to render both eyes and wait for them within the refresh they are shown in. So every set of eye images is rendered on one refresh for the next refresh's display time and released on that next refresh.
- On a refresh: the set rendered on the last refresh is waited for, released and submitted with exactly the poses and fields of view it was rendered with. Then one `xrLocateViews` for the next display time, the newest game frame, and both eyes recorded into one command buffer and submitted. Both eyes come from one head pose, so they agree to 0.1 mm. Eight command buffers are in flight so the CPU never waits while recording.
- At 120 Hz the game runs at 60 frames a second and the eyes are rendered on every refresh. If more than 5 % of the sets of the last 2 s missed their display, or both eyes took over 85 % of a refresh on the GPU, only the refresh that picks up a game frame renders and the one in between shows its set again for the compositor to reproject. It returns to every refresh after 10 s with at most 1 % missed and the eyes under 75 % of a refresh. `PETARI_VKPACE=every|second` fixes either. The log says when it changes.
- At other refresh rates every refresh renders and the game keeps its own clock.

The OpenGL ES fallback has its own loop with the same rule: both eyes are rendered from one head pose, kept acquired, and released two refreshes later once a fence confirms the GPU is done, with three sets of images rotating. It adds a refresh of latency, which `PETARI_RETRACE_STEP=2` in `petari_debug.env` can win back at the cost of a repeated frame whenever the game takes over 8 ms on one.

Things I tried that did not work: a CPU wait before release (the loop missed refreshes and both eyes ghosted), both eyes in one step on the GLES loop (too much CPU work in one step), and handing over a refresh earlier (the right eye alone stuttered).

### SteamVR per-app settings

`install.sh` sets, and `galaxyframe.sh` sets again at every launch, through `vrcmd` for the Steam shortcut: `preferredRefreshRate` 120 (or 90 with `refresh_rate = 90` in the ini), `framesToThrottle=0`, `additionalFramesToPredict=0` and `motionSmoothingOverride=2` (motion smoothing off). SteamVR throttles an app that has missed frames, which halved the game's speed while motion smoothing synthesized the frames in between and the picture ghosted on head movement. SteamVR also dropped the whole per-app block from its settings file once, which put the display at 72 Hz, so the wrapper writes it every time.

### Resolution

The default eye size is 0.9 of the size SteamVR's own resolution setting gives the game (1728 by 1728 per eye at 100 % on the Frame), held steady for the session (`min_resolution = resolution`), since each step of a dynamic resolution was a visible change in sharpness. At 0.9 both eyes render every refresh with some headroom, and 1.0 in most scenes; the pacing drops to every second refresh where the GPU cannot keep up and comes back on its own. Render targets of sizes no longer in use are kept and reused, so changing the size does not stall the render thread. The sharpening pass is AMD FidelityFX CAS.

### Controls and haptics

The Frame controllers are bound through `XR_VALVE_frame_controller_interaction` (`suggestFrame()` and `updateInput()` in `xr_app.cpp`), so SteamVR does not translate them into Quest Touch controllers. A and B are the Wii Remote's A and B, X, Y, the right bumper or a flick spin, the left D-pad is the Wii D-pad and View is Minus. The full table is in the README. In the diorama the right stick turns and tilts the view smoothly, and the left grip with the D-pad sets its height and size. A left-handed option swaps the hands.

The game drives the Wii Remote's rumble motor with `WPADControlMotor` (`platform/src/input/wpad.cpp`), which `updateInput()` reads through `port_input_rumble(0)` every frame. Both controllers buzz: while the motor is on each gets a 120 ms pulse at amplitude 0.5, renewed every 80 ms, and when the motor goes off the haptic is stopped at once. Short pulses mean a runtime that ignores the duration cannot leave a controller running. A 15 ms tick at amplitude 0.25 marks the pointer reaching a new target on the settings panel. The haptic action is bound on both hands' `output/haptic` for the Frame profile and the Touch profiles. `haptics = 0` in `petari_vr.ini` turns both off. The result of the first `xrApplyHapticFeedback` is logged once as `haptics: first pulse result N`.

### Comfort

The comfort vignette darkens the edges of the view when it moves by your input or on its own: turning, tilting, resizing, planet turns and flights. It is measured in viewing angle: a strength of 0.3 is a clear circle of 34 degrees with black edges, 1 a tunnel of 13 degrees. Two sliders set the strength at the slowest and at the fastest movement. Planet turns can be smooth, in steps (the world stays still and is re-levelled behind a short blink once it has drifted 35 degrees) or off (the right stick click levels it). A sudden change of gravity always happens behind a blink. The death screen and the game's other fades darken the whole view, not just the screen.

### Diorama, cutscenes and the giant screen

The diorama puts the game's world in front of you at a scale and height you set. Flights between planets and in-engine cutscenes show in it too, following the game's own camera, with a switch for each kind of cutscene on the panel. The game's movies stay on the flat screen, as do the cutscenes you switch off there. Hold A to skip a cutscene or a dialogue; a ring fills at the lower right of the view. On the giant screen the laser, the pointer and the skip ring are drawn over the flat picture, and the VR settings panel is placed clear of the game's pause menu. The panel has six tabs, with the switches that do nothing on the Frame hidden.

### Area loads and thread priority

Textures are decoded on two helper threads instead of the game thread, and the frame buffers are allocated and mapped ahead of use. The slowest game frame at an area load dropped from about 35 ms to about 20 ms.

The game threads, the frame loop and the frame preparation worker ask RealtimeKit (over D-Bus) for nice -15 and fall back to nice -8, the lowest the system allows without it (`platform/src/port/thread_prio.cpp`). Without a raised priority the game thread sometimes lost the CPU for 10 ms or more, which showed as a repeated frame. `PETARI_THREADPRIO=rt` makes the main game thread real-time (SCHED_RR 5, dropped back before RealtimeKit's CPU limit); `nice` and `off` are also accepted. The default stays at nice -15 because SteamVR's compositor threads are not real-time.

### Packaging and scripts

`steam-frame/` holds the scripts:

- `build.sh` builds and packages a release.
- `install.sh` installs a release tarball or a local build, adds or updates the Steam shortcut through Steam's local debugging port (`steam-cef.py`) and sets SteamVR's per-app settings. It also moves an install made under the old name GalaxyQuest over to the GalaxyFrame folders, once (see Building).
- `prepare-game.sh` extracts and converts your disc with [nodtool](https://github.com/encounter/nod), downloaded once into `~/.cache/galaxyframe` and checked against its sha256.
- `warm-shaders.sh` fills the shader caches headless.
- `galaxyframe.sh` is what the Steam shortcut runs. It applies the SteamVR settings, starts `tools/vrtiming.py` to log the compositor's timing, runs the game and collects Steam's performance log when it ends.

`tools/session-report.py` turns `petari_log.txt` and `vrtiming.log` into one table. `tools/privacy-check.sh` looks for personal data in the tree, the history or a tarball; the personal patterns come from a file outside the repository.

## Building

All on the Frame, with the tools SteamOS has: clang, lld, CMake 3.24 or newer, Ninja, the EGL/GLES and SDL2 headers, Python 3, and SteamVR installed.

```sh
./build_linux.sh                  # build-linux/galaxyframe, libgame.so, libgq_glsl.so, petari_headless (about 35 minutes the first time)
steam-frame/install.sh            # installs that build (stripped) and adds the shortcut
steam-frame/build.sh [1.0.0 [spirv.bin]]   # builds and packages steam-frame/release/galaxyframe-1.0.0-linux-arm64.tar.gz and SHA256SUMS
```

The release tarball holds `galaxyframe` (the launcher), `libgame.so` (the game, stripped), `libgq_glsl.so` (the Vulkan shader compiler, stripped; the game runs without it, on OpenGL ES), `petari_headless` (the headless runner for `warm-shaders.sh`), `galaxyframe.sh` (the launch wrapper), `vrtiming.py` and, if you gave one, `spirv.bin`. `steam-frame/release/` is ignored by git. To seed the shader cache, pass the `spirv.bin` of a played session (or set `GALAXYFRAME_SPIRV_SEED=FILE`); `install.sh` then places it into the data folder if there is no `spirv.bin` there. With `GALAXYFRAME_NO_BUILD=1` `build.sh` packages the existing `build-linux/` as it is.

The first build fetches the Khronos OpenXR headers (`tools/fetch_openxr.sh`; only the headers are used). `compat.h` and `ppc_intrinsics.h` are included in every file, so a change to them rebuilds everything. `tools/build-ccache.sh` builds a ccache that makes later builds much faster.

**Trial install.** `install.sh` can install into a scratch folder without touching Steam or SteamVR:

```sh
GALAXYFRAME_APP=/tmp/gf/app GALAXYFRAME_HOME=/tmp/gf/home GALAXYFRAME_SKIP_STEAM=1 steam-frame/install.sh TARBALL
```

Run `tools/privacy-check.sh TARBALL` on a release before publishing it.

**Moving over from the old name.** Before it was called GalaxyFrame the port lived in `~/Games/GalaxyQuest/app` and `~/.local/share/GalaxyQuest`. `install.sh` handles that once: if the old data folder exists and the new one does not, it moves the data folder; if the old program folder exists and the new one does not, it moves that; it removes the old `galaxyquest` and `galaxyquest.sh` from the new program folder; and it points the existing Steam shortcut's program and start folder at the new wrapper. It refuses to run while the game is running from the old folder. When `GALAXYFRAME_APP` or `GALAXYFRAME_HOME` redirects an install, the old folders are not touched unless `GALAXYFRAME_OLD_APP` or `GALAXYFRAME_OLD_HOME` names them, so a trial install cannot move a real one. The old variables `GALAXYQUEST_HOME`, `GALAXYQUEST_APP` and `GALAXYQUEST_SKIP_STEAM` still work for now.

**Headless test**, without the headset. It boots the game and saves a snapshot every 5 seconds:

```sh
mkdir -p /tmp/gf/nand && EGL_PLATFORM=surfaceless SDL_AUDIODRIVER=dummy PETARI_GFX=vk PETARI_SHOT_MS=5000 \
  build-linux/petari_headless ~/.local/share/GalaxyFrame/game /tmp/gf/nand 45     # -> /tmp/gf/shots/*.png
```

`steam-frame/dev/fix_include_case.py <repo> --apply` rewrites `#include` lines to the files' real spelling after merging upstream.

## Runtime facts, for anyone digging further

- SteamVR's Linux Arm64 OpenXR runtime advertises `XR_KHR_opengl_es_enable`, `XR_MNDX_egl_enable`, `XR_FB_display_refresh_rate`, `XR_META_recommended_layer_resolution`, `XR_META_performance_metrics`, `XR_FB_space_warp` and `XR_VALVE_frame_controller_interaction`. It has no `XR_FB_passthrough`.
- `XR_FB_display_refresh_rate` lists only the current rate. Requesting another returns `XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB`.
- Of `XR_META_performance_metrics`, only the app GPU counter answers. `XR_FB_space_warp` offers no OpenGL ES formats.
- A session may hold 16 swapchains. The game makes 9.
- SteamVR accepts a Vulkan swapchain with mip levels but allocates the images' memory for level 0 only (a 2048x1152 swapchain of 12 levels got 9.47 MB, where one level is 9.44 MB). Writing the other levels corrupts other images, so the Vulkan side makes single-level swapchains.
- `ptrace` attach is not permitted for user processes here. Debug with `gdb --args` from the start, or read `/proc/<pid>/task/*/wchan`.

## Pacing probes (xrprobe)

`tools/xrprobe/` is a small standalone OpenXR + Vulkan program (its own CMake project, GPLv3, not part of the game build and not in the release; see [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)) that answers the questions the Vulkan renderer depends on. `xrprobe --sequence` runs the first five cases in one OpenXR session and `xrprobe --sequence-sw` the two SpaceWarp cases, with the case number and label drawn in each eye; a fresh process per case spends 10 to 20 s waiting before SteamVR shows it. `tools/xrprobe/run-probes.sh` runs the cases one after another and collects the logs. The headset runs went through a Steam shortcut at 120 Hz with the game's per-app settings, with the panels recorded and `vrtiming.py` logging the compositor. Runtime: SteamVR/OpenXR 2.18.2 on the Steam Frame.

| Case | What it tests | Result |
|---|---|---|
| `--pace` | 14 ms of GPU work per frame, images released right after `vkQueueSubmit`. Does SteamVR show unfinished Vulkan images, as it does for GLES? | Yes. The GPU was busy at every release and the recording shows frames stitched from several frames (banded backgrounds). |
| `--pace=fence` | The same with `vkWaitForFences` before the release (control) | No unfinished frames, `xrEndFrame` 0.3 ms, vsync-to-photon latency no worse. The game releases only finished images. |
| `--synth` | `XR_EXT_frame_synthesis`: motion vectors (R16G16B16A16_SFLOAT) and depth at 30 fps with `REQUEST_RELAXED_FRAME_INTERVAL` | Accepted (swapchains and `xrEndFrame` without errors) but not used by the compositor: the same 30 fps steps as without the data. |
| `--synth --no-mv` | The same frame rate without synthesis data | Control for `--synth`; looked the same. |
| `--depth-only` | `XR_KHR_composition_layer_depth` at the display rate | Accepted (D32_SFLOAT), clean at 120 fps. |
| `--spacewarp` | `XR_FB_space_warp`: motion vectors and depth (D32_SFLOAT) on new content every second display slot (60 fps at 120 Hz), the last content submitted again between | Accepted (0 `xrEndFrame` failures, 1779 of 1791 steps of exactly two refreshes) but not used by the compositor: `vrtiming`'s motion column stayed 0 and I saw no difference to the control. The game keeps SpaceWarp off. |
| `--spacewarp --no-mv` | The same 60 fps content cadence without SpaceWarp data (control) | 1797 of 1801 steps of two refreshes; looked the same as `--spacewarp`. |

A short run with the headset asleep (no frames shown) showed that SteamVR/OpenXR 2.18.2 accepts Vulkan 1.0 to 1.2, recommends 1728x1728 per eye, gives 3 images per swapchain and listed only 90 Hz. It offers R8G8B8A8_SRGB, B8G8R8A8_SRGB, R32G32B32A32_SFLOAT, R32G32B32_SFLOAT, R16G16B16A16_SFLOAT, A2B10G10R10_UNORM, D32_SFLOAT, D16_UNORM, D24_UNORM_S8_UINT and D32_SFLOAT_S8_UINT to Vulkan apps.

## How this was made

I wanted Super Mario Galaxy on my Frame without Lepton, so I started from GalaxyQuest and had it built natively for SteamVR. I did not write the code by hand. The investigation, the port, the scripts and these notes were produced by Anthropic's Claude (Claude Opus 5.5 and Claude Fable 5.1, through Claude Code) working on my Steam Frame. I supplied the disc, reviewed what was written and ran each build in the headset.

The work went roughly like this. First, reading GalaxyQuest's platform layer to see how much was Android-specific. Little was: the launcher, the OpenXR session binding, the sound output and the storage paths. Then checking that SteamVR's native runtime offers OpenGL ES through EGL, and replacing those pieces. Then rounds of full builds against glibc and libstdc++, for code that compiles differently from Android's bionic and libc++. Two boot crashes were traced with gdb: a `(null)` path from glibc's `sprintf`, and a disc-read queue freed on the wrong heap. After that came the pacing work, where the first version drew through a translation layer and one eye jittered, then a Vulkan renderer of its own and pacing written for how SteamVR behaves, and finally the comfort and control work that makes the diorama pleasant for a longer session. Each step was tested in the headset, and everything under Status was checked by running the game there.

## Licence

GalaxyQuest is released into the public domain under [The Unlicense](../LICENSE), and so are GalaxyFrame's changes. The decompilation in `decomp/` is the Petari team's work under CC0 1.0 (`decomp/LICENSE-Petari.txt`). Third-party parts keep their own licences: see [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). The probe in `tools/xrprobe` is GPLv3. No Nintendo game data is included.
