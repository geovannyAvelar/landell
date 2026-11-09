"""UV-K5 k5ser: codec + Chirp-shaped hello/read/write over a PTY."""

from __future__ import annotations

import os
import struct
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from k5ser import DEFAULT_FW, MEM_SIZE  # noqa: E402
from k5ser.codec import FrameDecoder, crc16_xmodem, encode_frame, xorarr  # noqa: E402
from k5ser.eeprom import Eeprom  # noqa: E402
from k5ser.proto import RadioSession  # noqa: E402
from k5ser.server import open_pty, serve_fd  # noqa: E402


def test_xor_roundtrip():
    raw = bytes(range(40))
    assert xorarr(xorarr(raw)) == raw


def test_crc_matches_known_hello():
    hello = b"\x14\x05\x04\x00\x6a\x39\x57\x64"
    assert crc16_xmodem(hello) == 0x9C98


def test_encode_decode_frame_roundtrip():
    clear = b"\x14\x05\x04\x00\x6a\x39\x57\x64"
    frame = encode_frame(clear)
    dec = FrameDecoder()
    got = dec.feed(frame)
    assert got == [clear]


def test_decoder_handles_split_writes():
    clear = b"\x1b\x05\x08\x00\x00\x00\x10\x00\x6a\x39\x57\x64"
    frame = encode_frame(clear)
    dec = FrameDecoder()
    assert dec.feed(frame[:3]) == []
    assert dec.feed(frame[3:9]) == []
    assert dec.feed(frame[9:]) == [clear]


def test_hello_reply_firmware_string():
    sess = RadioSession(Eeprom(), firmware="k5_2.01.26")
    reply = sess.handle(b"\x14\x05\x04\x00\x6a\x39\x57\x64")
    assert reply is not None
    assert reply[:2] == b"\x15\x05"
    # Chirp `_getstring(rep, 4, 24)`
    end = reply.index(0, 4)
    assert reply[4:end].decode() == "k5_2.01.26"


def test_eeprom_read_write(tmp_path):
    path = tmp_path / "e.bin"
    sess = RadioSession(Eeprom(path))
    payload = bytes(range(16))
    write_cmd = (
        b"\x1d\x05"
        + struct.pack("<BBHBB", 16 + 8, 0, 0x100, 16, 1)
        + b"\x6a\x39\x57\x64"
        + payload
    )
    ack = sess.handle(write_cmd)
    assert ack is not None
    assert ack[0] == 0x1E
    assert ack[4] == 0x00 and ack[5] == 0x01

    read_cmd = b"\x1b\x05\x08\x00" + struct.pack("<HBB", 0x100, 16, 0) + b"\x6a\x39\x57\x64"
    rep = sess.handle(read_cmd)
    assert rep is not None
    assert rep[8:] == payload
    # Persisted
    assert Eeprom(path).read(0x100, 16) == payload


def test_reset_has_no_reply():
    sess = RadioSession(Eeprom())
    assert sess.handle(b"\xdd\x05\x00\x00") is None


def test_lock_returns_hello():
    sess = RadioSession(Eeprom(), firmware="k5_2.01.26")
    rep = sess.handle(b"\x2f\x05\x04\x00\x6a\x39\x57\x64")
    assert rep is not None and rep[:2] == b"\x15\x05"
    assert sess.locked


class _ChirpClient:
    """Minimal Chirp-shaped client on the PTY slave side."""

    def __init__(self, slave_path: str) -> None:
        self.fd = os.open(slave_path, os.O_RDWR | os.O_NOCTTY)
        self.dec = FrameDecoder()

    def close(self) -> None:
        os.close(self.fd)

    def call(self, clear: bytes, timeout: float = 2.0) -> bytes:
        os.write(self.fd, encode_frame(clear))
        deadline = time.time() + timeout
        while time.time() < deadline:
            import select

            r, _, _ = select.select([self.fd], [], [], 0.05)
            if not r:
                continue
            chunk = os.read(self.fd, 4096)
            if not chunk:
                break
            for got in self.dec.feed(chunk):
                return got
        raise TimeoutError("no reply from k5ser")


def test_chirp_clone_download_upload_over_pty(tmp_path):
    path = tmp_path / "eeprom.bin"
    # Seed a recognizable pattern
    seed = Eeprom(path)
    seed.write(0, b"LANDELL-K5SER" + b"\xFF" * (MEM_SIZE - 13))

    session = RadioSession(Eeprom(path), firmware=DEFAULT_FW)
    master, slave_fd, slave = open_pty()
    t = threading.Thread(target=serve_fd, args=(session, master), daemon=True)
    t.start()
    time.sleep(0.05)

    client = _ChirpClient(slave)
    try:
        # hello
        hello = b"\x14\x05\x04\x00\x6a\x39\x57\x64"
        rep = client.call(hello)
        assert rep[:2] == b"\x15\x05"
        end = rep.index(0, 4)
        assert rep[4:end].decode() == DEFAULT_FW

        # full download in 0x80 chunks like Chirp
        eeprom = bytearray()
        for addr in range(0, MEM_SIZE, 0x80):
            n = min(0x80, MEM_SIZE - addr)
            cmd = b"\x1b\x05\x08\x00" + struct.pack("<HBB", addr, n, 0) + b"\x6a\x39\x57\x64"
            chunk = client.call(cmd)[8:]
            assert len(chunk) == n
            eeprom.extend(chunk)
        assert eeprom[:13] == b"LANDELL-K5SER"

        # upload a block and read it back
        data = bytes([0xA5] * 16)
        wcmd = (
            b"\x1d\x05"
            + struct.pack("<BBHBB", 16 + 8, 0, 0x200, 16, 1)
            + b"\x6a\x39\x57\x64"
            + data
        )
        wrep = client.call(wcmd)
        assert wrep[0] == 0x1E and wrep[4] == 0x00 and wrep[5] == 0x02
        rcmd = b"\x1b\x05\x08\x00" + struct.pack("<HBB", 0x200, 16, 0) + b"\x6a\x39\x57\x64"
        assert client.call(rcmd)[8:] == data
        assert Eeprom(path).read(0x200, 16) == data
    finally:
        client.close()
        os.close(master)
        os.close(slave_fd)
        t.join(timeout=2)


def test_password_cmd_acks():
    sess = RadioSession(Eeprom())
    cmd = b"\x2d\x05\x10\x00" + b"\x00" * 16
    rep = sess.handle(cmd)
    assert rep is not None and rep[:2] == b"\x2e\x05"
