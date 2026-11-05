# AGENTS.md

This file holds the technical detail moved out of the README, for coding agents and developers.

Further reading:

- [docs/TECHNICAL_NOTES.md](docs/TECHNICAL_NOTES.md): how the Steam Frame port works, [building](docs/TECHNICAL_NOTES.md#building), runtime facts about SteamVR on the Frame, and the xrprobe pacing probes.
- [docs/TECHNICAL.md](docs/TECHNICAL.md): GalaxyQuest's own technical document. It still describes the shared code and lists the environment switches for testing.
- [docs/CONTROLS.md](docs/CONTROLS.md): every VR setting and every line of `petari_vr.ini`.
- [docs/USER_GUIDE.md](docs/USER_GUIDE.md): the user-facing detail (install details, full controls, settings, performance numbers, files, troubleshooting).
- [docs/PRIVACY_AUDIT.md](docs/PRIVACY_AUDIT.md): what was checked for personal data before publishing.
- [README.upstream.md](README.upstream.md): GalaxyQuest's README. Leave it untouched.

## What this adds to GalaxyQuest

GalaxyQuest draws with OpenGL ES and runs on the Quest's own runtime. On the Frame that would mean a translation layer and pacing that SteamVR does not like. GalaxyFrame changes the platform side and leaves the game itself as it is:

- A Linux Arm64 build: a launcher, an OpenXR session in SteamVR, SDL2 sound and plain folders for the game files, saves and settings.
- A Vulkan renderer beside the OpenGL ES one. It replays the game's recorded frames with Vulkan directly, with the shaders compiled to SPIR-V at run time and cached.
- Frame pacing written for SteamVR. Each refresh locates the head once, renders both eyes from that pose and hands them over only after the GPU has finished them. If the GPU cannot keep up, it drops to new eyes every second refresh and returns on its own.
- A control layout for the Frame controllers, smooth turning with a speed slider, Star Bits shot faster with a slider, shake to spin set for the Frame controllers, and haptics on both controllers.
- The game's in-engine cutscenes shown in the diorama, following their camera; the videos stay on the flat screen. A Cutscenes tab chooses this by kind.
- A comfort vignette measured in viewing angle, with its strength set for slow and fast movement.
- The VR settings panel reorganised into six tabs, with the switches that do nothing on the Frame hidden.
- Area loads with less of a hitch: textures are decoded off the game thread, the frame memory is mapped ahead, and the game's threads run at a raised priority.
- SteamVR per-app settings for the game (120 Hz, no throttling, no motion smoothing), set at install and again at every launch.
- Scripts to install the game, add it to Steam, convert your disc and compile the shaders ahead of play.

## Status

Version 1.1.0 is the current release. It has been played on one Steam Frame (SteamOS 0.4.5, SteamVR 2.18) with the US disc.

Seen working in the headset: the diorama and the giant screen, 120 Hz with new eyes every refresh at resolution 0.9 and 1.0, the controls and rumble, smooth turning, Star Bits at 3x, cutscenes in the diorama, the VR settings panel, the comfort vignette, the 2D screens for menus and videos, saving and loading.

The known issues are in [docs/USER_GUIDE.md](docs/USER_GUIDE.md#known-issues). The full status, including what was only tested headless, is in [docs/TECHNICAL_NOTES.md](docs/TECHNICAL_NOTES.md#status).

## Performance internals

The measured numbers are in [docs/USER_GUIDE.md](docs/USER_GUIDE.md#performance). Where the time went, and what was done about it:

- **The renderer.** The game records each frame as a list of GX commands, and the renderer replays it. Replaying with Vulkan instead of OpenGL ES over a translation layer made each eye about a third cheaper. On top of that, the per-pixel drawing state is sent once per run of draws instead of being read per pixel (26 % off each eye), draws whose output nothing reads are skipped, the game's channel-only copies are plain blits, the HUD is drawn once per game frame, and the GPU renders directly to memory instead of in tiles (another 7 %). Both eyes at 1.0 went from 8.2 ms to 6.1 ms in the headset simulation.
- **Pacing.** SteamVR shows an image the moment the game releases it, finished or not. So each refresh records both eyes into one command buffer, submits it, and the previous refresh's pair is released only once its GPU work is complete. Eight command buffers are kept in flight so the CPU never waits while recording. A refresh counts as late only if its pair really missed its display slot.
- **Area loads.** Textures are decoded on two helper threads instead of the game thread, the frame buffers are allocated and mapped ahead of use, and the game's threads get a raised priority through RealtimeKit. The slowest frame at a load dropped by 43 %.
- **Shaders.** The game writes its shaders as GLSL at run time. They are compiled to SPIR-V through glslang in a separate library and kept in a cache, and every pipeline in the cache is created in the background at launch.

What did not help, so you know: SteamVR on the Frame accepts both frame synthesis extensions (`XR_EXT_frame_synthesis` and `XR_FB_space_warp`) but does not use them, which a standalone probe in `tools/xrprobe` showed in the headset.

## Build from source

See the [technical notes](docs/TECHNICAL_NOTES.md#building). The build runs on the Frame itself with the tools SteamOS has, in about 35 minutes the first time. `tools/privacy-check.sh` checks the tree, the history and a release tarball for personal data before publishing.

## Credits in detail

- [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) by bigmak94: the VR port of Super Mario Galaxy that this is built on, including the diorama, the giant screen, the VR settings panel, the GX recorder and the OpenGL ES renderer that the Vulkan renderer mirrors. Released under the Unlicense.
- [Petari](https://github.com/SMGCommunity/Petari) by the SMGCommunity: the decompilation of Super Mario Galaxy that makes a native port possible at all. Released under CC0.
- [nod](https://github.com/encounter/nod) by encounter, which extracts the disc.
- [OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) from Khronos, [glslang](https://github.com/KhronosGroup/glslang) for the shader compiler, AMD FidelityFX CAS for the sharpening, and SDL2 for the sound.

[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) has the full list with licences.

Super Mario Galaxy, Mario and their logos are trademarks of Nintendo. This is an unofficial fan project. It is not affiliated with or endorsed by Nintendo, Valve, the Petari team or the GalaxyQuest author.

## Licence in detail

GalaxyQuest and GalaxyFrame's changes are in the public domain under [The Unlicense](LICENSE). The decompilation in `decomp/` is the Petari team's work under CC0 1.0 (`decomp/LICENSE-Petari.txt`). Third-party parts keep their own licences: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The probe in `tools/xrprobe` is GPLv3 and is not part of the game. No Nintendo game data is included.

## How this was made

I wanted Super Mario Galaxy on my Frame without Lepton, so I started from GalaxyQuest and built it natively for SteamVR. The first version drew through a translation layer and its pacing fought SteamVR; one eye jittered and the GPU ran out in heavy areas. The second step was a Vulkan renderer of its own, pacing written for how SteamVR actually behaves, and then the comfort and control work that makes the diorama pleasant for a longer session. It started as a project for my own headset. It ended up good enough to share.

I started this project and used AI (Claude Opus 5.5 and Claude Fable 5.1, through Claude Code) to speed up development. I write, review and fix code by hand all the way through, keep the AI's work checked and reined in, and aim to keep the port as lightweight as possible. I test it on my own Steam Frame. GalaxyQuest is also AI-written, as its README says. It is a starting point for anyone who wants Super Mario Galaxy, or another GalaxyQuest-style port, on the Frame.

Developer notes, the probe results and what was learned about SteamVR on the Frame are in the [technical notes](docs/TECHNICAL_NOTES.md).
