#!/bin/sh
# Builds the Click2Screen programs on Linux (clang + MinGW-w64).  ARCH=x64 (default) -> build/x64/   ARCH=x86 -> build/x86/
#   Click2Screen.exe         the recorder / screenshot tool
#   Click2Screen-Setup.exe   graphical installer (installs to Program Files, shortcuts, removal)
# ONLY=autotest ./build.sh   relinks only c2sautotest.exe (the program binaries stay byte for byte the same)
#   c2slauncher.exe          (x86 only) the package's root Click2Screen-Setup.exe: starts x64\ or x86\Click2Screen-Setup.exe
set -e
cd "$(dirname "$0")/src"
python3 ../tools/gen_lang.py >/dev/null     # src/lang/*.txt -> lang_table.inc (a stale table once showed an old example in the options)
ARCH=${ARCH:-x64}
if [ "$ARCH" = x86 ]; then TRIPLE=i686-w64-mingw32; TARGET=i686-w64-windows-gnu
else TRIPLE=x86_64-w64-mingw32; TARGET=x86_64-w64-windows-gnu; fi
O=../build/$ARCH
mkdir -p "$O"
T=${TMPDIR:-/tmp}/c2sapp.$$
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT
CXX="clang++ --target=$TARGET -std=c++17 -O2 -fno-exceptions -fno-rtti -municode
     -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -Wall -Wextra -Wno-unused-parameter
     -Wno-missing-field-initializers -isystem /usr/$TRIPLE/include"
for f in lang settings capture picker recorder sinks fx audiomix selftest main cxxrt applog c2sautotest c2ssetup c2slauncher; do
    [ -f $f.cpp ] && $CXX -c $f.cpp -o "$T/$f.o"
done
$TRIPLE-gcc -municode -mconsole -static -s -Wl,--no-insert-timestamp -Wl,-Map,"$O/c2sautotest.map" -o "$O/c2sautotest.exe" \
    "$T/c2sautotest.o" "$T/lang.o" "$T/settings.o" "$T/capture.o" "$T/picker.o" "$T/recorder.o" "$T/sinks.o" "$T/fx.o" "$T/audiomix.o" "$T/applog.o" "$T/cxxrt.o" \
    -lmfplat -lmfreadwrite -lmf -lmfuuid -lwindowscodecs -lwinmm -lmsimg32 -ldwmapi -lshlwapi -lcomctl32 -lcomdlg32 \
    -lshell32 -lgdi32 -ladvapi32 -luser32 -lole32 -loleaut32 -luuid -lpropsys -lsetupapi -lnewdev
if [ "$ONLY" = autotest ]; then echo "built $O/c2sautotest.exe only"; exit 0; fi
$TRIPLE-windres Click2Screen.rc -O coff -o "$T/app.res.o"
$TRIPLE-windres Click2Screen-Setup.rc -O coff -o "$T/setup.res.o"
$TRIPLE-gcc -municode -mwindows -static -s -Wl,-Map,"$O/Click2Screen.map" -o "$O/Click2Screen.exe" \
    "$T/main.o" "$T/lang.o" "$T/settings.o" "$T/capture.o" "$T/picker.o" "$T/recorder.o" "$T/sinks.o" "$T/fx.o" "$T/audiomix.o" "$T/selftest.o" "$T/applog.o" "$T/cxxrt.o" "$T/app.res.o" \
    -lmfplat -lmfreadwrite -lmf -lmfuuid -lwindowscodecs -lwinmm -lmsimg32 -ldwmapi -lshlwapi -lcomctl32 -lcomdlg32 \
    -lshell32 -lgdi32 -ladvapi32 -luser32 -lole32 -loleaut32 -luuid -lpropsys
if [ -f c2ssetup.cpp ]; then
    $TRIPLE-gcc -municode -mwindows -static -s -o "$O/Click2Screen-Setup.exe" \
        "$T/c2ssetup.o" "$T/lang.o" "$T/applog.o" "$T/cxxrt.o" "$T/setup.res.o" \
        -lcomctl32 -lshell32 -lgdi32 -ladvapi32 -luser32 -lole32 -loleaut32 -luuid
fi
if [ "$ARCH" = x86 ] && [ -f c2slauncher.cpp ]; then
    $TRIPLE-windres Click2Screen-Setup.rc -O coff -o "$T/launcher.res.o"
    $TRIPLE-gcc -municode -mwindows -static -s -o "$O/c2slauncher.exe" "$T/c2slauncher.o" "$T/cxxrt.o" "$T/launcher.res.o" \
        -lsetupapi -lshell32 -luser32 -ladvapi32 -lole32
fi
echo "built $O"
