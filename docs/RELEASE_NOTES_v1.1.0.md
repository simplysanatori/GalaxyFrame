# GalaxyFrame 1.1.0

Shake to spin is here. Flick either controller like a Wii Remote and Mario spins. GalaxyFrame is Super Mario Galaxy in VR, running natively on the Valve Steam Frame, a port of bigmak94's GalaxyQuest built on the Petari decompilation. You need your own disc; no game data is included.

### Controls

- Flick either controller to spin. The left controller shakes the Nunchuk. X, Y and the right bumper still spin as before.
- *Shake to spin* and *Shake strength* on the Controls tab. Shake to spin is on by default. Shake strength goes from 20 % to 160 %, 100 % by default; lower needs a gentler flick.
- The default is set for the Frame controllers: a flick of 1.5 m/s at 100 %, where GalaxyQuest's Quest default is 2.5 m/s.
- Shakes are ignored while the pause menu or the settings panel is open.
- The strength setting comes from GalaxyQuest, where bigmak94 added it for the Quest.

### The game

- Launch Star flights, pipes and arrivals are shown on the flat screen. The in-engine cutscenes on the Cutscenes tab still play in the diorama.
- Leaving the game used to upset SteamVR's compositor. Now quitting waits for the GPU to finish, and the game closes cleanly.

### From GalaxyQuest

- Movies left out when you converted the disc are skipped, with no black screen. `prepare-game.sh` includes the movies by default, so this only matters if you used `--no-movies`.
- Build fixes for file systems that tell upper and lower case apart.

## Download

- `galaxyframe-1.1.0-linux-arm64.tar.gz`
- `galaxyframe-1.1.0-framedrop-linux-arm64.zip`
- `galaxyframe.framedrop.json`
- `SHA256SUMS`

## Update

Update the way you installed it. Your game files, saves and settings stay where they are.

- **Tarball:** run `steam-frame/install.sh` from the repository with the new tarball as its argument. It updates the Super Mario Galaxy VR entry in your library instead of adding another.
- **Frametanium:** it picks up the new version and runs the release's `install.sh`.
- **FrameDrop:** install the new zip. Its `run.sh` installs the release on the next start, without adding a second library entry.

New here? The [README](https://github.com/simplysanatori/GalaxyFrame/blob/steam-frame/README.md) has the full steps, the controls and the settings. You make the game files from your own disc with `steam-frame/prepare-game.sh`, once.

## Known

- When Mario runs in the diorama and your eyes follow him, he can look slightly doubled. The view follows him on every refresh at 120 Hz, but the game moves him 60 times a second.
- Played on one Steam Frame with the US disc.

## Reporting problems

Open an issue with what you did, what you expected and what happened, your disc's region, and `petari_log.txt` and `launch.log` from `~/.local/share/GalaxyFrame`.
