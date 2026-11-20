#!/usr/bin/env python3
"""Run the imported protocol models without a device."""
from pathlib import Path
import shutil, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
device = Path('/Users/mac/Documents/deepseek-harness/default-workspace/unixnu-fuxi/package')
# The adapter includes a few XNU headers for snprintf and errno. The host has no XNU tree, so the tests get
# stubs: the adapter uses nothing else from them.
stubs = Path(__file__).resolve().parent / 'xnu-stubs'
includes = ['-I', str(root / 'baseband'), '-I', str(root / 'ipa'), '-I', str(root / 'platform/fuxi'),
            '-I', str(device), '-I', str(stubs)]
with tempfile.TemporaryDirectory(prefix='qcom-baseband-') as name:
    for source in sorted((root / 'tests').glob('*_test.c')):
        binary = Path(name) / source.stem
        subprocess.run([shutil.which('cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', *includes, str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
