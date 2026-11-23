#!/bin/sh
# Packs a release zip for sharing (e.g. Nexus Mods), after build.sh has made the Setup exe:
#   sh make_release.sh <version> <mod build>      e.g. sh make_release.sh v1.0-BETA bd129
# Zip contents: the Setup exe, README.txt, THIRD-PARTY-NOTICES.txt and "Manual install\bin\..." (the same
# version.dll, fc3vr.ini and bdvr-pictures that Setup installs). Only the mod's own files, never game files.
set -e
cd "$(dirname "$0")"
BUILD=${1:?version, e.g. v1.0-BETA}
BD=${2:-?}
EXE=out/BloodDragonVR-Setup-$BUILD.exe
[ -f "$EXE" ] || { echo "no $EXE: run build.sh first"; exit 1; }
STAGE=build/release/BloodDragonVR-$BUILD
rm -rf build/release
mkdir -p "$STAGE/Manual install/bin/bdvr-pictures" release
cp "$EXE" "$STAGE/"
cp build/payload/controls.pdf "$STAGE/Blood Dragon VR controls.pdf"
sed "s/<version>/$(echo $BUILD | tr - ' ')/g; s/Setup-$(echo $BUILD | tr - ' ').exe/Setup-$BUILD.exe/; s/<bd>/$BD/g" release-docs/README.txt | sed 's/$/\r/' > "$STAGE/README.txt"
sed 's/$/\r/' release-docs/THIRD-PARTY-NOTICES.txt > "$STAGE/THIRD-PARTY-NOTICES.txt"
cp build/payload/version.dll build/payload/fc3vr.ini build/payload/bdvr-prompts.txt "$STAGE/Manual install/bin/"
i=0
while IFS= read -r n; do
    [ -n "$n" ] && cp "build/payload/pic$i" "$STAGE/Manual install/bin/bdvr-pictures/$n"
    i=$((i + 1))
done < build/payload/pictures.txt
rm -f "release/BloodDragonVR-$BUILD.zip"
(cd build/release && zip -qr9 "../../release/BloodDragonVR-$BUILD.zip" "BloodDragonVR-$BUILD")
echo "release/BloodDragonVR-$BUILD.zip"
unzip -l "release/BloodDragonVR-$BUILD.zip"
