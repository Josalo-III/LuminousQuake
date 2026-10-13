#!/usr/bin/env python3
"""Compile owned GLSL to Vulkan SPIR-V and Metal MSL. GPL-2.0-or-later.
Requires glslangValidator and spirv-cross; optional spirv-val validates SPIR-V.
Outputs embedded arrays so the running application has no shader-tool dependency.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='lq-shaders-') as temporary:
    root = args.root
    result = ['/* Generated from renderer/shaders by tools/build_shaders.py. GPL-2.0-or-later. */']
    for stage in ('vert', 'frag'):
        spirv = Path(temporary) / ('world.' + stage + '.spv')
        metal = Path(temporary) / ('world.' + stage + '.msl')
        subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.0',
                        str(root / 'renderer/shaders' / ('world.' + stage)), '-o', str(spirv)], check=True)
        if shutil.which('spirv-val'):
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.0', str(spirv)], check=True)
        subprocess.run(['spirv-cross', str(spirv), '--msl', '--msl-version', '20100',
                        '--msl-decoration-binding', '--output', str(metal)], check=True)
        for kind, path in [('spv', spirv), ('msl', metal)]:
            data = path.read_bytes()
            if kind == 'msl': data += b'\0'
            result.append('_Alignas(4) static const unsigned char lq_world_' + stage + '_' + kind + '[] = {')
            for offset in range(0, len(data), 24):
                result.append('    ' + ','.join(str(n) for n in data[offset:offset + 24]) + ',')
            result.append('};')
    destination = root / 'renderer/generated/world_shaders.h'
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text('\n'.join(result) + '\n')
