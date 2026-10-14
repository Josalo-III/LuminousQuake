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
parser.add_argument('--renderer', choices=('software', 'classic'), default='software')
parser.add_argument('--capture', type=Path, help='Keep the verified frame at this explicit path')
args = parser.parse_args()
executable = args.executable.resolve()
packs = sorted((args.basedir.resolve() / 'id1').glob('pak*.pak'))
if not executable.is_file() or not packs:
    parser.error('Need a client executable and a basedir containing id1/pak*.pak')

with tempfile.TemporaryDirectory(prefix='lqr-') as directory:
    root = Path(directory)
    game = root / 'id1'
    game.mkdir()
    user = root / 'user'
    for pack in packs:
        try:
            (game / pack.name).symlink_to(pack)
        except OSError:
            shutil.copyfile(pack, game / pack.name)
    classic = args.renderer == 'classic'
    commands = ('lq_renderer ' + ('1' if classic else '0') + '\n' +
                'record lq_smoke e1m1\n' + 'wait\n' * 80 +
                '+forward\n+attack\n' + 'wait\n' * 40 +
                '-forward\n-attack\nstop\nsave lq_smoke\nload lq_smoke\n' +
                'wait\n' * 40 + 'playdemo lq_smoke\n' + 'wait\n' * 60)
    if classic:
        commands += ('map start\n' + 'wait\n' * 8 +
                     'lq_renderer 0\nvid_restart\n' + 'wait\n' * 8 +
                     'lq_renderer 1\nvid_restart\n')
    (game / 'autoexec.cfg').write_text(commands, encoding='ascii')
    (game / 'config.cfg').write_text('echo CLASSIC_CONFIG_SHOULD_NOT_LOAD\n', encoding='ascii')
    before = {p.name: p.read_bytes() for p in game.iterdir() if not p.is_symlink()}
    result = subprocess.run([str(executable), '-basedir', str(root), '-userdir', str(user), '-noudp',
                             '--run-seconds', '20' if classic else '8', '--screenshot', str(root / 'frame.bmp')],
                            cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=40)
    log = result.stdout
    expected = ('recording to ', 'Completed demo', 'Saving game to ',
                'Loading game from ', 'Playing demo from lq_smoke.dem.')
    failures = ('runtime error:', 'ERROR:', 'AddressSanitizer', 'Host_Error:', 'CLASSIC_CONFIG_SHOULD_NOT_LOAD',
                'Cvar_Set: variable campaign not found')
    if classic:
        expected += ('Classic GPU: copied live BSP world;',)
        failures += ('Software renderer fallback:',)
    checked_log = log.replace('Software renderer fallback: Software selected', 'Software renderer selected')
    good = (result.returncode == 0 and all(item in log for item in expected) and
            not any(item in checked_log for item in failures) and
            all((user / 'id1' / ('lq_smoke.' + ext)).stat().st_size > 100 for ext in ('dem', 'sav')) and
            (root / 'frame.bmp').stat().st_size > 1000)
    if classic:
        good = (good and log.count('Classic GPU: copied live BSP world;') >= 4 and
                'Software renderer fallback: Software selected' in log)
    good = good and before == {p.name: p.read_bytes() for p in game.iterdir() if not p.is_symlink()}
    if not good:
        print(log)
        raise SystemExit('FAIL content smoke; inspect the log above')
    if args.capture:
        shutil.copyfile(root / 'frame.bmp', args.capture.resolve())
    print('PASS content smoke: movement/attack command sequence, save/load, demo record/playback, frame capture')
    print('Unverified content remains a sample; this is not full gameplay or original-data validation.')
