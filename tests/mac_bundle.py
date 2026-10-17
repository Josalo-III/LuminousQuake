#!/usr/bin/env python3
"""Relocate the app and verify dyld loads its embedded SDL. GPL-2.0-or-later."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='lq-bundle-') as directory:
    # Spaces and a different working directory catch accidental path assumptions.
    app = Path(directory).resolve() / 'Moved Game' / source.name
    shutil.copytree(source, app, symlinks=True)
    binary = app / 'Contents/MacOS/LuminousQuake'
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(('DYLD_', 'SDL_'))}
    environment['DYLD_PRINT_LIBRARIES'] = '1'
    result = subprocess.run([str(binary), '--version'], cwd=directory, env=environment,
                            capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stderr
    libraries = re.findall(r'^dyld\[\d+\]: <[^>]+> (.+)$', result.stderr, re.M)
    embedded = str(app / 'Contents/Frameworks/libSDL3.0.dylib')
    assert embedded in libraries, f'Embedded SDL was not loaded; SDL images: {[p for p in libraries if "SDL" in p]}'
    external = [path for path in libraries if not (
        path.startswith(('/System/Library/', '/usr/lib/')) or
        Path(path).is_relative_to(app))]
    assert not external, f'Libraries loaded outside app/system: {external}'
    print('PASS relocated app loads bundled SDL; no external non-system libraries')
    # Without its own SDL, this copy must fail instead of falling back to Homebrew.
    (app / 'Contents/Frameworks/libSDL3.0.dylib').unlink()
    result = subprocess.run([str(binary), '--version'], cwd=directory, env=environment,
                            capture_output=True, text=True, timeout=15)
    assert result.returncode != 0, 'App silently used an external SDL'
    print('PASS missing bundled SDL fails instead of loading a developer-machine copy')
