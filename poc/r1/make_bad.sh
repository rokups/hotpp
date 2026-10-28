#!/bin/sh
# Step R1: show that a generation that does not compile is never loaded.
# Usage: poc/r1/make_bad.sh [build-dir]   (the build directory must be built)
# The script compiles gen3_bad.cpp. The compile must fail. The script then
# runs the R1 test with the path of the missing library. The reload is skipped
# and generation 2 keeps running.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${1:-$root/build}
cxx=${CXX:-clang++}
out=$build/gen3_missing.so

rm -f "$out"
if "$cxx" -std=c++20 -fPIC -shared -I"$root/runtime" -I"$root/engine" \
    "$root/poc/r1/gen3_bad.cpp" -o "$out"; then
    echo "make_bad: ERROR: gen3_bad.cpp compiled, but it must fail" >&2
    exit 1
fi
echo "make_bad: the compile failed as expected. The reload is skipped."
[ ! -e "$out" ] || { echo "make_bad: ERROR: output file exists" >&2; exit 1; }
"$build/minengine" --r1 "$build/gen1.so" "$build/gen2.so" "$build/gen4_base.so" "$out"
