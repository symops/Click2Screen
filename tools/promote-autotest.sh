#!/bin/sh
# Hands the current build/*/c2sautotest.exe over to the running soak (tools/wine-soak.sh): the soak runs only promoted
# autotests, so that a new, not yet tried test cannot reset its stopwatch.  Try the new tests on their own first
# (c2sautotest.exe --only name), then promote.
cd "$(dirname "$0")/.."
W=${SOAK_DIR:-${TMPDIR:-/tmp}/c2s-soak}
for A in x64 x86; do
    mkdir -p "$W/promoted/$A"
    cp "build/$A/c2sautotest.exe" "$W/promoted/$A/c2sautotest.exe" && echo "promoted $A: $(sha256sum build/$A/c2sautotest.exe | cut -c1-12)"
done
