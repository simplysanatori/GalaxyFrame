# GalaxyQuest on the Steam Frame (native Linux arm64)

This branch builds GalaxyQuest as a native Linux program for the Steam
Frame, running on the Frame's own SteamVR runtime instead of the Quest APK
through Lepton. The game, the VR presentation and every VR panel (settings,
HUD, giant screen, setup screen) are the upstream code unchanged; only the
Android glue has a Linux counterpart.

## What differs from the Quest build

| | Quest APK | Steam Frame build |
|---|---|---|
| Launcher | NativeActivity, `android_dlopen_ext` at 0x98000000 | `platform/linux/launcher.c`: reserves the game window, `dlopen` with libgame.so's image base at 0x98000000 (checked) |
| GL context | EGL on Android | EGL, Mesa's surfaceless platform (zink on Turnip) |
| OpenXR | `XR_KHR_android_create_instance` | `XR_MNDX_egl_enable` + `XR_KHR_opengl_es_enable`, SteamVR's own loader |
| Controllers | Touch profiles | plus `/interaction_profiles/valve/frame_controller` |
| Sound | AAudio | SDL2 (PipeWire/Pulse) |
| Storage | app folders | `~/.local/share/GalaxyQuest` (`game/`, `nand/`, `petari_vr.ini`, `petari_log.txt`, `shaders.bin`) |

### Steam Frame controller layout

| Steam Frame | In the game |
|---|---|
| Left stick | Move |
| A | Jump (hold: float, skip) |
| B | B (shoot star bits, back out of menus) |
| Y or right bumper (or a flick) | Spin |
| Right trigger | B too (shoot star bits) |
| Left trigger | Crouch |
| Left grip or left bumper | Camera behind Mario |
| Menu, X or View | Pause menu (VR settings beside it) |
| D-pad | Wii D-pad (left/right turn the diorama) |
| Right stick | Turn the view, up for first person |

## Building on the Frame

Needs clang, lld, CMake, Ninja, the EGL/GLES and SDL2 headers (all present on
SteamOS for the Frame) and SteamVR.

```
./build_linux.sh                         # build-linux/galaxyquest + libgame.so
```

The OpenXR headers come from `tools/fetch_openxr.sh` (the Khronos Android
loader package; only its headers are used here).

## Game files

Extract your own disc (for example `nodtool extract game.rvz extracted` with
[nod](https://github.com/encounter/nod)), then convert it:

```
python3 tools/cook/cook.py extracted ~/.local/share/GalaxyQuest/game --with-movies
```

## Running

`galaxyquest [game folder]`, or from a Steam shortcut. Set the shortcut's
SteamVR refresh rate to 120 Hz (per-app setting `preferredRefreshRate`): at
120 Hz each 60 fps game frame is shown for exactly two refreshes.

Headless test (no headset): `EGL_PLATFORM=surfaceless SDL_AUDIODRIVER=dummy
build-linux/petari_headless <game> <scratch>/nand 45` writes snapshots to
`<scratch>/shots`.

## Linux-specific fixes

- Includes spelled as the files are on disk (upstream builds on Windows).
- `compat.h` takes in `<cstdio>`/`<cwchar>` before its renames (libstdc++
  `#undef`s them, which brought back glibc's `sprintf` with its `(null)`).
- `ppc_intrinsics.h` renames `__fabs`/`__fabsf` (glibc declares its own).
- `PortHostAllocator` for queues filled on game threads and emptied on host
  threads (DVD requests, DSP mail, GL deletes).
- `_exit` after the session ends (static destructors hung with the game's
  threads still running).

Not available here: SpaceWarp (SteamVR offers no motion vector swapchain
formats to GLES apps) and passthrough (no `XR_FB_passthrough`).
