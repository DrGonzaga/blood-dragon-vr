# Blood Dragon VR

A VR mod for **Far Cry 3 Blood Dragon** (Steam version) for PC VR headsets: full 6DoF head tracking
through OpenXR, motion-controlled guns, VR hands and arms, two-handed aiming, rifle scopes, bow and
arrows, throwing, holsters on your body, the cyber-eye, slow motion, a synthwave VR settings menu and
plenty more.

**Version 0.71 BETA** (mod build bd135). Beta, like its author.

Vibe coded with Claude by the submissive lab-rat DrGonzaga.

- [Requirements](#requirements)
- [Install (download)](#install-download)
- [Install a build you compiled yourself](#install-a-build-you-compiled-yourself)
- [Building from source](#building-from-source)
- [Playing](#playing)
- [Uninstall](#uninstall)
- [Troubleshooting](#troubleshooting)
- [What the mod and Setup do to your PC](#what-the-mod-and-setup-do-to-your-pc)
- [Repository layout](#repository-layout)

## Requirements

- **Far Cry 3 Blood Dragon on Steam**, fully updated, started once without the mod. The mod finds the
  game's code by its exact bytes in `FC3_d3d11.dll` (Steam build of 17 Oct 2019, 28,693,672 bytes).
  Ubisoft Connect, GOG or older patches are untested and will most likely not work.
- **A PC VR headset with a 32-bit OpenXR runtime** (the game is 32-bit). Developed and tested on a
  Quest 3 with Virtual Desktop, using Virtual Desktop's own runtime, **VDXR**. Other headsets and
  runtimes should work if their runtime supports 32-bit apps, but are untested.
- Windows 10 or 11. A strong GPU helps: the mod renders at your headset's recommended size (VR menu, Graphics, Render scale).

## Install (download)

Download `BloodDragonVR-v0.71-BETA.zip` from [Releases](https://github.com/DrGonzaga/blood-dragon-vr/releases) or Nexus Mods and unzip it.

**With Setup (easiest)**

1. Close the game.
2. Run `BloodDragonVR-Setup-v0.71-BETA.exe`. It is not code-signed, so Windows may say
   "Windows protected your PC": click **More info**, then **Run anyway**, then **Yes** on the
   administrator prompt.
3. Choose **Install**, check the game folder it found (it asks Steam), and press **INSTALL NOW**.
   Mouse or keyboard both work.
4. Read **Setup tips** (NVIDIA settings matter), then start the game from the new desktop shortcut.

Setup backs up everything it changes into `<game>\BloodDragonVR\backup`. To update, run a newer Setup
and choose Install; tick **Keep my current settings** to keep your `fc3vr.ini`.

**By hand**

Copy everything inside the zip's `Manual install\bin` folder into the game's `bin` folder, usually
`C:\Program Files (x86)\Steam\steamapps\common\Far Cry 3 Blood Dragon\bin` (or `<your Steam library>\steamapps\common\Far Cry 3 Blood Dragon\bin`).
Then follow steps 2 to 4 of the next section.

## Install a build you compiled yourself

After [building](#building-from-source) you have `mod/build/fc3vr.dll` (and, if you built it,
`installer/out/BloodDragonVR-Setup-<name>.exe`, which you can simply run as above).

1. Close the game. Open the game's `bin` folder (in Steam: right-click the game, **Manage**,
   **Browse local files**, then open `bin`).
2. Copy `mod/build/fc3vr.dll` into `bin` and rename it to **`version.dll`**. If a `version.dll` is
   already there from another mod, move it somewhere safe first.
3. Copy `installer/src/default-fc3vr.ini` into `bin` as **`fc3vr.ini`** (the tuned default settings;
   without it the mod starts from its built-in defaults).
4. Copy the folder `installer/pictures` into `bin` as **`bdvr-pictures`** (the VR menu's font and the
   controls pictures).
5. Recommended: give the game 4 GB of memory by setting the large-address-aware flag on
   `bin\fc3_blooddragon_d3d11.exe` (Setup does this for you; by hand use any "4GB patch" tool, and
   keep a copy of the original exe).
6. Start `bin\fc3_blooddragon_d3d11.exe` directly (not through the Steam or Ubisoft launchers) with the
   headset connected.

Your `bin` folder should then contain, next to the game's own files:

    version.dll
    fc3vr.ini
    bdvr-pictures\menu-font.ttf, menu-font-licence.txt, controls-*.png

## Building from source

Both the mod and the installer cross-compile with MinGW-w64 on Linux. On Windows, use **WSL**
(Windows Subsystem for Linux) with Ubuntu: it is the same as the Linux steps.

### 1. Get a Linux shell (Windows users)

Open PowerShell as administrator and run `wsl --install -d Ubuntu`, restart if asked, then open
**Ubuntu** from the Start menu and create a user. Your Windows drives are under `/mnt/c`, `/mnt/d`...

### 2. Install the tools

    sudo apt-get update
    sudo apt-get install -y git make g++-mingw-w64-i686-posix python3 python3-pil python3-pip zip
    pip install img2pdf        # only needed for the installer's controls PDF
                               # (on newer Ubuntu: pip install --break-system-packages img2pdf, or use a venv)

### 3. Get the source

    git clone https://github.com/DrGonzaga/blood-dragon-vr.git
    cd blood-dragon-vr

### 4. Build the mod

    cd mod
    make NAME=my-build -j8 CXX=i686-w64-mingw32-g++-posix
    cd ..

Output: `mod/build/fc3vr.dll` (about 4 MB). `NAME` is just the name written on the first line of the
mod's log, so you can tell builds apart. `make clean` starts again from scratch.

To reproduce the released v0.71 BETA exactly, use `NAME=bd135-autores`: the result is byte-for-byte
identical to the released `version.dll` except the link timestamps in the PE headers.

### 5. Build the installer (optional)

    cd installer
    sh build.sh my-build ../mod/build/fc3vr.dll src/default-fc3vr.ini pictures "my build"
    sh make_release.sh my-build
    cd ..

Output: `installer/out/BloodDragonVR-Setup-my-build.exe` and the release zip
`installer/release/BloodDragonVR-my-build.zip`. The release itself was built with:

    sh build.sh v0.71-BETA ../mod/build/fc3vr.dll src/default-fc3vr.ini pictures "v0.71 BETA (bd135)"
    sh make_release.sh v0.71-BETA bd135

`build.sh` embeds the DLL, the default settings (`src/default-fc3vr.ini` with the forced values in
`src/shipped-overrides.txt`, applied by `make_ini.py`), every file in `pictures/`, the VR prompt texts (`prompts/bdvr-prompts.txt`) and a PDF made from
the controls pictures (`make_pdf.py`) into the exe. `make_art.py` draws the icon. More detail in
[BUILD.md](BUILD.md).

### 6. Copy it to Windows

From WSL, for example: `cp mod/build/fc3vr.dll "/mnt/c/Program Files (x86)/Steam/steamapps/common/Far Cry 3 Blood Dragon/bin/version.dll"`
(close the game first), or install it as described in
[Install a build you compiled yourself](#install-a-build-you-compiled-yourself).

## Playing

1. NVIDIA users: in NVIDIA Control Panel, Manage 3D settings, Program Settings, add
   `fc3_blooddragon_d3d11.exe` and set **Threaded optimization** to **Off**.
2. Turn off overlays for the game (Steam, Ubisoft Connect, NVIDIA, Discord).
3. Connect the headset (Virtual Desktop: OpenXR runtime set to VDXR), then start the game.
4. **Click both thumbsticks together** to open the VR menu (Insert on the keyboard). Its Controls page
   has every button, a body map of the holsters, and a tutorial page. The same pictures are in
   `Blood Dragon VR controls.pdf` in the download.
5. The picture starts at your headset's recommended size. For a sharper picture (costs GPU time), open
   the VR menu's **Graphics** page, raise **Render scale**, press **Save for next start** and restart
   the game. **Automatic size from the headset** goes back to the recommended size.

## Uninstall

Run Setup and choose **Uninstall**, or use Windows Settings, Apps, **Blood Dragon VR**. By hand: delete
`bin\version.dll` (or `bin\dinput8.dll` if Setup installed the mod under that name), `bin\fc3vr.ini`,
`bin\fc3vr.log`, `bin\bdvr-prompts.txt` and the `bin\bdvr-pictures` folder, and put back
the original exe if you 4 GB-patched it.

## Troubleshooting

- **Settings:** `bin\fc3vr.ini`. **Log:** `bin\fc3vr.log`. Please attach the log to bug reports.
- **Reporting a bug or giving feedback:** check the [Known issues](https://github.com/DrGonzaga/blood-dragon-vr/issues?q=is%3Aissue+label%3Aknown-issue)
  first, then use [New issue](https://github.com/DrGonzaga/blood-dragon-vr/issues/new/choose). The bug form asks for your headset,
  runtime and log, which saves a lot of back and forth.
- **Game won't start or crashes at once:** in `fc3vr.ini`, under `[bd]`, set `Engine=0`. That turns
  the VR hooks off (flat picture in the headset) and shows whether the mod is the cause.
- **Settings in a mess:** run Setup, Install, and untick **Keep my current settings** (your old file
  is backed up), or copy `installer/src/default-fc3vr.ini` over it.
- **Steam updated the game, or you verified the files:** run Setup again and choose Install.
- **Blurry picture:** the mod starts at your headset's recommended size. Raise **Render scale** on the VR
  menu's Graphics page, press **Save for next start** and restart. With Virtual Desktop, its own quality and
  bitrate settings matter too.
- **Game crashes at start-up on a CPU with 32 or more threads:** that is a bug in the game itself.
  From v0.70 the mod works around it, so you don't need Windows' compatibility fix. If you already set
  that fix up, Setup installs the mod as `dinput8.dll` instead, because the fix stops `version.dll` loading.
- **Button prompts:** the game's on-screen prompts use the VR controls from `bin\bdvr-prompts.txt`.
  You can edit that file (it's plain text) while the game runs; changes show within a second.

## What the mod and Setup do to your PC

- **`bin\version.dll`** is loaded by the game in place of Windows' own `version.dll`. It forwards
  every real `version.dll` call to the system copy (`mod/src/proxy.cpp`, `mod/src/proxy.def`), then
  hooks the game's Direct3D 11 rendering and some game functions in memory to render in stereo
  through the headset's OpenXR runtime and read the motion controllers.
- It reads and writes only its own files next to the game: `fc3vr.ini`, `fc3vr.log`,
  `fc3vr_bowstring.bin`, `bdvr-prompts.txt` and `bdvr-pictures\`, and sets the game's own GamerProfile options it needs
  (windowed, multithreaded rendering off).
- It makes **no network connections**. It does the opposite: the game's own online calls (Ubisoft's
  servers for this game are closed) are answered with "no connection" (`mod/src/online.cpp`;
  `[online] Block=0` turns that off).
- The game's files on disk are not changed by the mod; all patches are in memory while it runs.
- **Setup** (`installer/src/setup.cpp`, one file) finds the game through Steam's library list, backs
  up what it changes to `<game>\BloodDragonVR\backup`, copies the mod files into `bin\`, and
  optionally sets the large-address-aware flag in the game exe, marks Steam's `appmanifest` for the
  game read-only so an update can't undo the mod, and adds a desktop shortcut and an uninstall entry
  in Windows' Apps list. Uninstall reverses each step. It makes no network connections.

## Repository layout

| Path | What it is |
|---|---|
| `mod/src/` | The mod (C++17): `proxy.cpp` (version.dll forwarding), `hook.cpp` (start-up), `xr.cpp` (OpenXR), `engine.cpp`/`stereo.cpp` (camera and stereo), `menu.cpp` + `synth.inc` (VR menu), `hands.cpp`, `bow.cpp`, `scope.cpp` and the other features, one file each. |
| `mod/third_party/` | Dear ImGui, SMAA, OpenXR SDK headers. |
| `mod/Makefile` | Build of `fc3vr.dll`. |
| `installer/src/` | `setup.cpp` (the installer), its resources, the default `fc3vr.ini`, Setup tips. |
| `installer/pictures/` | Menu font and controls pictures installed to `bin\bdvr-pictures`. |
| `installer/prompts/` | `bdvr-prompts.txt`, the VR wording for the game's button prompts, installed to `bin\`. |
| `installer/release-docs/` | README and notices that go into the release zip. |

No game files are included. Far Cry 3 Blood Dragon is (c) Ubisoft; this project is not affiliated
with Ubisoft.

## Third-party code

Dear ImGui (MIT), SMAA (MIT-style), OpenXR SDK headers (Apache 2.0), Audiowide font (SIL OFL 1.1).
Their licences are in `mod/third_party/*/` and `installer/pictures/menu-font-licence.txt`.
