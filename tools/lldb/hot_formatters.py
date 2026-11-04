# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

"""LLDB formatters for Hot++ (design 4.12, experiment F3).

The translator makes C++ with generated names. These formatters show the script names:
  - a state block "Player_State_g2" or "PlayerState_g2" is shown as "Player"
  - a shell "Player_Shell" and a handle to it are shown as "Player"
  - a frame "Player__heal" is shown as "Player.heal at player.hot:12"
  - the internal member "__hot_owner" of a state block is hidden from the children

The first part of this file is pure Python. It does not import lldb, so a plain Python test
can run it (tests/unit/python/test_hot_formatters.py). The second part needs lldb. It runs
only when lldb loads the file. A real lldb session tests it (tests/unit/python/test_lldb_session.py).

Use: "command script import /path/to/hot_formatters.py" (see tools/lldb/hot.lldbinit).
"""

import re

# ---------------------------------------------------------------------------- pure helpers

_SHELL = re.compile(r"^(\w+?)_Shell$")
_STATE = re.compile(r"^(\w+?)State_g(\d+)$")
# "Cls__f", "Cls__f_virt", "Cls__get_P", "Cls__set_P". The class name has no "__".
_METHOD = re.compile(r"^(\w+?)__(\w+?)(?:_virt)?$")
_ACCESSOR = re.compile(r"^(get|set)_(\w+)$")
_HANDLE = re.compile(r"^(?:std::(?:__1::)?shared_ptr|hot::Intrusive)<(?:\w+::)*(\w+?)_Shell\b")


def class_of_type(type_name):
    """Return the script class of a generated type name, or None."""
    name = type_name.replace("const ", "").strip(" *&")
    name = name.split("::")[-1]
    m = _SHELL.match(name)
    if m:
        return m.group(1)
    m = _STATE.match(name)
    if m:
        return m.group(1)
    return None


def generation_of_type(type_name):
    """Return the generation number of a state block type, or None."""
    m = _STATE.match(type_name.split("::")[-1])
    return int(m.group(2)) if m else None


def class_of_handle(type_name):
    """Return the script class of a handle type such as "std::shared_ptr<Player_Shell>"."""
    m = _HANDLE.match(type_name.strip())
    return m.group(1) if m else None


def script_function_name(symbol):
    """Return "Cls.f" for the generated name "Cls__f". Return None for any other symbol."""
    base = symbol.split("(")[0].split("::")[-1]
    m = _METHOD.match(base)
    if not m:
        return None
    cls, member = m.group(1), m.group(2)
    a = _ACCESSOR.match(member)
    if a:
        return "%s.%s (%s)" % (cls, a.group(2), "read" if a.group(1) == "get" else "write")
    return "%s.%s" % (cls, member)


OWNER_MEMBER = "__hot_owner"


def visible_child_indices(names):
    """Return the real indices of the children to show.

    "names" is the list of the child names of a state block. The member "__hot_owner" is
    internal. It is the first member (index 0). The function hides it only at index 0.
    """
    return [i for i, n in enumerate(names) if not (i == 0 and n == OWNER_MEMBER)]


def is_script_file(path):
    return bool(path) and path.endswith(".hot")


def frame_label(symbol, path=None, line=None):
    """Return the text of one script frame. Return None when the frame is not a script frame."""
    name = script_function_name(symbol)
    if name is None and not is_script_file(path):
        return None
    name = name or symbol
    if is_script_file(path):
        return "%s at %s:%s" % (name, path.replace("\\", "/").split("/")[-1], line if line else "?")
    return name


def format_fields(class_name, fields, limit=8):
    """Return "Player {health=80.0, shield=25.0}". "fields" is a list of (name, text) pairs."""
    shown = ["%s=%s" % (n, v) for n, v in fields[:limit]]
    if len(fields) > limit:
        shown.append("... %d more" % (len(fields) - limit))
    return "%s {%s}" % (class_name, ", ".join(shown))


def state_summary(type_name, fields):
    """The summary of a state block value."""
    cls = class_of_type(type_name) or type_name
    gen = generation_of_type(type_name)
    text = format_fields(cls, fields)
    return text + (" [gen %d]" % gen if gen is not None else "")


def handle_summary(type_name, use_count, is_null):
    """The summary of a handle value."""
    cls = class_of_handle(type_name) or type_name
    if is_null:
        return "%s null" % cls
    return "%s handle (use_count=%s)" % (cls, use_count)


# ---------------------------------------------------------------------------- lldb part

try:  # pragma: no cover - lldb is not available in the test
    import lldb  # type: ignore
except ImportError:  # the pure helpers still work
    lldb = None


def _scalar_text(v):
    s = v.GetSummary()
    return s if s is not None else v.GetValue()


def _fields_of(value):
    out = []
    for i in range(value.GetNumChildren()):
        c = value.GetChildAtIndex(i)
        if c.GetName() and not c.GetName().startswith("_"):
            out.append((c.GetName(), _scalar_text(c)))
    return out


def state_summary_provider(valobj, internal_dict):  # pragma: no cover
    return state_summary(valobj.GetType().GetName(), _fields_of(valobj))


def shell_summary_provider(valobj, internal_dict):  # pragma: no cover
    cls = class_of_type(valobj.GetType().GetName()) or "?"
    return "%s object" % cls


def _find_member(value, name):
    """Return the member "name" of a value. The search goes into the base classes."""
    child = value.GetChildMemberWithName(name)
    if child.IsValid():
        return child
    for i in range(value.GetNumChildren()):
        base = value.GetChildAtIndex(i)
        if base.GetName() and base.GetName().startswith(("std::", "_")):
            child = _find_member(base, name)
            if child.IsValid():
                return child
    return child


def _use_count(valobj):
    """The use count of a shared_ptr (libstdc++ or libc++), or "?"."""
    try:
        pi = _find_member(valobj, "_M_pi")  # libstdc++
        if pi.IsValid() and pi.GetValueAsUnsigned() != 0:
            count = pi.Dereference().GetChildMemberWithName("_M_use_count")
            if count.IsValid():
                return str(count.GetValueAsSigned())
        cntrl = _find_member(valobj, "__cntrl_")  # libc++: the owners count starts at -1
        if cntrl.IsValid() and cntrl.GetValueAsUnsigned() != 0:
            owners = cntrl.Dereference().GetChildMemberWithName("__shared_owners_")
            if owners.IsValid():
                return str(owners.GetValueAsSigned() + 1)
    except Exception:  # a layout that is not known: show "?"
        pass
    return "?"


def handle_summary_provider(valobj, internal_dict):  # pragma: no cover
    name = valobj.GetType().GetName()
    try:
        valobj = valobj.GetNonSyntheticValue()
        ptr = _find_member(valobj, "_M_ptr")  # libstdc++
        if not ptr.IsValid():
            ptr = _find_member(valobj, "__ptr_")  # libc++
        if not ptr.IsValid():
            ptr = _find_member(valobj, "p_")  # hot::Intrusive
        null = (not ptr.IsValid()) or ptr.GetValueAsUnsigned() == 0
        return handle_summary(name, _use_count(valobj), null)
    except Exception:  # a layout that is not known: show "?"
        return handle_summary(name, "?", False)


class StateChildren:  # pragma: no cover - needs lldb
    """Synthetic children of a state block. It hides the internal member "__hot_owner"."""

    def __init__(self, valobj, internal_dict):
        self.valobj = valobj
        self.update()

    def update(self):
        names = [self.valobj.GetChildAtIndex(i).GetName() or ""
                 for i in range(self.valobj.GetNumChildren())]
        self.map = visible_child_indices(names)
        self.names = [names[i] for i in self.map]
        return False

    def num_children(self):
        return len(self.map)

    def get_child_at_index(self, index):
        if index < 0 or index >= len(self.map):
            return None
        return self.valobj.GetChildAtIndex(self.map[index])

    def get_child_index(self, name):
        return self.names.index(name) if name in self.names else -1

    def has_children(self):
        return len(self.map) > 0


def script_backtrace(debugger, command, result, internal_dict):  # pragma: no cover
    """The command "hot-bt": print the stack of the selected thread with script names."""
    thread = debugger.GetSelectedTarget().GetProcess().GetSelectedThread()
    for frame in thread:
        le = frame.GetLineEntry()
        path = le.GetFileSpec().GetFilename() if le.IsValid() else None
        label = frame_label(frame.GetFunctionName() or "", path, le.GetLine() if le.IsValid() else None)
        if label:
            result.write("#%d %s\n" % (frame.GetFrameID(), label))


def __lldb_init_module(debugger, internal_dict):  # pragma: no cover
    module = __name__
    # The regex of lldb has no "\w" (it is not a Python regex): use a character class.
    w = "[A-Za-z0-9_]"
    state = "^(%s+::)*%s+State_g[0-9]+$" % (w, w)
    shell = "^(%s+::)*%s+_Shell$" % (w, w)
    handle = "^(std::(__1::|__cxx11::)?shared_ptr|hot::Intrusive)<(%s+::)*%s+_Shell.*>$" % (w, w)
    add = "type summary add --regex --python-function %s.%%s \"%%s\"" % module
    debugger.HandleCommand(add % ("state_summary_provider", state))
    debugger.HandleCommand(add % ("shell_summary_provider", shell))
    debugger.HandleCommand(add % ("handle_summary_provider", handle))
    debugger.HandleCommand("type synthetic add --regex --python-class %s.StateChildren \"%s\""
                           % (module, state))
    debugger.HandleCommand("command script add -f %s.script_backtrace hot-bt" % module)
