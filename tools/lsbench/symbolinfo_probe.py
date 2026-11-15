#!/usr/bin/env python3
"""Probe: does clangd give a non-empty `usr` in textDocument/symbolInfo?

Run: python3 -I tools/lsbench/symbolinfo_probe.py [CDB_JSON]
Part 1 uses a small snippet. Part 2 needs the compile database CDB_JSON of a hot-ls
workspace. It uses generated files from it and compares the usr of one method.
"""
import json, os, re, subprocess, sys, tempfile, threading
from pathlib import Path

DEFAULT_CDB = "/home/rk/src/rbfx/BiteSizedWar/.cache/hot/compile_commands.json"
SNIPPET = "struct Log { void Write(); }; void f(Log& l) { l.Write(); }\n"
# (tag, file suffix, regex of the line, word) : the word is found in the first matching line.
REAL = [
    ("template member call", "Weapons/WeaponView.cpp", r"\.GetComponent<PlayerLook>\(\)", "GetComponent"),
    ("template member call", "Weapons/WeaponHolder.cpp", r"\.GetComponent<", "GetComponent"),
    ("word in macro argument", "Weapons/WeaponView.cpp", r"URHO3D_OBJECT\(WeaponView, Urho3D::Component\)", "Component"),
    ("overloaded call", "Weapons/WeaponView.cpp", r"\)\.SetPosition\(viewModelPosition_\)", "SetPosition"),
    ("overloaded call", "Effects/ProjectileEffects.cpp", r"\)\.SetPosition\(", "SetPosition"),
]


class Clangd:
    def __init__(self, cdb_dir):
        cmd = ["clangd", "--log=error", "--background-index=false"]
        if cdb_dir:
            cmd.append("--compile-commands-dir=" + cdb_dir)
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.cond, self.replies, self.n = threading.Condition(), {}, 0
        threading.Thread(target=self._read, daemon=True).start()
        self.req("initialize", {"processId": os.getpid(), "rootUri": None, "capabilities": {}})
        self.send({"method": "initialized", "params": {}})

    def send(self, m):
        b = json.dumps(dict(m, jsonrpc="2.0")).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(b) + b)
        self.p.stdin.flush()

    def _read(self):
        while True:
            n = None
            while True:
                line = self.p.stdout.readline()
                if not line:
                    return
                if not line.strip():
                    break
                if line.lower().startswith(b"content-length:"):
                    n = int(line.split(b":")[1])
            m = json.loads(self.p.stdout.read(n))
            with self.cond:
                if "method" in m and "id" in m:
                    self.send({"id": m["id"], "result": None})
                elif "id" in m:
                    self.replies[m["id"]] = m
                    self.cond.notify_all()

    def req(self, method, params, timeout=300):
        self.n += 1
        i = self.n
        self.send({"id": i, "method": method, "params": params})
        with self.cond:
            if not self.cond.wait_for(lambda: i in self.replies, timeout):
                raise TimeoutError(method)
            return self.replies.pop(i)

    def open(self, path, text):
        self.send({"method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": Path(path).as_uri(), "languageId": "cpp", "version": 1, "text": text}}})

    def info(self, path, line, col):
        r = self.req("textDocument/symbolInfo", {"textDocument": {"uri": Path(path).as_uri()},
                                                 "position": {"line": line, "character": col}})
        return r.get("result") or r.get("error")


def usr_of(res):
    return (res[0].get("usr") or "") if isinstance(res, list) and res else ""


def main():
    cdb = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CDB
    tmp = tempfile.mkdtemp(prefix="symprobe-")
    snip = os.path.join(tmp, "snip.cpp")
    Path(snip).write_text(SNIPPET)
    Path(tmp, "compile_commands.json").write_text(json.dumps(
        [{"directory": tmp, "file": snip, "arguments": ["clang++", "-std=c++17", "-c", snip]}]))
    c = Clangd(tmp)
    c.open(snip, SNIPPET)
    res = c.info(snip, 0, SNIPPET.index("l.Write") + 2)
    print("snippet:", json.dumps(res), "| usr non-empty:", bool(usr_of(res)))
    snippet_ok = bool(usr_of(res))
    c.p.kill()

    if not os.path.exists(cdb):
        print("no compile database:", cdb)
        return 0 if snippet_ok else 1
    entries = json.load(open(cdb))
    by_suffix = {}
    for e in entries:
        for _, suf, _, _ in REAL:
            if e["file"].endswith("/" + suf) and suf not in by_suffix:
                by_suffix[suf] = e
    sub = tempfile.mkdtemp(prefix="symprobe-cdb-")
    Path(sub, "compile_commands.json").write_text(json.dumps(list(by_suffix.values())))
    c = Clangd(sub)
    opened, found = set(), {}
    for tag, suf, rx, word in REAL:
        e = by_suffix.get(suf)
        if not e:
            print(tag, suf, ": no entry in the database")
            continue
        text = Path(e["file"]).read_text()
        if suf not in opened:
            c.open(e["file"], text)
            opened.add(suf)
        lines = text.split("\n")
        ln = next((i for i, l in enumerate(lines) if re.search(rx, l)), None)
        if ln is None:
            print(tag, suf, ": line not found")
            continue
        col = lines[ln].index(word, re.search(rx, lines[ln]).start()) + 1
        res = c.info(e["file"], ln, col)
        usr = usr_of(res)
        found.setdefault(word, []).append(usr)
        name = res[0].get("name") if isinstance(res, list) and res else None
        print("%s | %s:%d | %s | usr=%s" % (tag, suf, ln + 1, name, usr or "EMPTY"))
    c.p.kill()
    ok = snippet_ok and all(u for v in found.values() for u in v) and bool(found)
    for word, v in found.items():
        same = len(set(v)) == 1 and len(v) > 1
        print("same usr from two files for %s: %s" % (word, same if len(v) > 1 else "n/a"))
        ok = ok and (len(v) < 2 or same)
    print("USR assumption:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
