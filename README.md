# GalaxyFrame

Super Mario Galaxy in VR, running natively on the Valve Steam Frame.
It is a port of [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) by bigmak94, built on the [Petari](https://github.com/SMGCommunity/Petari) decompilation.

## Features

- A diorama in front of you, or a giant virtual screen
- 120 Hz, both eyes drawn on every refresh
- Steam Frame controllers with rumble
- Smooth turning, faster Star Bits, flick to spin
- Cutscenes play in the diorama
- Comfort vignette for motion sickness

## How it works

The game runs on the Frame itself, inside SteamVR.
You convert your own disc once, and the game reads those files.
Nothing is streamed from a PC.

## Requirements

- A Steam Frame with SteamOS (Stable, Beta or Preview) and SteamVR, and about 4 GB free
- Your own Super Mario Galaxy disc as an ISO, RVZ or WBFS image (US disc RMGE01 tested)
- A terminal on the Frame: SSH, or Konsole (SteamVR dashboard, Launch a program, Desktop)
- Steam running while you install

## Install

1. Get the repository:

   ```sh
   git clone https://github.com/simplysanatori/GalaxyFrame.git
   cd GalaxyFrame
   ```

2. Download `galaxyframe-1.1.0-linux-arm64.tar.gz` and `SHA256SUMS` from [Releases](https://github.com/simplysanatori/GalaxyFrame/releases). Check and install:

   ```sh
   sha256sum -c --ignore-missing SHA256SUMS
   steam-frame/install.sh ~/Downloads/galaxyframe-1.1.0-linux-arm64.tar.gz
   ```

3. Convert your disc once (3.3 GB):

   ```sh
   steam-frame/prepare-game.sh /path/to/SuperMarioGalaxy.rvz
   ```

4. Launch **Super Mario Galaxy VR** from your Steam library.

5. Optional, after your first save: compile shaders so new areas hitch less (about 45 minutes, game closed):

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

All controls: [user guide](docs/USER_GUIDE.md#controls).

## Update

Download the new tarball and install it:

```sh
steam-frame/install.sh ~/Downloads/galaxyframe-<version>-linux-arm64.tar.gz
```

## Uninstall

Quit the game first. Add `--purge` to also delete game files, saves and settings.

```sh
steam-frame/uninstall.sh
```

## Troubleshooting

- **Steam's debugging port is not reachable.** Start Steam and run `install.sh` again.
- **A setup screen asks for game files.** Run `steam-frame/prepare-game.sh` (step 3).
- **Motion ghosts.** In SteamVR, check 120 Hz and motion smoothing off.
- **Heavy scenes stutter.** Set **Resolution** to 0.8 on the **Display** tab.
- **New areas hitch.** Run `steam-frame/warm-shaders.sh` once after your first save.

More fixes: [user guide](docs/USER_GUIDE.md#troubleshooting).

## Reporting a problem

Attach `petari_log.txt` and `launch.log` from `~/.local/share/GalaxyFrame` to an [issue](https://github.com/simplysanatori/GalaxyFrame/issues), with your disc's region.
They hold no account names or serial numbers. Check them before sharing.

## More

- [User guide](docs/USER_GUIDE.md)
- [AGENTS.md](AGENTS.md): for developers and AI agents
- [Building from source](docs/TECHNICAL_NOTES.md#building)
- [Changelog](docs/CHANGELOG.md)

## Support

Support upstream first: star [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) and send game fixes there.
For more Steam Frame ports, you can [buy me a coffee](https://ko-fi.com/sana748653).

## Credits

- Built by [simplysanatori](https://github.com/simplysanatori)
- [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) by bigmak94: the VR port this is built on
- [Petari](https://github.com/SMGCommunity/Petari) by the SMGCommunity: the decompilation
- [nod](https://github.com/encounter/nod) by encounter: disc extraction

Full list: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Unofficial fan project, not affiliated with Nintendo, Valve, the Petari team or the GalaxyQuest author.

## Licence

Public domain under [The Unlicense](LICENSE). Third-party parts keep their own licences.

## How this was made

I started this project and used AI (Claude Opus 5.5 and Claude Fable 5.1, through Claude Code) to speed up development. I write, review and fix code by hand all the way through, keep the AI's work checked and reined in, and aim to keep the port as lightweight as possible. I test it on my own Steam Frame. GalaxyQuest, the upstream, is also AI-written.
