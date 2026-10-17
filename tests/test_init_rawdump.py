# SPDX-License-Identifier: MIT
"""init/rawdump, run end to end against the simulated unit."""
import hashlib
import json
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_init_install import Bench  # noqa: E402
from initkit import config, rawdump  # noqa: E402
from initkit.config import partitions  # noqa: E402
from initkit.console import StepFailed  # noqa: E402
from initkit.uart import MUTATING  # noqa: E402

KV, LV = partitions.NANDS


class RawDump(Bench):
    def dump(self, output=None):
        with mock.patch.object(rawdump, 'open_session', self.open_session):
            return rawdump.run(self.settings, output)

    def check_image(self, path, part, chip_bytes):
        expected = self.unit.raw_image(chip_bytes)
        self.assertEqual(path.read_bytes(), expected)
        meta = json.loads(path.with_suffix('.json').read_text())
        self.assertEqual(meta['sha256'], hashlib.sha256(expected).hexdigest())
        self.assertEqual(meta['bytes'], chip_bytes + chip_bytes // part.page * part.oob)
        self.assertEqual((meta['part'], meta['id']), (part.name, part.id.hex(' ')))
        self.assertEqual((meta['page_bytes'], meta['oob_bytes'], meta['block_bytes']),
                         (part.page, part.oob, part.block))
        self.assertIn('main area', meta['layout'])
        return meta

    def test_an_untested_part_is_dumped_raw_and_physically(self):
        unit = self.make_unit(part=LV, chip_bytes=0x800000)
        path = self.dump()
        self.assertEqual(path, config.unit_dir(self.settings.data_dir, 'W1700KTEST0001') /
                         rawdump.RAW_NAME)
        meta = self.check_image(path, LV, unit.chip_bytes)
        self.assertEqual(meta['serial'], 'W1700KTEST0001')
        self.assertTrue(meta['vendor_tables_valid'])
        self.assertEqual(meta['vendor_pool'], {'stock_u_boot': 163, 'ours': 163})
        self.assertEqual(path.with_suffix('.json').stat().st_mode & 0o777, 0o600)
        commands = self.commands('dumper')
        self.assertLess(commands.index('bmtdetach'),
                        next(i for i, line in enumerate(commands) if 'read.raw.oob' in line))
        self.assertEqual(path.stat().st_mode & 0o777, 0o400)

    def test_nothing_is_written_and_the_unit_boots_as_before(self):
        unit = self.make_unit(installed=True)
        self.dump()
        self.assertFalse(any(MUTATING.search(line) for line in self.commands()))
        self.assertFalse(any(line.startswith(('flash', 'ubi ', 'saveenv'))
                             for line in self.commands()))
        self.assertEqual(self.unit.log[-1], ('dumper', 'reset'))
        self.assertEqual(unit.state, 'rebooting')

    def test_a_chip_whose_tables_do_not_validate_is_still_dumped(self):
        unit = self.make_unit(translation='none')
        path = self.dump()
        meta = self.check_image(path, KV, unit.chip_bytes)
        self.assertFalse(meta['vendor_tables_valid'])
        self.assertNotIn('bmtdetach', self.commands())

    def test_a_dump_is_never_overwritten(self):
        self.make_unit()
        target = self.work / 'raw.bin'
        target.write_bytes(b'kept')
        with self.assertRaisesRegex(StepFailed, 'never overwritten'):
            self.dump(target)
        self.assertEqual(target.read_bytes(), b'kept')

    def test_an_output_file_can_be_named(self):
        unit = self.make_unit()
        target = self.work / 'out' / 'unit.bin'
        self.assertEqual(self.dump(target), target)
        self.check_image(target, KV, unit.chip_bytes)
        self.assertTrue((self.work / 'out' / 'unit.json').is_file())


if __name__ == '__main__':
    unittest.main()
