"""Main-ROM programming commands used by Chirp (and shared with CPS)."""

from __future__ import annotations

import struct

from . import DEFAULT_FW
from .eeprom import Eeprom

CMD_HELLO = 0x0514
CMD_EEPROM_READ = 0x051B
CMD_EEPROM_WRITE = 0x051D
CMD_SET_PASSWORD = 0x052D
CMD_LOCK = 0x052F
CMD_RESET = 0x05DD

REP_HELLO = 0x0515
REP_EEPROM_READ = 0x051C
REP_EEPROM_WRITE = 0x051E
REP_SET_PASSWORD = 0x052E


class RadioSession:
    def __init__(self, eeprom: Eeprom, firmware: str = DEFAULT_FW) -> None:
        self.eeprom = eeprom
        self.firmware = firmware
        self.locked = False

    def handle(self, clear: bytes) -> bytes | None:
        if len(clear) < 4:
            return None
        cmd = struct.unpack_from("<H", clear, 0)[0]
        if cmd == CMD_HELLO or cmd == CMD_LOCK:
            if cmd == CMD_LOCK:
                self.locked = True
            return self._hello_reply()
        if cmd == CMD_EEPROM_READ:
            return self._eeprom_read(clear)
        if cmd == CMD_EEPROM_WRITE:
            return self._eeprom_write(clear)
        if cmd == CMD_SET_PASSWORD:
            # CPS / OEM tools send this; ack without changing anything yet.
            return struct.pack("<HH", REP_SET_PASSWORD, 4) + b"\x00\x00\x00\x00"
        if cmd == CMD_RESET:
            return None  # real radio resets; no reply
        return None

    def _hello_reply(self) -> bytes:
        # Chirp reads a C-string at offset 4 with maxlen 24. OEM replies use
        # length 0x24 (32-byte version region + the usual +4 convention).
        fw = self.firmware.encode("ascii", "replace")[:23] + b"\x00"
        body = fw.ljust(32, b"\x00")
        return struct.pack("<HH", REP_HELLO, len(body) + 4) + body

    def _eeprom_read(self, clear: bytes) -> bytes | None:
        # 1b 05 | 08 00 | off_lo off_hi | length | 00 | trailer...
        if len(clear) < 8:
            return None
        offset, length = struct.unpack_from("<HB", clear, 4)
        data = self.eeprom.read(offset, length)
        # Prefix Chirp strips with rep[8:]: cmd, len, offset, length, pad.
        return (
            struct.pack("<HHHBB", REP_EEPROM_READ, 4 + length, offset, length, 0)
            + data
        )

    def _eeprom_write(self, clear: bytes) -> bytes | None:
        # 1d 05 | (dlen+8) 00 | off_lo off_hi | dlen | 01 | trailer(4) | data
        if len(clear) < 12:
            return None
        offset, dlen, _flag = struct.unpack_from("<HBB", clear, 4)
        data = clear[12 : 12 + dlen]
        if len(data) != dlen:
            return None
        self.eeprom.write(offset, data)
        return struct.pack("<HHHBB", REP_EEPROM_WRITE, 8, offset, dlen, 1)
