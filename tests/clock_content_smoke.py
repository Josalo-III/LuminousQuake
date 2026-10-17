"""Opt-in real-data clock integration check; reads PAKs without modifying them."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('basedir', type=Path)
parser.add_argument('--renderer', choices=('software', 'classic', 'luminous'), default='software')
args = parser.parse_args()
binary, installation = args.executable.resolve(), args.basedir.resolve()
packs = [p for p in (installation / 'id1').iterdir() if p.suffix.lower() == '.pak']
assert binary.is_file() and packs
with tempfile.TemporaryDirectory(prefix='lqclock-') as directory:
    root = Path(directory)
    (root / 'id1').mkdir()
    for pack in packs:
        (root / 'id1' / pack.name).symlink_to(pack)
    renderer = ('software', 'classic', 'luminous').index(args.renderer)
    commands = (f'lq_renderer {renderer}\nhost_framerate 0\nlq_clock_mode 2\nmap e1m1\n' +
                'wait\n' * 180 + 'lq_clock_report\nlq_clock_mode 1\n' +
                'wait\n' * 72 + 'lq_clock_report\nlq_clock_mode 0\n' +
                'wait\n' * 180 + 'lq_clock_report\necho CLOCK_SMOKE_DONE\n')
    (root / 'id1/autoexec.cfg').write_text(commands)
    result = subprocess.run([str(binary), '-basedir', str(root), '-userdir', str(root/'user'),
        '-noudp', '-nosound', '--run-seconds', '9'], capture_output=True, text=True, timeout=25)
    log = result.stdout + result.stderr
    assert result.returncode == 0 and 'CLOCK_SMOKE_DONE' in log, log
    assert not any(x in log for x in ('Host_Error:', 'runtime error:', 'AddressSanitizer', 'not found')), log
    reports = re.findall(r'Tick clock: ([^;]+);.*?tick=(\d+) published=(\d+)', log)
    assert len(reports) == 3, log
    assert [r[0] for r in reports] == ['continuous', 'wait until', 'early wake'], log
    ticks = [int(x[1]) for x in reports]
    assert ticks[1] - ticks[0] >= 71 and ticks[2] - ticks[1] >= 179, log
    assert all(int(published) <= int(tick) for _,tick,published in reports), log
    print('\n'.join(x for x in log.splitlines() if x.startswith('Tick clock:')))
    print('PASS real-time simulation across continuous, wait-until, and early-wake clock modes')
