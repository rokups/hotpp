#!/usr/bin/env python3
"""Corpus check of the lite document (HOT-43).

For each script of a workspace, the check asks hover and completion at some places inside own bodies with
HOT_LS_LITE_DOC=1 and HOT_LS_LITE_CHECK=1. The server asks the full document too and logs each difference.
The script prints the differences and the counters of hot/indexStats. Exit code 1 when a difference exists.

usage: lite_corpus.py --hot-ls PATH --root DIR --cdb DIR [--per-script 5] [--limit N] [--clangd clangd]
"""
import argparse
import os
import re
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lsbench  # noqa: E402

WORD = re.compile(r"[A-Za-z_]\w*")


def places(lines, count):
    """Places inside bodies: the end of a word that follows a dot, or the start of a call."""
    found = []
    for n, line in enumerate(lines):
        if not line.startswith("        ") or line.strip().startswith(("//", "/*", "*")):
            continue
        for m in re.finditer(r"\.([A-Za-z_]\w*)", line):
            found.append((n, m.start(1) + 1, m.end(1)))
    if not found:
        return []
    step = max(1, len(found) // count)
    return found[::step][:count]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hot-ls", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--cdb", required=True)
    ap.add_argument("--clangd", default="clangd")
    ap.add_argument("--per-script", type=int, default=5)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    os.environ["HOT_LS_SPLIT_SHELL"] = "1"
    os.environ["HOT_LS_LITE_DOC"] = "1"
    os.environ["HOT_LS_LITE_CHECK"] = "1"
    root = Path(a.root)
    scripts = sorted(p for p in root.rglob("*.hot") if not any(x.startswith(".") for x in p.relative_to(root).parts) and a.only in str(p))
    if a.limit:
        scripts = scripts[: a.limit]
    idx = tempfile.mkdtemp(prefix="lite_corpus_")
    c = lsbench.Client(os.path.abspath(a.hot_ls), str(root), idx,
                       ["--compile-commands-dir", a.cdb, "--clangd", a.clangd])
    asked = 0
    for path in scripts:
        text = path.read_text()
        lines = text.split("\n")
        uri = path.as_uri()
        c.open(str(path), text)
        time.sleep(1.0)
        for n, b, e in places(lines, a.per_script):
            for _ in range(2):  # the first request warms the document, the second can use the lite document
                c.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": n, "character": b}}, 120)
            c.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": n, "character": e}}, 120)
            asked += 1
        c.notify("textDocument/didClose", {"textDocument": {"uri": uri}})
        print("%-70s %d places" % (str(path.relative_to(root)), min(a.per_script, len(places(lines, a.per_script)))), flush=True)
    _, stats = c.request("hot/indexStats", {}, 120)
    r = stats.get("result", {})
    print("places asked: %d  liteHits=%s liteFallback=%s liteMismatch=%s" %
          (asked, r.get("liteHits"), r.get("liteFallback"), r.get("liteMismatch")))
    for m in c.lite_logs:
        print("MISMATCH", m[:600])
    c.proc.terminate()
    return 1 if c.lite_logs or r.get("liteMismatch") else 0


if __name__ == "__main__":
    sys.exit(main())
