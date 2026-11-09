"""File-backed 8 KiB UV-K5 configuration EEPROM."""

from __future__ import annotations

import os
from pathlib import Path

from . import MEM_SIZE


class Eeprom:
    def __init__(self, path: str | os.PathLike[str] | None = None) -> None:
        self.path = Path(path) if path is not None else None
        self._data = bytearray(b"\xFF" * MEM_SIZE)
        if self.path is not None and self.path.exists():
            raw = self.path.read_bytes()
            if len(raw) > MEM_SIZE:
                raise ValueError(f"EEPROM image larger than {MEM_SIZE:#x} bytes")
            self._data[: len(raw)] = raw

    def read(self, offset: int, length: int) -> bytes:
        if offset < 0 or length < 0 or offset + length > MEM_SIZE:
            raise ValueError(f"EEPROM read out of range: {offset:#x}+{length:#x}")
        return bytes(self._data[offset : offset + length])

    def write(self, offset: int, data: bytes) -> None:
        length = len(data)
        if offset < 0 or offset + length > MEM_SIZE:
            raise ValueError(f"EEPROM write out of range: {offset:#x}+{length:#x}")
        self._data[offset : offset + length] = data
        self.flush()

    def flush(self) -> None:
        if self.path is None:
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_bytes(bytes(self._data))

    def __len__(self) -> int:
        return MEM_SIZE
