#!/usr/bin/env bash
# Format S++ scripts (*.spp) with the CSharp style of the repo .clang-format.
# clang-format has no mapping for the .spp extension, so the script
# uses --assume-filename=<file>.cs to select the C# language.
#
# Usage: tools/format-spp.sh [--check] files...
#   default   format the files in place
#   --check   change nothing, exit 1 if a file needs formatting
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
style="file:${root}/.clang-format"
check=0

if [ "${1:-}" = "--check" ]; then
    check=1
    shift
fi

if [ "$#" -eq 0 ]; then
    echo "usage: $0 [--check] files..." >&2
    exit 2
fi

status=0
for file in "$@"; do
    if [ ! -f "$file" ]; then
        echo "$file: not a file" >&2
        status=2
        continue
    fi
    if [ "$check" -eq 1 ]; then
        # The formatter reads stdin, so --dry-run needs the file name for messages only.
        if ! clang-format --dry-run -Werror --style="$style" --assume-filename="${file}.cs" \
            < "$file" > /dev/null 2>&1; then
            echo "$file: needs formatting"
            [ "$status" -eq 0 ] && status=1
        fi
    else
        tmp="$(mktemp)"
        if clang-format --style="$style" --assume-filename="${file}.cs" < "$file" > "$tmp"; then
            if ! cmp -s "$tmp" "$file"; then
                cat "$tmp" > "$file"
                echo "$file: formatted"
            fi
        else
            echo "$file: clang-format failed" >&2
            status=2
        fi
        rm -f "$tmp"
    fi
done
exit "$status"
