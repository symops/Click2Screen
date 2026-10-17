#!/bin/bash
# Soak test under Wine: runs c2sautotest (random seed, alternating x64 / x86, random length) and /selftest over and over
# until the product has passed for SOAK_MINUTES (default 60) in a row. A failure (a FAIL line, a crash, a hang) or a
# change of the program binaries resets the stopwatch. Tests may be added or changed meanwhile: every run takes the
# newest c2sautotest.exe; only the product (Click2Screen.exe, Click2Screen-Setup.exe, the launcher) is tracked.
#   tools/wine-soak.sh            # needs wine, Xvfb (started on :77 when no DISPLAY is set), a build in build/x64 and build/x86
# Everything runs at the lowest priority (nice 19, idle I/O) so the machine stays usable.
# Prints one line per run (OK / FAIL), the clean time so far, and SOAK_PASSED at the end (exit 0); exit 1 on a failure.
cd "$(dirname "$0")/.."
ROOT=$PWD
W=${SOAK_DIR:-${TMPDIR:-/tmp}/c2s-soak}
GOAL=${SOAK_MINUTES:-60}
mkdir -p "$W"
export WINEPREFIX=${WINEPREFIX:-$W/prefix} WINEDEBUG=-all
if [ -z "$DISPLAY" ]; then
    export DISPLAY=:77
    pgrep -f 'Xvfb :77' >/dev/null || { nice -n 19 Xvfb :77 -screen 0 1280x800x24 >/dev/null 2>&1 & sleep 2; }
fi
LOW="nice -n 19 ionice -c3"
STATE=$W/state
product_hash() { cat build/x64/Click2Screen.exe build/x64/Click2Screen-Setup.exe build/x86/Click2Screen.exe build/x86/Click2Screen-Setup.exe build/x86/c2slauncher.exe 2>/dev/null | sha256sum | cut -c1-16; }
now() { date +%s; }
reset_clock() { echo "$(now) $(product_hash) $1" > "$STATE"; echo "CLOCK reset: $2"; }
[ -f "$STATE" ] || reset_clock "" "first start"
run=0
while true; do
    read -r START HASH LAST < "$STATE"
    CUR=$(product_hash)
    if [ "$CUR" != "$HASH" ]; then reset_clock "$LAST" "the program binaries changed ($HASH -> $CUR)"; read -r START HASH LAST < "$STATE"; fi
    CLEAN=$(( ($(now) - START) / 60 ))
    if [ "$CLEAN" -ge "$GOAL" ]; then echo "SOAK_PASSED: $CLEAN minutes without a failure, product $HASH, $run runs"; exit 0; fi
    run=$((run + 1))
    # a private copy of the newest build: rebuilding while a run is going does not disturb it
    RUN=$W/run; rm -rf "$RUN"; mkdir -p "$RUN"; cp build/x64/*.exe "$RUN/" 2>/dev/null; mkdir -p "$RUN/x86"; cp build/x86/*.exe "$RUN/x86/" 2>/dev/null
    if [ $((run % 2)) -eq 1 ]; then ARCH=x64; EXE=$RUN/c2sautotest.exe; APP=$RUN/Click2Screen.exe; else ARCH=x86; EXE=$RUN/x86/c2sautotest.exe; APP=$RUN/x86/Click2Screen.exe; fi
    SEED=$(( (RANDOM << 30) ^ (RANDOM << 15) ^ RANDOM ^ $(now) ))
    MIN=$(( 2 + RANDOM % 4 ))
    OUT=$W/out-$run.txt
    ST=$W/st-$run; rm -rf "$ST"; mkdir -p "$ST"
    S0=$(now)
    timeout 120 $LOW wine "$APP" /selftest "Z:$ST" >/dev/null 2>&1
    STFAIL=$(grep -c '^FAIL' "$ST/selftest.log" 2>/dev/null)
    ARGS="$MIN --seed $SEED --installer"
    timeout $(( MIN * 60 + 600 )) $LOW wine "$EXE" $ARGS > "$OUT" 2>&1
    RC=$?
    NFAIL=$(grep -c '^FAIL' "$OUT")
    NWARN=$(grep -c '^WARN' "$OUT")
    CRASH=$(grep -c 'CRASH' "$OUT")
    SUM=$(grep 'test(s) run' "$OUT" | sed 's/^INFO *//')
    if [ "$RC" -ne 0 ] || [ "${STFAIL:-0}" -ne 0 ] || [ "$NFAIL" -ne 0 ] || [ "$CRASH" -ne 0 ] || [ -z "$SUM" ]; then
        echo "FAIL run $run $ARCH seed $SEED rc=$RC selftest-fails=$STFAIL fails=$NFAIL crashes=$CRASH (output $OUT)"
        grep -E '^(FAIL|WARN)|CRASH' "$OUT" | head -20
        [ "${STFAIL:-0}" -ne 0 ] && grep '^FAIL' "$ST/selftest.log"
        reset_clock "$ARCH:$SEED" "failure in run $run"
        exit 1
    fi
    echo "OK run $run $ARCH seed $SEED ${MIN}min: $SUM; warnings $NWARN; clean for $(( ($(now) - START) / 60 )) of $GOAL min"
    [ "$NWARN" -gt 0 ] && grep '^WARN' "$OUT" | sort | uniq -c | head -5
done
