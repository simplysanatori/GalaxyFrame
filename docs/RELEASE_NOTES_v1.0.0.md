# GalaxyFrame 1.0.0

Super Mario Galaxy in VR, running natively on the Valve Steam Frame. GalaxyFrame is a port of bigmak94's GalaxyQuest, built on the Petari decompilation, with its own Vulkan renderer and frame pacing for SteamVR. You need your own disc; no game data is included.

### The game on the Frame

- A Linux Arm64 build that runs in the Frame's SteamVR. No Lepton, APK, adb or PC.
- The diorama in front of you, or a giant virtual screen, as in GalaxyQuest.
- Saves, settings and the converted disc in plain folders under your home.
- Sound through SDL2, and a clean exit when Steam stops the game.

### Rendering

- A native Vulkan renderer that replays the game's recorded frames, with OpenGL ES kept as the fallback.
- Both eyes rendered on every refresh of the 120 Hz display, from the head pose of that refresh, at SteamVR's recommended resolution. Each pair is handed to SteamVR only once the GPU has finished it.
- When the GPU falls behind in a heavy scene, new eyes every second refresh until it is lighter again, then back.
- The shaders compiled to SPIR-V at run time and cached; a warmed cache has every pipeline ready at launch.
- GPU work cut by about a quarter per eye: the drawing state is sent once per run of draws, draws nothing reads are skipped, channel-only copies are blits, the HUD is drawn once per game frame, and the GPU renders directly to memory.
- Area loads with less of a hitch: textures decoded off the game thread, frame memory mapped ahead, and the game's threads at a raised priority.

### Controls

- The Steam Frame controllers bound through SteamVR's own profile, with rumble on both controllers.
- Smooth turning of the diorama with the right stick, with a turn speed slider; 45 degree steps behind a blink as the alternative.
- Tilting the diorama with the stick, up to see Mario from higher, with a tilt speed slider. A stick click levels it.
- The diorama's height and size on the left grip and D-pad while you play.
- First person on the D-pad. Star Bits shot twice as fast, with a slider.
- A left-handed option that points with the left controller and swaps the hands.

### Comfort

- A comfort vignette: a clear circle around where you look, closing in as the view moves, with its strength set for slow and for fast movement.
- Planet turns: how the world turns on small planets so that Mario's up stays up. Smooth, in steps behind a blink, or off.
- Flights between planets, arrivals and the game's in-engine cutscenes stay in the diorama and follow the game's own camera; afterwards the view keeps the angle the scene chose. The videos stay on the flat screen. A Cutscenes tab chooses this by kind.
- The death screen and the game's other fades darken the whole view.

### The settings panel

- Six tabs: Diorama, Screen, Cutscenes, Controls, Comfort and Display. The switches that do nothing on the Frame are hidden.
- The panel sits clear of the game's pause menu, and the laser, the pointer and the skip ring draw over the flat screen.
- One resolution setting, shown in pixels: 1.00 is what SteamVR's own resolution setting gives the game.

### Install

- An installer that copies the program, adds the Steam library entry, sets SteamVR's per-app settings (120 Hz, no throttling, no motion smoothing) and sets them again at every launch.
- A script that converts your disc, and one that compiles the galaxies' shaders ahead of play.
- The Vulkan shader cache shipped in the tarball, so the first session does not compile.

## Download

- `galaxyframe-1.0.0-linux-arm64.tar.gz`
- `SHA256SUMS`

## Install

Unpack the tarball on the Frame and run `steam-frame/install.sh` from the repository with the tarball as its argument. It installs the program, creates the Steam shortcut and keeps your game files, saves and settings on an update. The [README](https://github.com/simplysanatori/GalaxyFrame/blob/steam-frame/README.md) has the full steps, the controls and the settings.

## Known

- Mario and the world move at the game's 60 frames a second. On the 120 Hz display an object your eye follows can look doubled along its movement. Head movement is not affected.
- Eye-tracked foveated rendering is built in but off; it saves little on this GPU.
- Tested on one Steam Frame with the US disc. Other discs, long sessions and the OpenGL ES fallback were tested only headless.

## Reporting problems

Open an issue with what you did, what you expected and what happened, your disc's region, and `petari_log.txt` and `launch.log` from `~/.local/share/GalaxyFrame`.
