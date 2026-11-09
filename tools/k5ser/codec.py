"""UV-K5 serial framing: ABCD / XOR / CRC16-XMODEM / DCBA."""

from __future__ import annotations

import struct

# Same 16-byte key Chirp's uvk5.py and the Quansheng CPS use.
XOR_KEY = bytes([
    0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40,
    0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80,
])


def xorarr(data: bytes) -> bytes:
    key = XOR_KEY
    return bytes(b ^ key[i % len(key)] for i, b in enumerate(data))


def crc16_xmodem(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode_frame(clear: bytes) -> bytes:
    """Wrap a cleartext command/reply (without CRC) the way Chirp sends it."""
    data2 = clear + struct.pack("<H", crc16_xmodem(clear))
    return (
        struct.pack(">HBB", 0xABCD, len(clear), 0)
        + xorarr(data2)
        + struct.pack(">H", 0xDCBA)
    )


def decode_body(xor_body: bytes) -> bytes:
    """XOR-decode the payload Chirp returns from `_receive_reply` (no CRC)."""
    return xorarr(xor_body)


class FrameDecoder:
    """Incremental ABCD…DCBA deframer matching Chirp's wire layout."""

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, data: bytes) -> list[bytes]:
        """Return cleartext payloads (without CRC) for each complete frame."""
        self._buf.extend(data)
        out: list[bytes] = []
        while True:
            clear = self._try_one()
            if clear is None:
                break
            out.append(clear)
        return out

    def _try_one(self) -> bytes | None:
        buf = self._buf
        # Find AB CD
        while True:
            if len(buf) < 4:
                return None
            try:
                i = buf.index(0xAB)
            except ValueError:
                buf.clear()
                return None
            if i:
                del buf[:i]
            if len(buf) < 4:
                return None
            if buf[1] != 0xCD:
                del buf[0]
                continue
            break

        body_len = buf[2]
        if buf[3] != 0x00:
            del buf[0]
            return None
        # header(4) + xor(body_len + 2 crc) + DC BA (2) = 6 + body_len + 2
        total = 4 + body_len + 2 + 2
        if len(buf) < total:
            return None
        xor_payload = bytes(buf[4 : 4 + body_len + 2])
        footer = buf[4 + body_len + 2 : total]
        if footer != b"\xDC\xBA":
            del buf[0]
            return None
        del buf[:total]
        clear_with_crc = xorarr(xor_payload)
        clear, crc_bytes = clear_with_crc[:-2], clear_with_crc[-2:]
        # Hosts (Chirp) often ignore reply CRC; still accept 0xFFFF.
        got = struct.unpack("<H", crc_bytes)[0]
        if got != 0xFFFF and got != crc16_xmodem(clear):
            # Drop bad frames rather than confuse the session.
            return None
        return clear
