"""Test of the pure helpers in tools/lldb/spp_formatters.py. lldb is not needed."""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools", "lldb"))
import spp_formatters as f  # noqa: E402


class Names(unittest.TestCase):
    def test_class_of_type(self):
        self.assertEqual(f.class_of_type("Player_Shell"), "Player")
        self.assertEqual(f.class_of_type("const PlayerState_g2 *"), "Player")
        self.assertEqual(f.class_of_type("ns::Player_Shell &"), "Player")
        self.assertIsNone(f.class_of_type("std::string"))

    def test_generation(self):
        self.assertEqual(f.generation_of_type("PlayerState_g12"), 12)
        self.assertIsNone(f.generation_of_type("Player_Shell"))

    def test_handle(self):
        self.assertEqual(f.class_of_handle("std::shared_ptr<Player_Shell>"), "Player")
        self.assertEqual(f.class_of_handle("std::__1::shared_ptr<Enemy_Shell>"), "Enemy")
        self.assertEqual(f.class_of_handle("spp::Intrusive<Foe_Shell, a, b>"), "Foe")
        self.assertIsNone(f.class_of_handle("std::shared_ptr<int>"))

    def test_function_names(self):
        self.assertEqual(f.script_function_name("Player__heal"), "Player.heal")
        self.assertEqual(f.script_function_name("Player__heal_virt"), "Player.heal")
        self.assertEqual(f.script_function_name("Player__get_Health"), "Player.Health (read)")
        self.assertEqual(f.script_function_name("Player__set_Health"), "Player.Health (write)")
        self.assertEqual(f.script_function_name("spp::Player__heal(Player_Shell*)"), "Player.heal")
        self.assertIsNone(f.script_function_name("main"))


class Text(unittest.TestCase):
    def test_frame_label(self):
        self.assertEqual(f.frame_label("Player__heal", "/a/b/player.spp", 12), "Player.heal at player.spp:12")
        self.assertEqual(f.frame_label("Player__heal", None, None), "Player.heal")
        self.assertEqual(f.frame_label("helper", "x/y.spp", None), "helper at y.spp:?")
        self.assertIsNone(f.frame_label("main", "main.cpp", 3))

    def test_state_summary(self):
        text = f.state_summary("PlayerState_g2", [("health", "80.0"), ("shield", "25.0")])
        self.assertEqual(text, "Player {health=80.0, shield=25.0} [gen 2]")

    def test_many_fields(self):
        fields = [("f%d" % i, str(i)) for i in range(10)]
        self.assertTrue(f.format_fields("C", fields).endswith("... 2 more}"))

    def test_handle_summary(self):
        self.assertEqual(f.handle_summary("std::shared_ptr<Player_Shell>", 3, False), "Player handle (use_count=3)")
        self.assertEqual(f.handle_summary("std::shared_ptr<Player_Shell>", 0, True), "Player null")


if __name__ == "__main__":
    unittest.main()
