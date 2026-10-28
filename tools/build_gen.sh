#!/bin/sh
# Translates a script and compiles the result into a generation library.
# Usage: tools/build_gen.sh <file.script> <generation> <out.so> [slots-file]
# Environment: SPC (path of spc), CXX (default clang++), CXXFLAGS (extra flags).
# The script prints the errors of spc and of the compiler. It exits with 1 on error.
set -u
if [ $# -lt 3 ]; then
    echo "usage: $0 <file.script> <generation> <out.so> [slots-file]" >&2
    exit 2
fi
root=$(cd "$(dirname "$0")/.." && pwd)
script=$1
gen=$2
out=$3
slots=${4:-}
spc=${SPC:-$root/build/spc}
cxx=${CXX:-clang++}
cpp=${out%.so}.gen.cpp

set -- "$script" --gen "$gen" -o "$cpp" -I "$root/engine" -I "$root/runtime"
[ -n "$slots" ] && set -- "$@" --slots "$slots"
rm -f "$out"
if ! "$spc" "$@"; then
    echo "build_gen: spc failed for $script" >&2
    exit 1
fi
# shellcheck disable=SC2086
if ! "$cxx" -std=c++20 -fPIC -shared -g ${CXXFLAGS:-} -I"$root/engine" -I"$root/runtime" \
    "$cpp" -o "$out"; then
    echo "build_gen: the compile failed for $script" >&2
    exit 1
fi
