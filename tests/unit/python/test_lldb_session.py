# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

"""A real lldb session with tools/lldb/hot_formatters.py.

Use: test_lldb_session.py <program> <hot_formatters.py> <target.hot> <lldb>
The program is an AOT build of target.hot. The test sets a breakpoint on a line of the .hot
file, so it also checks that the line table of the generated C++ names the script.
Return code 77: lldb does not work here (ctest shows "skipped").
"""

import os
import re
import signal
import subprocess
import sys

SKIP = 77
PROBE_TIMEOUT = 10  # seconds
BATCH_TIMEOUT = 80  # seconds. The ctest TIMEOUT is 120.
# The text of an environment error: lldb is not allowed to start or to trace a process.
# Any other failure is a real failure of the test.
ENVIRONMENT_ERRORS = re.compile(
    r"ptrace|personality|seccomp|Operation not permitted|Permission denied|not permitted|"
    r"Inappropriate ioctl|Function not implemented", re.IGNORECASE)


def run(args, timeout):
    """Run a command in its own process group. On a timeout, kill the whole group (lldb and the debuggee)."""
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         start_new_session=True)
    try:
        out, _ = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        out, _ = p.communicate()
        raise
    finally:
        try:
            os.killpg(p.pid, signal.SIGKILL)  # a debuggee that is left
        except OSError:
            pass
    return p.returncode, out


def lldb_batch(lldb, program, commands):
    args = [lldb, "-b", "--no-lldbinit"]
    for c in commands + ["quit"]:
        args += ["-o", c]
    args.append(program)
    return run(args, BATCH_TIMEOUT)[1]


def line_of(path, text):
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            if text in line:
                return number
    raise SystemExit("no line with '%s' in %s" % (text, path))


def main():
    program, formatters, script, lldb = sys.argv[1:5]
    try:
        code, text = run([lldb, "--version"], PROBE_TIMEOUT)
    except (OSError, subprocess.TimeoutExpired):
        print("lldb does not start")
        return SKIP
    if code != 0:
        print("lldb does not start: " + text)
        return SKIP

    line = line_of(script, "hp = hp + 1;")
    try:
        out = lldb_batch(lldb, program, [
            "command script import " + formatters,
            "breakpoint set --file target.hot --line %d" % line,
            "run",
            "frame variable *this",
            "frame variable local",
            "frame variable this->empty",
            "frame variable this->title",
            "frame variable this->bag",
            "up",
            "frame variable this",
            "hot-bt",
            "kill",
        ])
    except subprocess.TimeoutExpired:
        print("lldb did not finish in %d seconds" % BATCH_TIMEOUT)
        return 1
    if "error: process launch failed" in out or "Process" not in out:
        print(out)
        if ENVIRONMENT_ERRORS.search(out):
            print("lldb cannot start a process here")
            return SKIP
        return 1

    errors = []

    def expect(pattern):
        if not re.search(pattern, out):
            errors.append("missing: " + pattern)

    expect(r"target\.hot:%d" % line)  # the breakpoint is in the .hot file
    # A state block: the script class name, the fields, the generation. The owner is hidden.
    # The text of the string and of the array comes from lldb: the pattern is loose there.
    expect(r"\(HeroState_g1\) \*this = Hero \{hp=80, shield=25\.5, title=\"knight\", bag=[^,]+, "
           r"buddy=Mate handle \(use_count=2\), empty=Mate null\} \[gen 1\]")
    # A handle: local and buddy share the object.
    expect(r"local = Mate handle \(use_count=2\)")
    expect(r"this->empty = Mate null")
    # A string and an array use the formatters of lldb.
    expect(r"\(std::[\w:]*string[^)]*\) this->title = \"knight\"")
    expect(r"\[0\] = 1\s+\[1\] = 2\s+\[2\] = 3")
    # A shell object, one frame up.
    expect(r"\(Hero_Shell \*\) this = 0x[0-9a-f]+ Hero object")
    # The command hot-bt.
    expect(r"#0 HeroState_g1::Tick\(\) at target\.hot:%d" % line)
    expect(r"#1 Hero_Shell::Tick\(\) at target\.hot:")
    if "__hot_owner" in out:
        errors.append("__hot_owner is visible")
    if errors:
        print(out)
        print("\n".join(errors))
        return 1
    print("lldb session ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
