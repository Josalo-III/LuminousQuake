"""Generated Quake v29 BSP -> owned GPU scene, without a GPU or game assets.
GPL-2.0-or-later.
"""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
binary = Path(sys.argv[1]).resolve()

def run(root, *arguments, expected=0):
    result = subprocess.run([str(binary), '-basedir', str(root), '--scene-info', *arguments],
                            cwd=root, capture_output=True, text=True, timeout=15)
    output = result.stdout + result.stderr
    assert result.returncode == expected, output
    assert 'runtime error:' not in output and 'AddressSanitizer' not in output, output
    return output

lumps = [b''] * 15
lumps[0] = b'{\n"classname" "worldspawn"\n}\n{\n"classname" "info_player_start"\n"origin" "0 0 0"\n}\n\0'
lumps[0] = lumps[0][:-1] + (
    b'{"classname" "light" "origin" "8 16 32" "light" "500" "style" "32" '
    b'"spawnflags" "1" "targetname" "switch" "target" "aim" "angle" "25"}\n'
    b'{"classname" "light_torch_small_walltorch" "origin" "0 0 8"}\n'
    b'{"classname" "light" "origin" "0 0 7" "_color" "255 128 64"}\n'
    b'{"classname" "light" "origin" "0 0 9" "_light" "1 0.5 0.25 400"}\n'
    b'{"classname" "info_null" "origin" "8 16 0" "targetname" "aim"}\n'
    b'{"classname" "light" "origin" "0 0 10" "light" "2500" "delay" "3" "wait" "2" '
    b'"mangle" "90 -45 0" "_softangle" "20"}\n'
    b'{"classname" "light" "origin" "0 0 11" "delay" "9"}\n'
    b'{"classname" "light" "origin" "nan 0 9"}\n'
    b'{"classname" "light" "origin" "0 0 9" "style" "99999999999999"}\n\0')
lumps[1] = struct.pack('<4fi', 1, 0, 0, 128, 0)
texture = struct.pack('<16s6I', b'fixture', 16, 16, 40, 296, 360, 376) + bytes([128]) * 340
lumps[2] = struct.pack('<2i', 1, 8) + texture
lumps[3] = b''.join(struct.pack('<3f', *p) for p in [(128, -64, -64), (128, 64, -64), (128, 64, 64), (128, -64, 64)])
lumps[4] = b'\1'
lumps[5] = struct.pack('<i2h6h2H', 0, -1, -2, 127, -64, -64, 129, 64, 64, 0, 1)
lumps[6] = struct.pack('<8f2i', 0, 1, 0, 0, 0, 0, 1, 0, 0, 0)
lumps[7] = struct.pack('<Hhihh4Bi', 0, 0, 0, 4, 0, 0, 255, 255, 255, 0)
lumps[8] = bytes([128]) * 81
lumps[9] = struct.pack('<i2h', 0, -1, -2)
lumps[10] = (struct.pack('<ii6h2H4B', -2, -1, 127, -64, -64, 129, 64, 64, 0, 0, 0, 0, 0, 0) +
             struct.pack('<ii6h2H4B', -1, 0, 127, -64, -64, 129, 64, 64, 0, 1, 0, 0, 0, 0))
lumps[11] = struct.pack('<H', 0)
lumps[12] = b''.join(struct.pack('<2H', *e) for e in [(0, 0), (0, 1), (1, 2), (2, 3), (3, 0)])
lumps[13] = struct.pack('<4i', 1, 2, 3, 4)
lumps[14] = struct.pack('<9f7i', 127, -64, -64, 129, 64, 64, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1)

def bsp(entities):
    header = [29]
    body = b''
    for lump in [entities] + lumps[1:]:
        body += bytes((-len(body)) % 4)
        header += [124 + len(body), len(lump)]
        body += lump
    return struct.pack('<31i', *header) + body

# ericw-tools data (the rerelease): a `_sun` light aimed at a target, and a
# `_surface` template copied onto the 128-unit face (four 64-unit pieces,
# 2 units into the open leaf at x < 128). Neither stays a point light.
ericw = (b'{"classname" "worldspawn" "_sunlight" "1.0 1.0 1.0" "_sunlight_penumbra" "4"}\n'
         b'{"classname" "info_null" "origin" "8 16 0" "targetname" "aim"}\n'
         b'{"classname" "light" "origin" "0 0 64" "light" "80" "_sun" "1" "target" "aim"}\n'
         b'{"classname" "light" "origin" "-500 0 0" "light" "150" "_color" "255 0 0" '
         b'"_surface" "FIXTURE" "_surface_offset" "-2"}\n'
         b'{"classname" "light" "origin" "0 0 9"}\n\0')
# Worldspawn sunlight wins when stronger: intensity times the 0.5 range scale.
worldspawn_sun = (b'{"classname" "worldspawn" "_sunlight" "200" "_sun_mangle" "0 -60 0" '
                  b'"_sunlight_color" "255 128 0"}\n'
                  b'{"classname" "light" "origin" "0 0 64" "light" "80" "_sun" "1"}\n\0')
assets = {'gfx/palette.lmp': bytes(range(256)) * 3,
          'gfx/colormap.lmp': bytes(range(256)) * 64,
          'maps/fixture.bsp': bsp(lumps[0]),
          'maps/ericw.bsp': bsp(ericw),
          'maps/worldsun.bsp': bsp(worldspawn_sun)}
with tempfile.TemporaryDirectory(prefix='lq-bsp-') as temporary:
    root = Path(temporary)
    output = run(root, '-map', 'fixture', expected=1)
    assert 'Missing/invalid gfx/palette.lmp' in output, output
    (root / 'id1').mkdir()
    payload = b''
    directory = b''
    for name, data in assets.items():
        directory += struct.pack('<56sii', name.encode(), 12 + len(payload), len(data))
        payload += data
    (root / 'id1/pak0.pak').write_bytes(struct.pack('<4sii', b'PACK', 12 + len(payload), len(directory)) + payload + directory)
    output = run(root, '-map', 'fixture')
    assert '2 triangles, 1 draws, 1 indexed materials' in output, output
    assert 'Light inventory: 6 sources, 1 targeted, 1 styled, 2 unresolved' in output, output
    output = run(root, '-map', 'fixture', '--light-info')
    assert 'origin 8.000 16.000 32.000 intensity 500.000 style 32 spawnflags 1 cone 25.000 target [aim] targetname [switch]' in output, output
    assert 'light_torch_small_walltorch origin 0.000 0.000 8.000 intensity 300.000' in output, output
    assert 'origin 0.000 0.000 7.000 intensity 300.000' in output and 'color [255 128 64] flags 0' in output, output
    assert 'intensity 0.000 style 0 spawnflags 0 cone 0.000 target [] targetname [] rawlight [1 0.5 0.25 400] color [] flags 1' in output, output
    assert 'skipping invalid source entity' in output, output
    # A matched target aims a cone with half the full "angle"; LIGHT's default
    # linear falloff ends where the value drops below one unit.
    assert 'flags 0 formula 0 attenuation 1.000 range 499.0 spot 0.000 0.000 -1.000 outer 0.9763 inner 0.9763' in output, output
    assert 'formula 3 attenuation 2.000 range 16384.0 spot' in output, output
    assert '0.000 0.707 -0.707 outer 0.9397 inner 0.9848' in output, output  # mangle yaw 90, pitch -45
    assert 'origin 0.000 0.000 11.000 intensity 300.000' in output and 'flags 1 formula 0' in output, output
    output = run(root, '-map', 'ericw', '--light-info')
    assert 'Light inventory: 1 surface templates on 1 faces became 4 lights' in output, output
    assert 'Light inventory: sun 40 40 40 along 0.12 0.24 -0.96, penumbra 4 degrees' in output, output
    assert 'Light inventory: 5 sources' in output, output
    for corner in ('-32.000 -32.000', '32.000 -32.000', '-32.000 32.000', '32.000 32.000'):
        assert f'origin 126.000 {corner} intensity 150.000' in output, output
    assert 'origin -500.000' not in output and 'origin 0.000 0.000 64.000' not in output, output
    output = run(root, '-map', 'worldsun')
    assert 'Light inventory: sun 100 50 0 along 0.50 0.00 -0.87, penumbra 0 degrees' in output, output
    assert 'Light inventory: 0 sources' in output, output
    output = run(root, '-map', '../escape', expected=1)
    assert 'without a path or extension' in output, output
    output = run(root, '--run-seconds', 'nan', expected=1)
    assert '--run-seconds needs' in output, output
print('PASS generated BSP/PAK import, scene conversion, ericw-tools sun and surface templates, '
      'missing data and argument rejection (no GPU required)')
