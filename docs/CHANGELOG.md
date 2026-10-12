# Changelog

## 1.0.0 (October 2026)

The first release of GalaxyFrame, Super Mario Galaxy in VR running natively on the Valve Steam Frame. It is a port of bigmak94's GalaxyQuest, built on the Petari decompilation.

### The game on the Frame

- A Linux Arm64 build that runs in the Frame's SteamVR. No Lepton, APK, adb or PC.
- The diorama in front of you, or a giant virtual screen, as in GalaxyQuest.
- Saves, settings and the converted disc in plain folders under your home.
- Sound through SDL2, and a clean exit when Steam stops the game.

### Rendering

- A native Vulkan renderer that replays the game's recorded frames.
- Both eyes rendered on every refresh of the 120 Hz display, from the head pose of that refresh, at SteamVR's recommended resolution. Each pair is handed to SteamVR only once the GPU has finished it.
- When the GPU falls behind in a heavy scene, new eyes every second refresh until it is lighter again, then back.
- The shaders compiled to SPIR-V at run time and cached; a warmed cache has every pipeline ready at launch.
- GPU work cut by about a quarter per eye: the drawing state is sent once per run of draws, draws nothing reads are skipped, channel-only copies are blits, the HUD is drawn once per game frame, and the GPU renders directly to memory.
- Area loads with less of a hitch: textures decoded off the game thread, frame memory mapped ahead, and the game's threads at a raised priority.

### Controls

- The Steam Frame controllers bound through SteamVR's own profile, with rumble on both controllers.
- Smooth turning of the diorama with the right stick, with a turn speed slider; 45 degree steps behind a blink as the alternative.

### Comfort

- A comfort vignette: a clear circle around where you look, closing in as the view moves, with its strength set for slow and for fast movement.

### The settings panel

- Six tabs: Diorama, Screen, Cutscenes, Controls, Comfort and Display. The switches that do nothing on the Frame are hidden.
- One resolution setting, shown in pixels: 1.00 is what SteamVR's own resolution setting gives the game.

### Install

- An installer that copies the program, adds the Steam library entry, sets SteamVR's per-app settings (120 Hz, no throttling, no motion smoothing) and sets them again at every launch.
- A script that converts your disc, and one that compiles the galaxies' shaders ahead of play.
- The Vulkan shader cache shipped in the tarball, so the first session does not compile.
