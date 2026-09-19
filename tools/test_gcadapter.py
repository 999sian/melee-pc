#!/usr/bin/env python3
"""Build the real adapter parser with stubbed device output."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=root / 'build')
options = parser.parse_args()
entries = json.loads((options.build / 'compile_commands.json').read_text())
entry = next(e for e in entries if e['file'].endswith('/pc/gcadapter.c'))
original = entry.get('arguments') or shlex.split(entry['command'])
args = []
i = 0
while i < len(original):
    arg = original[i]
    if arg in ('-o', '-c', '-MF', '-MT', '-MQ'):
        i += 2
        continue
    if arg not in ('-DNDEBUG', '-MD', '-MMD', '-MP') and not arg.startswith(('-MF', '-MT', '-MQ')):
        args.append(arg)
    i += 1

with tempfile.TemporaryDirectory(prefix='gcadapter-test-') as directory:
    executable = str(Path(directory) / 'test')
    subprocess.run(args + [
        '-UNDEBUG', '-pthread', '-ffunction-sections', '-fdata-sections',
        '-Wl,--gc-sections', '-no-pie', str(root / 'tools/test_gcadapter.c'),
        '-o', executable,
    ], cwd=entry['directory'], check=True)
    subprocess.run([executable], check=True)
