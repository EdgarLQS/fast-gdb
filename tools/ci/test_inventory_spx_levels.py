#!/usr/bin/env python3
"""Boundary tests for the standalone read-only inventory."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('inventory', Path(__file__).with_name('inventory_spx_levels.py'))
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)


def fixture(version=1):
    page, tail, oid = (4096, 22, 4) if version == 1 else (65536, 30, 8)
    header = 12 if version == 1 else 20
    values = header + ((page - header) // (oid + 8)) * oid
    data = bytearray(page * 3 + tail)

    def put(offset, value, size=oid):
        data[offset:offset + size] = value.to_bytes(size, 'little')

    put(oid, 1, 4)
    put(oid + 4, 2)
    put(oid + 4 + oid, 3)
    put(page, 3)
    for leaf, keys in ((1, [(2 << 62) | 2]), (2, [2, (1 << 62) | 2])):
        put(leaf * page + oid, len(keys), 4)
        for i, key in enumerate(keys):
            put(leaf * page + header + i * oid, leaf + i)
            put(leaf * page + values + i * 8, key, 8)
    data[page * 3] = 8
    put(page * 3 + 2, 1, 4)
    put(page * 3 + 6, 2, 4)
    put(page * 3 + 10, 3)
    put(len(data) - 4, version, 4)
    return data, page, oid, header, values


class InventoryTest(unittest.TestCase):
    def inspect(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'sample.spx'
            path.write_bytes(data)
            result = inventory.inspect(path)
            self.assertEqual(path.read_bytes(), data)
            return result

    def test_v1_v2_signed_levels_and_leaf_totals(self):
        for version in (1, 2):
            result = self.inspect(fixture(version)[0])
            self.assertEqual(result['status'], 'read')
            self.assertEqual(result['levels'], {'0': 1, '1': 1, '2': 1})
            self.assertEqual(result['entries'], 3)
            self.assertEqual(result['leaf_pages'], 2)
            self.assertTrue(result['signed_leaf_order'])

    def test_cycles_bad_counts_ids_totals_and_truncation(self):
        for version in (1, 2):
            original, page, oid, header, _ = fixture(version)
            for offset, value, width in ((page, 2, oid), (oid + 4, 99, oid),
                                         (page + oid, 0xffffffff, 4),
                                         (page + header, 0, oid),
                                         (page * 3 + 10, 4, oid)):
                data = original.copy()
                data[offset:offset + width] = value.to_bytes(width, 'little')
                self.assertEqual(self.inspect(data)['status'], 'error')
            self.assertEqual(self.inspect(original[:-1])['status'], 'error')

    def test_order_is_reported_separately_from_inventory_success(self):
        data, page, _, _, values = fixture()
        struct.pack_into('<Q', data, page * 2 + values + 8, 1)
        result = self.inspect(data)
        self.assertEqual(result['status'], 'read')
        self.assertFalse(result['signed_leaf_order'])


if __name__ == '__main__':
    unittest.main()
