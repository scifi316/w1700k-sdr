# SPDX-License-Identifier: MIT
"""Which SPI-NAND part the unit carries, from what our U-Boots print.

The W1700K and most W1700K2s carry a W25N04KV; some W1700K2s carry a W25N04LV
(4 KiB pages, 256 KiB blocks), which this project supports but has never run on
a unit. Our U-Boots name the part when they probe it (`spi-nand: ef 8b 23, ...`)
and `mtd list` shows its geometry; either identifies it.
"""
import re

from .config import partitions
from .console import StepFailed

# shared U-Boot patch 0006: maker and device ID bytes, then the geometry
PROBE = re.compile(r'spi-nand: ([0-9a-f]{2}) ([0-9a-f]{2}) ([0-9a-f]{2}), \d+ MiB, '
                   r'(\d+) KiB blocks of \d+ x \((\d+) \+ (\d+)\) bytes')
BLOCK = re.compile(r'- block size: (0x[0-9a-f]+) bytes')
PAGE = re.compile(r'- min I/O: (0x[0-9a-f]+) bytes')
OOB = re.compile(r'- OOB size: (\d+) bytes')

# Stock U-Boot at every boot, after the vendor pool scan and clamp; and our
# U-Boots when the translation attaches.
STOCK_POOL = re.compile(r'^bmt pool size: (\d+)\s*$', re.M)
OUR_POOL = re.compile(r'airoha-bmt: attached \(system_blocks=\d+, pool=(\d+)')

UNTESTED_FLAG = '--i-dont-care-about-nand-corruption'
SEND_DUMP = 'PLEASE SEND RAW NAND DUMP TO MAINTAINERS (init/rawdump takes one).'
RULE = '!' * 78


def untested_banner(nand):
    lines = ['UNTESTED NAND IC DETECTED!',
             'PLEASE SEND RAW NAND DUMP TO MAINTAINERS.',
             f'Restart with {UNTESTED_FLAG} to continue at your own risk.',
             '',
             f'This unit carries a {nand.name} (ID {nand.id.hex(" ")}). Take the raw dump',
             'with init/rawdump; it writes nothing.']
    return '\n'.join(['', RULE, *(f'!!  {line}'.rstrip() for line in lines), RULE, ''])


def identify(text):
    """The part a U-Boot console transcript names: by the probe line's ID when
    it holds one (checked against the geometry it prints), else by the geometry
    of `mtd list`. Raises StepFailed for a part the layout does not support."""
    probe = PROBE.search(text)
    if probe:
        ident = bytes.fromhex(''.join(probe.group(1, 2, 3)))
        block, page, oob = int(probe.group(4)) << 10, int(probe.group(5)), int(probe.group(6))
        nand = partitions.nand_by_id(ident)
        if nand is None:
            raise StepFailed(f'UNSUPPORTED NAND IC: ID {ident.hex(" ")} ({page} + {oob} byte '
                             f'pages, {block >> 10} KiB blocks). Only the '
                             f'{" and ".join(n.name for n in partitions.NANDS)} are supported; '
                             'nothing was written. ' + SEND_DUMP)
        if (nand.page, nand.oob, nand.block) != (page, oob, block):
            raise StepFailed(f'the {nand.name} reports {page} + {oob} byte pages and '
                             f'{block >> 10} KiB blocks, not its own geometry; nothing was '
                             'written')
        return nand
    geometry = listed_geometry(text)
    if geometry is None:
        raise StepFailed('U-Boot reported no SPI-NAND part; nothing was written:\n'
                         + text[-600:])
    page, oob, block = geometry
    nand = partitions.nand_by_geometry(page, block, oob)
    if nand is None:
        raise StepFailed(f'UNSUPPORTED NAND IC: {page} + {oob} byte pages, {block >> 10} KiB '
                         'blocks; nothing was written. ' + SEND_DUMP)
    return nand


def listed_geometry(text):
    """(page, OOB, block) bytes as `mtd list` prints them, or None."""
    found = [pattern.search(text) for pattern in (PAGE, OOB, BLOCK)]
    if not all(found):
        return None
    return int(found[0].group(1), 16), int(found[1].group(1)), int(found[2].group(1), 16)


def pools(transcript, listing):
    """(stock's pool, ours): stock's from the last `bmt pool size` of its boot
    in this run's console transcript, ours from our U-Boot's attach line in
    `listing`; either is None when not printed."""
    stock = STOCK_POOL.findall(transcript.replace('\r', ''))
    ours = OUR_POOL.search(listing)
    return (int(stock[-1]) if stock else None), (int(ours.group(1)) if ours else None)


def check_pool(nand, transcript, listing, verb='install'):
    """Stock and our U-Boot must agree on the vendor reserve pool before anything
    is written: on the W25N04LV ours comes from a rule read in a stock U-Boot
    that does not know the part."""
    stock, ours = pools(transcript, listing)
    if stock is not None and ours is not None and stock != ours:
        raise StepFailed(f'stock U-Boot reports a vendor bad-block pool of {stock} blocks '
                         f'and ours computes {ours}: this unit\'s tables are not handled '
                         f'the way stock handles them, so {verb} stopped with nothing '
                         'written. ' + SEND_DUMP)
    if stock is None and not nand.tested:
        print(f'{verb}: WARNING: stock U-Boot printed no `bmt pool size` in this run, so its '
              f'vendor bad-block pool could not be checked against ours ({ours})', flush=True)


def require_tested(nand, accept_untested, verb='install'):
    """Stop at a part never run on a unit, unless the user accepted the risk."""
    if nand.tested:
        return
    if not accept_untested:
        raise StepFailed(untested_banner(nand) + f'\nNothing was written; {verb} stopped.')
    print(untested_banner(nand) + f'\n{verb}: continuing at your own risk ({UNTESTED_FLAG})',
          flush=True)
