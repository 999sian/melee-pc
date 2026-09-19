#!/usr/bin/env python3
"""Inject inherited mode settings; inspect launch environments without a game."""
import os
import tempfile
import unittest
from unittest.mock import patch

import net_test


class FixtureEnvironmentTest(unittest.TestCase):
    def test_inherited_mode_settings_cannot_override_selected_mode(self):
        polluted = {
            "MELEE_DEBUG_VS": "cpu",
            "MELEE_NET": "192.0.2.1:1",
            "MELEE_NET_PLAYER": "3",
            "MELEE_NET_REPLAY": "/injected/replay.rec",
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
                    self.assertEqual(env["MELEE_DEBUG_VS"], "1")


if __name__ == "__main__":
    unittest.main()
