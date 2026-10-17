#!/usr/bin/env python3
"""Check the half-console compositor with local game data and a real GPU.

Uses temporary profiles and reads the supplied installation without modifying it.
GPL-2.0-or-later.
"""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('basedir', type=Path)
parser.add_argument('--renderer', choices=('classic', 'luminous'), default='classic')
parser.add_argument('--capture', type=Path)
args = parser.parse_args()
binary, installation = args.executable.resolve(), args.basedir.resolve()
packs = sorted((installation / 'id1').glob('pak*.pak', case_sensitive=False))
if not binary.is_file() or not packs:
    parser.error('Need an executable and an installation containing id1/pak*.pak')

with tempfile.TemporaryDirectory(prefix='lqc-') as directory:
    root = Path(directory)
    game = root / 'id1'
    game.mkdir()
    for pack in packs:
        (game / pack.name).symlink_to(pack)
    kpf = installation / 'QuakeEX.kpf'
    if kpf.exists():
        (root / kpf.name).symlink_to(kpf)
    mode = 2 if args.renderer == 'luminous' else 1
    # Settle at E1M1's spawn, pause, then leave the console half open. The
    # uncovered world must still appear between the console and status bar.
    (game / 'autoexec.cfg').write_text(
        f'lq_renderer {mode}\nvid_width 800\nvid_height 600\n'
        'map e1m1\n' + 'wait\n' * 80 +
        'pause\ntoggleconsole\necho CONSOLE_SMOKE_READY\n', encoding='ascii')
    frame = root / 'console.bmp'
    result = subprocess.run(
        [str(binary), '-basedir', str(root), '-userdir', str(root / 'user'),
         '-noudp', '-nosound', '--run-seconds', '10', '--screenshot', str(frame)],
        cwd=root, capture_output=True, text=True, timeout=90)
    log = result.stdout + result.stderr
    assert result.returncode == 0 and 'CONSOLE_SMOKE_READY' in log, log
    assert 'Software renderer fallback:' not in log, log
    assert not any(error in log for error in ('Host_Error:', 'runtime error:', 'AddressSanitizer')), log
    data = frame.read_bytes()
    if args.capture:
        shutil.copyfile(frame, args.capture.resolve())
    offset = struct.unpack_from('<I', data, 10)[0]
    width, height = struct.unpack_from('<ii', data, 18)
    bits, compression = struct.unpack_from('<HI', data, 28)
    assert bits in (24, 32) and compression in (0, 3), (bits, compression)
    stride = ((width * bits + 31) // 32) * 4
    visible = samples = 0
    for y in range(abs(height) * 3 // 5, abs(height) * 3 // 4):
        row = abs(height) - 1 - y if height > 0 else y
        for x in range(width // 4, width * 3 // 4):
            pixel = offset + row * stride + x * (bits // 8)
            visible += max(data[pixel:pixel + 3]) > 8
            samples += 1
    fraction = visible / samples
    assert fraction > 0.1, f'World below console is blank ({fraction:.1%} visible pixels)'
    print(f'PASS {installation.name} / {args.renderer}: world below console remains visible ({fraction:.1%})')
