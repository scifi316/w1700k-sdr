# SPDX-License-Identifier: MIT
"""The installer's TFTP server, over loopback; no device and no privileged port."""

import hashlib
import importlib.util
import os
import socket
import struct
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('init_tftp', ROOT / 'init/initkit/tftp.py')
tftpd = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tftpd)

RRQ, WRQ, DATA, ACK, ERROR, OACK = 1, 2, 3, 4, 5, 6
HOST = '127.0.0.1'


class Server:
    """Run `serve` on a loopback port in a thread, for exactly one transfer."""

    def __init__(self, root, sequence=tftpd.SEQUENCE, **options):
        self.root = root
        self.sequence = sequence
        self.options = options
        self.port = None
        self.log = []
        self._bound = threading.Event()

    def __enter__(self):
        class Sink:
            def __init__(self, sink):
                self.sink = sink

            def write(self, text):
                if text.strip():
                    self.sink.append(text.strip())

            def flush(self):
                pass

        def ready(bound):
            self.port = bound[1]
            self._bound.set()

        settings = dict(allow_write=True, overwrite=False, timeout=2.0, retries=3,
                        once=True, windowsize_cap=tftpd.MAX_WINDOWSIZE)
        settings.update(self.options)
        self.thread = threading.Thread(target=tftpd.serve, kwargs=dict(
            root=self.root, address=HOST, port=0, out=Sink(self.log),
            sequence=self.sequence, ready=ready, **settings), daemon=True)
        self.thread.start()
        assert self._bound.wait(5), 'server did not bind'
        return self

    def __exit__(self, *exc):
        self.thread.join(timeout=10)
        return False


class Client:
    """A minimal TFTP client, wrapping block numbers the way U-Boot does."""

    def __init__(self, port, sequence=tftpd.SEQUENCE):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.settimeout(5)
        self.address = (HOST, port)
        self.sequence = sequence

    def close(self):
        self.socket.close()

    def request(self, opcode, name, options):
        packet = struct.pack('!H', opcode) + name.encode() + b'\0octet\0'
        for key, value in options.items():
            packet += key.encode() + b'\0' + str(value).encode() + b'\0'
        self.socket.sendto(packet, self.address)
        reply, peer = self.socket.recvfrom(65536)
        self.address = peer
        return reply

    def get(self, name, **options):
        reply = self.request(RRQ, name, options)
        opcode = struct.unpack('!H', reply[:2])[0]
        if opcode == ERROR:
            return None, struct.unpack('!H', reply[2:4])[0]
        blksize = int(options.get('blksize', 512))
        data = bytearray()
        if opcode == OACK:
            fields = reply[2:].split(b'\0')
            granted = dict(zip(fields[0::2], fields[1::2]))
            blksize = int(granted.get(b'blksize', blksize))
            self.socket.sendto(struct.pack('!HH', ACK, 0), self.address)
            reply = None
        index = 0
        while True:
            if reply is None:
                reply, _ = self.socket.recvfrom(65536)
            opcode, block = struct.unpack('!HH', reply[:4])
            reply = None
            if opcode == ERROR:
                return None, struct.unpack('!HH', reply[:4])[1]
            if block != tftpd.wire_number(index, self.sequence):
                continue
            body = reply if False else None
            payload = b''
            data.extend(payload)
            raise AssertionError('unreachable')

    def get_file(self, name, **options):
        """Read a whole file, returning (bytes, error_code)."""
        reply = self.request(RRQ, name, options)
        opcode = struct.unpack('!H', reply[:2])[0]
        if opcode == ERROR:
            return None, struct.unpack('!H', reply[2:4])[0]
        blksize = int(options.get('blksize', 512))
        pending = None
        if opcode == OACK:
            fields = reply[2:].split(b'\0')
            granted = dict(zip(fields[0::2], fields[1::2]))
            blksize = int(granted.get(b'blksize', str(blksize).encode()))
            self.socket.sendto(struct.pack('!HH', ACK, 0), self.address)
        else:
            pending = reply
        data = bytearray()
        index = 0
        while True:
            packet = pending if pending is not None else self.socket.recvfrom(65536)[0]
            pending = None
            opcode, block = struct.unpack('!HH', packet[:4])
            if opcode == ERROR:
                return None, block
            if opcode != DATA or block != tftpd.wire_number(index, self.sequence):
                continue
            body = packet[4:]
            data.extend(body)
            self.socket.sendto(struct.pack('!HH', ACK, block), self.address)
            index += 1
            if len(body) < blksize:
                return bytes(data), None

    def put_file(self, name, payload, **options):
        """Write a whole file, returning the error code or None on success."""
        reply = self.request(WRQ, name, options)
        opcode, first = struct.unpack('!HH', reply[:4])
        if opcode == ERROR:
            return first
        blksize = int(options.get('blksize', 512))
        if opcode == OACK:
            fields = reply[2:].split(b'\0')
            granted = dict(zip(fields[0::2], fields[1::2]))
            blksize = int(granted.get(b'blksize', str(blksize).encode()))
        index = 0
        while True:
            body = payload[index * blksize:(index + 1) * blksize]
            self.socket.sendto(
                struct.pack('!HH', DATA, tftpd.wire_number(index, self.sequence)) + body,
                self.address)
            while True:
                packet, _ = self.socket.recvfrom(65536)
                opcode, block = struct.unpack('!HH', packet[:4])
                if opcode == ERROR:
                    return block
                if opcode == ACK and block == tftpd.wire_number(index, self.sequence):
                    break
            index += 1
            if len(body) < blksize:
                return None


class WireNumbering(unittest.TestCase):
    def test_matches_uboot_wrap(self):
        # U-Boot's own sequence: 1..65535 then 0, then 1 again (net/tftp.c:278-281).
        self.assertEqual(tftpd.wire_number(0), 1)
        self.assertEqual(tftpd.wire_number(65534), 65535)
        self.assertEqual(tftpd.wire_number(65535), 0)
        self.assertEqual(tftpd.wire_number(65536), 1)

    def test_offset_formula_agrees_with_uboot(self):
        # U-Boot: offset = block * blksize + wrap * blksize * 65536 - blksize.
        blksize = 1468
        for index in (0, 1, 65534, 65535, 65536, 131071, 131072):
            block = tftpd.wire_number(index)
            wrap = (index + 1) // tftpd.SEQUENCE
            uboot = block * blksize + wrap * blksize * tftpd.SEQUENCE - blksize
            self.assertEqual(uboot, index * blksize,
                             f'index {index} disagrees with the U-Boot formula')

    def test_the_446_mib_dump_wraps_the_counter(self):
        dump = 0x1be00000                      # the dumper's backup length
        for blksize, wraps in ((1468, 4), (512, 13)):
            blocks = -(-dump // blksize)
            self.assertEqual(blocks // tftpd.SEQUENCE, wraps)
            # A receiver ignoring the wrap would stop being correct here.
            self.assertLess(65535 * blksize, dump)


class Serving(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name).resolve()
        self.addCleanup(self.directory.cleanup)

    def test_get_with_blksize_and_window(self):
        payload = os.urandom(9000)
        (self.root / 'image.itb').write_bytes(payload)
        with Server(self.root) as server:
            client = Client(server.port)
            self.addCleanup(client.close)
            data, error = client.get_file('image.itb', blksize=1468, windowsize=4)
        self.assertIsNone(error)
        self.assertEqual(data, payload)

    def test_get_of_an_exact_multiple_ends_with_an_empty_block(self):
        payload = os.urandom(1024)
        (self.root / 'exact.bin').write_bytes(payload)
        with Server(self.root) as server:
            client = Client(server.port)
            self.addCleanup(client.close)
            data, error = client.get_file('exact.bin', blksize=512)
        self.assertIsNone(error)
        self.assertEqual(data, payload)

    def test_streaming_fallback_is_byte_exact(self):
        """A file above PREBUILD_LIMIT takes the _Streamed path, not a list.

        Small images are serialized up front to win the device's wall-clock
        timeout budget, but the later whole-rootfs download is far too large
        for that, so both accessors must produce identical bytes.
        """
        payload = os.urandom(9000)
        (self.root / 'big.bin').write_bytes(payload)
        limit = tftpd.PREBUILD_LIMIT
        tftpd.PREBUILD_LIMIT = 1024          # force the streaming accessor
        self.addCleanup(setattr, tftpd, 'PREBUILD_LIMIT', limit)
        with Server(self.root) as server:
            client = Client(server.port)
            self.addCleanup(client.close)
            data, error = client.get_file('big.bin', blksize=1468)
        self.assertIsNone(error)
        self.assertEqual(data, payload)

    def test_both_accessors_agree_packet_for_packet(self):
        """The prebuilt list and the streamed reader are interchangeable."""
        payload = os.urandom(5000)
        target = self.root / 'agree.bin'
        target.write_bytes(payload)
        chunks = len(payload) // 1468 + 1
        transfer = tftpd.Transfer('127.0.0.1', ('127.0.0.1', 9), 1, 0, lambda _: None)
        self.addCleanup(transfer.close)
        prebuilt = transfer.packets(target, 1468, chunks)
        streamed = tftpd._Streamed(target, 1468, chunks, tftpd.SEQUENCE)
        self.addCleanup(streamed.stream.close)
        self.assertIsInstance(prebuilt, list)
        for index in range(chunks):
            self.assertEqual(prebuilt[index], streamed[index], f'block {index}')

    def test_client_error_ends_download_without_retransmitting(self):
        (self.root / 'abort.itb').write_bytes(b'A' * 1024)
        with Server(self.root, timeout=.1, retries=2) as server:
            client = Client(server.port)
            self.addCleanup(client.close)
            packet = client.request(RRQ, 'abort.itb', {})
            self.assertEqual(struct.unpack('!HH', packet[:4]), (DATA, 1))
            client.socket.sendto(tftpd.error_packet(0, 'client restarted'), client.address)
            server.thread.join(timeout=1)
            self.assertFalse(server.thread.is_alive())
            client.socket.settimeout(.15)
            with self.assertRaises(socket.timeout):
                client.socket.recvfrom(65536)
        self.assertTrue(any('client aborted' in line for line in server.log))
        self.assertFalse(any('retransmitting' in line for line in server.log))

    def test_put_writes_the_file_and_reports_its_hash(self):
        payload = os.urandom(5000)
        with Server(self.root) as server:
            client = Client(server.port)
            self.addCleanup(client.close)
            error = client.put_file('unit001-nand.bin', payload, blksize=1468)
            log = server.log
        self.assertIsNone(error)
        written = (self.root / 'unit001-nand.bin').read_bytes()
        self.assertEqual(written, payload)
        self.assertTrue(any(hashlib.sha256(payload).hexdigest() in line for line in log),
                        f'the received hash was not reported: {log}')

    def test_put_refuses_to_overwrite_an_existing_file(self):
        (self.root / 'unit001-nand.bin').write_bytes(b'original backup')
        with Server(self.root) as server:
            client = Client(server.port)
            self.addCleanup(client.close)
            error = client.put_file('unit001-nand.bin', os.urandom(600))
        self.assertEqual(error, tftpd.E_EXISTS)
        self.assertEqual((self.root / 'unit001-nand.bin').read_bytes(),
                         b'original backup')

    def test_put_is_refused_when_the_server_is_read_only(self):
        with Server(self.root, allow_write=False) as server:
            client = Client(server.port)
            self.addCleanup(client.close)
            error = client.put_file('new.bin', b'x' * 10)
        self.assertEqual(error, tftpd.E_ACCESS)
        self.assertFalse((self.root / 'new.bin').exists())

    def test_path_escape_is_refused(self):
        with Server(self.root) as server:
            client = Client(server.port)
            self.addCleanup(client.close)
            data, error = client.get_file('../../etc/passwd')
        self.assertIsNone(data)
        self.assertEqual(error, tftpd.E_ACCESS)

    def test_absolute_path_is_refused(self):
        with Server(self.root) as server:
            client = Client(server.port)
            self.addCleanup(client.close)
            data, error = client.get_file('/etc/passwd')
        self.assertIsNone(data)
        self.assertEqual(error, tftpd.E_ACCESS)

    def test_missing_file_is_refused(self):
        with Server(self.root) as server:
            client = Client(server.port)
            self.addCleanup(client.close)
            data, error = client.get_file('absent.itb')
        self.assertIsNone(data)
        self.assertEqual(error, tftpd.E_NOTFOUND)

    def test_upload_crossing_a_block_wrap_is_byte_exact(self):
        # The real 446 MiB dump wraps the 16-bit counter four times. Shrink the
        # modulus instead of the transfer so the wrap path runs for real.
        sequence, blksize = 8, 16
        payload = os.urandom(blksize * (sequence * 3 + 2) + 5)
        with Server(self.root, sequence=sequence) as server:
            client = Client(server.port, sequence=sequence)
            self.addCleanup(client.close)
            error = client.put_file('wrapped.bin', payload, blksize=blksize)
        self.assertIsNone(error)
        self.assertEqual((self.root / 'wrapped.bin').read_bytes(), payload)

    def test_download_crossing_a_block_wrap_is_byte_exact(self):
        sequence, blksize = 8, 16
        payload = os.urandom(blksize * (sequence * 2 + 3) + 7)
        (self.root / 'wrapped.itb').write_bytes(payload)
        with Server(self.root, sequence=sequence) as server:
            client = Client(server.port, sequence=sequence)
            self.addCleanup(client.close)
            data, error = client.get_file('wrapped.itb', blksize=blksize, windowsize=3)
        self.assertIsNone(error)
        self.assertEqual(data, payload)


class Arguments(unittest.TestCase):
    def test_wildcard_bind_is_refused(self):
        for listen in ('0.0.0.0:6969', ':6969', '*:6969'):
            with self.assertRaises(SystemExit):
                tftpd.main(['--root', '.', '--listen', listen])

    def test_a_missing_root_is_refused(self):
        with self.assertRaises(SystemExit):
            tftpd.main(['--root', '/nonexistent-root', '--listen', '127.0.0.1:6969'])


if __name__ == '__main__':
    unittest.main()
