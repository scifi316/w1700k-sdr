# SPDX-License-Identifier: MIT
"""Take a raw image of a unit's whole NAND, to send to the maintainers.

    1. stop stock U-Boot and boot the write-free dumper from RAM
    2. name the part and read the unit's serial
    3. drop the vendor translation (`bmtdetach`, a dumper-only command) and read
       every page of the chip with the ECC off, main area and OOB, into RAM
    4. hash it on the device, send it to the host and check the hash
    5. reset the unit, which then boots as it did before

Nothing is written to the unit. The image holds every page's main area in
physical order, then every page's OOB in the same order (the layout of U-Boot's
`mtd read.raw.oob`): 512 MiB + 32 MiB on both supported parts. It shows the
chip as it is, bad-block markers, vendor tables, spare bytes and ECC parity
included, which is what bringing up a part needs. It also carries the chip's
uncorrected bit errors, so it is not a backup: restores use init/install's
logical backup. nand-raw.json beside it records the part, the geometry, the
layout and the SHA-256. Like the backup, it holds the unit's stock credentials
(in the `calibration` window): share it with the maintainers only.
"""
from datetime import datetime, timezone
import json
import re
import shutil

from . import backup, config, nand as parts, stock
from . import console as unit
from .config import partitions
from .console import StepFailed, command
from .session import open_session

RAW_NAME = 'nand-raw.bin'
BUFFER = backup.DUMP_BUFFER
EXTENT = re.compile(r'- 0x([0-9a-f]+)-0x([0-9a-f]+) : "spi-nand0"')
LAYOUT = ("every page's main area in physical page order, then every page's OOB in "
          'the same order (U-Boot mtd read.raw.oob)')


def chip(listing):
    """(page, OOB, block, chip bytes) as `mtd list` reports the chip."""
    geometry, extent = parts.listed_geometry(listing), EXTENT.search(listing)
    if geometry is None or not extent:
        raise StepFailed('the dumper reported no NAND geometry; nothing was read:\n'
                         + listing[-600:])
    page, oob, block = geometry
    size = int(extent.group(2), 16) - int(extent.group(1), 16)
    if not page or size % block or block % page:
        raise StepFailed(f'implausible geometry: {page} + {oob} byte pages, {block}-byte '
                         f'blocks, {size} bytes')
    return page, oob, block, size


def part_name(listing):
    """(name or None, ID string or None) from the dumper's probe line."""
    probe = parts.PROBE.search(listing)
    if not probe:
        return None, None
    ident = bytes.fromhex(''.join(probe.group(1, 2, 3)))
    nand = partitions.nand_by_id(ident)
    return (nand.name if nand else None), ident.hex(' ')


def serial_or_none(console):
    try:
        return backup.read_serial(console)
    except StepFailed:
        return None


def run(settings, output=None):
    """Raw-dump the connected unit to `output`, or to nand-raw.bin in its unit
    directory; returns the image path."""
    config.require_images(config.DUMPER)
    if output is not None and output.exists():
        raise StepFailed(f'{output} exists; a raw dump is never overwritten')
    with open_session(settings, 'rawdump') as session:
        console, log = session.console, session.log
        unit.to_stock(console, log, unit.identify(console, log), fresh=True)
        print('rawdump: booting the write-free dumper')
        stock.stage(console, log, settings, config.DUMPER, session.served)
        stock.boot(console, 'dumper')
        listing = backup.list_chip(console, log)
        name, ident = part_name(listing)
        page, oob, block, size = chip(listing)
        translated = bool(backup.TRANSLATED.search(listing))
        stock_pool, pool = parts.pools(console.view(), listing)
        print(f'rawdump: NAND {name or "of an unsupported part"}'
              f'{f" (ID {ident})" if ident else ""}: {size >> 20} MiB, {block >> 10} KiB '
              f'blocks of {page} + {oob} byte pages; vendor tables '
              f'{"valid" if translated else "NOT valid"}')
        serial = serial_or_none(console) if translated else None
        if translated:
            command(console, 'bmtdetach', expect='addressed physically', limit=20)
        physical = backup.list_chip(console, log, 'mtd-list-physical.txt')
        if not re.search(r'translation:\s*none', physical):
            raise StepFailed('the dumper still translates the chip; nothing was read')
        if serial is None:
            serial = serial_or_none(console)     # the window, physically addressed
        if output is None:
            if serial is None:
                raise StepFailed('the unit\'s serial could not be read; name the file with '
                                 '--output; nothing was read')
            directory = config.unit_dir(settings.data_dir, serial)
            directory.mkdir(parents=True, exist_ok=True, mode=0o700)
            output = directory / RAW_NAME
        if output.exists():
            raise StepFailed(f'{output} exists; a raw dump is never overwritten')
        dump_bytes = size + size // page * oob
        backup.set_network(console, settings)
        print(f'  reading all {dump_bytes >> 20} MiB raw, ECC off (about two minutes, silent)')
        text = console.must_run(f'mtd read.raw.oob.dontskipbad spi-nand0 {BUFFER:#x} 0 '
                                f'{size:#x}', limit=1800, heartbeat=15, about=100)
        if 'Failure while reading' in text or 'failed with error' in text:
            raise StepFailed('the raw read failed; no dump was made:\n' + text[-600:])
        partial, received = backup.send(console, session.served, BUFFER, dump_bytes, RAW_NAME,
                                        'the raw image', dump_bytes // (12 << 20))
        meta = {
            'serial': serial, 'part': name, 'id': ident,
            'page_bytes': page, 'oob_bytes': oob, 'block_bytes': block, 'chip_bytes': size,
            'bytes': dump_bytes, 'sha256': received, 'layout': LAYOUT,
            'ecc': 'off: raw reads, the chip\'s bit errors uncorrected',
            'addressing': 'physical: no vendor BBT/BMT translation',
            'vendor_tables_valid': translated,
            'vendor_pool': {'stock_u_boot': stock_pool, 'ours': pool},
            'dumper_mtd_list': listing.replace('\r', ''),
            'taken_utc': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        }
        output.parent.mkdir(parents=True, exist_ok=True)
        partial.chmod(0o400)
        shutil.move(partial, output)
        meta_path = output.with_suffix('.json')
        meta_path.write_text(json.dumps(meta, indent=2) + '\n')
        meta_path.chmod(0o600)
        log.event('rawdump', path=str(output), sha256=received, bytes=dump_bytes)
        print('rawdump: resetting the unit; it boots as before')
        console.send_async('reset')
    print(f'rawdump: done; {output} ({dump_bytes} bytes, SHA-256 {received})\n'
          f'rawdump: description in {meta_path}\n'
          'rawdump: it holds the unit\'s stock credentials; send it to the maintainers only')
    return output
