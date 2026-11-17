#!/usr/bin/env python3
"""Benchmark of hot-ls over a generated workspace, with the background index on and off.

Run with: python3 -I tools/lsbench/index_bench.py --hot-ls PATH [--json out.json]
The tool generates the workspace (default: a temporary directory), starts hot-ls and measures:
the time until the index is ready, one cold sample of rename and references (first request,
index not ready), and warm samples of each request kind (median and p95 in ms).
It uses the client of lsbench.py. Lines and columns here are 0-based (LSP).
"""
import argparse, importlib.util, json, os, shutil, statistics, subprocess, sys, tempfile, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
spec = importlib.util.spec_from_file_location("lsbench", HERE / "lsbench.py")
lsbench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lsbench)

GROUPS = 10
PER_GROUP = 30

BASE = """using "Actor.h";

class Base : Actor {
    float energy = 1.0;

    void Pulse(float amount) {
        this.energy = this.energy + amount;
    }
}

float Shared(float x) {
    return x * 2.0;
}
"""

LIB = """class Lib{g} {{
    float k = 1.0;

    float Calc(float a) {{
        return a + this.k;
    }}
}}

float Util{g}(float v) {{
    return v * 2.0;
}}
"""

UNIT = """using "Actor.h";
using "../common/base.hot";
using "../lib/lib{g}.hot";

class Unit{n} : Base {{
    float hp{n} = 1.0;
    Lib{g} helper;

    override void Tick(float dt) {{
        this.Move(Vec3(dt, 0, 0));
        this.Pulse(dt);
        float v = Util{g}(this.hp{n}) + Shared(dt);
        float w = this.helper.Calc(dt);
    }}

    void Work{n}(float amount) {{
        this.hp{n} = this.hp{n} + amount;
        this.Pulse(amount);
    }}
}}
"""


def generate(root):
    root = Path(root)
    (root / "common").mkdir(parents=True, exist_ok=True)
    (root / "lib").mkdir(exist_ok=True)
    (root / "common" / "base.hot").write_text(BASE)
    for g in range(GROUPS):
        (root / "lib" / ("lib%d.hot" % g)).write_text(LIB.format(g=g))
        d = root / ("g%d" % g)
        d.mkdir(exist_ok=True)
        for i in range(PER_GROUP):
            n = g * PER_GROUP + i
            (d / ("unit%d.hot" % n)).write_text(UNIT.format(g=g, n=n))


def pos_of(text, word, nth=0):
    at = -1
    for _ in range(nth + 1):
        at = text.index(word, at + 1)
    line = text.count("\n", 0, at)
    col = at - (text.rfind("\n", 0, at) + 1)
    return {"line": line, "character": col}


def pct(v, p):
    s = sorted(v)
    return s[min(len(s) - 1, int(round(p / 100.0 * (len(s) - 1))))]


class Run:
    def __init__(self, a, root, index_on):
        self.a = a
        self.root = str(root)
        self.index_on = index_on
        self.rows = []

    def start(self):
        extra = ["-I", str(ROOT / "include"), "-I", str(ROOT / "tests" / "engine"), "--clangd", self.a.clangd]
        if not self.index_on:
            extra.append("--no-background-index")
        self.idx = tempfile.mkdtemp(prefix="idx-", dir=self.a.scratch)
        t0 = time.perf_counter()
        self.c = lsbench.Client(os.path.abspath(self.a.hot_ls), self.root, self.idx, extra)
        return t0

    def wait_ready(self, t0, limit=600):
        while time.perf_counter() - t0 < limit:
            st = self.c.stats()
            ix = st.get("index") or {}
            if ix.get("ready") and ix.get("pending", 0) == 0:
                return (time.perf_counter() - t0) * 1000.0, st
            time.sleep(0.05)
        return None, self.c.stats()

    def rss_mb(self):
        try:
            for line in open("/proc/%d/status" % self.c.proc.pid):
                if line.startswith("VmRSS:"):
                    return round(int(line.split()[1]) / 1024.0, 1)
        except OSError:
            pass
        return None

    def stop(self):
        self.c.close()

    def doc(self, rel):
        p = os.path.join(self.root, rel)
        uri, text, n, t0 = self.c.open(p)
        return uri, text

    def sample(self, name, fn, count):
        ms = []
        for _ in range(count):
            ms.append(fn())
            if ms[-1] > self.a.slow_ms and len(ms) >= 3:
                break
        res = (getattr(self, "last", None) or {})
        err = res.get("error")
        val = res.get("result")
        size = "error: %s" % err.get("message") if err else ("null" if val is None else (len(val) if isinstance(val, list) else (len(val.get("changes", {})) if isinstance(val, dict) and "changes" in val else (len(val.get("items", [])) if isinstance(val, dict) and "items" in val else "object"))))
        self.last = None
        row = {"name": name, "n": len(ms), "result": size, "median": round(statistics.median(ms), 1), "p95": round(pct(ms, 95), 1),
               "max": round(max(ms), 1)}
        self.rows.append(row)
        print("  %-34s n=%-3d median %8.1f  p95 %8.1f  max %8.1f  result %s" % (name, len(ms), row["median"], row["p95"], row["max"], size), flush=True)
        return row

    def req(self, method, uri, pos, extra=None):
        params = {"textDocument": {"uri": uri}, "position": pos}
        params.update(extra or {})
        def fn():
            ms, msg = self.c.request(method, params, timeout=600)
            self.last = msg
            return ms
        return fn


def cold(a, root, index_on, which):
    r = Run(a, root, index_on)
    t0 = r.start()
    uri, text = r.doc("lib/lib3.hot")
    pos = pos_of(text, "Lib3")
    if which == "rename":
        ms, msg = r.c.request("textDocument/rename", {"textDocument": {"uri": uri}, "position": pos, "newName": "LibX3"}, timeout=900)
    else:
        ms, msg = r.c.request("textDocument/references", {"textDocument": {"uri": uri}, "position": pos,
                                                          "context": {"includeDeclaration": True}}, timeout=900)
    st = r.c.stats()
    r.stop()
    err = msg.get("error")
    print("  cold %-10s index %-3s %9.1f ms %s" % (which, "on" if index_on else "off", ms, "ERROR %s" % err if err else ""), flush=True)
    return {"name": "cold " + which, "ms": round(ms, 1), "error": err and err.get("message"),
            "entryHits": st.get("entryHits"), "entryMisses": st.get("entryMisses")}


def warm(a, root, index_on):
    r = Run(a, root, index_on)
    t0 = r.start()
    # With the index off the server has no index state to wait for.
    ready_ms, st0 = r.wait_ready(t0) if index_on else (None, {})
    rss_ready = r.rss_mb()
    print("  index ready: %s ms, server RSS %s MB" % (None if ready_ms is None else round(ready_ms), rss_ready), flush=True)
    lib_uri, lib_text = r.doc("lib/lib3.hot")
    base_uri, base_text = r.doc("common/base.hot")
    unit_rel = "g3/unit90.hot"
    unit_uri, unit_text = r.doc(unit_rel)
    N = a.samples
    ctx = {"context": {"includeDeclaration": True}}
    # A first request of each kind warms clangd and the document.
    for m, u, p, e in [("textDocument/hover", unit_uri, pos_of(unit_text, "helper", 1), None),
                       ("textDocument/references", lib_uri, pos_of(lib_text, "Lib3"), ctx)]:
        r.c.request(m, {"textDocument": {"uri": u}, "position": p, **(e or {})}, timeout=900)
    r.sample("hover (script member)", r.req("textDocument/hover", unit_uri, pos_of(unit_text, "helper", 1)), N)
    r.sample("hover (field use)", r.req("textDocument/hover", unit_uri, pos_of(unit_text, "hp90", 1)), N)
    r.sample("hover (native member Move)", r.req("textDocument/hover", unit_uri, pos_of(unit_text, "Move")), N)
    r.sample("definition (free function)", r.req("textDocument/definition", unit_uri, pos_of(unit_text, "Util3")), N)
    r.sample("completion (this.)", r.req("textDocument/completion", unit_uri, pos_of(unit_text, "this.Pulse", 0) | {"character": pos_of(unit_text, "this.Pulse")["character"] + 5}), N)
    r.sample("references Lib3 (30 files)", r.req("textDocument/references", lib_uri, pos_of(lib_text, "Lib3"), ctx), N)
    r.sample("references Calc (30 files)", r.req("textDocument/references", lib_uri, pos_of(lib_text, "Calc"), ctx), N)
    r.sample("references Base.Pulse (300 files)", r.req("textDocument/references", base_uri, pos_of(base_text, "Pulse"), ctx), N)
    r.sample("prepareRename Lib3", r.req("textDocument/prepareRename", lib_uri, pos_of(lib_text, "Lib3")), N)
    r.sample("rename Lib3 (30 files)", r.req("textDocument/rename", lib_uri, pos_of(lib_text, "Lib3"), {"newName": "LibX3"}), N)
    r.sample("rename Calc (30 files)", r.req("textDocument/rename", lib_uri, pos_of(lib_text, "Calc"), {"newName": "CalcX"}), N)
    r.sample("prepareRename Base.Pulse", r.req("textDocument/prepareRename", base_uri, pos_of(base_text, "Pulse")), N)
    r.sample("rename Base.Pulse (300 files)", r.req("textDocument/rename", base_uri, pos_of(base_text, "Pulse"), {"newName": "PulseX"}), N)
    def wsym():
        ms, msg = r.c.request("workspace/symbol", {"query": "Unit1"}, timeout=600)
        r.last = msg
        return ms
    r.sample("workspace/symbol Unit1", wsym, N)
    # Diagnostics after an edit: the time from didChange to the next publishDiagnostics.
    version = [1]
    def edit():
        version[0] += 1
        n = r.c.diag_count(unit_uri)
        t0 = time.perf_counter()
        r.c.notify("textDocument/didChange", {"textDocument": {"uri": unit_uri, "version": version[0]},
                                              "contentChanges": [{"text": unit_text + "// edit %d\n" % version[0]}]})
        t = r.c.wait_diag(unit_uri, n, version[0], timeout=120)
        return (t - t0) * 1000.0 if t else 120000.0
    r.sample("diagnostics after an edit", edit, N)
    final = r.c.stats()
    rss_end = r.rss_mb()
    print("  server RSS at the end: %s MB" % rss_end, flush=True)
    r.stop()
    return {"ready_ms": ready_ms and round(ready_ms), "rss_ready_mb": rss_ready, "rss_end_mb": rss_end, "rows": r.rows,
            "stats": {k: final.get(k) for k in ("analyzed", "tableAnalyses", "loaded", "entryHits", "entryMisses", "translations")}}


def edit_bench(a, root, rel, label, cdb=None):
    """Typing latency: 12 edits that insert one char. Time to the diagnostics publish, to a hover
    after the publish, and to a hover sent right after the didChange (the blocking case)."""
    import re
    r = Run(a, root, True)
    t0 = r.start() if not cdb else None
    if cdb:
        idx = tempfile.mkdtemp(prefix="idx-", dir=a.scratch)
        t0 = time.perf_counter()
        r.c = lsbench.Client(os.path.abspath(a.hot_ls), str(root), idx, ["--compile-commands-dir", cdb])
    r.wait_ready(t0)
    uri, text = r.doc(rel)
    r.c.wait_diag(uri, 0, timeout=300)
    m = re.search(r"^\s*(?:export\s+)?(?:class|def|struct|enum)\s+(\w+)", text, re.M) or re.search(r"\w+", text)
    line = text.count("\n", 0, m.start(m.lastindex or 0))
    hpos = {"line": line, "character": m.start(m.lastindex or 0) - text.rfind("\n", 0, m.start(m.lastindex or 0)) - 1}
    hov = {"textDocument": {"uri": uri}, "position": hpos}
    r.c.request("textDocument/hover", hov, timeout=300)
    ver = [1]
    out = {"diag": [], "hover_after": [], "hover_direct": [], "diag_direct": []}
    def change():
        ver[0] += 1
        cur = text.rstrip("\n") + "\n//" + "x" * (ver[0] - 1) + "\n"
        n = r.c.diag_count(uri)
        t = time.perf_counter()
        r.c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": ver[0]}, "contentChanges": [{"text": cur}]})
        return n, t
    for direct in (False, True):
        for _ in range(12):
            n, t = change()
            if direct:
                ms, msg = r.c.request("textDocument/hover", hov, timeout=300)
                out["hover_direct"].append(ms)
                d = r.c.wait_diag(uri, n, ver[0], timeout=120)
                out["diag_direct"].append((d - t) * 1000.0)
            else:
                d = r.c.wait_diag(uri, n, ver[0], timeout=120)
                out["diag"].append((d - t) * 1000.0)
                out["hover_after"].append(r.c.request("textDocument/hover", hov, timeout=300)[0] + (d - t) * 1000.0)
    lines = text.count("\n")
    print("%s (%d lines) load %.1f" % (label, lines, os.getloadavg()[0]))
    for k, v in out.items():
        print("  %-13s median %7.1f  p95 %7.1f  max %7.1f" % (k, statistics.median(v), pct(v, 95), max(v)), flush=True)
    r.stop()


def edit_kinds_bench(a, root, rel, cdb):
    """The phases of didChange to publishDiagnostics for 4 kinds of edit, 12 runs each. The edits
    change the text in memory only. hot/indexStats "lastUpdate" gives the phases of the server."""
    import re
    r = Run(a, root, True)
    idx = tempfile.mkdtemp(prefix="idx-", dir=a.scratch)
    t0 = time.perf_counter()
    r.c = lsbench.Client(os.path.abspath(a.hot_ls), str(root), idx, ["--compile-commands-dir", cdb] if cdb else
                         ["-I", str(ROOT / "include"), "-I", str(ROOT / "tests" / "engine"), "--clangd", a.clangd])
    r.wait_ready(t0)
    uri, text = r.doc(rel)
    r.c.wait_diag(uri, 0, timeout=300)
    # A statement in a function body: a line with 8 spaces, a name, "=", a number and ";".
    m = re.search(r"^( {8,})([\w.]+) = (\d+)(\.\d+)?;[ \t]*$", text, re.M)
    if not m:
        print("%s: no literal line in a body" % rel)
        r.stop()
        return
    lit_line = m.group(0)
    ind = m.group(1)
    members = ["Length", "LengthSquared", "Normalized", "Abs", "ToHash", "IsNaN", "IsInf", "ToString", "x_", "y_", "z_", "ToVector2"]
    ver = [1]
    def run_kind(label, make):
        rows = []
        for i in range(12):
            for tag, cur in make(i):
                ver[0] += 1
                n = r.c.diag_count(uri)
                seq0 = r.c.stats().get("lastUpdate", {}).get("seq", 0)
                t = time.perf_counter()
                r.c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": ver[0]}, "contentChanges": [{"text": cur}]})
                d = r.c.wait_diag(uri, n, ver[0], timeout=120)
                total = (d - t) * 1000.0 if d else 120000.0
                lu = {}
                for _ in range(100):
                    lu = r.c.stats().get("lastUpdate", {})
                    if lu.get("seq", 0) > seq0:
                        break
                    time.sleep(0.01)
                lu = dict(lu)
                if os.environ.get("EDIT_REQ") and d:
                    # A hover just after the diagnostics, then a completion just after a second didChange.
                    line_no = cur[:cur.find(lit_line.strip())].count("\n") if lit_line.strip() in cur else 0
                    pos = {"line": line_no, "character": len(ind) + 1}
                    tdoc = {"uri": uri}
                    t = time.perf_counter()
                    r.c.request("textDocument/hover", {"textDocument": tdoc, "position": pos}, timeout=120)
                    lu["hoverMs"] = (time.perf_counter() - t) * 1000.0
                    ver[0] += 1
                    n2 = r.c.diag_count(uri)
                    r.c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": ver[0]},
                                                          "contentChanges": [{"text": cur + "\n//c%d\n" % ver[0]}]})
                    t = time.perf_counter()
                    r.c.request("textDocument/completion", {"textDocument": tdoc, "position": pos}, timeout=120)
                    lu["compMs"] = (time.perf_counter() - t) * 1000.0
                    r.c.wait_diag(uri, n2, ver[0], timeout=120)
                rows.append((tag, total, lu))
        return rows
    def kind_a(i):
        yield "a comment", text.rstrip("\n") + "\n//" + "x" * (i + 1) + "\n"
    def kind_b(i):
        yield "b literal", text.replace(lit_line, "%s%s = %d;" % (ind, m.group(2), 7001 + i), 1)
    def kind_c(i):
        yield "c unknown", text.replace(lit_line, lit_line + "\n%sint zq%d = zzUnknown%d + 1;" % (ind, i, i), 1)
        yield "c fix", text
    def kind_d(i):
        yield "d native", text.replace(lit_line, lit_line + "\n%sVector3 zv%d;\n%sauto zr%d = zv%d.%s%s;" % (
            ind, i, ind, i, i, members[i], "()" if members[i][-1] != "_" else ""), 1)
    print("%s (%d lines) load %.1f, median ms of 12" % (rel, text.count("\n"), os.getloadavg()[0]))
    print("  %-10s %7s %7s %7s %7s %7s %6s %9s %8s" % ("edit", "front", "lower", "check", "emit", "parse", "passes", "post-work", "total"))
    only = os.environ.get("EDIT_KINDS", "abcd")
    for kind in [k for k in (kind_a, kind_b, kind_c, kind_d) if k.__name__[-1] in only]:
        if kind is kind_b:
            print("BEGIN", flush=True)
        perf = None
        if os.environ.get("PERF_OUT") and kind is kind_b:
            perf = subprocess.Popen(["perf", "record", "-e", "cpu-clock:u", "-F", "499", "-o", os.environ["PERF_OUT"], "-p", str(r.c.proc.pid)],
                                    stdout=subprocess.DEVNULL, stderr=open(os.environ["PERF_OUT"] + ".err", "w"))
            time.sleep(1)
        rows = run_kind(kind.__name__, kind)
        if perf:
            perf.terminate()
            perf.wait()
        for tag in sorted({x[0] for x in rows}):
            sel = [x for x in rows if x[0] == tag]
            med = lambda k: statistics.median([x[2].get(k, 0) for x in sel])
            print("  %-10s %7.1f %7.1f %7.1f %7.1f %7.1f %6.1f %9.1f %8.1f" % (
                tag, med("frontMs"), med("lowerMs"), med("checkMs"), med("emitMs"), med("parseMs"), med("passes"),
                med("postWorkMs"), statistics.median([x[1] for x in sel])), flush=True)
            p95 = lambda vals: sorted(vals)[min(len(vals) - 1, int(round(0.95 * (len(vals) - 1))))]
            print("    total median %.1f p95 %.1f" % (statistics.median([x[1] for x in sel]), p95([x[1] for x in sel])), flush=True)
            if os.environ.get("EDIT_REQ"):
                for key in ("hoverMs", "compMs"):
                    vals = [x[2].get(key, 0) for x in sel]
                    print("    %s median %.1f p95 %.1f" % (key, statistics.median(vals), p95(vals)), flush=True)
            print("    import read/parse/view %.1f %.1f %.1f | classes foreign %.1f ms (%d) own %.1f ms (%d) | check_class %.1f" % (
                med("importReadMs"), med("importParseMs"), med("importViewMs"), med("foreignClassMs"), med("foreignClasses"),
                med("ownClassMs"), med("ownClasses"), med("checkClassMs")), flush=True)
            print("    front steps %s" % " ".join("%.1f" % med("frontStep%dMs" % i) for i in range(13)), flush=True)
            for who in ("foreign", "own"):
                print("    steps %-7s pre/base/check/const/slots/shell/state/rest %s" % (
                    who, " ".join("%.1f" % med("%sStep%dMs" % (who, i)) for i in range(8))), flush=True)
                print("    sub %-7s const/null/structtext/fieldinit/table/gen13 %s" % (who, " ".join("%.1f" % med("%sStep%dMs" % (who, i)) for i in (32, 33, 25, 34, 35, 36, 37, 31))), flush=True)
    r.stop()


def main():
    global GROUPS
    ap = argparse.ArgumentParser()
    ap.add_argument("--hot-ls", required=True)
    ap.add_argument("--clangd", default="clangd")
    ap.add_argument("--workspace", help="the directory for the generated workspace (default: temporary)")
    ap.add_argument("--samples", type=int, default=30)
    ap.add_argument("--groups", type=int, default=GROUPS, help="the number of groups of %d units (default %d)" % (PER_GROUP, GROUPS))
    ap.add_argument("--slow-ms", type=float, default=20000, help="stop the samples of a request after this slow sample")
    ap.add_argument("--mode", choices=["on", "off", "both"], default="both")
    ap.add_argument("--json")
    ap.add_argument("--edit-bench", action="store_true", help="measure typing edits (didChange, diagnostics, hover) and exit")
    ap.add_argument("--edit-kinds", action="store_true", help="with --edit-bench: 4 kinds of edit, the phases of the server")
    ap.add_argument("--edit-root")
    ap.add_argument("--edit-file")
    ap.add_argument("--edit-cdb")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    GROUPS = a.groups
    a.scratch = tempfile.mkdtemp(prefix="indexbench-")
    root = Path(a.workspace) if a.workspace else Path(a.scratch) / "ws"
    root.mkdir(parents=True, exist_ok=True)
    generate(root)
    if a.edit_bench:
        if a.edit_kinds:
            edit_kinds_bench(a, Path(a.edit_root), a.edit_file, a.edit_cdb)
        elif a.edit_root:
            edit_bench(a, Path(a.edit_root), a.edit_file, a.edit_file, a.edit_cdb)
        else:
            edit_bench(a, root, a.edit_file or "g3/unit90.hot", a.edit_file or "g3/unit90.hot")
        shutil.rmtree(a.scratch, ignore_errors=True)
        return
    load0 = os.getloadavg()
    result = {"load_start": load0, "scripts": GROUPS * PER_GROUP + GROUPS + 1, "modes": {}}
    for index_on in ([True, False] if a.mode == "both" else [a.mode == "on"]):
        key = "on" if index_on else "off"
        print("== index %s (load %.2f) ==" % (key, os.getloadavg()[0]), flush=True)
        # A saved index of an earlier run must not make the cold run warm: each Run has a new index dir.
        c = {w: cold(a, root, index_on, w) for w in ("references", "rename")}
        w = warm(a, root, index_on)
        result["modes"][key] = {"cold": c, "warm": w, "load_end": os.getloadavg()}
    result["load_end"] = os.getloadavg()
    result["loaded"] = max(result["load_start"][0], result["load_end"][0]) > 4
    print("load average start %s end %s%s" % (load0, result["load_end"], "  LOADED" if result["loaded"] else ""))
    if a.json:
        Path(a.json).write_text(json.dumps(result, indent=1))
    shutil.rmtree(a.scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
