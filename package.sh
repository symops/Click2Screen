#!/bin/sh
# Builds everything on Linux (clang + MinGW-w64) and assembles the package:
#   dist/Click2Screen-Setup.exe   the release: ONE file - the launcher with the whole package below appended
#                                 (a cabinet, MSZIP; extracted to a temporary folder when it runs)
#   dist/Click2Screen/            the same package unpacked (for development / testing)
#     Click2Screen-Setup.exe      x86 launcher: starts x64\ or x86\Click2Screen-Setup.exe (whichever Windows this is)
#     uninstall.cmd               removal without a window
#     x64\  x86\                  the program and its installer for each bitness
set -e
cd "$(dirname "$0")"
[ -f tools/make_icon.py ] && [ ! -f src/Click2Screen.ico ] && python3 tools/make_icon.py
ARCH=x64 ./build.sh
ARCH=x86 ./build.sh

D=dist/Click2Screen
rm -rf dist
mkdir -p $D/x64 $D/x86
cp build/x86/c2slauncher.exe $D/Click2Screen-Setup.exe
cp scripts/uninstall.cmd $D/
for A in x64 x86; do
    cp build/$A/Click2Screen.exe build/$A/Click2Screen-Setup.exe $D/$A/
    cp scripts/uninstall.cmd $D/$A/
done
cp docs/INSTALL.txt docs/INSTALL.en.txt $D/ 2>/dev/null || true
# Windows cmd.exe reads .cmd files in the OEM code page; convert the Russian text to CP866.
for f in $D/uninstall.cmd $D/x64/uninstall.cmd $D/x86/uninstall.cmd; do
    iconv -f UTF-8 -t CP866 "$f" | sed 's/$/\r/' > "$f.tmp" && mv "$f.tmp" "$f"
done

# One file: the launcher + the package as a cabinet (MSZIP; Windows extracts it with SetupIterateCabinet) + a trailer
# "C2SPAYLD" + the cabinet's size (little endian, 8 bytes). See src/c2slauncher.cpp.
(cd $D && find uninstall.cmd x64 x86 -type f | sort | xargs gcab -c -z ../payload.cab)
python3 - <<'PY'
import struct
launcher = open('build/x86/c2slauncher.exe', 'rb').read()
cab = open('dist/payload.cab', 'rb').read()
open('dist/Click2Screen-Setup.exe', 'wb').write(launcher + cab + b'C2SPAYLD' + struct.pack('<Q', len(cab)))
PY
rm -f dist/payload.cab
ls -la dist/Click2Screen-Setup.exe

# Link maps of this build (a crash line in panel.log names module + offset; the map names the function), kept per version.
VER=$(sed -n 's/#define C2S_VER_STR *"\(.*\)"/\1/p' src/version.h)
mkdir -p "maps/$VER/x64" "maps/$VER/x86"
cp build/x64/*.map "maps/$VER/x64/" 2>/dev/null || true
cp build/x86/*.map "maps/$VER/x86/" 2>/dev/null || true
echo "package: dist/Click2Screen-Setup.exe (version $VER)"
