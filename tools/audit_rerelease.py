#!/usr/bin/env python3
"""Read-only QuakeC/source and optional PAK program inventory; GPL-2.0-or-later.
This is lexical analysis, not an FTE preprocessor/compiler or proof of compatibility.
No downloaded code is executed and no game files are changed.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess


def clean(text):
    pattern = r'("(?:\\.|[^"\\])*")|//[^\n]*|/\*.*?\*/'
    return re.sub(pattern, lambda m: m[1] if m[1] else '\n' * m[0].count('\n'), text, flags=re.S)


def revision(root):
    return subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()


def declarations(text):
    result = []
    for match in re.finditer(r'(?m)^([^\n;{}]*?)=\s*#(\d+)(?::(\w+))?\s*;', text):
        prefix = match[1].strip()
        name = re.search(r'(\w+)\s*$', prefix)
        if not name:
            name = re.search(r'(\w+)\s*\(', prefix)
        if not name:
            raise ValueError('Cannot identify builtin declaration: ' + match[0])
        result.append({'name': name[1], 'number': int(match[2]), 'binding': match[3]})
    return result


def system_blocks(text):
    text = clean(text)
    globals_block, rest = re.split(r'\bend_sys_globals\s*;', text, maxsplit=1)
    fields_block = re.split(r'\bend_sys_fields\s*;', rest, maxsplit=1)[0]
    # Whitespace/comments do not affect these fixed engine-owned declarations.
    tokenize = lambda block: re.findall(r'\w+|[.(),;]', block)
    return {'globals': tokenize(globals_block), 'fields': tokenize(fields_block)}


def inventory(root):
    source = clean((root / 'progs.src').read_text(encoding='utf-8-sig'))
    files = []
    for line in source.splitlines():
        name = line.strip().strip('"')
        if name.endswith('.qc') and not name.startswith('#'):
            path = (root / name).resolve()
            if not path.is_relative_to(root.resolve()):
                raise ValueError('Manifest path outside source root')
            files.append(path)
    parts = {str(p.relative_to(root)): clean(p.read_text(encoding='utf-8-sig')) for p in files}
    defs = parts['defs.qc']
    body = '\n'.join(text for name, text in parts.items() if name != 'defs.qc')
    builtins = declarations(defs)
    for builtin in builtins:
        builtin['call_sites_outside_defs'] = len(re.findall(r'\b' + re.escape(builtin['name']) + r'\s*\(', body))
    constants = dict(re.findall(r'\bfloat\s+((?:MOVETYPE|SOLID|SVC|TE|EF)_[A-Z0-9_]+)\s*=\s*([^;\n]+);', defs))
    uses = {}
    for name in constants:
        locations = []
        for filename, text in parts.items():
            if filename == 'defs.qc':
                continue
            for n, line in enumerate(text.splitlines(), 1):
                if re.search(r'\b' + name + r'\b', line):
                    locations.append(f'{filename}:{n}')
        if locations:
            uses[name] = locations
    return {'manifest_files': len(files), 'builtins': builtins, 'constants': constants,
            'constant_uses_outside_defs': uses,
            'extension_queries': sorted(set(re.findall(r'checkextension\s*\(\s*"([^"\n]+)"', body))),
            'cvar_reads': sorted(set(re.findall(r'\bcvar\s*\(\s*"([^"\n]+)"', body)))}


def engine_builtins(root):
    text = clean((root / 'WinQuake/pr_cmds.c').read_text())
    body = text.split('builtin_t pr_builtin[]', 1)[1].split('};', 1)[0]
    body = re.sub(r'#ifdef QUAKE2.*?#else(.*?)#endif', lambda m: m[1], body, flags=re.S)
    names = re.findall(r'\b(?:PF_|SV_)\w+', body)
    return {n: name for n, name in enumerate(names)}


def program_inventory(pak, table):
    with pak.open('rb') as file:
        magic, offset, length = struct.unpack('<4sii', file.read(12))
        if magic != b'PACK' or offset < 12 or length < 0 or length % 64:
            raise ValueError('Invalid PAK directory')
        file.seek(offset)
        directory = file.read(length)
        if len(directory) != length:
            raise ValueError('Truncated PAK directory')
        data = None
        for name, position, size in struct.iter_unpack('<56sii', directory):
            if name.split(b'\0')[0] == b'progs.dat':
                if position < 12 or size < 60 or size > 64 * 1024 * 1024:
                    raise ValueError('Invalid progs.dat range')
                file.seek(position)
                data = file.read(size)
                if len(data) != size:
                    raise ValueError('Truncated progs.dat')
                break
    if data is None:
        raise ValueError('PAK has no progs.dat')
    header = struct.unpack_from('<15i', data)
    if header[0] != 6:
        raise ValueError('This inventory supports QuakeC v6 only')
    for offset, count, size in [(header[2], header[3], 8), (header[8], header[9], 36),
                                (header[10], header[11], 1)]:
        if offset < 60 or count < 0 or count * size > len(data) - offset:
            raise ValueError('Invalid program lump')
    strings = data[header[10]:header[10]+header[11]]
    builtins = []
    for i in range(header[9]):
        function = struct.unpack_from('<7i8B', data, header[8]+i*36)
        if function[0] < 0:
            if not 0 <= function[4] < len(strings):
                raise ValueError('Invalid function string offset')
            name = strings[function[4]:].split(b'\0', 1)[0].decode('ascii', errors='replace')
            number = -function[0]
            implementation = table.get(number)
            builtins.append({'name': name, 'number': number, 'engine': implementation,
                             'supported_slot': implementation is not None and implementation != 'PF_Fixme'})
    opcodes = collections.Counter(struct.unpack_from('<H', data, header[2]+i*8)[0] for i in range(header[3]))
    return {'sha256': hashlib.sha256(data).hexdigest(), 'version': header[0], 'system_crc': header[1],
            'entity_fields': header[14], 'functions': header[9], 'statements': header[3],
            'opcodes': sorted(opcodes), 'builtins': builtins}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rerelease', required=True, type=Path)
    parser.add_argument('--original', required=True, type=Path, help='Quake-Tools checkout')
    parser.add_argument('--engine', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--pak', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    table = engine_builtins(args.engine)
    result = {'rerelease_revision': revision(args.rerelease), 'original_revision': revision(args.original),
              'engine_revision': revision(args.engine), 'engine_worktree_changes_not_in_revision': True,
              'analysis': 'lexical, manifest-based; conditional compilation and runtime reachability not evaluated',
              'engine_numeric_builtins': table, 'original': inventory(args.original / 'qcc/v101qc'),
              'rerelease': {p.name: inventory(p) for p in sorted(args.rerelease.glob('quakec*')) if (p/'progs.src').exists()}}
    baseline = system_blocks((args.original/'qcc/v101qc/defs.qc').read_text())
    for name, entry in result['rerelease'].items():
        blocks = system_blocks((args.rerelease/name/'defs.qc').read_text())
        entry['original_system_layout_matches'] = {key: blocks[key] == baseline[key] for key in baseline}
    if args.pak:
        result['installed_program'] = program_inventory(args.pak, table)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print('Engine builtin slots:', len(table), 'including the unimplemented zero slot')
    for name, entry in result['rerelease'].items():
        named = [b for b in entry['builtins'] if b['binding']]
        numeric = [b for b in entry['builtins'] if not b['binding'] and b['number'] not in table]
        print(name, 'files', entry['manifest_files'], 'named builtins', len(named),
              'missing numeric slots', [(b['name'], b['number']) for b in numeric])
    if args.pak:
        print('Installed missing slots:', [(b['name'], b['number']) for b in result['installed_program']['builtins'] if not b['supported_slot']])


if __name__ == '__main__':
    main()
