#!/usr/bin/env python3
"""Exercise real CLI processes on typed ATX fixtures and fixed GDB metadata."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile


def write_atx(path, encoded, flags):
    width = len(encoded[0])
    page = bytearray(4096)
    capacity = (4096 - 12) // (4 + width)
    assert len(encoded) <= capacity
    struct.pack_into('<I', page, 4, len(encoded))
    for i, value in enumerate(encoded):
        struct.pack_into('<I', page, 12 + 4 * i, i + 1)
        page[12 + capacity * 4 + i * width:12 + capacity * 4 + (i + 1) * width] = value
    trailer = struct.pack('<BBIII4xI', width, flags, 1, 1, len(encoded), 1)
    path.write_bytes(page + trailer)


def snapshot(directory):
    return {str(p.relative_to(directory)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in directory.rglob('*') if p.is_file()}


def main():
    args = argparse.ArgumentParser()
    args.add_argument('--cli', type=Path, required=True)
    args.add_argument('--gdb', type=Path, required=True)
    options = args.parse_args()
    checks = 0

    def run(*arguments, code=0):
        nonlocal checks
        result = subprocess.run([str(options.cli), *map(str, arguments)],
                                capture_output=True, text=True, timeout=30)
        checks += 1
        assert result.returncode == code, (arguments, result.returncode, result.stdout, result.stderr)
        if code:
            assert result.stderr.strip(), arguments
            assert 'Matched FIDs:' not in result.stdout, (arguments, result.stdout)
        return result.stdout

    def expect_fids(output, expected):
        count = re.search(r'Matched FIDs: (\d+)', output)
        assert count and int(count[1]) == len(expected), (output, expected)
        fids = [int(n) for n in re.findall(r'^\s*FID(?:=| )(\d+)(?:$| \(blob_len)', output, re.M)]
        assert fids == expected, (fids, expected, output)

    before = snapshot(options.gdb)
    with tempfile.TemporaryDirectory(prefix='fast_gdb_中文_cli_') as folder:
        folder = Path(folder)
        floating = folder / '浮点.atx'
        double_precision = folder / '双精度.atx'
        integers = folder / '整数.atx'
        uuids = folder / '标识.atx'
        floats = [-1.5, 0.125, 1.5, 100.25]
        doubles = [-1.5, 0.10000000000000002, 0.125, 1.5, 100.25]
        exact = [-(2**63), -9007199254740993, -9007199254740992,
                 9007199254740992, 9007199254740993, 2**63 - 1]
        guid_values = ['00000000-0000-0000-0000-000000000000',
                       '12345678-9abc-def0-1234-567890abcdef',
                       '12345678-9abc-def0-1234-567890abcdef',
                       'ffffffff-ffff-ffff-ffff-ffffffffffff']
        write_atx(floating, [struct.pack('<f', value) for value in floats], 0x60)
        write_atx(integers, [struct.pack('<q', value) for value in exact], 0x40)
        write_atx(double_precision, [struct.pack('<d', value) for value in doubles], 0x60)
        write_atx(uuids, [('{' + value.upper() + '}').encode('ascii') for value in guid_values], 0)
        raw_before = snapshot(folder)
        operations = {'=': lambda a, b: a == b, '!=': lambda a, b: a != b,
                      '<': lambda a, b: a < b, '<=': lambda a, b: a <= b,
                      '>': lambda a, b: a > b, '>=': lambda a, b: a >= b}
        for operation, matches in operations.items():
            for path, data, target, kind in [(floating, floats, 1.5, 'float32'),
                                               (integers, exact, 9007199254740993, 'int64'),
                                               (double_precision, doubles, 0.10000000000000002, 'float64'),
                                               (uuids, guid_values, guid_values[1], 'uuid')]:
                result = run('query-atx', path, operation, target, kind)
                expect_fids(result, [i for i, value in enumerate(data) if matches(value, target)])
        for i, value in enumerate(exact):
            expect_fids(run('query-atx', integers, '=', value, 'int64'), [i])
        dump = run('dump-atx', integers, 'int64')
        for value in exact:
            assert f'value={value}\n' in dump, dump
        assert 'value=1.5\n' in run('dump-atx', floating, 'float32')
        assert 'value=0.10000000000000002\n' in run('dump-atx', double_precision, 'float64')
        expect_fids(run('query-atx', uuids, '=', '{' + guid_values[1].upper() + '}', 'uuid'), [1, 2])
        for command in [('query-atx', floating, '=', '1.5'), ('dump-atx', floating),
                        ('query-atx', floating, 'invalid', '1.5', 'float32'),
                        ('query-atx', floating, '=', '1.5junk', 'float32'),
                        ('query-atx', floating, '=', 'nan', 'float32'),
                        ('query-atx', floating, '=', 'inf', 'float32'),
                        ('query-atx', floating, '=', '1e999', 'float32'),
                        ('query-atx', integers, '=', str(2**63), 'int64'),
                        ('query-atx', integers, '=', '1.5', 'int64'),
                        ('query-atx', integers, '=', '+-1', 'int64'),
                        ('query-atx', integers, '=', '+', 'int64'),
                        ('query-atx', uuids, '=', 'bad-uuid', 'uuid'),
                        ('dump-atx', floating, 'unknown')]:
            run(*command, code=2)
        assert raw_before == snapshot(folder), 'CLI modified its ATX inputs'

    # This fixed, manifested ArcGIS fixture supplies real field/index metadata.
    uuid_index = options.gdb / 'a0000000f.FDO_GlobalID.atx'
    dump = run('dump-atx', uuid_index)
    assert 'type=UUID_2' in dump, dump
    stored = [(int(fid), text) for fid, text in re.findall(r'FID=(\d+)\s+value="([^"]+)"', dump)]
    assert stored and len(stored) == 5, dump
    target = stored[0][1]
    expected = sorted(fid - 1 for fid, text in stored if text == target)
    expect_fids(run('query-atx', uuid_index, '=', target.lower().strip('{}')), expected)
    expect_fids(run('query', options.gdb, '15', 'attr', 'FDO_GlobalID', '=', target), expected)
    run('query', options.gdb, '15', 'attr', 'FDO_Global', '=', target, code=2)
    run('dump-atx', uuid_index, 'int32', code=2)
    run('query', options.gdb, '15junk', 'attr', 'FDO_GlobalID', '=', target, code=2)
    overview = run('explore', options.gdb)
    assert '=== .atx attribute index files ===' in overview
    assert 'FDO_GlobalID.atx' in overview and 'type=UUID_2' in overview
    assert before == snapshot(options.gdb), 'CLI modified the GDB inputs'

    historical = Path(os.environ.get('FAST_GDB_HISTORICAL_DATASET', ''))
    float_index = historical / 'a0000000a.idx_float.atx'
    historical_checked = float_index.is_file()
    if historical_checked:
        expect_fids(run('query-atx', float_index, '=', '1.5'), list(range(5)))
        expect_fids(run('query', historical, '10', 'attr', 'idx_float', '=', '1.5'), list(range(5)))
        dump = run('dump-atx', float_index)
        assert 'type=FLOAT32' in dump and 'value=1.5\n' in dump, dump
    print(json.dumps({'status': 'PASS', 'cli_processes': checks,
                      'historical_float32_checked': historical_checked,
                      'gdb_input_files_unchanged': len(before)}, ensure_ascii=False))


if __name__ == '__main__':
    main()
