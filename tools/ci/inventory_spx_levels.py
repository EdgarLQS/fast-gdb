#!/usr/bin/env python3
"""Read-only SPX leaf inventory; this does not prove source geometry coverage."""
import argparse
from collections import Counter
import json
from pathlib import Path
import os
import struct


def identity(stat):
    return (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns)


def inspect(path):
    result = {'file': path.name}
    try:
        with path.open('rb') as stream:
            before = identity(os.fstat(stream.fileno()))
            size = before[2]
            if size < 4:
                raise ValueError('truncated version')
            stream.seek(-4, os.SEEK_END)
            version = struct.unpack('<I', stream.read(4))[0]
            if version not in (1, 2):
                raise ValueError('unsupported SPX version')
            page_size, trailer_size, oid = (4096, 22, 4) if version == 1 else (65536, 30, 8)
            if size < trailer_size or (size - trailer_size) % page_size:
                raise ValueError('invalid page/trailer size')
            pages = (size - trailer_size) // page_size
            stream.seek(-trailer_size, os.SEEK_END)
            trailer = stream.read(trailer_size)
            width, magic, depth = trailer[0], struct.unpack_from('<I', trailer, 2)[0], struct.unpack_from('<I', trailer, 6)[0]
            declared = int.from_bytes(trailer[10:10 + oid], 'little')
            if width != 8 or magic != 1 or not 1 <= depth <= 4 or pages < depth:
                raise ValueError('unsupported trailer layout')
            leaf_header = 12 if version == 1 else 20
            capacity = (page_size - leaf_header) // (oid + 8)
            value_offset = leaf_header + capacity * oid
            visited = set()

            def read_page(page_id):
                if not 1 <= page_id <= pages or page_id in visited:
                    raise ValueError('invalid or cyclic page reference')
                visited.add(page_id)
                stream.seek((page_id - 1) * page_size)
                page = stream.read(page_size)
                if len(page) != page_size:
                    raise ValueError('short page read')
                count = struct.unpack_from('<I', page, oid)[0]
                if count > capacity:
                    raise ValueError('page count exceeds capacity')
                return page, count

            page_id = 1
            for _ in range(depth - 1):
                page, count = read_page(page_id)
                if not count:
                    raise ValueError('empty internal page')
                branch_header = 8 if version == 1 else 12
                page_id = int.from_bytes(page[branch_header:branch_header + oid], 'little')
            levels = Counter()
            total = leaves = 0
            previous = None
            ordered = True
            while page_id:
                page, count = read_page(page_id)
                leaves += 1
                for i in range(count):
                    key = struct.unpack_from('<Q', page, value_offset + i * 8)[0]
                    fid = int.from_bytes(page[leaf_header + i * oid:leaf_header + (i + 1) * oid], 'little')
                    if not fid:
                        raise ValueError('zero ObjectID')
                    order = key ^ (1 << 63)
                    if previous is not None and previous > order:
                        ordered = False
                    previous = order
                    levels[str(key >> 62)] += 1
                total += count
                page_id = int.from_bytes(page[:oid], 'little')
            if total != declared:
                raise ValueError('leaf-chain total differs from trailer')
            if identity(os.fstat(stream.fileno())) != before or identity(path.stat()) != before:
                raise ValueError('source identity changed during inventory')
            result.update(status='read', version=version, depth=depth, pages=pages,
                          leaf_pages=leaves, entries=total, levels=dict(sorted(levels.items())),
                          signed_leaf_order=ordered)
    except (OSError, ValueError, struct.error) as error:
        result.update(status='error', error=str(error))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gdb', type=Path, action='append', required=True)
    options = parser.parse_args()
    datasets = []
    failed = False
    for directory in options.gdb:
        if not directory.is_dir():
            datasets.append({'dataset': str(directory), 'error': 'directory missing'})
            failed = True
            continue
        files = [inspect(path) for path in sorted(directory.glob('*.spx'))]
        failed |= any(item['status'] == 'error' for item in files)
        datasets.append({'dataset': str(directory), 'spx_files': len(files), 'files': files})
    print(json.dumps({'scope': 'leaf inventory only; not source coverage verification',
                      'datasets': datasets}, indent=2))
    return 2 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
