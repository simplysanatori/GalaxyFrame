# GalaxyFrame

Super Mario Galaxy in VR on the Valve Steam Frame.
A port of [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) by bigmak94.

## Features

- Diorama in front of you, or a giant virtual screen
- 120 Hz, both eyes drawn on every refresh
- Smooth turning, faster Star Bits, flick to spin, rumble
- Cutscenes play in the diorama
- Comfort vignette for motion sickness

## How it works

The game runs on the Frame itself, in SteamVR, from files you convert from your own disc.
Nothing is streamed from a PC.

## Requirements

- A Steam Frame with SteamOS (Stable, Beta or Preview) and SteamVR, and about 4 GB free
- Your own Super Mario Galaxy disc as an ISO, RVZ or WBFS image (US disc RMGE01 tested)
- A terminal on the Frame: SSH, or Konsole (SteamVR dashboard, Launch a program, Desktop)

## Install

1. Get the repository:

   ```sh
   git clone https://github.com/simplysanatori/GalaxyFrame.git
   cd GalaxyFrame
   ```

2. Download `galaxyframe-1.1.2-linux-arm64.tar.gz` and `SHA256SUMS` from [Releases](https://github.com/simplysanatori/GalaxyFrame/releases). With Steam running, check and install:

   ```sh
   (cd ~/Downloads && sha256sum -c --ignore-missing SHA256SUMS)
   steam-frame/install.sh ~/Downloads/galaxyframe-1.1.2-linux-arm64.tar.gz
   ```

3. Convert your disc:

   ```sh
   steam-frame/prepare-game.sh /path/to/SuperMarioGalaxy.rvz
   ```

4. Launch **Super Mario Galaxy VR** from your Steam library.

5. Optional: after your first save, with the game closed, compile shaders so new areas hitch less (about 45 minutes):

   ```sh
   steam-frame/warm-shaders.sh
   ```

Other installers (you still need step 3):

- [Frametanium](https://github.com/Cogential/frametanium): add `simplysanatori/GalaxyFrame`.
- [FrameDrop](https://framedropvr.com): [Install with FrameDrop](https://framedropvr.com/install?manifest=https%3A%2F%2Fgithub.com%2Fsimplysanatori%2FGalaxyFrame%2Freleases%2Flatest%2Fdownload%2Fgalaxyframe.framedrop.json).

## First run

The game starts on a giant virtual screen. For the diorama:

1. Press **Menu** and aim at the VR settings panel.
2. On the **Screen** tab, turn off **Giant screen**.

## Controls

| Steam Frame | In the game |
|---|---|
| Left stick | Move |
| **A** | Jump. Hold to skip a cutscene |
| **B** or right trigger | Shoot Star Bits |
| **X**, **Y**, right bumper, or flick | Spin |
| Left trigger | Crouch, ground pound |
| **Menu** or **View** | Pause menu and VR settings |
| Right stick | Turn the diorama |
| Right controller aim | Collect Star Bits, point at menus |

## Update

Download the new tarball, then in your `GalaxyFrame` folder:

```sh
git pull
steam-frame/install.sh ~/Downloads/galaxyframe-<version>-linux-arm64.tar.gz
```

## Uninstall

Quit the game first. Add `--purge` to also delete game files, saves and settings.

```sh
steam-frame/uninstall.sh
```

## Troubleshooting

- **A setup screen asks for game files.** Run `steam-frame/prepare-game.sh` (step 3).
- **Motion ghosts.** In SteamVR, check 120 Hz and motion smoothing off.
- **Heavy scenes stutter.** Set **Resolution** to 0.8 on the **Display** tab.

## Reporting a problem

Open an [issue](https://github.com/simplysanatori/GalaxyFrame/issues) with your disc's region.
Check `petari_log.txt` and `launch.log` in `~/.local/share/GalaxyFrame`, then attach them.

## More

- [User guide](docs/USER_GUIDE.md): all controls, settings and fixes
- [AGENTS.md](AGENTS.md): for developers and AI agents
- [Building from source](docs/TECHNICAL_NOTES.md#building)
- [Changelog](docs/CHANGELOG.md)

## Support

Support upstream first: [sponsor GalaxyQuest](https://buy.stripe.com/6oU4gB5450PZ18h8aK1wY00) and send game fixes there.
For more Steam Frame ports: [buy me a coffee](https://ko-fi.com/sana748653).

## Credits

- [simplysanatori](https://github.com/simplysanatori): built GalaxyFrame
- [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) by bigmak94: the VR port
- [Petari](https://github.com/SMGCommunity/Petari) by the SMGCommunity: the decompilation
- [nod](https://github.com/encounter/nod) by encounter: disc extraction

Unofficial fan project, not affiliated with Nintendo, Valve, the Petari team or the GalaxyQuest author.

## Licence

Public domain under [The Unlicense](LICENSE). Third-party parts keep their own licences: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## How this was made

I started this project and used AI (Claude Opus 5.5 and Claude Fable 5.1, through Claude Code) to speed up development. I write, review and fix code by hand all the way through, keep the AI's work checked and reined in, and aim to keep the port as lightweight as possible. I test it on my own Steam Frame. Upstream GalaxyQuest is also AI-written.
