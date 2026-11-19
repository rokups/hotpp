#!/usr/bin/env python3
"""Benchmark driver for the language server hot-ls.

Run with: python3 -I tools/lsbench/lsbench.py --hot-ls PATH --preset bsw
Each scenario starts its own server process. Scenarios run one after another.
Lines and columns on the command line are 1-based.
"""
import argparse, json, os, shutil, statistics, subprocess, sys, tempfile, threading, time
from pathlib import Path

BSW_ROOT = "/home/rk/src/rbfx/BiteSizedWar"
PRESETS = {
    "bsw": {
        "root": BSW_ROOT,
        "class_file": "Source/Game/Scripts/Weapons/WeaponHolder.hot",
        "class_pos": (25, 7),
        "hover_pos": (25, 7),
        "native_file": "Source/Game/Scripts/Weapons/WeaponView.hot",
        "native_pos": (333, 13),
        "cdb": ".cache/hot",
    }
}
SCENARIOS = ["analyze", "refs", "native", "typing", "hover", "cancel", "rml"]
RML_GOAL_MS = 500.0    # goal: p95 of each request kind
RML_LIMIT_MS = 1000.0  # hard limit: any request


def now():
    return time.perf_counter()


class Client:
    """One hot-ls process with a reader thread. Requests do not block each other."""

    def __init__(self, exe, root, index_dir, extra):
        self.root = root
        self.cond = threading.Condition()
        self.replies = {}      # id -> (time, message)
        self.diag = {}         # uri -> list of (time, version, count of diagnostics)
        self.messages = []     # window/showMessage texts
        self.lite_logs = []    # window/logMessage texts about the lite document
        self.stderr_lines = 0
        self.next_id = 1
        cmd = [exe, "--stdio", "--index-dir", index_dir] + extra
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, cwd=root)
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=self._read_err, daemon=True).start()
        self.request("initialize", {"processId": os.getpid(), "rootUri": Path(root).as_uri(),
                                    "workspaceFolders": [{"uri": Path(root).as_uri(), "name": "w"}],
                                    "capabilities": {}}, timeout=120)
        self.notify("initialized", {})

    def _send(self, msg):
        body = json.dumps(msg).encode()
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.proc.stdin.flush()

    def _read_err(self):
        for _ in self.proc.stderr:
            self.stderr_lines += 1

    def _read(self):
        out = self.proc.stdout
        while True:
            length = None
            while True:
                line = out.readline()
                if not line:
                    return
                line = line.strip()
                if not line:
                    break
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":")[1])
            if length is None:
                continue
            msg = json.loads(out.read(length))
            t = now()
            with self.cond:
                if "method" in msg and "id" in msg:  # a request of the server: answer null
                    try:
                        self._send({"jsonrpc": "2.0", "id": msg["id"], "result": None})
                    except OSError:
                        pass
                elif "method" in msg:
                    m, p = msg["method"], msg.get("params") or {}
                    if m == "textDocument/publishDiagnostics":
                        self.diag.setdefault(p["uri"], []).append(
                            (t, p.get("version"), len(p.get("diagnostics", []))))
                    elif m == "window/logMessage" and "lite mismatch" in p.get("message", ""):
                        self.lite_logs.append(p["message"])
                    elif m in ("window/showMessage", "window/logMessage"):
                        if m == "window/showMessage":
                            self.messages.append(p.get("message", ""))
                else:
                    self.replies[msg["id"]] = (t, msg)
                self.cond.notify_all()

    def notify(self, method, params):
        self._send({"jsonrpc": "2.0", "method": method, "params": params})

    def send_request(self, method, params):
        rid = self.next_id
        self.next_id += 1
        self._send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        return rid, now()

    def wait_reply(self, rid, timeout=300):
        end = now() + timeout
        with self.cond:
            while rid not in self.replies:
                left = end - now()
                if left <= 0:
                    raise TimeoutError("no reply for request %d" % rid)
                self.cond.wait(left)
            return self.replies[rid]

    def request(self, method, params, timeout=300):
        rid, t0 = self.send_request(method, params)
        t1, msg = self.wait_reply(rid, timeout)
        return (t1 - t0) * 1000.0, msg

    def diag_count(self, uri):
        with self.cond:
            return len(self.diag.get(uri, []))

    def wait_diag(self, uri, after_count, version=None, timeout=120):
        """Wait for a publish after `after_count` ones. Give its time, or None on timeout."""
        end = now() + timeout
        with self.cond:
            while True:
                lst = self.diag.get(uri, [])
                for t, v, _ in lst[after_count:]:
                    if version is None or v is None or v >= version:
                        return t
                left = end - now()
                if left <= 0:
                    return None
                self.cond.wait(left)

    def open(self, path, text=None):
        uri = Path(path).as_uri()
        if text is None:
            text = Path(path).read_text()
        n = self.diag_count(uri)
        t0 = now()
        self.notify("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "hot",
                                                              "version": 1, "text": text}})
        return uri, text, n, t0

    def stats(self):
        return self.request("hot/indexStats", {})[1].get("result") or {}

    def close(self):
        try:
            self.request("shutdown", None, timeout=20)
            self.notify("exit", None)
            self.proc.wait(10)
        except Exception:
            self.proc.kill()


STAT_KEYS = ["translations", "translateMs", "loaded", "loadMs", "analyzed", "tableHits", "tableAnalyses"]


def delta(a, b):
    return {k: round(b.get(k, 0) - a.get(k, 0), 1) for k in STAT_KEYS if k in b}


def n_places(msg):
    res = msg.get("result") or []
    return len(res), len({r.get("uri") for r in res})


class Bench:
    def __init__(self, a):
        self.a = a
        self.rows = []
        p = PRESETS.get(a.preset, {})
        self.root = a.root or p.get("root")
        self.class_file = self.abs(a.class_file or p.get("class_file"))
        self.class_pos = tuple(a.class_pos or p.get("class_pos") or (1, 1))
        self.hover_pos = tuple(a.hover_pos or p.get("hover_pos") or self.class_pos)
        self.native_file = self.abs(a.native_file or p.get("native_file"))
        self.native_pos = tuple(a.native_pos or p.get("native_pos") or (1, 1))
        cdb = a.compile_commands_dir or p.get("cdb")
        self.extra = ["--compile-commands-dir", self.abs(cdb)] if cdb else []
        self.extra += a.server_arg
        self.scratch = tempfile.mkdtemp(prefix="lsbench-")

    def abs(self, p):
        return str(Path(self.root) / p) if p else None

    def new_index(self):
        d = tempfile.mkdtemp(dir=self.scratch)
        return d

    def start(self, index_dir):
        return Client(os.path.abspath(self.a.hot_ls), self.root, index_dir, self.extra)

    def row(self, name, ms, counters=None, **extra):
        r = {"name": name, "ms": None if ms is None else round(ms, 1)}
        r.update(counters or {})
        r.update(extra)
        self.rows.append(r)
        parts = ["%s: %s" % (name, r["ms"])]
        parts += ["%s=%s" % (k, v) for k, v in r.items() if k not in ("name", "ms")]
        print(" ".join(parts), flush=True)

    def pos(self, pos):
        return {"line": pos[0] - 1, "character": pos[1] - 1}

    def refs_params(self, uri, pos):
        return {"textDocument": {"uri": uri}, "position": self.pos(pos),
                "context": {"includeDeclaration": True}}

    def open_ready(self, c, path):
        uri, text, n, t0 = c.open(path)
        t = c.wait_diag(uri, n)
        return uri, text, (None if t is None else (t - t0) * 1000.0)

    def scripts(self):
        out = []
        for p in sorted(Path(self.root).rglob("*.hot")):
            if any(part.startswith(".") for part in p.relative_to(self.root).parts):
                continue
            out.append(str(p))
        return out

    # a. analyze cost
    def s_analyze(self):
        c = self.start(self.new_index())
        try:
            s0 = c.stats()
            times = []
            for path in self.scripts()[: self.a.scripts]:
                _, _, ms = self.open_ready(c, path)
                times.append(ms)
            s1 = c.stats()
            ok = [t for t in times if t is not None]
            self.row("analyze.first_diag_median", statistics.median(ok) if ok else None,
                     delta(s0, s1), n=len(times), timeouts=len(times) - len(ok))
            self.row("analyze.first_diag_max", max(ok) if ok else None)
            self.row("analyze.first_diag_total", sum(ok))
        finally:
            c.close()

    # b. script-class references
    def s_refs(self):
        index = self.new_index()
        c = self.start(index)
        try:
            uri, _, _ = self.open_ready(c, self.class_file)
            s0 = c.stats()
            ms, msg = c.request("textDocument/references", self.refs_params(uri, self.class_pos))
            s1 = c.stats()
            places, files = n_places(msg)
            self.row("refs.cold", ms, delta(s0, s1), places=places, scripts=files,
                     error=(msg.get("error") or {}).get("message"))
            ms, msg = c.request("textDocument/references", self.refs_params(uri, self.class_pos))
            s2 = c.stats()
            self.row("refs.warm_same_session", ms, delta(s1, s2), places=n_places(msg)[0])
        finally:
            c.close()
        c = self.start(index)
        try:
            uri, _, _ = self.open_ready(c, self.class_file)
            s0 = c.stats()
            ms, msg = c.request("textDocument/references", self.refs_params(uri, self.class_pos))
            s1 = c.stats()
            self.row("refs.warm_new_process", ms, delta(s0, s1), places=n_places(msg)[0])
        finally:
            c.close()

    # c. native-class references
    def s_native(self):
        c = self.start(self.new_index())
        try:
            uri, _, _ = self.open_ready(c, self.native_file)
            s0 = c.stats()
            e0 = c.stderr_lines
            c.messages.clear()
            ms, msg = c.request("textDocument/references", self.refs_params(uri, self.native_pos))
            s1 = c.stats()
            places, files = n_places(msg)
            time.sleep(0.3)
            self.row("native.refs_cold", ms, delta(s0, s1), places=places, scripts=files,
                     stderr_lines=c.stderr_lines - e0, show_message=" | ".join(c.messages) or None,
                     error=(msg.get("error") or {}).get("message"))
            ms, msg = c.request("textDocument/references", self.refs_params(uri, self.native_pos))
            s2 = c.stats()
            self.row("native.refs_warm", ms, delta(s1, s2), places=n_places(msg)[0])
        finally:
            c.close()

    # d. typing
    def s_typing(self):
        runs = []
        for _ in range(self.a.runs):
            c = self.start(self.new_index())
            try:
                uri, text, _ = self.open_ready(c, self.class_file)
                s0 = c.stats()
                for v in range(2, 22):
                    n = c.diag_count(uri)
                    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": v},
                             "contentChanges": [{"text": text + "\n// edit %d\n" % v}]})
                    last = now()
                    if v < 21:
                        time.sleep(0.05)
                t = c.wait_diag(uri, n, version=21)
                s1 = c.stats()
                runs.append(((t - last) * 1000.0 if t else None, delta(s0, s1)))
            finally:
                c.close()
        ok = sorted(r[0] for r in runs if r[0] is not None)
        p95 = ok[min(len(ok) - 1, int(0.95 * len(ok)))] if ok else None
        self.row("typing.last_change_to_diag_median", statistics.median(ok) if ok else None,
                 runs[-1][1], runs=len(runs), p95=None if p95 is None else round(p95, 1))

    # e. hover under load
    def s_hover(self):
        c = self.start(self.new_index())
        try:
            uri, _, _ = self.open_ready(c, self.class_file)
            rid, t0 = c.send_request("textDocument/references", self.refs_params(uri, self.class_pos))
            time.sleep(0.1)
            hid, h0 = c.send_request("textDocument/hover", {"textDocument": {"uri": uri},
                                                            "position": self.pos(self.hover_pos)})
            ht, hmsg = c.wait_reply(hid)
            rt, rmsg = c.wait_reply(rid)
            self.row("hover.reply_under_load", (ht - h0) * 1000.0,
                     refs_ms=round((rt - t0) * 1000.0, 1), hover_null=hmsg.get("result") is None)
        finally:
            c.close()

    # f. cancel
    def s_cancel(self):
        c = self.start(self.new_index())
        try:
            uri, _, _ = self.open_ready(c, self.class_file)
            rid, t0 = c.send_request("textDocument/references", self.refs_params(uri, self.class_pos))
            time.sleep(0.05)
            c.notify("$/cancelRequest", {"id": rid})
            rt, msg = c.wait_reply(rid)
            err = (msg.get("error") or {}).get("code")
            self.row("cancel.reply", (rt - t0) * 1000.0, error_code=err)
        finally:
            c.close()

    # g. a markup document (.rml) with two script blocks, in a generated host
    def rml_document(self):
        """Give (text, blocks). Each block is the 0-based first code line and the count of methods."""
        lines = ["<rml>", "<head>", "<title>Bench \u00e9\u65e5\u672c</title>"]
        blocks = []
        for b in range(2):
            for i in range(50):
                lines.append("<p>caf\u00e9 %d %d</p>" % (b, i))
            lines.append("<script>")
            blocks.append(len(lines))
            lines.append("class Blk%d {" % b)
            lines.append("    int f0(int a) { int t = a; return t; }")
            for i in range(1, 58):
                lines.append("    int f%d(int a) { int t = f%d(a) + %d; return t; }" % (i, i - 1, i))
            lines.append("}")
            lines.append("</script>")
        lines += ["</head>", "<body>"]
        while len(lines) < 299:
            lines.append("<p>\u00fcber</p>")
        lines += ["</body>", "</rml>"]
        return "\n".join(lines) + "\n", blocks

    def s_rml(self):
        repo = Path(__file__).resolve().parents[2]
        root = tempfile.mkdtemp(dir=self.scratch)
        (Path(root) / "build").mkdir()
        (Path(root) / "build" / "CMakeCache.txt").write_text("# cache\n")
        (Path(root) / "prefix.txt").write_text('using "Actor.h";\n')
        (Path(root) / "hosts").mkdir()
        res = subprocess.run(["clang", "-print-resource-dir"], capture_output=True, text=True).stdout.strip()
        host = {"version": 1, "name": "t", "binaryDir": str(Path(root) / "build"), "compiler": "/usr/bin/c++",
                "std": "c++17", "resourceDir": res, "sourceDir": root, "scriptDirs": [root],
                "includeDirs": [str(repo / "tests" / "engine"), str(repo / "include")],
                "embedded": [{"suffix": ".rml", "tag": "script", "prefixFile": "prefix.txt"}]}
        (Path(root) / "hosts" / "t.json").write_text(json.dumps(host))
        text, blocks = self.rml_document()
        path = str(Path(root) / "page.rml")
        Path(path).write_text(text)
        c = Client(os.path.abspath(self.a.hot_ls), root, self.new_index(),
                   ["--hosts-dir", str(Path(root) / "hosts"), "--no-background-index", "-I" + str(repo / "include"),
                    "-I" + str(repo / "tests" / "engine")])
        runs = self.a.rml_runs
        try:
            uri, _, _, _ = c.open(path, text)
            if c.wait_diag(uri, 0, timeout=300) is None:
                raise RuntimeError("rml: no first diagnostics")
            lines = text.split("\n")
            # Places inside the blocks: the name "t" in "return t;" (hover), the call "f<i-1>" (definition).
            hover, defin, compl = [], [], []
            for k in range(runs):
                blk = blocks[k % 2]
                ln = blk + 2 + (k * 2) % 55
                s = lines[ln]
                hover.append((ln, s.index("return t") + 7))
                defin.append((ln, s.index("= f") + 3))
                compl.append((ln, s.index("return t") + 8))
            ver = [1]

            def change(k):
                ver[0] += 1
                cur = list(lines)
                ln = blocks[0] + 2 + k % 50
                cur[ln] = cur[ln].replace("; return", "; int u%d = 0; return" % ver[0], 1)
                n = c.diag_count(uri)
                t0 = now()
                c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": ver[0]},
                                                    "contentChanges": [{"text": "\n".join(cur)}]})
                t = c.wait_diag(uri, n, ver[0], timeout=60)
                return None if t is None else (t - t0) * 1000.0

            def ask(method):
                def f(pos):
                    t0 = now()
                    msg = c.request(method, {"textDocument": {"uri": uri},
                                             "position": {"line": pos[0], "character": pos[1]}}, timeout=60)[1]
                    return (now() - t0) * 1000.0, msg.get("result")
                return f

            results = {}
            change(0)  # warm-up, not counted
            results["diag"] = [change(k) for k in range(runs)]
            for name, method, places in (("hover", "textDocument/hover", hover),
                                         ("definition", "textDocument/definition", defin),
                                         ("completion", "textDocument/completion", compl)):
                f = ask(method)
                f(places[0])  # warm-up, not counted
                ms, nulls = [], 0
                for pos in places:
                    t, res = f(pos)
                    ms.append(t)
                    nulls += res is None
                results[name] = ms
                results[name + "_nulls"] = nulls
        except (TimeoutError, RuntimeError) as e:
            self.failed = True
            print("rml.limit: FAIL, %s: %s" % (type(e).__name__, e), flush=True)
            return
        finally:
            c.close()
        worst = worst_p95 = 0.0
        bad = False
        for name in ("diag", "hover", "definition", "completion"):
            ms = results[name]
            if name == "completion" and results["completion_nulls"] == runs:
                self.row("rml." + name, None, result="n/a")
                continue
            if any(m is None for m in ms):
                self.row("rml." + name, None, result="timeout")
                worst = float("inf")
                bad = True
                continue
            ok = sorted(ms)
            p95 = ok[min(len(ok) - 1, int(0.95 * len(ok)))]
            extra = {"nulls": results[name + "_nulls"]} if name + "_nulls" in results else {}
            self.row("rml." + name, statistics.mean(ms), p95=round(p95, 1), max=round(ok[-1], 1), n=len(ms), **extra)
            worst = max(worst, ok[-1])
            worst_p95 = max(worst_p95, p95)
            if name in ("hover", "definition") and results[name + "_nulls"] > 0:
                bad = True
                print("rml.%s: FAIL, %d null answers" % (name, results[name + "_nulls"]), flush=True)
        if not bad:
            goal = "goal ok" if worst_p95 < RML_GOAL_MS else "goal missed"
            print("rml.goal: p95 < %d ms: %s (worst p95 %.1f)" % (RML_GOAL_MS, goal, worst_p95), flush=True)
        if bad or worst > RML_LIMIT_MS:
            self.failed = True
            print("rml.limit: FAIL, %s (limit %d ms)" % ("a timeout or a null answer" if bad else "a request took %.1f ms" % worst, RML_LIMIT_MS), flush=True)
        else:
            print("rml.limit: ok, max %.1f ms (limit %d ms)" % (worst, RML_LIMIT_MS), flush=True)

    def run(self):
        self.failed = False
        try:
            self.run_scenarios()
        finally:
            shutil.rmtree(self.scratch, ignore_errors=True)
        return self.finish()

    def run_scenarios(self):
        for name in self.a.only or SCENARIOS:
            if name == "rml" and not self.a.only:
                continue  # run rml only with --only rml
            getattr(self, "s_" + name)()

    def finish(self):
        if self.a.json:
            Path(self.a.json).write_text(json.dumps({"hot_ls": self.a.hot_ls, "root": self.root, "loadavg": os.getloadavg(), "cpus": os.cpu_count(),
                                                     "rows": self.rows}, indent=1) + "\n")
        return 1 if self.failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--hot-ls", required=True)
    ap.add_argument("--preset", choices=sorted(PRESETS))
    ap.add_argument("--root")
    ap.add_argument("--class-file", help="path relative to the root")
    ap.add_argument("--class-pos", nargs=2, type=int, metavar=("LINE", "COL"))
    ap.add_argument("--hover-pos", nargs=2, type=int, metavar=("LINE", "COL"))
    ap.add_argument("--native-file", help="path relative to the root")
    ap.add_argument("--native-pos", nargs=2, type=int, metavar=("LINE", "COL"))
    ap.add_argument("--compile-commands-dir")
    ap.add_argument("--server-arg", action="append", default=[], help="extra argument for hot-ls")
    ap.add_argument("--only", action="append", choices=SCENARIOS)
    ap.add_argument("--scripts", type=int, default=10, help="scripts of scenario analyze")
    ap.add_argument("--runs", type=int, default=3, help="runs of scenario typing")
    ap.add_argument("--rml-runs", type=int, default=20, help="requests of each kind in scenario rml")
    ap.add_argument("--json", help="write the rows to this file")
    a = ap.parse_args()
    if not a.root and not a.preset and a.only != ["rml"]:
        ap.error("give --preset or --root")
    return Bench(a).run()


if __name__ == "__main__":
    sys.exit(main())
