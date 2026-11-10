# Changelog

## 1.1.1 (November 2026)

- GalaxyFrame now runs on SteamOS Stable, Beta and Preview. 1.1.0 needed the Beta's newer glibc; this release is built against glibc 2.39, and the OpenXR loader it needs is packed with the game instead of being taken from SteamVR.

## 1.1.0 (November 2026)

### Controls

- Shake to spin. Flick either controller like a Wii Remote and Mario spins. The left controller shakes the Nunchuk. X, Y and the right bumper still spin as before.
- Two new settings on the Controls tab: *Shake to spin*, on by default, and *Shake strength*, from 20 % to 160 %, 100 % by default. Lower needs a gentler flick.
- The default is set for the Frame controllers. At 100 % a flick of 1.5 m/s spins. GalaxyQuest's Quest default is 2.5 m/s.
- Shakes are ignored while the pause menu or the settings panel is open.
- The strength setting comes from GalaxyQuest, where bigmak94 added it for the Quest.

### The game

- Launch Star flights, pipes and arrivals are shown on the flat screen. The in-engine cutscenes on the Cutscenes tab still play in the diorama.
- Leaving the game used to upset SteamVR's compositor. Now quitting waits for the GPU to finish, and the game closes cleanly.

### From GalaxyQuest

- Movies left out when you converted the disc are skipped, with no black screen. `prepare-game.sh` includes the movies by default, so this only matters if you used `--no-movies`.
- Build fixes for file systems that tell upper and lower case apart.

## 1.0.1 (November 2026)

- You can now install and update GalaxyFrame with Frametanium. The release carries its own `install.sh`, which installs the unpacked release in place and updates the Super Mario Galaxy VR entry in your library instead of adding another.
- The release also carries `steam-cef.py` and a new `uninstall.sh`.
- Nothing in the game changed: the program and the shader cache seed are the 1.0.0 ones.
- The release also has a `galaxyframe-<version>-framedrop-linux-arm64.zip` and a manifest for FrameDrop. It adds its own library entry, and its `run.sh` installs the release without adding a second one.

## 1.0.0 (October 2026)

The first release of GalaxyFrame, Super Mario Galaxy in VR running natively on the Valve Steam Frame. It is a port of bigmak94's GalaxyQuest, built on the Petari decompilation.

### The game on the Frame

- A Linux Arm64 build that runs in the Frame's SteamVR. No Lepton, APK, adb or PC.
- The diorama in front of you, or a giant virtual screen, as in GalaxyQuest.
- Saves, settings and the converted disc in plain folders under your home.
- Sound through SDL2, and a clean exit when Steam stops the game.

### Rendering

- A native Vulkan renderer that replays the game's recorded frames.
- Both eyes rendered on every refresh of the 120 Hz display, from the head pose of that refresh, at 0.9 of SteamVR's recommended resolution. Each pair is handed to SteamVR only once the GPU has finished it.
- When the GPU falls behind in a heavy scene, new eyes every second refresh until it is lighter again, then back.
- The shaders compiled to SPIR-V at run time and cached; a warmed cache has every pipeline ready at launch.
- GPU work cut by about a quarter per eye: the drawing state is sent once per run of draws, draws nothing reads are skipped, channel-only copies are blits, the HUD is drawn once per game frame, and the GPU renders directly to memory.
- Area loads with less of a hitch: textures decoded off the game thread, frame memory mapped ahead, and the game's threads at a raised priority.

### Controls

- The Steam Frame controllers bound through SteamVR's own profile, with rumble on both controllers.
- Smooth turning of the diorama with the right stick, with a turn speed slider; 45 degree steps behind a blink as the alternative.
- Star Bits shot three times as fast, with a slider from 1x to 5x.

### Comfort

- A comfort vignette: a clear circle around where you look, closing in as the view moves, with its strength set for slow and for fast movement.
- The game's in-engine cutscenes in the diorama, following their camera, with a switch per kind on a Cutscenes tab. The videos stay on the flat screen.

### The settings panel

- Six tabs: Diorama, Screen, Cutscenes, Controls, Comfort and Display. The switches that do nothing on the Frame are hidden.
- One resolution setting, shown in pixels: 1.00 is what SteamVR's own resolution setting gives the game; 0.90 by default.

### Install

- An installer that copies the program, adds the Steam library entry, sets SteamVR's per-app settings (120 Hz, no throttling, no motion smoothing) and sets them again at every launch.
- A script that converts your disc, and one that compiles the galaxies' shaders ahead of play.
- The Vulkan shader cache shipped in the tarball, so the first session does not compile.
