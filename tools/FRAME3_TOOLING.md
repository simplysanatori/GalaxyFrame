# Tooling for the frame.3 work (Steam Frame, native arm64)

| Tool | Use |
|---|---|
| `tools/build-ccache.sh [version]` | Builds ccache from the release tarball into `~/.local/bin/ccache` and writes `~/.cache/ccache-gq/ccache.conf` (20 GB, `base_dir` = `~/Games/GalaxyQuest` so the `src` checkout and every `wt/*` worktree share hits, `hash_dir = false`). `gqbuild.sh` and `build_linux.sh` use it when it is on PATH. |
| `build_linux.sh` | Same configure and ninja as before, now with ccache, the machine build lock `~/Games/GalaxyQuest/.build.lock` (one build at a time) and `-j5` (`JOBS=n`). `GQ_NO_LOCK=1` skips the lock when already inside `gqbuild.sh`. |
| `tools/build-vvl.sh [tag]` | Builds the Khronos validation layer (Vulkan-ValidationLayers, default `vulkan-sdk-1.4.309.0`, dependencies at the commits of its `known_good.json`) and installs it in `~/.local/share/vulkan/explicit_layer.d`. Takes the build lock, `nice`, `-j4`. Use: `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation <app>`. |
| `tools/golden.sh`, `tools/golden_runner.c`, `tools/imgdiff.py` | Frame-locked headless golden pictures and the PNG comparison (standard library only). `golden.sh` documents itself in its header. |
| `tools/vrtiming.py` | SteamVR compositor frame timing, a line a second. Started by `steam-frame/galaxyquest.sh` into `~/.local/share/GalaxyQuest/vrtiming.log` (`GQ_VRTIMING=0` turns it off). |
| `tools/session-report.py` | One table from `petari_log.txt` and `vrtiming.log` (current and previous session, or named logs). |

`steam-frame/build.sh` and `install.sh` must ship `vrtiming.py` next to `galaxyquest.sh` for an installed
copy to log the compositor (the wrapper also looks in `../src/tools/` and `../tools/`).
