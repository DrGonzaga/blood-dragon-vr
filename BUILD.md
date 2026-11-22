# Building Blood Dragon VR

Both parts cross-compile on Linux (tested on Ubuntu 24.04) with MinGW-w64. The release was built
exactly this way.

## 1. Tools

    sudo apt-get update
    sudo apt-get install -y make g++-mingw-w64-i686-posix python3 python3-pil zip
    pip install img2pdf          # only for the installer's controls PDF

## 2. The mod (version.dll)

    cd mod
    make NAME=bd129-palmup -j8 CXX=i686-w64-mingw32-g++-posix

Output: `mod/build/fc3vr.dll`. Rename it to `version.dll` and put it in the game's `bin` folder.
`NAME` is only the build name written to the log's first line.

Checking against the released file: building this source gives a DLL that is byte-for-byte the same
as the released `version.dll` (v0.69 BETA, build bd129, 4,089,856 bytes) except the link timestamp in
the PE headers (the COFF TimeDateStamp, the optional-header CheckSum field and the export directory's
TimeDateStamp), which change on every link.

## 3. The installer (BloodDragonVR-Setup.exe)

    cd installer
    sh build.sh v0.69-BETA ../mod/build/fc3vr.dll src/default-fc3vr.ini pictures "v0.69 BETA (bd129)"
    sh make_release.sh v0.69-BETA bd129

Output: `installer/out/BloodDragonVR-Setup-v0.69-BETA.exe` and the release zip
`installer/release/BloodDragonVR-v0.69-BETA.zip`.

`build.sh` embeds the DLL, the default settings (`src/default-fc3vr.ini` with
`src/shipped-overrides.txt` applied by `make_ini.py`), every file in `pictures/` and a PDF made from
the controls pictures (`make_pdf.py`) as resources in the exe. `make_art.py` draws the icon.
