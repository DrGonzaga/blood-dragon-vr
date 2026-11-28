#!/bin/sh
# Builds BloodDragonVR-Setup-<build>.exe
#   sh build.sh <name> <path to version.dll> [source fc3vr.ini] [pictures folder] [label]
# Release: sh build.sh v1.0-BETA ../builds/bd129/version.dll src/default-fc3vr.ini pictures "v1.0 BETA (bd129)"
# Example:
#   sh build.sh bd124 ../builds/bd124/version.dll src/default-fc3vr.ini pictures
# Every file in the pictures folder is installed into the game's bin\bdvr-pictures (font, licence, controls PNGs).
# To change a picture: replace the file in pictures/ (same name), then run this again.
# Needs: g++-mingw-w64-i686-posix and python3 with Pillow (apt-get install g++-mingw-w64-i686-posix python3-pil)
set -e
cd "$(dirname "$0")"
BUILD=${1:?build name, e.g. bd124}
DLL=${2:?path to version.dll}
INI=${3:-src/default-fc3vr.ini}
PICS=${4:-pictures}
LABEL=${5:-$BUILD}   # what Setup and Windows' Apps list show, e.g. "v1.0 BETA (bd129)"
CXX=i686-w64-mingw32-g++-posix
RC=i686-w64-mingw32-windres

rm -rf build/payload
mkdir -p build/payload out
cp "$DLL" build/payload/version.dll
# xrloader2: the 32-bit Khronos OpenXR loader (Apache-2.0), installed to bin\openxr_loader.dll
XRLOADER=${XRLOADER:-../builds/bd137/openxr_loader.dll}
[ -f "$XRLOADER" ] || XRLOADER=third_party/openxr_loader.dll
cp "$XRLOADER" build/payload/openxr_loader.dll
# the shipped settings = the source ini plus the forced values in src/shipped-overrides.txt
python3 make_ini.py "$INI" src/shipped-overrides.txt build/payload/fc3vr.ini
# the pictures: an index (file names, one per line) and one resource each, numbered from IDR_PIC0 (200)
: > build/payload/pictures.txt
: > build/pictures.rc
i=0
for f in "$PICS"/*; do
    [ -f "$f" ] || continue
    n=$(basename "$f")
    cp "$f" "build/payload/pic$i"
    printf '%s\n' "$n" >> build/payload/pictures.txt
    printf '%d RCDATA "../build/payload/pic%d"\n' $((200 + i)) $i >> build/pictures.rc
    i=$((i + 1))
done
[ $i -gt 0 ] || { echo "no pictures in $PICS"; exit 1; }
python3 make_pdf.py "$PICS" build/payload/controls.pdf
# prompts1: the VR button names for the game's prompts (bin\bdvr-prompts.txt); DrGonzaga edits ../prompts/bdvr-prompts.txt
# (the public repo keeps its copy in installer/prompts/)
if [ -z "$PROMPTS" ]; then
  if [ -f ../prompts/bdvr-prompts.txt ]; then PROMPTS=../prompts/bdvr-prompts.txt; else PROMPTS=prompts/bdvr-prompts.txt; fi
fi
cp "$PROMPTS" build/payload/bdvr-prompts.txt
echo "prompts: $(md5sum < "$PROMPTS" | cut -c1-32) from $PROMPTS"
# lang1 (bd168): prompt texts for the game's other languages (bin\bdvr-prompts-<Language>.txt)
LANGDIR=${LANGDIR:-$(dirname "$PROMPTS")/lang}
: > build/payload/langs.txt
: > build/langs.rc
j=0
for f in "$LANGDIR"/bdvr-prompts-*.txt; do
    [ -f "$f" ] || continue
    cp "$f" "build/payload/lang$j"
    printf '%s\n' "$(basename "$f")" >> build/payload/langs.txt
    printf '%d RCDATA "../build/payload/lang%d"\n' $((300 + j)) $j >> build/langs.rc
    j=$((j + 1))
done
[ $j -gt 0 ] || printf '\n' > build/payload/langs.txt  # windres wants a non-empty file
echo "prompt languages: $j files from $LANGDIR"
echo "pictures: $i files from $PICS"
[ -f build/icon.ico ] || python3 make_art.py build

$RC -DBDVR_BUILD="\\\"$LABEL\\\"" -I src -i src/setup.rc -O coff -o build/setup.res
$CXX -O2 -std=c++17 -municode -DBDVR_BUILD="\"$LABEL\"" -Wall -Wno-missing-field-initializers \
    src/setup.cpp build/setup.res -o "out/BloodDragonVR-Setup-$BUILD.exe" \
    -static -static-libgcc -static-libstdc++ -s -lole32 -luuid -lshell32 -ladvapi32
echo "built out/BloodDragonVR-Setup-$BUILD.exe"
md5sum "out/BloodDragonVR-Setup-$BUILD.exe" build/payload/version.dll build/payload/fc3vr.ini
