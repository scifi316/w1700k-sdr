# SPDX-License-Identifier: MIT
"""The vendor BMT layer that the loader and Linux compile, tested on the host.

platform/bmt/airoha_bmt.c translates vendor-logical NAND addresses exactly as
stock U-Boot does; airoha_bmt_write.c and airoha_bmt_transaction.c replace worn
blocks and publish new tables. These tests compile the shipped C files for the
host and drive them:

* translation, through ctypes, against an independent Python model;
* the table-writing and transaction code, with the C harnesses in
  platform/bmt/tests (needs OpenSSL's libcrypto headers);
* the U-Boot and Linux bindings, together with the real UBI and SPI-NAND
  functions extracted from the prepared U-Boot and Linux trees (needs `make`
  to have prepared them).
"""
import ctypes
from pathlib import Path
import resource
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BMT = ROOT / 'platform/bmt'
LINUX_SRC = ROOT / 'platform/build/src/linux'
UBOOT_SRC = ROOT / 'platform/build/src/u-boot-loader'
sys.path[:0] = [str(Path(__file__).resolve().parent), str(ROOT / 'platform')]
import partitions                       # noqa: E402
import platform_bmt_model as model      # noqa: E402

CC = shutil.which('cc') or shutil.which('gcc')
TOTAL = model.TOTAL_BLOCKS
SYSTEM = model.system_blocks_for(TOTAL)
POOL = model.clamped_pool(TOTAL)
# Factory bad blocks straddling the ownership boundaries, and two worn blocks
# substituted into the reserve pool.
FACTORY = [5, 11, 20, 40, 100]
REMAP = [(7, SYSTEM + 3), (41, SYSTEM + 4)]
ERRORS = {'SHORT': -1, 'SIGNATURE': -2, 'VERSION': -3, 'SIZE': -4, 'CHECKSUM': -5,
          'UNPINNABLE': -6, 'RANGE': -7, 'ORDER': -8, 'NOTLOADED': -9, 'POOL': -10,
          'MARKER': -11}
# The W25N04LV: 2048 blocks, a pool the scan decides. Two factory-bad blocks in
# the scanned span make it 165 (platform/bmt/tests/geometry.h uses the same).
LV_TOTAL = 2048
LV_TOP_BAD = {LV_TOTAL - 41, LV_TOTAL - 42}
# Every C harness runs once per part.
GEOMETRIES = {'W25N04KV': [], 'W25N04LV': ['-DTEST_W25N04LV']}
# The table-level harnesses also run the W25N04LV at the clamp (pool 250).
TABLE_GEOMETRIES = {**GEOMETRIES, 'W25N04LV-pool-250': ['-DTEST_W25N04LV', '-DT_POOL=250u']}
FactoryBad = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32)


def has_header(header):
    if CC is None:
        return False
    probe = subprocess.run([CC, '-x', 'c', '-fsyntax-only', '-'],
                           input=f'#include <{header}>\n', text=True, capture_output=True)
    return probe.returncode == 0


LIBCRYPTO = has_header('openssl/sha.h')


class Table(ctypes.Structure):
    _fields_ = [('total_blocks', ctypes.c_uint16), ('system_blocks', ctypes.c_uint16),
                ('page_bytes', ctypes.c_uint16), ('oob_bytes', ctypes.c_uint16),
                ('geometry_set', ctypes.c_uint8), ('bbt_loaded', ctypes.c_uint8),
                ('bmt_loaded', ctypes.c_uint8), ('bbt_ascending', ctypes.c_uint8),
                ('bbt_size', ctypes.c_uint16), ('bmt_size', ctypes.c_uint16),
                ('bbt_entries', ctypes.c_uint16 * model.MAX_ENTRIES),
                ('bmt_from', ctypes.c_uint16 * model.MAX_ENTRIES),
                ('bmt_to', ctypes.c_uint16 * model.MAX_ENTRIES)]


@unittest.skipIf(CC is None, 'needs a host C compiler')
class Translation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory()
        library = Path(cls.work.name) / 'libairoha_bmt.so'
        subprocess.run([CC, '-DAIROHA_BMT_HOST', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-std=c11', '-fPIC', '-shared', BMT / 'airoha_bmt.c',
                        BMT / 'host_shim.c', '-o', library], check=True)
        lib = ctypes.CDLL(str(library))
        lib.airoha_bmt_sizeof.restype = ctypes.c_size_t
        for name in ('airoha_bmt_pool_floor', 'airoha_bmt_pool_ceiling',
                     'airoha_bmt_clamped_pool'):
            getattr(lib, name).restype = ctypes.c_uint32
            getattr(lib, name).argtypes = [ctypes.c_uint32]
        lib.airoha_bmt_set_geometry.argtypes = [ctypes.POINTER(Table), ctypes.c_uint32,
                                                ctypes.c_uint32]
        lib.airoha_bmt_set_pages.argtypes = [ctypes.POINTER(Table), ctypes.c_uint32,
                                             ctypes.c_uint32]
        lib.airoha_bmt_vendor_pool.argtypes = [ctypes.c_uint32, FactoryBad, ctypes.c_void_p]
        for name in ('airoha_bmt_parse_bbt', 'airoha_bmt_parse_bmt'):
            getattr(lib, name).argtypes = [ctypes.POINTER(Table), ctypes.c_char_p,
                                           ctypes.c_uint32]
        for name in ('airoha_bmt_translate', 'airoha_bmt_shift', 'airoha_bmt_untranslate'):
            getattr(lib, name).argtypes = [ctypes.POINTER(Table), ctypes.c_uint32]
        lib.airoha_bmt_usable_blocks.argtypes = [ctypes.POINTER(Table)]
        cls.lib = lib

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def loaded(self, bbt=None, bmt=None):
        table = Table()
        bbt = model.bbt(FACTORY) if bbt is None else bbt
        bmt = model.bmt(REMAP) if bmt is None else bmt
        self.assertEqual(self.lib.airoha_bmt_set_geometry(ctypes.byref(table), TOTAL, POOL), 0)
        self.assertEqual(self.lib.airoha_bmt_parse_bbt(ctypes.byref(table), bbt, len(bbt)), 0)
        self.assertEqual(self.lib.airoha_bmt_parse_bmt(ctypes.byref(table), bmt, len(bmt)), 0)
        return table

    def test_struct_layout_matches_the_c_definition(self):
        self.assertEqual(self.lib.airoha_bmt_sizeof(), ctypes.sizeof(Table))

    def test_pool_is_clamped_to_the_table_capacity(self):
        self.assertEqual(self.lib.airoha_bmt_pool_floor(TOTAL), TOTAL * 8 // 100)
        self.assertEqual(self.lib.airoha_bmt_pool_ceiling(TOTAL), TOTAL // 8)
        self.assertEqual(self.lib.airoha_bmt_clamped_pool(TOTAL), model.MAX_ENTRIES)
        self.assertIsNone(model.clamped_pool(LV_TOTAL))
        self.assertEqual(self.lib.airoha_bmt_clamped_pool(LV_TOTAL), 0)

    def scan(self, total, bad, unreadable=(), flaky=()):
        reads = []

        def factory_bad(_ctx, block):
            reads.append(block)
            if block in flaky:              # set on the first read only
                return int(reads.count(block) == 1)
            return -5 if block in unreadable else int(block in bad)
        callback = FactoryBad(factory_bad)
        return self.lib.airoha_bmt_vendor_pool(total, callback, None), reads

    def test_the_pool_is_the_vendor_scan(self):
        # The W25N04KV: the clamp fixes 250 and nothing is read.
        self.assertEqual(self.scan(TOTAL, set(range(3800, 4096))), (250, []))
        # The W25N04LV: 163, plus every factory-bad block in the span, up to the
        # clamp; past the ceiling (256) there is no BMT.
        top = LV_TOTAL - 1
        cases = [set(), LV_TOP_BAD, {5, 100, 1700}, set(range(top - 86, top + 1)),
                 set(range(top - 87, top + 1)), set(range(top - 92, top + 1)),
                 set(range(top - 93, top + 1)), {top, top - 162}]
        for bad in cases:
            with self.subTest(bad=len(bad)):
                expected = model.vendor_pool(LV_TOTAL, bad)
                pool, reads = self.scan(LV_TOTAL, bad)
                self.assertEqual(pool, ERRORS['POOL'] if expected is None else expected)
                self.assertEqual(reads[0], top)
                self.assertEqual(reads, sorted(reads, reverse=True))
        self.assertEqual(self.scan(LV_TOTAL, set())[0], 163)
        self.assertEqual(self.scan(LV_TOTAL, LV_TOP_BAD)[0], 165)
        self.assertEqual(self.scan(LV_TOTAL, set(range(top - 87, top + 1)))[0], 250)
        self.assertEqual(self.scan(LV_TOTAL, set(range(top - 93, top + 1)))[0], ERRORS['POOL'])
        # An unreadable marker inside the span is refused, never guessed; a set
        # marker is read twice, and two reads that disagree are refused too.
        self.assertEqual(self.scan(LV_TOTAL, set(), unreadable={top - 3})[0], ERRORS['MARKER'])
        self.assertEqual(self.scan(LV_TOTAL, set(), flaky={top - 3})[0], ERRORS['MARKER'])
        reads = self.scan(LV_TOTAL, LV_TOP_BAD)[1]
        self.assertEqual([reads.count(block) for block in sorted(LV_TOP_BAD)], [2, 2])
        self.assertEqual(self.scan(LV_TOTAL, set(), unreadable={top - 300})[0], 163)

    def test_page_geometry_is_one_of_the_two_parts(self):
        lib = self.lib
        table = Table()
        self.assertEqual(lib.airoha_bmt_set_pages(ctypes.byref(table), 2048, 56),
                         ERRORS['RANGE'])           # before the geometry
        lib.airoha_bmt_set_geometry(ctypes.byref(table), LV_TOTAL, 165)
        for page, oob, expected in [(2048, 56, 0), (4096, 112, 0), (1024, 28, 'RANGE'),
                                    (8192, 112, 'RANGE'), (4096, 113, 'RANGE'),
                                    (4096, 3, 'RANGE')]:
            with self.subTest(page=page, oob=oob):
                self.assertEqual(lib.airoha_bmt_set_pages(ctypes.byref(table), page, oob),
                                 ERRORS.get(expected, 0))

    def test_only_a_pool_the_vendor_rule_can_return_is_accepted(self):
        for total, pool, expected in [(TOTAL, 250, 0), (TOTAL, 249, 'UNPINNABLE'),
                                      (TOTAL, 251, 'UNPINNABLE'), (LV_TOTAL, 163, 0),
                                      (LV_TOTAL, 162, 'UNPINNABLE'), (LV_TOTAL, 250, 0),
                                      (LV_TOTAL, 251, 'UNPINNABLE'), (100, 8, 0),
                                      (64, 4, 'UNPINNABLE')]:
            with self.subTest(total=total, pool=pool):
                table = Table()
                status = self.lib.airoha_bmt_set_geometry(ctypes.byref(table), total, pool)
                self.assertEqual(status, ERRORS.get(expected, 0))
                if not status:
                    self.assertEqual(table.system_blocks, total - pool)

    def test_a_w25n04lv_translates_like_the_model(self):
        pool = model.vendor_pool(LV_TOTAL, LV_TOP_BAD)
        system = LV_TOTAL - pool
        remap = [(7, system + 3), (41, system + 4), (1700, LV_TOTAL - 2)]
        table = Table()
        self.assertEqual(self.lib.airoha_bmt_set_geometry(ctypes.byref(table), LV_TOTAL,
                                                          pool), 0)
        first, second = model.bbt(FACTORY), model.bmt(remap, pool_blocks=pool)
        self.assertEqual(self.lib.airoha_bmt_parse_bbt(ctypes.byref(table), first,
                                                       len(first)), 0)
        self.assertEqual(self.lib.airoha_bmt_parse_bmt(ctypes.byref(table), second,
                                                       len(second)), 0)
        bbt, bmt = model.parse_bbt(first), model.parse_bmt(second, pool)
        shifts = model.shift_map(bbt['entries'], system)
        for logical in range(LV_TOTAL):
            self.assertEqual(self.lib.airoha_bmt_translate(ctypes.byref(table), logical),
                             model.translate(logical, bbt, bmt, system, shifts), logical)
        # The checksum covers the pool's 165 entries: a 250-entry sum is refused.
        wrong = model.bmt(remap, pool_blocks=model.MAX_ENTRIES)
        status = self.lib.airoha_bmt_parse_bmt(ctypes.byref(table), wrong, len(wrong))
        self.assertEqual(status, ERRORS['CHECKSUM'])

    def test_every_block_translates_like_the_model(self):
        table = self.loaded()
        bbt, bmt = model.parse_bbt(model.bbt(FACTORY)), model.parse_bmt(model.bmt(REMAP))
        usable = model.usable_blocks(bbt, SYSTEM)
        self.assertEqual(self.lib.airoha_bmt_usable_blocks(ctypes.byref(table)), usable)
        shifts = model.shift_map(bbt['entries'], SYSTEM)
        for logical in range(usable + 8):
            self.assertEqual(self.lib.airoha_bmt_translate(ctypes.byref(table), logical),
                             model.translate(logical, bbt, bmt, SYSTEM, shifts), logical)

    def test_a_worn_block_is_substituted_into_the_pool(self):
        # Logical 6 shifts past bad block 5 to physical 7, which REMAP replaces.
        self.assertEqual(self.lib.airoha_bmt_translate(ctypes.byref(self.loaded()), 6),
                         SYSTEM + 3)

    def test_partition_boundaries_round_trip(self):
        table = self.loaded()
        bbt = model.parse_bbt(model.bbt(FACTORY))
        usable = self.lib.airoha_bmt_usable_blocks(ctypes.byref(table))
        shifted = 0
        for name, (offset, _) in partitions.GEOMETRY.items():
            logical = offset // model.BLOCK_BYTES
            if logical >= usable:
                continue
            final = self.lib.airoha_bmt_translate(ctypes.byref(table), logical)
            self.assertGreaterEqual(final, 0, name)
            self.assertEqual(self.lib.airoha_bmt_untranslate(ctypes.byref(table), final),
                             logical, name)
            shift = self.lib.airoha_bmt_shift(ctypes.byref(table), logical)
            self.assertEqual(shift, model.shift(logical, bbt, SYSTEM), name)
            shifted += shift > 0
        self.assertTrue(shifted, 'the fixture must shift some boundary')

    def test_invalid_tables_are_refused_never_repaired(self):
        table = Table()
        self.lib.airoha_bmt_set_geometry(ctypes.byref(table), TOTAL, POOL)
        broken = bytearray(model.bmt(REMAP))
        broken[model.BMT_HEADER + 4 * (POOL - 1)] ^= 1
        cases = {
            'SIGNATURE': [(b'RAWX' + model.bbt(FACTORY)[4:], 'bbt'),
                          (b'BMX' + model.bmt(REMAP)[3:], 'bmt')],
            'CHECKSUM': [(model.bbt(FACTORY, checksum=0x1234), 'bbt'),
                         (model.bmt(REMAP, checksum=0x5a), 'bmt'),
                         # a byte beyond the claimed entries is still covered
                         (bytes(broken), 'bmt')],
            'SIZE': [(model.bbt(FACTORY, size=251), 'bbt'),
                     (model.bmt([(1, 2)], size=251), 'bmt')],
            'SHORT': [(model.bbt(FACTORY)[:-1], 'bbt'), (model.bmt(REMAP)[:-1], 'bmt')],
            'VERSION': [(model.BBT_MAGIC + bytes(model.BBT_BYTES - 4), 'bbt'),
                        (model.BMT_MAGIC + bytes(model.BMT_BYTES - 3), 'bmt')],
        }
        for error, blobs in cases.items():
            for blob, kind in blobs:
                parse = getattr(self.lib, f'airoha_bmt_parse_{kind}')
                self.assertEqual(parse(ctypes.byref(table), blob, len(blob)),
                                 ERRORS[error], f'{kind} {error}')

    def test_a_descending_bbt_refuses_translation(self):
        table = Table()
        self.lib.airoha_bmt_set_geometry(ctypes.byref(table), TOTAL, POOL)
        descending = model.bbt(list(reversed(FACTORY)))
        self.lib.airoha_bmt_parse_bbt(ctypes.byref(table), descending, len(descending))
        bmt = model.bmt(REMAP)
        self.lib.airoha_bmt_parse_bmt(ctypes.byref(table), bmt, len(bmt))
        self.assertEqual(table.bbt_ascending, 0)
        self.assertEqual(self.lib.airoha_bmt_translate(ctypes.byref(table), 6), ERRORS['ORDER'])

    def test_entries_outside_the_vendor_bounds_are_refused(self):
        for bbt, bmt, expected in [
                ([SYSTEM - 1], [], 0), ([SYSTEM], [], 'RANGE'), ([TOTAL], [], 'RANGE'),
                ([], [(64, SYSTEM)], 0), ([], [(64, TOTAL - 1)], 0),
                ([], [(64, 0)], 'RANGE'), ([], [(64, SYSTEM - 1)], 'RANGE'),
                ([], [(64, TOTAL)], 'RANGE'), ([], [(TOTAL, SYSTEM)], 'RANGE'),
                ([], [(TOTAL - 1, SYSTEM)], 0)]:
            table = Table()
            self.lib.airoha_bmt_set_geometry(ctypes.byref(table), TOTAL, POOL)
            first, second = model.bbt(bbt), model.bmt(bmt)
            status = self.lib.airoha_bmt_parse_bbt(ctypes.byref(table), first, len(first)) or \
                self.lib.airoha_bmt_parse_bmt(ctypes.byref(table), second, len(second))
            self.assertEqual(status, ERRORS.get(expected, 0), (bbt, bmt))
            if status:
                self.assertEqual(self.lib.airoha_bmt_translate(ctypes.byref(table), 64),
                                 ERRORS['NOTLOADED'])


def no_core_dump():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def compile_and_run(command, binary, timeout=60):
    """Compile a harness and run it; harnesses fail by assert(), so no core files."""
    subprocess.run([*map(str, command), '-o', str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True, timeout=timeout,
                          preexec_fn=no_core_dump)


@unittest.skipUnless(CC and LIBCRYPTO, 'needs a host C compiler and OpenSSL headers')
class TableWriting(unittest.TestCase):
    """Canonical table bytes, publication and journalled replacement, in a NAND model."""

    def test_harnesses_pass(self):
        with tempfile.TemporaryDirectory() as work:
            for part, flags in TABLE_GEOMETRIES.items():
                for name in ('write_contract', 'publication', 'remap'):
                    with self.subTest(name, part=part):
                        result = compile_and_run(
                            [CC, '-DAIROHA_BMT_HOST', *flags, '-std=c11', '-O1', '-g', '-Wall',
                             '-Wextra', '-Werror', '-fsanitize=undefined',
                             '-fno-sanitize-recover=all', f'-I{BMT}', BMT / f'tests/{name}.c',
                             BMT / 'airoha_bmt.c', BMT / 'airoha_bmt_write.c',
                             BMT / 'airoha_bmt_transaction.c', '-lcrypto'],
                            Path(work) / f'{name}-{part}', timeout=600)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


PREPARED = LINUX_SRC.is_dir() and UBOOT_SRC.is_dir()


@unittest.skipUnless(CC and LIBCRYPTO and PREPARED,
                     'needs a host C compiler, OpenSSL headers and the trees `make` prepares')
class Bindings(unittest.TestCase):
    """The U-Boot and Linux MTD bindings, driven through fault-injecting NAND callbacks.

    The kernel's real spinand_probe/remove and mtdchar read path, and both trees'
    UBI write/erase callers, are cut from the prepared sources and compiled in.
    """

    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory()
        out = Path(cls.work.name)
        (out / 'airoha_bmt_policy.h').write_text(partitions.policy_header())
        core = (LINUX_SRC / 'drivers/mtd/nand/spi/core.c').read_text()
        start = core.index('static int spinand_probe(')
        probe = core[start:core.index('static const struct spi_device_id', start)]
        (out / 'lifecycle.inc').write_text((BMT / 'tests/lifecycle.h').read_text() + probe +
                                           (BMT / 'tests/lifecycle_tail.h').read_text())
        mtdchar = (LINUX_SRC / 'drivers/mtd/mtdchar.c').read_text()
        start = mtdchar.index('static noinline_for_stack int\nmtdchar_read_ioctl(')
        memread = mtdchar[start:mtdchar.index('static int mtdchar_ioctl(', start)]
        (out / 'memread.inc').write_text((BMT / 'tests/memread.h').read_text() + memread +
                                         (BMT / 'tests/memread_tail.h').read_text())
        cls.out = out

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def command(self, layer, linux):
        command = [CC, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=undefined', '-fno-sanitize-recover=all',
                   f'-I{BMT / "tests/include"}', f'-I{BMT}', f'-I{self.out}',
                   f'-DLAYER_FILE="{layer}"']
        return command + (['-DTEST_LINUX'] if linux else [])

    def ubi_consumer(self, name, tree):
        io = (tree / 'drivers/mtd/ubi/io.c').read_text()
        snippets = '\n'.join(function(io, signature) for signature in (
            'int ubi_io_write(', 'static int do_sync_erase(', 'int ubi_io_mark_bad('))
        wl = (tree / 'drivers/mtd/ubi/wl.c').read_text()
        begin = wl.index('\n\t/* It is %-EIO, the PEB went bad */')
        tail = ('static int actual_bad_peb_tail(struct ubi_device *ubi,int pnum)\n'
                '{\n int err=-EIO,available_consumed=0;\n' + wl[begin:wl.index('\n}', begin) + 2])
        include = self.out / f'ubi-{name}.inc'
        include.write_text((BMT / 'tests/ubi_consumer.h').read_text() +
                           '\n#pragma GCC diagnostic push\n'
                           '#pragma GCC diagnostic ignored "-Wsign-compare"\n' + snippets +
                           '\n' + tail + '\n#pragma GCC diagnostic pop\n' +
                           (BMT / 'tests/ubi_consumer_tail.h').read_text())
        return include

    def check_binding(self, name, layer, tree, write_config):
        linux = name == 'linux'
        include = self.ubi_consumer(name, tree)
        for part, flags in GEOMETRIES.items():
            with self.subTest(part):
                result = compile_and_run(self.command(layer, linux) + flags +
                                         [BMT / 'tests/bindings.c', BMT / 'airoha_bmt.c'],
                                         self.out / f'{name}-{part}', timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = compile_and_run(
                    self.command(layer, linux) + flags +
                    [f'-D{write_config}=1', '-DAIROHA_BMT_HOST',
                     f'-DUBI_CONSUMER_FILE="{include}"', BMT / 'tests/managed_bindings.c',
                     BMT / 'airoha_bmt.c', BMT / 'airoha_bmt_write.c',
                     BMT / 'airoha_bmt_transaction.c', '-lcrypto', '-pthread'],
                    self.out / f'managed-{name}-{part}', timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_uboot_binding(self):
        self.check_binding('uboot', ROOT / 'platform/loader/airoha_bmt_layer.c', UBOOT_SRC,
                           'CONFIG_AIROHA_BMT_WRITE')

    def test_linux_binding(self):
        self.check_binding('linux', ROOT / 'platform/linux/sdr_bmt_layer.c', LINUX_SRC,
                           'CONFIG_AIROHA_SDR_BMT_WRITE')


@unittest.skipUnless(CC and PREPARED, 'needs a host C compiler and the trees `make` prepares')
class IoGuards(unittest.TestCase):
    """A NAND DMA error must reset the SoC, not release buffers the controller may
    still write; a NAND command whose completion is unknown must stop the caller.

    The SPI controller's DMA error tails and spinand_wait() are cut from each
    prepared tree and run against a model; removing the reset guard must fail.
    """

    GUARD = 'panic("AN7581 NAND DMA error %d; reset required", err);'

    @staticmethod
    def extract(spi, core, uboot):
        tails = spi.split('error_dma_unmap:\n')
        assert len(tails) == 3, 'expected exactly two DMA error tails'
        context = 'priv' if uboot else 'as_ctrl'
        functions = []
        for direction, tail in zip(('read', 'write'), tails[1:]):
            functions.append(f'static int dma_{direction}_error(int err, int before_map) {{\n'
                             f' void *{context}=0; if(before_map) goto error_dma_mode_off;\n'
                             ' goto error_dma_unmap;\nerror_dma_unmap:\n'
                             + tail[:tail.index('\n}')] + '\n}\n')
        start = core.index('int spinand_wait(')
        return '\n'.join(functions) + '\n' + core[start:core.index(
            '\nstatic int spinand_read_id_op', start)]

    def run_guard(self, name, code, work):
        source = Path(work) / f'{name}.c'
        source.write_text((BMT / 'tests/io_guards.h').read_text() + code +
                          (BMT / 'tests/io_guards_tail.h').read_text())
        command = [CC, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=undefined', '-fno-sanitize-recover=all', source]
        if name.startswith('uboot'):
            command.insert(1, '-DTEST_UBOOT')
        return compile_and_run(command, Path(work) / name, timeout=10)

    def test_guards_in_both_trees(self):
        trees = {'linux': (LINUX_SRC / 'drivers/spi/spi-airoha-snfi.c', LINUX_SRC),
                 'uboot': (UBOOT_SRC / 'drivers/spi/airoha_snfi_spi.c', UBOOT_SRC)}
        with tempfile.TemporaryDirectory() as work:
            for name, (spi, tree) in trees.items():
                with self.subTest(name):
                    code = self.extract(spi.read_text(), (tree / 'drivers/mtd/nand/spi/core.c')
                                        .read_text(), name == 'uboot')
                    result = self.run_guard(name, code, work)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn(self.GUARD, code)
                    unguarded = self.run_guard(f'{name}-unguarded',
                                               code.replace(self.GUARD, 'return err;', 1), work)
                    self.assertNotEqual(unguarded.returncode, 0,
                                        'the model passes without the reset guard')

    def test_a_panic_cannot_enter_another_kernel(self):
        config = ROOT / 'platform/build/linux/.config'
        if not config.is_file():
            self.skipTest('the kernel is not configured')
        lines = config.read_text().splitlines()
        for symbol in ('CONFIG_KEXEC', 'CONFIG_KEXEC_CORE', 'CONFIG_KEXEC_FILE',
                       'CONFIG_CRASH_DUMP'):
            self.assertNotIn(f'{symbol}=y', lines)


if __name__ == '__main__':
    unittest.main()
