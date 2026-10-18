#!/bin/bash
# Checks the finished package under Wine: dist/Click2Screen-Setup.exe (the launcher with the cabinet appended) and the
# unpacked dist/Click2Screen/ folder install the right files, clean up their temporary folder and remove cleanly.
#   ./package.sh && tools/test-package-wine.sh
# Own Wine prefix (WINEPREFIX_PKG), own Xvfb display when DISPLAY is not set; prints PASS / FAIL, exit 1 on a failure.
cd "$(dirname "$0")/.."
ROOT=$PWD
W=${PKG_TEST_DIR:-${TMPDIR:-/tmp}/c2s-pkgtest}
mkdir -p "$W"
export WINEPREFIX=${WINEPREFIX_PKG:-$W/prefix} WINEDEBUG=-all
LOW="nice -n 19 ionice -c3"
if [ -z "$DISPLAY" ]; then
    export DISPLAY=:79
    [ -S /tmp/.X11-unix/X79 ] || { nice -n 19 Xvfb :79 -screen 0 1280x800x24 >/dev/null 2>&1 & sleep 3; }
fi
FAILS=0
ok()   { echo "PASS  $1"; }
bad()  { echo "FAIL  $1"; FAILS=$((FAILS + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

SETUP="$ROOT/dist/Click2Screen-Setup.exe"
UNPACKED="$ROOT/dist/Click2Screen/Click2Screen-Setup.exe"
[ -f "$SETUP" ] || { echo "FAIL  $SETUP does not exist: run ./package.sh first"; exit 1; }
PF="$WINEPREFIX/drive_c/Program Files/Click2Screen"
TEMPDIR="$WINEPREFIX/drive_c/users/$USER/AppData/Local/Temp"
[ -d "$TEMPDIR" ] || TEMPDIR=$(ls -d "$WINEPREFIX"/drive_c/users/*/AppData/Local/Temp 2>/dev/null | head -1)
rm -rf "$PF"

# the package is one file: the launcher + a cabinet + the trailer
check "the single file ends with the C2SPAYLD trailer" "tail -c 16 '$SETUP' | head -c 8 | grep -q C2SPAYLD"
check "the single file is much bigger than the launcher (the payload is in it)" "[ \$(stat -c %s '$SETUP') -gt \$(( \$(stat -c %s '$ROOT/build/x86/c2slauncher.exe') + 200000 )) ]"

for MODE in single unpacked; do
    EXE=$SETUP; [ $MODE = unpacked ] && EXE=$UNPACKED
    $LOW timeout 180 wine "$EXE" /install /silent /autostart; RC=$?
    check "[$MODE] silent install returns 0" "[ $RC -eq 0 ]"
    # Wine runs the 64-bit installer (the machine is 64-bit): the program must be the x64 build
    check "[$MODE] Click2Screen.exe is in Program Files and is the x64 build" "cmp -s '$PF/Click2Screen.exe' '$ROOT/build/x64/Click2Screen.exe'"
    check "[$MODE] the installer copy for removal is there" "cmp -s '$PF/Click2Screen-Setup.exe' '$ROOT/build/x64/Click2Screen-Setup.exe'"
    check "[$MODE] uninstall.cmd is there" "[ -f '$PF/uninstall.cmd' ]"
    check "[$MODE] the temporary extraction folder is gone" "[ -z \"\$(ls -d '$TEMPDIR'/Click2Screen-Setup-* 2>/dev/null)\" ]"
    $LOW timeout 120 wine "C:\\Program Files\\Click2Screen\\Click2Screen-Setup.exe" /remove /silent; RC=$?
    sleep 6
    check "[$MODE] silent removal returns 0" "[ $RC -eq 0 ]"
    check "[$MODE] the program folder is gone after the removal" "[ ! -e '$PF/Click2Screen.exe' ]"
done
# the 32-bit installer on its own (what a 32-bit Windows would run)
$LOW timeout 180 wine "$ROOT/dist/Click2Screen/x86/Click2Screen-Setup.exe" /install /silent; RC=$?
check "[x86] the 32-bit installer installs (return 0)" "[ $RC -eq 0 ]"
PF86="$WINEPREFIX/drive_c/Program Files/Click2Screen"; [ -d "$WINEPREFIX/drive_c/Program Files (x86)/Click2Screen" ] && PF86="$WINEPREFIX/drive_c/Program Files (x86)/Click2Screen"
check "[x86] the 32-bit program is installed" "cmp -s '$PF86/Click2Screen.exe' '$ROOT/build/x86/Click2Screen.exe'"
$LOW timeout 120 wine "$ROOT/dist/Click2Screen/x86/Click2Screen-Setup.exe" /remove /silent; sleep 6
check "[x86] and removed again" "[ ! -e '$PF86/Click2Screen.exe' ]"
echo "package test: $FAILS failure(s)"
[ $FAILS -eq 0 ]
