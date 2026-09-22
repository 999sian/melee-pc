#!/usr/bin/env python3
"""Inject inherited mode settings; inspect launch environments without a game."""
import os
import tempfile
import unittest
from unittest.mock import patch

import net_test
from net_test_support import standalone_compile_args


class FixtureEnvironmentTest(unittest.TestCase):
    def test_standalone_compiler_cannot_write_build_dependencies(self):
        entry = {"file": "/source/game.c", "arguments": [
            "cc", "-I/source include", "-DTARGET_PC=1", "-MD", "-MMD", "-MP",
            "-MF", "build/game.d", "-MTbuild/game.o", "-MQ", "build/game.o",
            "-DNDEBUG", "-o", "build/game.o", "-c", "/source/game.c"]}
        self.assertEqual(standalone_compile_args(entry),
                         ["cc", "-I/source include", "-DTARGET_PC=1"])

    def test_inherited_mode_settings_cannot_override_selected_mode(self):
        polluted = {
            "MELEE_DEBUG_VS": "cpu",
            "MELEE_NET": "192.0.2.1:1",
            "MELEE_NET_PLAYER": "3",
            "MELEE_NET_REPLAY": "/injected/replay.rec",
            # A stale key is worse than none: a LAN fixture that kept it would
            # pin a key its peer cannot derive, and every datagram between
            # them would go unauthenticated instead (src/pc/net_wire.c).
            "MELEE_NET_KEY": "stale",
        }
        for lan in (True, False):
            with self.subTest(lan=lan), tempfile.TemporaryDirectory() as work:
                with patch.dict(os.environ, polluted, clear=True), \
                     patch.object(net_test, "CACHE_ROOT", work), \
                     patch.object(net_test.subprocess, "Popen") as launch:
                    inst = net_test.Instance("a", "unused", "unused", work,
                                             42050, 42051, {}, lan)
                    inst.log.close()
                    env = launch.call_args.kwargs["env"]
                self.assertNotIn("MELEE_NET_REPLAY", env)
                if lan:
                    for key in polluted:
                        self.assertNotIn(key, env)
                else:
                    self.assertEqual(env["MELEE_NET"], "127.0.0.1:42051")
                    self.assertEqual(env["MELEE_NET_PLAYER"], "0")
                    self.assertEqual(env["MELEE_NET_KEY"], net_test.NET_KEY)
                    self.assertEqual(env["MELEE_DEBUG_VS"], "1")


if __name__ == "__main__":
    unittest.main()
