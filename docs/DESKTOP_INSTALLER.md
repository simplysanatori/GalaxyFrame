# Desktop installer

The desktop installer combines disc extraction, game conversion, APK
installation and copying in one window. Packaged builds include Python,
DolphinTool, the release's APK and Google's Android platform-tools. Users do
not need Python, Dolphin, a terminal, or a separate adb download. The installer
works offline and contains no Nintendo game data.

## For players

Download the
[Windows installer](https://github.com/bigmak94/GalaxyQuest/releases/latest/download/GalaxyQuest-Installer-windows-x86_64.exe)
from the [latest release](https://github.com/bigmak94/GalaxyQuest/releases/latest).
For macOS, Linux, or installation without the installer, use the
[manual instructions](../README.upstream.md#installing-manually).

1. Enable developer mode, connect the headset with a USB data cable, and allow
   USB debugging in the headset. See [Meta's setup guide](https://developers.meta.com/horizon/documentation/android-apps/enable-developer-mode/).
2. Open the installer for your computer. Click **Browse** and choose your
   own Super Mario Galaxy disc image (`.iso`, `.rvz` or `.wbfs`; `.gcz`, `.wia`
   and `.ciso` are also accepted). If you already extracted the disc, use
   **Use an already extracted folder** and choose that folder or its `DATA` folder.
3. Click **Install**. It checks storage, extracts the game with DolphinTool,
   converts the files, installs the app, and copies the game. Open
   **Library > Unknown Sources > GalaxyQuest** when
   it finishes.

Movies are included by default. Leave about 16 GB free on the computer for a
disc image (10 GB for an already extracted folder) and 7 GB on the headset.
The installer creates and removes its working folder automatically. It first
tries the GalaxyQuest cache, then the disk holding the game, then system
temporary storage, choosing a writable location with enough free space.
There is no working-folder question. Temporary extracted and converted files
are removed when installation ends; your original copy is left intact.
Each run saves a log
under the computer's GalaxyQuest cache folder, with its path shown in the
window. If installation stops, fix the reported issue and click Install again.

If several devices are connected, select the headset in the list. An Android
phone is rejected before conversion. Updates use `install -r` and preserve
existing app data; the installer does not uninstall the app.

Windows uses an `.exe`, macOS a zipped `.app` (separate Intel and Apple Silicon
downloads), and Linux a `.tar.gz` containing an executable. Extract archives
before opening the app. The initial builds have no publisher signing. macOS signing and
notarization still need to be added before broad distribution. Linux may need
USB permissions configured for Android devices; the app does not change system
configuration or install drivers.

## Building releases

Build on each target operating system. PyInstaller does not cross-compile.
Use an isolated Python environment with Tk support:

```
python -m pip install -r tools/installer-requirements.txt
python -m unittest discover -s tools/tests -p test_installer.py -v
python tools/build_installer.py --apk out/GalaxyQuest.apk --dolphin-tool /path/to/DolphinTool --dolphin-licenses /path/to/dolphin --dolphin-source-url https://github.com/dolphin-emu/dolphin/tree/2609
```

Supply the exact APK you want users to install. The builder downloads the
platform-tools for its host from Google's official download server, including
their notices. Alternatively, add `--platform-tools /path/to/platform-tools`
to reuse an existing download. Supply a native DolphinTool executable and
its distribution/source directory containing `COPYING` and `Licenses` or
`LICENSES`. Windows names the tool `DolphinTool.exe`; Linux/macOS use
`dolphin-tool`. PyInstaller collects the tool's runtime libraries. The
source URL must identify the exact Dolphin version supplied. Keep Dolphin's
licenses and publish the matching source companion alongside the installers.
Build output is under `out/installers/`, with a SHA-256 file for each artifact.
No game files are packaged. If an older Windows installer is open, use
`--output-suffix updated` to write a separate executable.

The builder runs a packaged smoke test that checks converter imports, Tcl,
spawned conversion workers, bundled APK/adb paths, and the bundled DolphinTool
extraction command. For a real extraction/conversion
test without a headset:

```
GalaxyQuest-Installer --convert-only "/path/to/game.rvz" "/path/to/new/output"
```

Add `--without-movies` to skip movies. Conversion details go to
`/path/to/new/output-conversion.log`. An extracted folder can also be supplied.
Use a new output folder for each test.

The **Build desktop installers** GitHub Actions workflow can produce Windows,
Linux, Intel Mac and Apple Silicon Mac artifacts. Run it manually and supply
an existing release tag; it downloads that release's `GalaxyQuest.apk`, builds
DolphinTool from the pinned Dolphin 2609 source (including its submodules),
then builds installers and uploads them with the matching source companion.
It does not publish a release.
The macOS/Linux workflow builds still need to be run and checked, and a full
installation on a connected headset is required before releasing any platform.

Source mode remains available with `python tools/install.py`; put the APK in
`out/GalaxyQuest.apk` and provide adb on PATH or `platform-tools` beside the
repository root. For images, also provide DolphinTool on PATH or in `dolphin`
beside the repository root. The existing converter/push commands continue to work.
