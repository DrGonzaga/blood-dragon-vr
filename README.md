# Blood Dragon VR

A VR mod for **Far Cry 3 Blood Dragon** (Steam version): 6DoF head tracking through OpenXR,
motion-controlled guns, VR hands, bow, throwing, a synthwave VR menu and more.

Vibe coded with Claude by the submissive lab-rat DrGonzaga. Beta, like its author.

Downloads: Nexus Mods (link to come).

## What is in this repository

| Folder | What it is |
|---|---|
| `mod/` | Source of the mod, a 32-bit `version.dll` proxy (C++17, MinGW). |
| `installer/` | Source of `BloodDragonVR-Setup.exe`, the text-mode installer, plus the files it ships (default `fc3vr.ini`, controls pictures, menu font). |

No game files are included. Far Cry 3 Blood Dragon is (c) Ubisoft.

## What the mod does to your PC

- **`bin\version.dll`** is loaded by the game in place of Windows' own `version.dll`. It forwards
  every real `version.dll` call to `C:\Windows\SysWOW64\version.dll` (`mod/src/proxy.cpp`,
  `mod/src/proxy.def`), then hooks the game's Direct3D 11 rendering and some game functions in
  memory to render in stereo through the headset's OpenXR runtime and read the motion controllers.
- It reads and writes only its own files next to the game: `fc3vr.ini` (settings), `fc3vr.log`,
  `fc3vr_bowstring.bin` and `bdvr-pictures\`. It also sets the game's own GamerProfile options it
  needs (windowed, multithreaded rendering off).
- It makes **no network connections**. It does the opposite: the game's own online calls (Ubisoft's
  servers for this game are closed) are answered with "no connection" (`mod/src/online.cpp`).
- Nothing is changed on disk in the game's code; all patches are in memory while the game runs.

## What Setup does

`installer/src/setup.cpp`, one file. It finds the game through Steam's library list, backs up what it
changes to `<game>\BloodDragonVR\backup`, copies `version.dll`, `fc3vr.ini` and `bdvr-pictures\` into
`bin\`, and optionally: sets the large-address-aware flag in the game exe (the usual "4 GB patch"),
marks Steam's `appmanifest` read-only so an update can't undo the mod, adds a desktop shortcut and an
uninstall entry in Windows' Apps list. Uninstall reverses each step. It makes no network connections.

## Building

See [BUILD.md](BUILD.md).

## Third-party code

Dear ImGui (MIT), SMAA (MIT-style), OpenXR SDK headers (Apache 2.0), Audiowide font (SIL OFL 1.1).
Their licences are in `mod/third_party/*/` and `installer/pictures/menu-font-licence.txt`.
