#!/usr/bin/env python3
"""HOT-57: the latency of the first completion at a qualified native name (`ea.|`).

Run with: python3 -I tools/lsbench/first_completion_bench.py --hot-ls PATH --workspace DIR --cdb DIR \
    --file REL --line N --char N --mode plain|typed [--index-dir DIR] [--limit-ms 1000]
Line and char are 0-based (LSP). Modes:
  plain: the server starts with an empty index. The bench asks completion at the existing `ea.` position.
  typed: the server loads the index of an earlier run (give the same --index-dir). The bench replaces
         the line by `    ea.` with didChange and asks completion at the end of that text.
The bench asks twice. It prints the latency and the HOT_LS_TIMING lines of each request.
Exit code 1: the first latency is above --limit-ms.
"""
import argparse, importlib.util, os, sys, tempfile, time
from pathlib import Path

spec = importlib.util.spec_from_file_location("lsbench", Path(__file__).with_name("lsbench.py"))
lsbench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lsbench)


def load():
    return open("/proc/loadavg").read().split()[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hot-ls", required=True)
    ap.add_argument("--workspace", required=True)
    ap.add_argument("--cdb", required=True, help="the directory of compile_commands.json")
    ap.add_argument("--file", required=True, help="path relative to the workspace")
    ap.add_argument("--line", type=int, required=True)
    ap.add_argument("--char", type=int, required=True)
    ap.add_argument("--mode", choices=["plain", "typed"], default="plain")
    ap.add_argument("--index-dir", help="default: a new temporary directory")
    ap.add_argument("--limit-ms", type=float, default=1000.0)
    ap.add_argument("--quiet", type=float, default=30.0, help="seconds to wait after the first diagnostics")
    a = ap.parse_args()
    if a.mode == "typed" and not a.index_dir:
        ap.error("--mode typed needs the --index-dir of an earlier run")
    idx = a.index_dir or tempfile.mkdtemp(prefix="h57_idx_")
    tf = tempfile.NamedTemporaryFile(prefix="h57_timing_", delete=False).name
    os.environ["HOT_LS_TIMING"] = tf
    t0 = time.time()
    c = lsbench.Client(a.hot_ls, a.workspace, idx,
                       ["--compile-commands-dir", a.cdb, "--hosts-dir", a.workspace + "/.cache/hot/hosts"])
    while time.time() - t0 < 1500:
        ix = c.stats().get("index") or {}
        if ix.get("ready") and ix.get("pending", 0) == 0:
            break
        time.sleep(0.5)
    print("index ready after %.0f s, load %s" % (time.time() - t0, load()), flush=True)
    uri, text, _, _ = c.open(os.path.join(a.workspace, a.file))
    c.wait_diag(uri, 0, timeout=300)
    time.sleep(a.quiet)
    print("load before requests", open("/proc/loadavg").read().strip(), flush=True)
    char = a.char
    if a.mode == "typed":
        lines = text.split("\n")
        lines[a.line] = "    ea."
        char = len(lines[a.line])
        c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 2},
                                            "contentChanges": [{"text": "\n".join(lines)}]})
    rows = []
    for tag in ("first", "second"):
        off = os.path.getsize(tf)
        t = time.perf_counter()
        _, m = c.request("textDocument/completion", {"textDocument": {"uri": uri},
                                                     "position": {"line": a.line, "character": char}})
        ms = (time.perf_counter() - t) * 1000
        r = m.get("result") or {}
        n = len(r.get("items", []) if isinstance(r, dict) else r)
        time.sleep(1)
        with open(tf) as f:
            f.seek(off)
            rows.append((tag, ms, n, f.read().strip()))
    c.close()
    print("\nmode=%s  limit=%.0f ms  load=%s" % (a.mode, a.limit_ms, load()))
    print("%-8s %10s %6s" % ("request", "ms", "items"))
    for tag, ms, n, _ in rows:
        print("%-8s %10.0f %6d" % (tag, ms, n))
    for tag, _, _, phases in rows:
        print("-- phases of the %s request --\n%s" % (tag, phases))
    ok = rows[0][1] <= a.limit_ms
    print("RESULT %s: first %.0f ms %s limit %.0f ms" % ("green" if ok else "RED", rows[0][1], "<=" if ok else ">", a.limit_ms))
    return 0 if ok else 1


sys.exit(main())
