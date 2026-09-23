#!/usr/bin/env python3
"""Install the pinned Emscripten SDK into build/browser/emsdk (or MELEE_EMSDK)."""
import subprocess

from common import EMSCRIPTEN_VERSION, SDK

if not SDK.exists():
    subprocess.run(['git', 'clone', 'https://github.com/emscripten-core/emsdk.git', str(SDK)], check=True)
for action in ('install', 'activate'):
    subprocess.run([str(SDK / 'emsdk'), action, EMSCRIPTEN_VERSION], check=True)
