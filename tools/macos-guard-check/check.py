#!/usr/bin/env python3
# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# A syntax check of the macOS code paths on Linux. No macOS SDK is necessary.
#
# The check takes the compile commands of a Linux build. For each source with a macOS branch it
# compiles again with -fsyntax-only, with __APPLE__ defined and __linux__ and __ELF__ not defined.
# The Linux headers stand in for the SDK. The shim directory adds the few macOS names.
#
# What it proves: the macOS branches parse and the names in them resolve. What it does not prove:
# the macOS APIs have these signatures, the code links, or the code runs. The CI job "macos"
# checks those.
#
# Usage: tools/macos-guard-check/check.py <build dir with compile_commands.json>

import json
import pathlib
import shlex
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent

# The sources with a macOS branch (__APPLE__) or with code that the branches change.
SOURCES = [
    "src/runtime/paths.cpp",
    "src/runtime/module.cpp",
    "src/runtime/command_line.cpp",
    "src/runtime/clang_files_backend.cpp",
    "src/runtime/jit_backend.cpp",
    "src/runtime/errors.cpp",
    "src/translator/file_time.cpp",
    "src/ls/clangd_process.cpp",
    "tests/engine/FileWatcher.cpp",
]

APPLE_FLAGS = [
    "-fsyntax-only",
    "-U__linux__", "-U__linux", "-Ulinux", "-U__gnu_linux__", "-U__ELF__",
    "-D__APPLE__=1", "-D__MACH__=1",
    "-I" + str(HERE / "shim"),
    "-include", str(HERE / "shim" / "hot_macos_shim.h"),
]


def main():
    if len(sys.argv) != 2:
        print(__doc__ or "usage: check.py <build dir>", file=sys.stderr)
        return 2
    db = json.loads((pathlib.Path(sys.argv[1]) / "compile_commands.json").read_text())
    by_file = {}
    for entry in db:
        by_file.setdefault(str(pathlib.Path(entry["directory"], entry["file"]).resolve()), entry)
    failed = 0
    for rel in SOURCES:
        path = str((ROOT / rel).resolve())
        entry = by_file.get(path)
        if entry is None:
            print(f"skip {rel}: not in this build")
            continue
        args = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
        # Remove the output and the dependency file: a syntax check writes no file.
        out = []
        skip = False
        for a in args:
            if skip:
                skip = False
                continue
            if a in ("-o", "-MF", "-MT", "-MQ"):
                skip = True
                continue
            if a in ("-c", "-MD", "-MMD"):
                continue
            out.append(a)
        cmd = out[:1] + APPLE_FLAGS + out[1:]
        r = subprocess.run(cmd, cwd=entry["directory"], capture_output=True, text=True)
        if r.returncode != 0:
            failed += 1
            print(f"FAIL {rel}\n{r.stderr}")
        else:
            print(f"ok   {rel}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
