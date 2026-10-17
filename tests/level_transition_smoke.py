#!/usr/bin/env python3
"""Stress local level changes with real game data and catch-up ticks.

Uses temporary profiles and read-only PAK links. GPL-2.0-or-later.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('basedir', type=Path)
parser.add_argument('--run-seconds', type=int, default=3, help='Allow more wall time for a windowed client')
args = parser.parse_args()
binary = args.executable.resolve()
installation = args.basedir.resolve()
packs = sorted((installation / 'id1').glob('pak*.pak', case_sensitive=False))
if not binary.is_file() or not packs:
    parser.error('Need an executable and id1/pak*.pak game data')

with tempfile.TemporaryDirectory(prefix='lqt-') as directory:
    root = Path(directory)
    game = root / 'id1'
    game.mkdir()
    for pack in packs:
        (game / pack.name).symlink_to(pack)
    kpf = installation / 'QuakeEX.kpf'
    if kpf.exists():
        (root / kpf.name).symlink_to(kpf)
    # Seven simulation ticks per frame force transitions to occur between
    # queued updates from the old level and the client's next rendered frame.
    commands = 'host_framerate 0.1\nskill 1\nmap start\n' + 'wait\n' * 40
    for number, level in enumerate(('e1m1', 'start', 'e1m1')):
        commands += f'changelevel {level}\n' + 'wait\n' * 40
        commands += f'echo TRANSITION_{number}_COMPLETE\n'
    commands += 'restart\n' + 'wait\n' * 40 + 'echo RESTART_COMPLETE\n'
    (game / 'autoexec.cfg').write_text(commands, encoding='ascii')
    result = subprocess.run(
        [str(binary), '-basedir', str(root), '-userdir', str(root / 'user'),
         '-noudp', '-nosound', '--run-seconds', str(args.run_seconds)],
        cwd=root, capture_output=True, text=True, timeout=max(60, args.run_seconds + 15))
    log = result.stdout + result.stderr
    assert result.returncode == 0, log
    for marker in ('TRANSITION_0_COMPLETE', 'TRANSITION_1_COMPLETE',
                   'TRANSITION_2_COMPLETE', 'RESTART_COMPLETE'):
        assert marker in log, log
    assert not any(error in log for error in
                   ('Host_Error:', 'runtime error:', 'AddressSanitizer',
                    'i >= cl.maxclients', 'SZ_GetSpace: overflow')), log
    print(f'PASS {installation.name}: three level changes and restart during catch-up frames')
