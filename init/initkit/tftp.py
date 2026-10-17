#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A TFTP server for the installer: unprivileged, one directory, one peer at a time.

It serves the images U-Boot fetches and receives the backups and raw dumps U-Boot
sends.

* An upload never overwrites a file: a write request for an existing name is
  refused with TFTP error 6.
* It binds one specific local address, never a wildcard.
* Block numbers are 16 bits and wrap several times during the 446 MiB backup.
  U-Boot wraps 65535 -> 0 -> 1 and carries the wrap in its file offset, so this
  server addresses the file by a monotonic block index, not the wire number.

Standalone, it serves a directory by hand:

    init/initkit/tftp.py --root DIR --listen 192.168.1.10:7070 [--allow-write]
"""

import argparse
import hashlib
import os
import socket
import struct
import threading
import sys
import time
from pathlib import Path

RRQ, WRQ, DATA, ACK, ERROR, OACK = 1, 2, 3, 4, 5, 6
E_NOTFOUND, E_ACCESS, E_ILLEGAL, E_EXISTS = 1, 2, 4, 6

SEQUENCE = 1 << 16          # the 16-bit block counter's modulus
DEFAULT_BLKSIZE = 512
MAX_BLKSIZE = 65464
MAX_WINDOWSIZE = 16
PROGRESS_BYTES = 64 << 20
PREBUILD_LIMIT = 64 << 20   # above this, stream instead of serializing up front


class Refused(Exception):
    """A request we answer with a TFTP error packet and no transfer."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


class ClientAborted(Exception):
    """The peer sent ERROR; retransmitting cannot continue this transfer."""


def wire_number(index, sequence=SEQUENCE):
    """Wire block number for 0-based data chunk `index`: 1, 2, ... 65535, 0, 1."""
    return (index + 1) % sequence


def error_packet(code, message):
    return struct.pack('!HH', ERROR, code) + message.encode()[:500] + b'\0'


def parse_request(payload):
    """Split a RRQ/WRQ body into (filename, mode, options)."""
    fields = payload.split(b'\0')
    if len(fields) < 3 or not fields[0]:
        raise Refused(E_ILLEGAL, 'malformed request')
    name = fields[0].decode('latin-1')
    mode = fields[1].decode('latin-1').lower()
    if mode != 'octet':
        raise Refused(E_ILLEGAL, f'only octet mode is served, not {mode!r}')
    rest = fields[2:]
    options = {}
    for key, value in zip(rest[0::2], rest[1::2]):
        if key:
            options[key.decode('latin-1').lower()] = value.decode('latin-1')
    return name, options


def resolve(root, name):
    """Resolve a request name inside `root`, refusing any escape."""
    if name.startswith('/') or '\0' in name:
        raise Refused(E_ACCESS, 'absolute paths are not served')
    target = (root / name)
    try:
        final = target.resolve()
    except OSError as exc:
        raise Refused(E_ACCESS, f'cannot resolve path: {exc}') from exc
    if final != root and root not in final.parents:
        raise Refused(E_ACCESS, 'path escapes the served directory')
    return final


def negotiate(options, is_read, size, windowsize_cap):
    """Pick the options we accept, returning (blksize, windowsize, oack dict)."""
    accepted = {}
    blksize = DEFAULT_BLKSIZE
    if 'blksize' in options:
        try:
            wanted = int(options['blksize'])
        except ValueError:
            wanted = DEFAULT_BLKSIZE
        blksize = max(8, min(wanted, MAX_BLKSIZE))
        accepted['blksize'] = str(blksize)
    windowsize = 1
    # RFC 7440 on the read path only; U-Boot offers it for get and says so
    # ("Implemented only for tftp get", net/tftp.c:362-368).
    if is_read and 'windowsize' in options:
        try:
            wanted = int(options['windowsize'])
        except ValueError:
            wanted = 1
        windowsize = max(1, min(wanted, windowsize_cap))
        accepted['windowsize'] = str(windowsize)
    if 'timeout' in options:
        accepted['timeout'] = options['timeout']
    if 'tsize' in options:
        accepted['tsize'] = str(size if is_read else 0)
    return blksize, windowsize, accepted


def oack_packet(accepted):
    body = b''
    for key, value in accepted.items():
        body += key.encode() + b'\0' + str(value).encode() + b'\0'
    return struct.pack('!H', OACK) + body


class _Streamed:
    """Blocks read on demand, for a file too large to serialize up front."""

    def __init__(self, path, blksize, chunks, sequence):
        self.stream = path.open('rb')
        self.blksize, self.chunks, self.sequence = blksize, chunks, sequence

    def __getitem__(self, index):
        self.stream.seek(index * self.blksize)
        return (struct.pack('!HH', DATA, wire_number(index, self.sequence))
                + self.stream.read(self.blksize))

    def close(self):
        self.stream.close()


class Transfer:
    """One RRQ or WRQ, on its own socket, locked to one peer."""

    def __init__(self, bind_address, peer, timeout, retries, log, sequence=SEQUENCE):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind((bind_address, 0))
        self.socket.settimeout(timeout)
        self.peer = peer
        self.retries = retries
        self.log = log
        self.sequence = sequence

    def close(self):
        self.socket.close()

    def send(self, packet):
        self.socket.sendto(packet, self.peer)

    def receive(self):
        while True:
            packet, source = self.socket.recvfrom(65536)
            if source == self.peer:          # ignore anything from elsewhere
                return packet

    def fail(self, code, message):
        self.send(error_packet(code, message))
        self.log(f'error to client: {message}')

    def packets(self, path, blksize, chunks):
        """Every DATA packet for `path`, serialized before the transfer starts.

        Stock U-Boot's TFTP timer is broken: it spends its fixed budget of ten
        timeouts at about 100 per second of wall clock whatever is on the wire,
        and never refills it. A piece therefore has to arrive within roughly
        100 ms, so the hot loop is one sendto per acknowledgement and nothing
        else.
        """
        if path.stat().st_size > PREBUILD_LIMIT:
            return _Streamed(path, blksize, chunks, self.sequence)
        body = path.read_bytes()
        return [struct.pack('!HH', DATA, wire_number(index, self.sequence))
                + body[index * blksize:(index + 1) * blksize]
                for index in range(chunks)]

    def read(self, path, blksize, windowsize, accepted):
        """Serve `path` to the client. Returns bytes sent."""
        size = path.stat().st_size
        chunks = size // blksize + 1          # the last one may be short or empty
        blocks = self.packets(path, blksize, chunks)
        try:
            return self._serve_blocks(blocks, blksize, chunks, size,
                                      windowsize, accepted)
        finally:
            if hasattr(blocks, 'close'):
                blocks.close()

    def _serve_blocks(self, blocks, blksize, chunks, size, windowsize, accepted):
        """The hot loop: one sendto per acknowledgement and nothing else."""
        if accepted:
            if not self._await_ack(oack_packet(accepted), expect=0):
                return 0
        base = 0                               # next unacknowledged chunk
        started = time.monotonic()
        marker = PROGRESS_BYTES
        send, collect = self.socket.sendto, self._collect_ack
        peer = self.peer
        while base < chunks:
            for attempt in range(self.retries + 1):
                window = range(base, min(base + windowsize, chunks))
                for index in window:
                    send(blocks[index], peer)
                try:
                    advanced = collect(base, window)
                except ClientAborted:
                    return min(base * blksize, size)
                if advanced is not None:
                    base = advanced
                    break
                self.log(f'retransmitting from block {base + 1}'
                         f' (attempt {attempt + 1}/{self.retries + 1})')
            else:
                self.log(f'giving up at block {base + 1}')
                return base * blksize
            if base * blksize >= marker:
                self._progress(base * blksize, size, started)
                marker += PROGRESS_BYTES
        return size

    def _collect_ack(self, base, window):
        """Wait for an ACK inside `window`; return the new base, or None."""
        wanted = {wire_number(index, self.sequence): index for index in window}
        try:
            while True:
                packet = self.receive()
                if len(packet) < 4:
                    continue
                opcode, block = struct.unpack('!HH', packet[:4])
                if opcode == ERROR:
                    reason = packet[4:].rstrip(b'\0').decode('latin-1', 'replace')
                    self.log(f'client aborted: {reason}')
                    raise ClientAborted(reason)
                if opcode == ACK and block in wanted:
                    return wanted[block] + 1
        except socket.timeout:
            return None

    def _await_ack(self, packet, expect):
        for _ in range(self.retries + 1):
            self.send(packet)
            try:
                reply = self.receive()
            except socket.timeout:
                continue
            if len(reply) >= 4:
                opcode, block = struct.unpack('!HH', reply[:4])
                if opcode == ACK and block == expect:
                    return True
                if opcode == ERROR:
                    self.log('client rejected our option acknowledgement')
                    return False
        self.log('no acknowledgement of our option reply')
        return False

    def write(self, path, blksize, accepted, overwrite):
        """Receive into `path`. Returns (bytes, sha256) or (None, None)."""
        flags = os.O_WRONLY | os.O_CREAT | (0 if overwrite else os.O_EXCL)
        try:
            handle = os.open(path, flags, 0o600)
        except FileExistsError:
            self.fail(E_EXISTS, f'{path.name} exists; uploads never overwrite a file')
            return None, None
        except OSError as exc:
            self.fail(E_ACCESS, f'cannot create {path.name}: {exc.strerror}')
            return None, None
        digest = hashlib.sha256()
        received = 0                           # monotonic count of accepted chunks
        expected = 1                           # next wire number we will accept
        previous = None
        started = time.monotonic()
        marker = PROGRESS_BYTES
        try:
            with os.fdopen(handle, 'wb') as stream:
                opening = (oack_packet(accepted) if accepted
                           else struct.pack('!HH', ACK, 0))
                self.send(opening)
                while True:
                    for attempt in range(self.retries + 1):
                        try:
                            packet = self.receive()
                        except socket.timeout:
                            self.send(opening if previous is None
                                      else struct.pack('!HH', ACK, previous))
                            continue
                        if len(packet) < 4:
                            continue
                        opcode, block = struct.unpack('!HH', packet[:4])
                        if opcode == ERROR:
                            self.log('client aborted the upload')
                            return None, None
                        if opcode != DATA:
                            continue
                        body = packet[4:]
                        if block == expected:
                            stream.write(body)
                            digest.update(body)
                            received += 1
                            previous = block
                            expected = (expected + 1) % self.sequence
                            self.send(struct.pack('!HH', ACK, block))
                            if len(body) < blksize:
                                stream.flush()
                                os.fsync(stream.fileno())
                                total = received * blksize - (blksize - len(body))
                                return total, digest.hexdigest()
                            break
                        if previous is not None and block == previous:
                            self.send(struct.pack('!HH', ACK, block))  # duplicate
                            continue
                    else:
                        where = 'the first block' if previous is None else 'the next block'
                        self.log(f'too many retries waiting for {where}')
                        return None, None
                    if received * blksize >= marker:
                        self._progress(received * blksize, None, started)
                        marker += PROGRESS_BYTES
        except OSError as exc:
            self.log(f'write failed: {exc}')
            return None, None

    def _progress(self, done, total, started):
        elapsed = max(time.monotonic() - started, 1e-6)
        rate = done / elapsed / (1 << 20)
        of = f' of {total / (1 << 20):.0f} MiB' if total else ''
        self.log(f'{done / (1 << 20):.0f} MiB{of} in {elapsed:.0f}s ({rate:.1f} MiB/s)')


def prioritize(log):
    """Take this process out of contention, so a block is never late.

    The device's ten-timeout budget is spent on wall clock, not on packets, so
    an ordinary scheduling delay costs the transfer part of a budget it cannot
    get back. Best effort: a failure here is logged and ignored.
    """
    try:
        os.sched_setscheduler(0, os.SCHED_FIFO, os.sched_param(10))
        log('scheduling: SCHED_FIFO priority 10')
        return
    except (OSError, AttributeError, PermissionError) as exc:
        reason = getattr(exc, 'strerror', None) or type(exc).__name__
    try:
        os.nice(-20)
        log(f'scheduling: nice -20 (no SCHED_FIFO: {reason})')
    except OSError:
        log(f'scheduling: left at default ({reason})')


def serve(root, address, port, allow_write, overwrite, timeout, retries,
          once, windowsize_cap, out=sys.stderr, sequence=SEQUENCE, ready=None,
          priority=False):
    """Serve `root` on one address until interrupted. Returns transfers served."""
    def log(message):
        print(f'tftpd: {message}', file=out, flush=True)

    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((address, port))
    bound = listener.getsockname()
    log(f'serving {root} on {bound[0]}:{bound[1]} '
        f'({"read/write" if allow_write else "read-only"})')
    log(f'device: setenv serverip {bound[0]}'
        + (f'; setenv tftpdstp {bound[1]}' if bound[1] != 69 else ''))
    if priority:
        prioritize(log)
    served = 0
    try:
        if ready is not None:
            ready(bound)
        while True:
            packet, peer = listener.recvfrom(65536)
            if len(packet) < 2:
                continue
            opcode = struct.unpack('!H', packet[:2])[0]
            if opcode not in (RRQ, WRQ):
                continue
            transfer = Transfer(bound[0], peer, timeout, retries, log, sequence)
            try:
                name, options = parse_request(packet[2:])
                path = resolve(root, name)
                if opcode == RRQ:
                    if not path.is_file():
                        raise Refused(E_NOTFOUND, f'{name} is not a file here')
                    size = path.stat().st_size
                    blksize, windowsize, accepted = negotiate(
                        options, True, size, windowsize_cap)
                    log(f'{peer[0]}:{peer[1]} get {name} '
                        f'({size} bytes, blksize {blksize}, windowsize {windowsize})')
                    sent = transfer.read(path, blksize, windowsize, accepted)
                    log(f'sent {sent} bytes of {name}'
                        + ('' if sent == size else ' — INCOMPLETE'))
                else:
                    if not allow_write:
                        raise Refused(E_ACCESS, 'this server was started read-only')
                    blksize, _, accepted = negotiate(options, False, 0, windowsize_cap)
                    log(f'{peer[0]}:{peer[1]} put {name} (blksize {blksize})')
                    total, digest = transfer.write(path, blksize, accepted, overwrite)
                    if total is None:
                        log(f'upload of {name} failed; partial file left in place')
                    else:
                        log(f'received {total} bytes')
                        log(f'sha256 {digest}  {name}')
            except Refused as refusal:
                transfer.fail(refusal.code, refusal.message)
            finally:
                transfer.close()
            served += 1
            if once:
                return served
    except KeyboardInterrupt:
        log(f'stopped after {served} transfer(s)')
        return served
    finally:
        listener.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', required=True, help='directory to serve')
    parser.add_argument('--listen', required=True, metavar='ADDR:PORT',
                        help='specific local address and port, e.g. 192.168.1.10:6969')
    parser.add_argument('--allow-write', action='store_true',
                        help='accept uploads')
    parser.add_argument('--overwrite', action='store_true',
                        help='permit an upload to replace an existing file (off by default)')
    parser.add_argument('--timeout', type=float, default=5.0, help='per-packet timeout')
    parser.add_argument('--retries', type=int, default=5)
    parser.add_argument('--windowsize', type=int, default=MAX_WINDOWSIZE,
                        help=f'largest read window to grant (<= {MAX_WINDOWSIZE})')
    parser.add_argument('--once', action='store_true', help='exit after one transfer')
    parser.add_argument('--priority', action='store_true',
                        help='raise scheduling priority; the device charges a '
                             'late block against a budget it cannot refill')
    arguments = parser.parse_args(argv)

    root = Path(arguments.root).resolve()
    if not root.is_dir():
        raise SystemExit(f'tftpd: {root} is not a directory')
    if ':' not in arguments.listen:
        raise SystemExit('tftpd: --listen needs ADDR:PORT')
    address, _, port = arguments.listen.rpartition(':')
    if address in ('', '0.0.0.0', '::', '*'):
        raise SystemExit('tftpd: bind a specific local interface address, '
                         'not a wildcard')
    try:
        port = int(port)
    except ValueError:
        raise SystemExit(f'tftpd: {port!r} is not a port') from None
    if port < 1024 and os.geteuid() != 0:
        print(f'tftpd: port {port} needs root; see init/host/tftp-forward',
              file=sys.stderr)
    serve(root, address, port, arguments.allow_write, arguments.overwrite,
          arguments.timeout, arguments.retries, arguments.once,
          max(1, min(arguments.windowsize, MAX_WINDOWSIZE)),
          priority=arguments.priority)
    return 0


if __name__ == '__main__':
    sys.exit(main())


# ---------------------------------------------------------------------------
# One server per role, each in its own thread, for the whole of an init run.
# ---------------------------------------------------------------------------

class ServerThread:
    """Serve `root` for as long as a run needs, one fresh socket per transfer.

    Stock U-Boot picks its source port from a timer that barely advances, so
    consecutive transfers come from nearly the same port; a socket kept from the
    previous transfer would answer into the next one ("First block is not
    block 1"). A listener that lives for exactly one transfer cannot.
    """

    def __init__(self, root, address, port, allow_write=False, timeout=1.0,
                 retries=30, windowsize_cap=MAX_WINDOWSIZE, priority=False,
                 log=None, name='tftp'):
        self.root = Path(root).resolve()
        self.address = address
        self.port = port
        self.allow_write = allow_write
        self.timeout = timeout
        self.retries = retries
        self.windowsize_cap = windowsize_cap
        self.priority = priority
        self.name = name
        self.log = log or (lambda line: None)
        self.transfers = 0
        self.failure = None
        self.bound = None
        self._ready = threading.Event()
        self._stop = threading.Event()
        self._thread = None
        self._sink = _Sink(self.log)

    def start(self, wait=5.0):
        if not self.root.is_dir():
            raise SystemExit(f'tftp {self.name}: {self.root} is not a directory')
        if self.address in ('', '0.0.0.0', '::', '*'):
            raise SystemExit(f'tftp {self.name}: bind a specific local address')
        self._thread = threading.Thread(target=self._loop, name=self.name, daemon=True)
        self._thread.start()
        if not self._ready.wait(wait):
            raise SystemExit(f'tftp {self.name}: did not bind {self.address}:{self.port} '
                             f'within {wait:.0f}s')
        if self.failure is not None:
            raise self.failure
        return self

    def stop(self):
        self._stop.set()

    def _loop(self):
        first = True
        while not self._stop.is_set():
            try:
                served = serve(self.root, self.address, self.port, self.allow_write,
                               False, self.timeout, self.retries, True,
                               self.windowsize_cap, out=self._sink,
                               ready=self._bound, priority=self.priority and first)
            except OSError as exc:
                self.failure = _bind_failure(self.address, self.port, exc)
                self._ready.set()
                return
            except Exception as exc:                      # never silently stop serving
                self.log(f'{self.name}: transfer raised {type(exc).__name__}: {exc}')
                served = 0
            first = False
            self.transfers += served

    def _bound(self, bound):
        self.bound = bound
        # Pin the port the first bind chose. Rebinding per transfer is the point of
        # this loop, and rebinding port 0 would land somewhere else each time.
        self.port = bound[1]
        self._ready.set()


class _Sink:
    """`serve` writes with print(file=...); turn those lines into log calls."""

    def __init__(self, log):
        self.log = log
        self.partial = ''

    def write(self, text):
        self.partial += text
        while '\n' in self.partial:
            line, self.partial = self.partial.split('\n', 1)
            if line.strip():
                self.log(line.strip())
        return len(text)

    def flush(self):
        pass


def _bind_failure(address, port, exc):
    if isinstance(exc, PermissionError) and port < 1024:
        return SystemExit(
            f'cannot bind {address}:{port}: {exc.strerror}. Stock U-Boot can only '
            'fetch from UDP 69: run as root, or enable init/host/tftp-forward and '
            'use the default unprivileged port.')
    return SystemExit(f'cannot bind {address}:{port}: {exc}')
