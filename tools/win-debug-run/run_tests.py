#!/usr/bin/env python3
# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.
"""Runs some ctest tests again under debug_run.exe (a small debugger), for CI on Windows.

For each pattern, the script runs the first test whose name matches it (fnmatch). It uses the
command, the working directory and the environment of the test from ctest. The output goes into
the job log. With --cdb, the first test also runs under cdb.exe. A test that matches a pattern of
--cdb-also runs under cdb.exe too. The symbol path of cdb.exe has the Microsoft symbol server, so
the stack shows the functions of the system DLLs (ucrtbase.dll, for example). A test that matches a
pattern of --cdb-eh runs under cdb.exe with a log of each first-chance C++ exception (0xE06D7363)
and access violation: the exception record (with its parameters: magic, object, throw info and
image base) and the stack. Then the test continues.

Usage: run_tests.py --preset <ctest preset> --harness <debug_run.exe> [--timeout s] [--cdb path]
       [--cdb-also pattern]... [--cdb-eh pattern]... pattern...
"""

import argparse
import fnmatch
import json
import os
import subprocess
import sys


def ctest_tests(preset):
    out = subprocess.run(["ctest", "--preset", preset, "--show-only=json-v1"], check=True, capture_output=True)
    return json.loads(out.stdout.decode("utf-8-sig"))["tests"]


def run(name, cmd, cwd, env, timeout):
    print(f"::group::{name}: {' '.join(cmd)}", flush=True)
    try:
        r = subprocess.run(cmd, cwd=cwd, env=env, timeout=timeout, stdout=sys.stdout, stderr=subprocess.STDOUT)
        print(f"[run_tests] {name}: exit code 0x{r.returncode & 0xFFFFFFFF:08x}", flush=True)
    except subprocess.TimeoutExpired:
        print(f"[run_tests] {name}: timeout after {timeout} s", flush=True)
    print("::endgroup::", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", required=True)
    ap.add_argument("--harness", required=True)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--cdb", default="")
    ap.add_argument("--cdb-also", action="append", default=[])
    ap.add_argument("--cdb-eh", action="append", default=[])
    ap.add_argument("patterns", nargs="+")
    args = ap.parse_args()

    tests = ctest_tests(args.preset)
    first = True
    for pattern in args.patterns:
        test = next((t for t in tests if fnmatch.fnmatchcase(t["name"], pattern)), None)
        if test is None or "command" not in test:
            print(f"[run_tests] no test matches {pattern}", flush=True)
            continue
        props = {p["name"]: p["value"] for p in test.get("properties", [])}
        env = dict(os.environ)
        for item in props.get("ENVIRONMENT", []):
            key, _, value = item.partition("=")
            env[key] = value
        cwd = props.get("WORKING_DIRECTORY")
        cmd = test["command"]
        run(f"debug_run {test['name']}", [args.harness] + cmd, cwd, env, args.timeout)
        also = any(fnmatch.fnmatchcase(test["name"], p) for p in args.cdb_also)
        eh = any(fnmatch.fnmatchcase(test["name"], p) for p in args.cdb_eh)
        if (first or also or eh) and args.cdb and os.path.isfile(args.cdb):
            # At the first break after the start: the last event, the exception record, the registers,
            # the stack and the verifier state.
            script = "g; .lastevent; .exr -1; r; kv 40; !avrf; q"
            if eh:
                # Each first-chance C++ exception and access violation writes its record and stack,
                # and the process continues. The stop at the end is a second-chance exception or
                # the exit of the process.
                script = ('sxe -c ".echo [cdb-eh] C++ exception;.exr -1;kc 25;gc" eh; '
                          'sxe -c ".echo [cdb-eh] access violation;.exr -1;r;kv 30;gc" av; ' + script)
            cache = os.path.join(os.environ.get("RUNNER_TEMP", os.getcwd()), "symbols")
            sym = f"{os.path.dirname(cmd[0])};srv*{cache}*https://msdl.microsoft.com/download/symbols"
            run(f"cdb {test['name']}", [args.cdb, "-lines", "-y", sym, "-c", script] + cmd, cwd, env, args.timeout)
        first = False
    return 0


if __name__ == "__main__":
    sys.exit(main())
