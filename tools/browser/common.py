"""Paths and toolchain locations shared by the browser build scripts."""
import os
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'build/browser'

# Emscripten has no GCC, and Clang has no scalar_storage_order, so game C goes
# through disc_lower (LLVM LibTooling) before emcc. See platforms/browser/README.md.
EMSCRIPTEN_VERSION = '6.0.9'
LLVM = Path(os.environ.get('LLVM_ROOT', '/opt/homebrew/opt/llvm@22'))
SDK = Path(os.environ.get('MELEE_EMSDK', BUILD / 'emsdk'))
EMSCRIPTEN = SDK / 'upstream/emscripten'
SYSROOT = EMSCRIPTEN / 'cache/sysroot'
DISC_LOWER = Path(os.environ.get('DISC_LOWER', BUILD / 'disc_lower'))
WASM_TARGET = '--target=wasm32-unknown-emscripten'


def node():
    """The SDK's bundled node, else whatever is on PATH."""
    bundled = sorted((SDK / 'node').glob('*/bin/node'))
    return str(bundled[-1]) if bundled else shutil.which('node')
