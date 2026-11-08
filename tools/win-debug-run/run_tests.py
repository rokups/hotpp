#!/usr/bin/env python3
# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.
"""Runs some ctest tests again under debug_run.exe (a small debugger), for CI on Windows.

For each pattern, the script runs the first test whose name matches it (fnmatch). It uses the
command, the working directory and the environment of the test from ctest. The output goes into
the job log. With --cdb, the first test also runs under cdb.exe.

Usage: run_tests.py --preset <ctest preset> --harness <debug_run.exe> [--timeout s] [--cdb path] pattern...
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
        if first and args.cdb and os.path.isfile(args.cdb):
            # At the first break after the start: the last event, the stack and the verifier state.
            script = "g; .lastevent; kv 40; !avrf; q"
            sym = os.path.dirname(cmd[0])
            run(f"cdb {test['name']}", [args.cdb, "-lines", "-y", sym, "-c", script] + cmd, cwd, env, args.timeout)
        first = False
    return 0


if __name__ == "__main__":
    sys.exit(main())
