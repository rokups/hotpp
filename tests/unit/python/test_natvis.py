# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

"""A static check of tools/natvis/hot.natvis. No Windows debugger is needed.

The test checks four things:
  - the file is well formed XML with the natvis root element;
  - each Type has a name and a DisplayString or an Expand element;
  - each "hot::Name" in the file is a struct or a class in include/hot;
  - the type name patterns match the names that the translator makes ("_Shell", "State_g").
"""

import os
import re
import unittest
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
NATVIS = os.path.join(ROOT, "tools", "natvis", "hot.natvis")
NS = "{http://schemas.microsoft.com/vstudio/debugger/natvis/2010}"


def read_tree(folder):
    text = []
    for base, _dirs, files in os.walk(folder):
        for name in files:
            if name.endswith((".h", ".cpp")):
                with open(os.path.join(base, name), encoding="utf-8") as f:
                    text.append(f.read())
    return "\n".join(text)


class Natvis(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(NATVIS, encoding="utf-8") as f:
            cls.text = f.read()
        cls.root = ET.fromstring(cls.text)
        cls.headers = read_tree(os.path.join(ROOT, "include", "hot"))
        cls.translator = read_tree(os.path.join(ROOT, "src", "translator"))

    def test_root(self):
        self.assertEqual(self.root.tag, NS + "AutoVisualizer")

    def test_types(self):
        types = self.root.findall(NS + "Type")
        self.assertGreaterEqual(len(types), 3)
        for t in types:
            self.assertTrue(t.get("Name"), "a Type has no Name")
            self.assertTrue(t.find(NS + "DisplayString") is not None or t.find(NS + "Expand") is not None,
                            t.get("Name"))

    def test_hot_types_exist(self):
        names = set(re.findall(r"\bhot::(\w+)", self.text))
        self.assertIn("ShellCore", names)
        for n in names:
            self.assertRegex(self.headers, r"\b(?:struct|class)\s+%s\b" % n, "hot::%s is not in include/hot" % n)

    def test_cast_is_a_base(self):
        # The expansion of a shell casts "this" to hot::ShellCore: a shell must derive from it.
        self.assertRegex(self.headers, r"struct\s+(?:HOT_RTTI\s+)?ShellBase\s*:\s*ShellCore\b")
        self.assertRegex(self.translator, r"hot::ShellBase\b")

    def test_generated_names(self):
        for t in self.root.findall(NS + "Type"):
            name = t.get("Name")
            for part in ("State_g", "_Shell"):
                if part in name:
                    self.assertIn(part, self.translator, "the translator makes no name with " + part)


if __name__ == "__main__":
    unittest.main()
