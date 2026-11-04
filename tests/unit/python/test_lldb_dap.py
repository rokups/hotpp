# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

"""A real lldb-dap session: a breakpoint in a .hot file stops, and the stack names the .hot file.

Use: test_lldb_dap.py <program> <target.hot> <lldb-dap>
The program is an AOT build of target.hot (tests/unit/python/lldb_target).
Return code 77: lldb-dap does not work here (ctest shows "skipped").
"""

import json
import os
import queue
import re
import signal
import subprocess
import sys
import threading
import time

SKIP = 77
DEADLINE = 80  # seconds for the whole session. The ctest TIMEOUT is 120.
# The text of an environment error: the debugger is not allowed to trace a process.
# Any other launch failure is a real failure of the test.
ENVIRONMENT_ERRORS = re.compile(
    r"ptrace|personality|seccomp|Operation not permitted|Permission denied|not permitted|"
    r"Inappropriate ioctl|Function not implemented", re.IGNORECASE)


class Adapter:
    """The adapter process in its own process group. A thread reads the messages, so a read has a deadline."""

    def __init__(self, path):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  start_new_session=True)
        self.seq = 0
        self.messages = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        try:
            while True:
                header = b""
                while not header.endswith(b"\r\n\r\n"):
                    c = self.p.stdout.read(1)
                    if not c:
                        self.messages.put(None)
                        return
                    header += c
                size = int(header.split(b":")[1].split(b"\r\n")[0])
                self.messages.put(json.loads(self.p.stdout.read(size)))
        except (OSError, ValueError):
            self.messages.put(None)

    def send(self, command, arguments=None):
        self.seq += 1
        body = json.dumps({"seq": self.seq, "type": "request", "command": command,
                           "arguments": arguments or {}}).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()

    def receive(self, deadline):
        """The next message, or None at the end of the stream or at the deadline."""
        try:
            return self.messages.get(timeout=max(0.0, deadline - time.monotonic()))
        except queue.Empty:
            return None

    def close(self):
        """Kill the adapter and the debuggee (the same group) and wait for them."""
        try:
            os.killpg(self.p.pid, signal.SIGKILL)
        except OSError:
            pass
        try:
            self.p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        for stream in (self.p.stdin, self.p.stdout):
            try:
                stream.close()
            except OSError:
                pass


def line_of(path, text):
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            if text in line:
                return number
    raise SystemExit("no line with '%s' in %s" % (text, path))


def session(a, program, script, line):
    """Return (status, breakpoint, frames). The status is "ok", "timeout" or "launch failed: <text>"."""
    deadline = time.monotonic() + DEADLINE
    a.send("initialize", {"adapterID": "lldb-dap", "linesStartAt1": True, "columnsStartAt1": True})
    a.send("launch", {"program": program, "stopOnEntry": False})
    a.send("setBreakpoints", {"source": {"path": script}, "breakpoints": [{"line": line}]})
    a.send("configurationDone")
    verified = None
    while True:
        m = a.receive(deadline)
        if m is None:
            return "timeout", verified, None
        if m.get("type") == "response" and m.get("command") == "launch" and not m.get("success"):
            return "launch failed: %s" % m.get("message"), verified, None
        if m.get("type") == "response" and m.get("command") == "setBreakpoints":
            verified = m["body"]["breakpoints"][0]
        if m.get("type") == "event" and m.get("event") == "stopped":
            a.send("stackTrace", {"threadId": m["body"]["threadId"]})
        if m.get("type") == "response" and m.get("command") == "stackTrace":
            return "ok", verified, m["body"]["stackFrames"]


def main():
    program, script, adapter_path = sys.argv[1:4]
    line = line_of(script, "hp = hp + 1;")
    try:
        a = Adapter(adapter_path)
    except OSError:
        print("lldb-dap does not start")
        return SKIP
    try:
        status, verified, frames = session(a, program, script, line)
    finally:
        a.close()
    if status.startswith("launch failed"):
        print(status)
        return SKIP if ENVIRONMENT_ERRORS.search(status) else 1
    if status == "timeout":
        print("lldb-dap did not stop in %d seconds" % DEADLINE)
        return 1
    errors = []
    if not verified or not verified.get("verified") or verified.get("line") != line:
        errors.append("the breakpoint is not verified on line %d: %s" % (line, verified))
    top = frames[0] if frames else {}
    if top.get("name") != "HeroState_g1::Tick()" or top.get("line") != line \
            or not top.get("source", {}).get("path", "").endswith("target.hot"):
        errors.append("wrong top frame: %s" % top)
    if errors:
        print("\n".join(errors))
        return 1
    print("lldb-dap session ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
