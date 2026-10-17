#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the write-free NAND dumper that backs up a unit before installation.

    platform/dumper/build.py      -> platform/build/images/w1700k-dumper.itb

Stock U-Boot receives this FIT over TFTP and boots it from RAM. It reads the
NAND's data as stock firmware addresses it, through the vendor BBT/BMT
translation and the chip's ECC, into RAM, hashes it and sends it to the host
with `tftpput`. That backup is the way back to the stock operating state, and it
is taken before anything is written, so the dumper cannot write: there is no
UBI, no saved environment, no autoboot, the BMT write engine is not linked, and
the NAND write, erase and mark-bad paths are not compiled in at all. The build
checks the linked image for each of them. For a raw image (init/rawdump),
`bmtdetach` drops the translation and `mtd read.raw.oob` reads every page
physically, ECC off. A chip whose vendor tables do not validate is shown
untranslated, so it can still be read and examined; the installer backs up only
a translated chip.
"""
import gzip
from pathlib import Path
import re
import sys

UNIT = Path(__file__).resolve().parent
LOADER = UNIT.parent / 'loader'
BMT = UNIT.parent / 'bmt'
sys.path.insert(0, str(UNIT.parent))
from buildlib import BUILD, IMAGES, LOGS, kconfig, main_guard, require, write_if_changed
import partitions
import uboot

FIT = IMAGES / 'w1700k-dumper.itb'
DEFCONFIG = 'an7581_w1700k_dumper_defconfig'
FILES = {
    f'configs/{DEFCONFIG}': UNIT / DEFCONFIG,
    'defenvs/an7581_w1700k_dumper_env': UNIT / 'an7581_w1700k_dumper_env',
    'dts/upstream/src/arm64/airoha/an7581-w1700k-dumper.dts': UNIT / 'an7581-w1700k-dumper.dts',
    'arch/arm/dts/an7581-w1700k-dumper-u-boot.dtsi': UNIT / 'an7581-w1700k-dumper-u-boot.dtsi',
    # The loader's BMT binding and the shared translation core, read side only.
    'drivers/mtd/nand/airoha_bmt_layer.c': LOADER / 'airoha_bmt_layer.c',
    'drivers/mtd/nand/airoha_bmt_layer.h': LOADER / 'airoha_bmt_layer.h',
    'drivers/mtd/nand/airoha_bmt.c': BMT / 'airoha_bmt.c',
    'drivers/mtd/nand/airoha_bmt.h': BMT / 'airoha_bmt.h',
}
# The loader's patch that attaches the translation in the spinand probe; the
# dumper's own patches from 0007 on build on it.
BMT_PATCH = LOADER / 'patches/0002-mtd-nand-airoha-bmt-translation-layer.patch'
# The data stock firmware addresses, vendor-logical 0 up to `reserved-bmt`
# (446 MiB), is read into one buffer.
DUMP_BUFFER = 0x90000000
DUMP_BYTES = partitions.GEOMETRY['reserved-bmt'][0]
ENABLED = ('AN7581_W1700K_SDR', 'POSITION_INDEPENDENT', 'LINUX_KERNEL_IMAGE_HEADER',
           'OF_SEPARATE', 'ENV_IS_NOWHERE', 'CMD_MTD', 'CMD_HASH', 'SHA256',
           'CMD_TFTPBOOT', 'CMD_TFTPPUT', 'MTD_SPI_NAND', 'MTD_SPI_NAND_READ_ONLY',
           'AIROHA_SNFI_SPI', 'AIROHA_ETH', 'AIROHA_BMT')
DISABLED = ('MTD_UBI', 'CMD_UBI', 'AUTOBOOT', 'CMD_SAVEENV', 'CMD_EDITENV',
            'AIROHA_BMT_WRITE', 'CMD_MTD_MARKBAD', 'CMD_MTD_NAND_WRITE_TEST',
            'CMD_MTD_OTP', 'BOOTCOUNT_LIMIT', 'USE_PREBOOT', 'ENV_IS_IN_MTD', 'ENV_IS_IN_UBI')
# Every NAND mutation implementation, and environment saving. Each must be
# absent from the linked image, not merely unreachable.
ABSENT = ('mtd_erase', 'mtd_write', 'mtd_write_oob', 'mtd_panic_write', 'mtd_writev',
          'mtd_block_markbad', 'spinand_mtd_write', 'spinand_mtd_erase',
          'spinand_mtd_block_markbad', 'spinand_write_page', 'spinand_erase_op',
          'do_mtd_erase', 'spinand_erase', 'spinand_markbad', 'env_save', 'env_export',
          'airoha_bmt_encode', 'airoha_bmt_publish', 'airoha_bmt_replace',
          'airoha_bmt_write_validate', 'airoha_bmt_intent_encode')
ABSENT_PREFIXES = ('ubi_', 'ubiblock_', 'do_ubi', 'airoha_bmt_transaction_',
                   'airoha_bmt_intent_')
# The read path, its translation, and the refusing nand_ops stubs without which
# the chip does not probe at all (nanddev_init requires erase, markbad and isbad
# entries).
REQUIRED = ('mtd_read_oob', 'spinand_mtd_read', 'mtd_block_isbad', 'do_mtd_io',
            'spinand_erase_refused', 'spinand_markbad_refused', 'airoha_bmt_attach',
            'airoha_bmt_attached', 'airoha_bmt_translate', 'airoha_bmt_parse_bmt',
            'airoha_bmt_vendor_pool', 'do_bmtdetach', 'w25n04lv_ooblayout_ecc')


def patches():
    own = sorted((UNIT / 'patches').glob('*.patch'))
    first = [patch for patch in own if int(patch.name[:4]) < 7]
    return [uboot.BOARD_PATCH, *uboot.SHARED_PATCHES, *first, BMT_PATCH,
            *own[len(first):]]


def check_write_free(linked, mapfile):
    present = sorted(name for name in ABSENT if name in linked)
    present += sorted(name for name in linked if name.startswith(ABSENT_PREFIXES))
    require(not present, 'dumper: NAND mutation code is linked: ' + ', '.join(present[:8]))
    allocated = mapfile[:mapfile.index('Discarded input sections')] + \
        mapfile[mapfile.index('Memory Configuration'):]
    for name in ABSENT:
        require(not re.search(rf'^\s*\.(?:text|rodata|data)\.{re.escape(name)}\b',
                              allocated, re.M),
                f'dumper: {name} is laid out in the image')
    missing = [name for name in REQUIRED if name not in linked]
    require(not missing, 'dumper: the read path is incomplete: ' + ', '.join(missing))


def main():
    src = uboot.prepare('dumper', patches(), FILES)
    uboot.mkimage(src)
    out = uboot.build('dumper', src, DEFCONFIG)
    config = kconfig(out / '.config')
    uboot.require_config('dumper', config, ENABLED, DISABLED)
    require(int(config['CONFIG_SYS_LOAD_ADDR'], 0) == DUMP_BUFFER,
            'dumper: the load address is not the dump buffer')
    image, dtb, linked = uboot.check_image('dumper', out, config, DUMP_BUFFER)
    require(DUMP_BUFFER + DUMP_BYTES <= uboot.RAM_END, 'dumper: the dump buffer passes RAM')
    check_write_free(linked, (out / 'u-boot.map').read_text(errors='replace'))

    # gzip shrinks the FIT to six 32 KiB pieces, the most stock's TFTP is relied
    # on to move: its ten-timeout budget per transfer is never refilled.
    payload = BUILD / 'dumper/u-boot.bin.gz'
    write_if_changed(payload, gzip.compress(image, 9, mtime=0))
    IMAGES.mkdir(parents=True, exist_ok=True)
    its = uboot.stock_fit('W1700K write-free NAND dumper (gzip)', 'dumper', payload, 'gzip',
                          out / 'u-boot.dtb')
    uboot.make_fit(its, FIT, out / 'scripts/dtc', LOGS / 'dumper.log')
    print(f'dumper: {FIT} ({FIT.stat().st_size} bytes)')
    return 0


if __name__ == '__main__':
    main_guard(main)
