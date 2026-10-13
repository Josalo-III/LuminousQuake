#!/usr/bin/env python3
"""Opt-in content smoke test. Uses local PAKs; never modifies the installation.
GPL-2.0-or-later. Not a comprehensive or original-data compatibility test.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('basedir', type=Path)
args = parser.parse_args()
executable = args.executable.resolve()
packs = sorted((args.basedir.resolve() / 'id1').glob('pak*.pak'))
if not executable.is_file() or not packs:
    parser.error('Need a client executable and a basedir containing id1/pak*.pak')

with tempfile.TemporaryDirectory(prefix='lqr-') as directory:
    root = Path(directory)
    game = root / 'id1'
    game.mkdir()
    for pack in packs:
        try:
            (game / pack.name).symlink_to(pack)
        except OSError:
            shutil.copyfile(pack, game / pack.name)
    commands = ('record lq_smoke e1m1\n' + 'wait\n' * 80 +
                '+forward\n+attack\n' + 'wait\n' * 40 +
                '-forward\n-attack\nstop\nsave lq_smoke\nload lq_smoke\n' +
                'wait\n' * 40 + 'playdemo lq_smoke\n' + 'wait\n' * 60)
    (game / 'autoexec.cfg').write_text(commands, encoding='ascii')
    result = subprocess.run([str(executable), '-basedir', str(root), '-noudp',
                             '--run-seconds', '8', '--screenshot', str(root / 'frame.bmp')],
                            cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=25)
    log = result.stdout
    expected = ('recording to ', 'Completed demo', 'Saving game to ',
                'Loading game from ', 'Playing demo from lq_smoke.dem.')
    failures = ('runtime error:', 'ERROR:', 'AddressSanitizer', 'Host_Error:',
                'Cvar_Set: variable campaign not found')
    good = (result.returncode == 0 and all(item in log for item in expected) and
            not any(item in log for item in failures) and
            all((game / ('lq_smoke.' + ext)).stat().st_size > 100 for ext in ('dem', 'sav')) and
            (root / 'frame.bmp').stat().st_size > 1000)
    if not good:
        print(log)
        raise SystemExit('FAIL content smoke; inspect the log above')
    print('PASS content smoke: movement/attack command sequence, save/load, demo record/playback, frame capture')
    print('Unverified content remains a sample; this is not full gameplay or original-data validation.')
