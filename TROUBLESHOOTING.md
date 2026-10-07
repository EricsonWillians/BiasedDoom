# BiasedDoom Troubleshooting

This guide covers the most common startup, IWAD, audio, mod, Python, and build
problems. Commands use `biaseddoom.exe` on Windows and `./build/biaseddoom` on
Linux/macOS; substitute the executable path for your installation.

## Start Here

1. Extract a release into a new empty directory. Do not overwrite an older
   installation.
2. Keep the executable, PK3 files, packaged DLLs, `soundfonts`, and `fm_banks`
   together. Native packages that include Python also need their `python`
   directory.
3. Do not copy `openal32.dll`, `sndfile.dll`, or codec DLLs from GZDoom or
   another source port into BiasedDoom. Official Windows packages already
   contain the supported audio runtime and codecs.
4. Confirm that the IWAD and every mod work in the exact release being
   reported. Include the version printed at startup in bug reports.

To capture general startup output:

```cmd
biaseddoom.exe -stdout +logfile biaseddoom.log
```

On Linux/macOS:

```bash
./build/biaseddoom -stdout +logfile biaseddoom.log
```

## The Linux AppImage Does Not Start

AppImages normally need the host FUSE 2 library:

- Ubuntu 22.04 and derivatives: `sudo apt install libfuse2`
- Ubuntu 24.04 and derivatives: `sudo apt install libfuse2t64`

If FUSE cannot be installed, extract and run the image in one step:

```bash
APPIMAGE_EXTRACT_AND_RUN=1 ./BiasedDoom-<version>-Linux-x86_64.AppImage
```

Fixed releases bundle the engine's SDL, display-protocol, codec,
OpenAL, GTK, C++, and CPython extension dependencies. If an older AppImage
reports `error while loading shared libraries`, upgrade to a fixed release
instead of copying random system libraries into the package.

## BiasedDoom Cannot Find Doom

Print every searched directory and every validated IWAD:

```text
biaseddoom -findiwads
```

Then launch a detected game by short name:

```text
biaseddoom -iwad doom2
```

An explicit path always takes precedence:

```text
biaseddoom -iwad C:\Games\Doom\DOOM2.WAD
./build/biaseddoom -iwad "$HOME/Games/Doom/DOOM2.WAD"
```

BiasedDoom reads modern and legacy Steam library metadata, including external
libraries and Linux's `~/.steam/debian-installation` layout. If an unusual
installation is not detected:

- set `DOOMWADDIR` for one directory or `DOOMWADPATH` for several;
- set `STEAM_DIR` or `STEAM_HOME` to a portable Steam root;
- add `Path=` or `RecursivePath=` entries under `[IWADSearch.Directories]` in
  the user configuration;
- check Flatpak/Snap permissions for libraries outside the sandbox.

See [IWAD discovery](docs/engine/iwad-discovery.md) for the full search order,
supported stores, and configuration syntax.

## No Sound On Windows

Run the self-contained diagnostic command:

```cmd
biaseddoom.exe -iwad C:\Games\Doom\DOOM2.WAD -stdout -audiodiagnostics -norun
```

Press a key when the diagnostic finishes. The report is written to:

```text
%LOCALAPPDATA%\biaseddoom\biaseddoom-audio.log
```

If AppData is unwritable, BiasedDoom tries `biaseddoom-audio.log` beside the
executable and prints the path it used. The report identifies the OpenAL build,
configured and active devices, available endpoints, ALC/AL extensions, decoder
availability, driver overrides, and exact initialization errors.

Inside the console, these commands provide live status:

```text
snd_status
snd_listdrivers
```

If the saved device disappeared after changing a monitor, GPU, USB headset, or
Windows default endpoint, use:

```text
snd_aldevice Default
snd_reset
```

BiasedDoom normally uses OpenAL Soft's WASAPI backend. To isolate a
WASAPI/driver problem for one Command Prompt session:

```cmd
set ALSOFT_DRIVERS=dsound
biaseddoom.exe -stdout -audiodiagnostics -norun
set ALSOFT_DRIVERS=
```

Attach both the normal and DirectSound logs if only the latter works. See the
[audio troubleshooting guide](docs/audio-troubleshooting.md) for endpoint
recovery and maintainer probes.

## A Mod's OGG Or Custom Sound Does Not Play

First verify the official package is unmodified and remove loose audio/codec
DLLs copied from other applications. Load the mod with `-stdout`, then use its
logical sound name in the console:

```text
snd_status
cachesound mod/soundname
playsound mod/soundname
```

When decoding fails, BiasedDoom reports both the logical sound name and the
WAD/PK3 resource path. Include those lines, the mod name/version, and the audio
diagnostic log in the report. OGG, FLAC, Opus, and MPEG decoding is linked into
official Windows packages and does not require a separately downloaded
`sndfile.dll`.

## A Mod Crashes Or Behaves Differently

Reduce the command line until the problem disappears:

1. Run only the IWAD.
2. Add the primary gameplay mod.
3. Add remaining mods one at a time in their original order.
4. Use a new temporary configuration to rule out archived CVars.

Example:

```text
biaseddoom -config clean-test.ini -iwad doom2 -file first.pk3 second.pk3 -stdout +logfile mod-test.log
```

Record the smallest load order that reproduces the issue. Do not report a
problem as an engine regression until it has been tested without launchers that
rewrite arguments or inject files.

## DECOHack Or MBF21 Weapon Sprites Are Invisible

First confirm the engine version at startup. Update to the current 4.15.18
build or newer before diagnosing a DECOHack/DSDHacked weapon mod: older builds
could reject a valid sparse `[SPRITES]` number through the legacy dense-sprite
growth guard. The usual DECOHack MBF21 range starts at sprite ID `8000`.

Run the smallest mod load with a fresh configuration and retain its startup
log:

```text
biaseddoom -config deh-test.ini -iwad doom2 -file weapon-mod.pk3 -stdout +logfile deh-test.log
```

The log should show the patch being added and installed. Search it for the
following messages:

- `Sprite number ... out of range.` means the patch supplied an invalid
  unsigned/overflowing ID, or exceeded the deliberate extended-sprite budget.
- `Frame ... Sprite ... is undefined` means the patch named a sprite which is
  not present after its own resources and dependencies load.
- No `Adding dehacked patch` line usually means the patch was not discovered
  in the expected load order. Check the mod's own installation instructions
  and any required base/resource PK3s.

Current builds accept DSDHacked (`Doom version = 2021`) sparse sprite IDs
through the signed 32-bit range and keep up to 262,144 distinct extended
`[SPRITES]` mappings per load. That restores the conventional `8000` range
without turning one large ID into an equally large allocation. The limit is
intentional: it protects the engine from malformed patch data and is not a
user-adjustable CVar. See [classic-mod compatibility](docs/engine/mod-compatibility.md)
for scope and load-path details.

## Python Mod Troubleshooting

Python mods are trusted native-equivalent code and are disabled unless the
player explicitly opts in:

```text
biaseddoom -iwad doom2 -file python-mod.pk3 -python -stdout +logfile python-mod.log
```

Use `py_status` in the console. It reports whether Python was compiled, whether
the trust opt-in is active, and which modules/callbacks loaded.

Common causes:

- `-python` was omitted or `-nopython` is present;
- the PK3 lacks a root-level `PYTHON` manifest;
- the manifest path is absolute, contains `..` or backslashes, or references a
  file outside its own container;
- the Windows MinGW package was used. MinGW retains ACS and ZScript but does
  not embed CPython; use the native Windows package for Python mods;
- a callback exceeded its configured time budget or attempted a gameplay
  mutation during multiplayer/demo execution.

Read the [Python security, packaging, API, and debugging guide](docs/scripting/python.md)
before distributing a Python mod.

## Renderer Or Window Startup Problems

- Update the GPU driver and test the same IWAD without mods.
- Remove renderer CVars from a temporary configuration rather than deleting
  the normal user configuration.
- Try `-width 1280 -height 720` to rule out an invalid saved mode.
- Capture `-stdout +logfile biaseddoom.log`; include the reported GPU, driver,
  API, and shader error.
- On Linux, distinguish native Wayland/X11 issues from missing SDL, Vulkan, or
  OpenGL runtime packages.

## Build Problems

Prefer the maintained helpers:

```bash
./supreme-build.sh --clean --release
./tools/build-windows-mingw.sh --clean --package
```

Native Windows:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-windows.ps1 -Configuration Release -Clean -Package
```

After changing vcpkg features, compiler families, or MinGW thread models, use a
clean build directory. Do not reuse Linux-host vcpkg archives in a Windows
triplet or mix MinGW Win32-thread and POSIX-thread libraries. The Windows build
helpers validate the audio linkage and run the OGG/FLAC regression probe.

## What To Include In A Report

- BiasedDoom version and exact downloaded artifact name;
- operating system, CPU, GPU, and—for audio reports—playback device and driver;
- complete command line and mod load order;
- the smallest reproducible IWAD/map/action sequence;
- `biaseddoom.log`, `biaseddoom-audio.log`, or Python log as appropriate;
- whether a clean configuration and unmodified extracted package reproduce it;
- whether upstream GZDoom behaves differently with the same IWAD and mods.

Security vulnerabilities should follow [SECURITY.md](SECURITY.md), not a public
bug report.
