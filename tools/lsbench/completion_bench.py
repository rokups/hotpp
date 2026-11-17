"""HOT-51: the latency of a completion request, scenarios A and B, 3 places.

Run with: HOT_LS_TIMING=/tmp/t.txt python3 -I tools/lsbench/completion_bench.py --hot-ls PATH --root DIR --cdb DIR --file REL
Scenario A: didChange, wait for publishDiagnostics, then completion.
Scenario B: didChange and completion with no wait.
It prints the median of each PHASE label and the PROBE lines of the timing file.
The text of the file is only changed in memory (a comment at the end).
"""
import argparse, importlib.util, os, re, statistics, sys, tempfile, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("lsbench", HERE / "lsbench.py")
lsbench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lsbench)


def places(text):
    """The cursor places (line, character): a plain name, after a dot, a type."""
    lines = text.split("\n")
    out = {}
    for i, ln in enumerate(lines):
        if "plain" not in out:
            m = re.match(r"^( {8,})([a-z]\w*) = ", ln)
            if m:
                out["plain"] = (i, len(m.group(1)) + 1)
        if "dot" not in out:
            m = re.match(r"^ {8,}[a-z]\w*(?:\.\w+)*?\.", ln)
            m = re.match(r"^( {8,}[a-z]\w* = [A-Za-z_]\w*\.)\w", ln) or m
            if m and i > 40:
                out["dot"] = (i, len(m.group(1)) if m.lastindex else m.end())
        if "type" not in out:
            m = re.match(r"^( {8,})([A-Z]\w+) \w+ = ", ln)
            if m:
                out["type"] = (i, len(m.group(1)) + 1)
    return out


def read_new(path, off):
    with open(path) as f:
        f.seek(off)
        return f.read(), f.tell()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hot-ls", required=True)
    ap.add_argument("--clangd", default="clangd")
    ap.add_argument("--root", required=True)
    ap.add_argument("--cdb", required=True)
    ap.add_argument("--file", required=True)
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--scenarios", default="AB")
    a = ap.parse_args()
    tfile = os.environ["HOT_LS_TIMING"]
    idx = tempfile.mkdtemp(prefix="idx-")
    c = lsbench.Client(os.path.abspath(a.hot_ls), a.root, idx, ["--compile-commands-dir", a.cdb, "--clangd", a.clangd])
    t0 = time.time()
    while time.time() - t0 < 600:
        ix = c.stats().get("index") or {}
        if ix.get("ready") and ix.get("pending", 0) == 0:
            break
        time.sleep(0.1)
    uri, text, n, _ = c.open(os.path.join(a.root, a.file))
    c.wait_diag(uri, 0, timeout=300)
    pl = places(text)
    print("places", pl, flush=True)
    ver = 1
    open(tfile, "a").close()
    off = os.path.getsize(tfile)
    for sc in a.scenarios:
        for name, (line, ch) in pl.items():
            lat, phases, probes = [], {}, {}
            for i in range(a.runs + 1):  # the first run warms the caches: not counted
                ver += 1
                cur = text + "\n// c%d\n" % ver
                n = c.diag_count(uri)
                _, off = read_new(tfile, off)
                c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": ver}, "contentChanges": [{"text": cur}]})
                if sc == "A":
                    c.wait_diag(uri, n, ver, timeout=120)
                t = time.perf_counter()
                c.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": line, "character": ch}}, timeout=120)
                ms = (time.perf_counter() - t) * 1000.0
                c.wait_diag(uri, n, ver, timeout=120)
                time.sleep(0.2)
                new, off = read_new(tfile, off)
                if i == 0:
                    continue
                lat.append(ms)
                ph = {}
                for ln in new.splitlines():
                    m = re.match(r"PHASE (.+?)\s+([\d.]+) ms$", ln)
                    if m:
                        ph[m.group(1)] = ph.get(m.group(1), 0) + float(m.group(2))
                    elif ln.startswith("PROBE "):
                        probes.setdefault(ln.split(" passes=")[0][6:], []).append(ln)
                for k, v in ph.items():
                    phases.setdefault(k, []).append(v)
            print("SCEN %s %s latency_median %.1f" % (sc, name, statistics.median(lat)), flush=True)
            for k, v in phases.items():
                print("SCEN %s %s PHASE %s median %.1f n=%d" % (sc, name, k, statistics.median(v), len(v)), flush=True)
            for k, v in probes.items():
                print("SCEN %s %s %s" % (sc, name, v[len(v) // 2]), flush=True)
                print("SCEN %s %s %s count=%d" % (sc, name, k, len(v)), flush=True)
    c.close()


if __name__ == "__main__":
    main()
