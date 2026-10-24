#!/usr/bin/env python3
"""Exercise the build script's signing selection without building an APK."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('build_android.sh').read_text()
SIGNING = SCRIPT.split('echo "=== Preparing release signing key ==="', 1)[1].split(
    'echo "=== Building Melee Android APK ==="', 1)[0]


class AndroidSigning(unittest.TestCase):
    def run_signing(self, custom=False, exists=True, encoded=False, local_credentials=False):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            android = work / 'android'
            android.mkdir()
            key = work / 'custom key.keystore' if custom else android / 'melee-release.keystore'
            if exists:
                key.write_bytes(b'key')
            env = {k: v for k, v in os.environ.items() if not k.startswith('MELEE_')}
            env.update(ANDROID_DIR=str(android), BUILD_DIR=str(work))
            if custom:
                env['MELEE_KEYSTORE_FILE'] = str(key)
            credential_check = ''
            if local_credentials:
                (android / 'release-signing.env').write_text(
                    'MELEE_KEYSTORE_PASSWORD=local\nMELEE_KEY_ALIAS=local\nMELEE_KEY_PASSWORD=local\n')
                env.update(MELEE_KEYSTORE_PASSWORD='custom', MELEE_KEY_ALIAS='custom',
                           MELEE_KEY_PASSWORD='custom')
                credential_check = '\n[[ $MELEE_KEYSTORE_PASSWORD == custom && $MELEE_KEY_ALIAS == custom && $MELEE_KEY_PASSWORD == custom ]]'
            if encoded:
                env['MELEE_KEYSTORE_BASE64'] = 'a2V5'
            result = subprocess.run(
                ['bash', '-c', 'set -euo pipefail\n' + SIGNING +
                 '\nprintf "%s" "$MELEE_KEYSTORE_FILE"' + credential_check],
                env=env, text=True, capture_output=True)
            if exists or encoded:
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, str(key))
                self.assertEqual(key.read_bytes(), b'key')
            else:
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('error: no signing key', result.stderr)

    def test_custom_key_outside_android_directory(self):
        self.run_signing(custom=True)

    def test_custom_credentials_override_local_defaults(self):
        self.run_signing(custom=True, local_credentials=True)

    def test_default_key_is_exported_for_gradle(self):
        self.run_signing()

    def test_missing_custom_key_fails(self):
        self.run_signing(custom=True, exists=False)

    def test_base64_key_uses_selected_path(self):
        self.run_signing(custom=True, exists=False, encoded=True)


if __name__ == '__main__':
    unittest.main()
