#!/bin/sh
# Step R1: show that a generation that does not compile is never loaded.
# Usage: poc/r1/make_bad.sh [build-dir]   (the build directory must be built)
# The script compiles gen3_bad.cpp. The compile must fail. The library is then
# missing. The test R1 uses the path of this missing library.
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
echo "make_bad: the compile failed as expected."
[ ! -e "$out" ] || { echo "make_bad: ERROR: output file exists" >&2; exit 1; }
