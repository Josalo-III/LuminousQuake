"""Generate small Quake-format fixtures and exercise the real native engine.
No retail assets are used. GPL-2.0-or-later.
"""
import pathlib
import struct
import subprocess
import sys
import tempfile

binary = pathlib.Path(sys.argv[1]).resolve()


def run(root, args, expected=0, contains=None):
    result = subprocess.run([str(binary), '-userdir', str(root / 'user'), *args], cwd=root, capture_output=True,
                            text=True, timeout=10)
    output = result.stdout + result.stderr
    assert result.returncode == expected, (args, result.returncode, output)
    assert "runtime error:" not in output and "ERROR: AddressSanitizer" not in output, output
    if contains:
        assert contains in output, (args, output)
    return output


def progs(entityfields, misalign=False):
    statements = [(0, 0, 0, 0), (6, 100, 101, 102), (0, 102, 0, 0),
                  (30, 103, 104, 105), (37, 102, 105, 0),
                  (24, 103, 104, 106), (0, 106, 0, 0)]
    # OPCODE constants: ADD_F=6, ADDRESS=30, STOREP_F=37, LOAD_F=24.
    functions = [(0, 0, 0, 0, 0, 0, 0), (1, 110, 0, 0, 1, 1, 0),
                 (3, 110, 0, 0, 1, 1, 0)]
    lumps = [b"".join(struct.pack("<Hhhh", *x) for x in statements),
             b"", b"", b"".join(struct.pack("<7i8B", *x, *([0] * 8)) for x in functions),
             b"\0test\0\0\0", bytes(128 * 4)]
    counts = [len(statements), 0, 0, len(functions), len(lumps[4]), 128]
    body = b"X" if misalign else b""
    header = [6, 5927]
    for data, count in zip(lumps, counts):
        header += [60 + len(body), count]
        body += data
    return struct.pack("<15i", *header, entityfields) + body


with tempfile.TemporaryDirectory(prefix="luminousquake-test-") as tmp:
    root = pathlib.Path(tmp)
    id1 = root / "id1"
    id1.mkdir()
    run(root, ["--check-data", "-basedir", str(root)], 2,
        "Original Quake data is missing")
    run(root, ["-noudp"], 1, "couldn't load gfx.wad")
    # Loose fixture files exercise discovery, without copying game assets.
    (id1 / "gfx").mkdir()
    (id1 / "gfx.wad").write_bytes(struct.pack("<4sii", b"WAD2", 0, 12))
    (id1 / "gfx/palette.lmp").write_bytes(bytes(768))
    (id1 / "gfx/colormap.lmp").write_bytes(bytes(64 * 256 + 4))
    # The generated entity field count is supplied by the native diagnostic.
    layout = run(root, ["--self-test"])
    fields = int(layout.split("entityfields=")[1].split()[0])
    args = ["--self-test", "--test-progs", "-basedir", str(root)]
    for misalign in [False, True]:
        (id1 / "progs.dat").write_bytes(progs(fields, misalign))
        run(root, args, contains="PASS QuakeC v6 loader")
    run(root, ["--check-data", "-basedir", str(root)], contains="Required files found")
    (id1 / "progs.dat").write_bytes(b"truncated")
    run(root, args, 1, "truncated header")
    data = bytearray(progs(fields))
    struct.pack_into("<i", data, 8, len(data) + 1)
    (id1 / "progs.dat").write_bytes(data)
    run(root, args, 1, "invalid lump range")
    (id1 / "progs.dat").write_bytes(progs(fields))
    # Execute an actual dedicated Host_Init/Host_Frame and command shutdown.
    (id1 / "quake.rc").write_text("echo FIXTURE_STARTUP_OK\ncampaign\ncampaign 7\ncampaign\nquit\n")
    mode = [] if "server" in binary.name else ["-dedicated"]
    output = run(root, [*mode, "-noudp", "-basedir", str(root)],
                 contains="FIXTURE_STARTUP_OK")
    assert '"campaign" is "0"' in output and '"campaign" is "7"' in output, output
    # PAK discovery has to initialize endian helpers even in diagnostics mode.
    payload = b""; directory = b""
    for name in ["gfx.wad", "gfx/palette.lmp", "gfx/colormap.lmp", "progs.dat"]:
        content = (id1 / name).read_bytes()
        directory += struct.pack("<56sii", name.encode(), 12 + len(payload), len(content))
        payload += content
    (id1 / "pak0.pak").write_bytes(struct.pack("<4sii", b"PACK", 12 + len(payload),
                                               len(directory)) + payload + directory)
    run(root, ["--check-data", "-basedir", str(root)], contains="Required files found")
print("PASS generated QuakeC, rejected malformed headers/ranges, loose/PAK discovery, dedicated startup")
