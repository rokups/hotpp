#!/usr/bin/env python3
"""Body-edit reload benchmark for the clang files backend (runs bench_reload).

Run with: python3 -I tools/bench/bench_reload.py --exe PATH/bench_reload --out DIR [--kind hot|cpp|both]

The compile flags come from an entry of a compile_commands.json of a host that uses the rbfx
headers (default: BiteSizedWar). The .hot script is a real BiteSizedWar script, the .cpp script is
tools/bench/reload/heavy_reload.cpp. Each run is a new process: one load that is not timed, one
warm-up reload, then --reps timed reloads that each edit one number in a method body.
The tool prints the median and the p95 of each phase, the load average before the run, and the
reason lines for a compile with no shared PCH. --trace adds one more run with -ftime-trace.
"""
import argparse, json, math, os, re, shutil, statistics, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
BSW = Path("/home/rk/src/rbfx/BiteSizedWar")
RBFX_SCRIPT = Path("/home/rk/src/rbfx/rbfx-script")
HOT_SCRIPT = BSW / "Source/Game/Scripts/Weapons/WeaponHolder.hot"
HOT_EDIT_LINE = "return score != null ? score.GetSlot() : 0;"


# The engine library, and the game library for the native symbols of the BiteSizedWar scripts
# (Shared/NetRead.h). The script modules find them in the global scope.
PRELOAD_LIBS = ["libUrho3D.so", "libGame.Main.so"]


def pctl(values, p):
    s = sorted(values)
    if not s:
        return float("nan")
    k = max(0, min(len(s) - 1, math.ceil(p / 100.0 * len(s)) - 1))
    return s[k]


def flags_from_cdb(cdb, match):
    entries = json.load(open(cdb))
    entry = next(e for e in entries if match in e["file"])
    args = entry["arguments"]
    out, skip = [], False
    for a in args[1:]:
        if skip:
            skip = False
        elif a == "-c":
            continue
        elif a == "-o":
            skip = True
        elif a == entry["file"] or a.startswith("-resource-dir="):
            continue
        else:
            out.append(a)
    return out


def load_avg():
    return os.getloadavg()[0]


def wait_for_quiet(limit, tries):
    """Return (load average at the start, True when the machine was quiet)."""
    for i in range(tries + 1):
        la = load_avg()
        if la < limit:
            return la, True
        if i < tries:
            time.sleep(20)
    return load_avg(), False


def parse_stdout(text):
    reps = []
    for m in re.finditer(r"^rep \d+ total (\S+) begin (\S+) compile (\S+) finish (\S+) swap (\S+)$", text, re.M):
        reps.append(dict(zip(["total", "begin", "compile_link", "finish", "swap"], map(float, m.groups()))))
    return reps


def parse_stderr(text, nreps):
    """The per-unit lines of HOT_TIMING. The last nreps lines are the timed reloads."""
    units = []
    for m in re.finditer(r"^hot: (\S+) translate (cached|\d+ ms), compile (\d+) ms, link (\d+) ms(.*)$", text, re.M):
        tr = 0.0 if m.group(2) == "cached" else float(m.group(2).split()[0])
        d = {"translate": tr, "compile": float(m.group(3)), "link": float(m.group(4))}
        pm = re.search(r"parse (\d+) ms", m.group(5))
        d["parse"] = float(pm.group(1)) if pm else 0.0
        fm = re.search(r"front (\d+)", m.group(5))
        d["front"] = float(fm.group(1)) if fm else 0.0
        units.append(d)
    fin = []
    for m in re.finditer(r"^hot: (\S+) finish record_stage (\d+) ms, complete_load (\d+) ms, dlopen (\d+) ms", text, re.M):
        fin.append({"record_stage": float(m.group(2)), "complete_load": float(m.group(3)), "dlopen": float(m.group(4))})
    rows = []
    for i in range(min(nreps, len(units))):
        u = units[len(units) - nreps + i] if len(units) >= nreps else units[i]
        f = fin[len(fin) - nreps + i] if len(fin) >= nreps else {}
        rows.append({**u, **f})
    why = re.findall(r"^hot: \S+ no shared pch: .*$", text, re.M)
    return rows, why


def run_once(exe, script, extra, flags_file, work, cache, env_extra, label, args):
    shutil.rmtree(work, ignore_errors=True)
    shutil.rmtree(cache, ignore_errors=True)
    work.mkdir(parents=True)
    cmd = [str(exe), "--script", str(script), "--flags", str(flags_file), "--reps", str(args.reps),
           "--work", str(work), "--cache", str(cache), "--prelude", str(RBFX_SCRIPT / "Plugins/Rbfx.Script.Core/Prelude/Rbfx.prelude.hot")] + extra
    # LD_PRELOAD gives the engine library a static TLS block. A dlopen of it fails with "static TLS".
    env = dict(os.environ, HOT_TIMING="1", LD_PRELOAD=":".join(str(BSW / "cmake-build/linux-gcc-x64-dll/bin/Debug" / n) for n in PRELOAD_LIBS), **env_extra)
    la, quiet = wait_for_quiet(args.load_limit, 3)
    t = time.time()
    p = subprocess.run(cmd, env=env, capture_output=True, text=True)
    (args.out / f"{label}.stdout").write_text(p.stdout)
    (args.out / f"{label}.stderr").write_text(p.stderr)
    if p.returncode != 0:
        sys.exit(f"{label}: bench_reload failed (rc {p.returncode}): {p.stderr[-1500:]}")
    return p, la, quiet, time.time() - t


def trace_summary(work, out, label):
    traces = [t for d in work for t in Path(d).rglob("*.json") if t.name != "compile_commands.json"]
    traces = sorted((t for t in traces if "traceEvents" in t.read_text()[:200]), key=lambda p: p.stat().st_mtime)
    if not traces:
        return "no -ftime-trace file found"
    t = traces[-1]
    shutil.copy(t, out / f"{label}.trace.json")
    ev = json.load(open(t))["traceEvents"]
    tot = [(e["dur"] / 1000.0, e["args"].get("detail", ""), e["name"]) for e in ev if e.get("ph") == "X" and e["name"].startswith("Total ") and e["name"] != "Total ExecuteCompiler"]
    tot.sort(reverse=True)
    return "\n".join(f"  {ms:8.1f} ms  {name}" for ms, _, name in tot[:5])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--kind", default="both", choices=["hot", "cpp", "both"])
    ap.add_argument("--reps", type=int, default=10)
    ap.add_argument("--load-limit", type=float, default=4.0)
    ap.add_argument("--trace", action="store_true", help="one more run per kind with -ftime-trace")
    ap.add_argument("--cdb", default=str(BSW / ".cache/hot/compile_commands.json"))
    args = ap.parse_args()
    args.out = Path(args.out).resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    flags = flags_from_cdb(args.cdb, "Weapons/WeaponHolder.cpp")
    flags_file = args.out / "flags.txt"
    flags_file.write_text("\n".join(flags) + "\n")
    trace_file = args.out / "flags_trace.txt"
    trace_file.write_text("\n".join(flags + ["-ftime-trace"]) + "\n")
    scripts = {}
    if args.kind in ("hot", "both"):
        text = HOT_SCRIPT.read_text()
        assert HOT_EDIT_LINE in text
        edited = text.replace(HOT_EDIT_LINE, "return score != null ? score.GetSlot() : /*BENCH_EDIT*/ 0;", 1)
        d = args.out / "src_hot"
        d.mkdir(exist_ok=True)
        (d / "WeaponHolder.hot").write_text(edited)
        scripts["hot"] = (d / "WeaponHolder.hot", ["--include", str(HOT_SCRIPT.parent), "--include", str(BSW / "Source/Game/Scripts")])
    if args.kind in ("cpp", "both"):
        scripts["cpp"] = (HERE / "reload/heavy_reload.cpp", [])
    print(f"build: {Path(args.exe).resolve()}  reps: {args.reps}")
    for kind, (script, extra) in scripts.items():
        p, la, quiet, secs = run_once(args.exe, script, extra, flags_file, args.out / f"work_{kind}", args.out / f"cache_{kind}", {}, kind, args)
        reps = parse_stdout(p.stdout)
        rows, why = parse_stderr(p.stderr, len(reps))
        print(f"\n== {kind}: load average before {la:.2f} ({'quiet' if quiet else 'LOADED'}), {len(reps)} reps")
        print(f"{'phase':<22}{'median ms':>10}{'p95 ms':>10}")
        for name, vals in [("total (wall)", [r["total"] for r in reps]),
                           ("translate", [r["translate"] for r in rows]),
                           ("  front (.cpp parse)", [r["front"] for r in rows]),
                           ("compile (clang run)", [r["compile"] for r in rows]),
                           ("link", [r["link"] for r in rows]),
                           ("begin_load (all)", [r["begin"] for r in reps]),
                           ("compile()+link (all)", [r["compile_link"] for r in reps]),
                           ("finish (dlopen etc)", [r["finish"] for r in reps]),
                           ("  complete_load", [r.get("complete_load", 0.0) for r in rows]),
                           ("  dlopen", [r.get("dlopen", 0.0) for r in rows]),
                           ("swap", [r["swap"] for r in reps])]:
            print(f"{name:<22}{statistics.median(vals):>10.1f}{pctl(vals, 95):>10.1f}")
        for line in sorted(set(why)):
            print(line)
        if args.trace:
            _, la2, _, _ = run_once(args.exe, script, extra, trace_file, args.out / f"work_{kind}_tr", args.out / f"cache_{kind}_tr", {"HOT_KEEP_TEMP": "1"}, f"{kind}_trace", argparse.Namespace(**{**vars(args), "reps": 1}))
            print(f"-ftime-trace of the last compile ({kind}), load average {la2:.2f}:")
            print(trace_summary([args.out / f"work_{kind}_tr", args.out / f"cache_{kind}_tr"], args.out, kind))


if __name__ == "__main__":
    main()
