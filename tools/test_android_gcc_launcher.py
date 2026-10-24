#!/usr/bin/env python3
"""Check the real GCC bridge's emitted commands without requiring an NDK."""
import os
from pathlib import Path
import runpy
import sys
import unittest
from unittest.mock import patch

LAUNCHER = Path(__file__).with_name('gcc_launcher.py')


class AndroidGccLauncher(unittest.TestCase):
    def command(self, args):
        env = {'ANDROID_NDK_HOME': '/fake/ndk',
               'GCC_AARCH64_BIN': sys.executable, 'GCC_X86_64_BIN': sys.executable}
        with patch.dict(os.environ, env), patch.object(sys, 'platform', 'linux'), \
                patch.object(sys, 'argv', [str(LAUNCHER), '/fake/clang', *args]), \
                patch('os.execv', side_effect=RuntimeError('captured exec')) as execute:
            with self.assertRaisesRegex(RuntimeError, 'captured exec'):
                runpy.run_path(str(LAUNCHER), run_name='__main__')
        return execute.call_args.args

    def test_arm64_reserves_android_register_and_uses_baseline_isa(self):
        compiler, args = self.command([
            '--target=aarch64-none-linux-android26', '--sysroot=/fake/ndk/sysroot',
            '-D_FORTIFY_SOURCE=2', '-c', '/project/src/melee/game.c', '-o', 'game.o'])
        self.assertEqual(compiler, sys.executable)
        self.assertIn('-ffixed-x18', args)
        self.assertIn('-march=armv8-a', args)
        self.assertNotIn('-march=armv8-a+crc+crypto', args)
        self.assertIn('/fake/ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/aarch64-linux-android', args)
        self.assertNotIn('--target=aarch64-none-linux-android26', args)
        self.assertNotIn('-D_FORTIFY_SOURCE=2', args)
        self.assertIn('-D__ANDROID_API__=26', args)

    def test_x86_64_keeps_its_own_abi(self):
        _, args = self.command(['--target=x86_64-none-linux-android26',
                                '-c', '/project/src/melee/game.c'])
        self.assertIn('-march=x86-64', args)
        self.assertNotIn('-ffixed-x18', args)
        self.assertNotIn('-march=armv8-a', args)
        self.assertIn('/fake/ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/x86_64-linux-android', args)

    def test_nondecomp_forwards_to_clang_unchanged(self):
        original = ['--target=aarch64-none-linux-android26', '-c', '/project/src/pc/main.c']
        compiler, args = self.command(original)
        self.assertEqual(compiler, '/fake/clang')
        self.assertEqual(args, ['/fake/clang', *original])


if __name__ == '__main__':
    unittest.main()
