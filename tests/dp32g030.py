import ctypes
import os
import re
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "src" / "DP32G030" / "DP32G030.h"
DEFAULT_ARCHIVE = ROOT / "build" / "src" / "DP32G030" / "libdp32g030.a"

FLASH_SIZE = 64 * 1024
SRAM_SIZE = 16 * 1024
SRAM_BASE = 0x20000000
SRAM_TOP = SRAM_BASE + SRAM_SIZE
CODE_BASE = 8

FLAG_N = 1 << 31
FLAG_Z = 1 << 30
FLAG_C = 1 << 29
FLAG_V = 1 << 28
XPSR_THUMB = 1 << 24

EMU_OK, EMU_FAULT, EMU_UNDEF = 0, 1, 2

BKPT = 0xBE00


def _parse_ops():
    text = HEADER.read_text()
    body = re.search(r"typedef enum \{(.*?)\} dp32g030_op_t;", text, re.S).group(1)
    names = [n.split("=")[0].strip() for n in body.split(",") if n.strip()]
    return SimpleNamespace(**{n[3:]: i for i, n in enumerate(names)})


Op = _parse_ops()


class Insn(ctypes.Structure):
    _fields_ = [
        ("op", ctypes.c_uint),
        ("rd", ctypes.c_uint8),
        ("rn", ctypes.c_uint8),
        ("rm", ctypes.c_uint8),
        ("imm", ctypes.c_uint32),
        ("cond", ctypes.c_uint8),
        ("reglist", ctypes.c_uint16),
        ("size", ctypes.c_uint8),
    ]


class Cpu(ctypes.Structure):
    _fields_ = [
        ("r", ctypes.c_uint32 * 16),
        ("insn_pc", ctypes.c_uint32),
        ("xpsr", ctypes.c_uint32),
        ("primask", ctypes.c_uint32),
        ("control", ctypes.c_uint32),
        ("halted", ctypes.c_bool),
        ("sleeping", ctypes.c_bool),
        ("flash", ctypes.c_uint8 * FLASH_SIZE),
        ("sram", ctypes.c_uint8 * SRAM_SIZE),
    ]


def load_lib(workdir):
    archive = Path(os.environ.get("DP32G030_LIB", DEFAULT_ARCHIVE))
    if not archive.exists():
        raise RuntimeError(
            f"{archive} not found: run `meson compile -C build` or set DP32G030_LIB"
        )

    so = Path(workdir) / "libdp32g030_test.so"
    subprocess.run(
        [
            os.environ.get("CC", "cc"),
            "-shared",
            "-o",
            str(so),
            "-Wl,--whole-archive",
            str(archive),
            "-Wl,--no-whole-archive",
        ],
        check=True,
    )

    lib = ctypes.CDLL(str(so))
    lib.dp32g030_insn_decode.argtypes = [ctypes.c_uint16, ctypes.c_uint16]
    lib.dp32g030_insn_decode.restype = Insn
    lib.dp32g030_cpu_new.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
    lib.dp32g030_cpu_new.restype = ctypes.POINTER(Cpu)
    lib.dp32g030_cpu_run.argtypes = [
        ctypes.POINTER(ctypes.POINTER(Cpu)),
        ctypes.c_size_t,
    ]
    lib.dp32g030_cpu_run.restype = ctypes.c_uint8
    lib.dp32g030_cpu_free.argtypes = [ctypes.POINTER(Cpu)]
    lib.dp32g030_cpu_free.restype = None
    lib.dp32g030_bus_read.argtypes = [
        ctypes.POINTER(Cpu),
        ctypes.c_uint32,
        ctypes.c_uint,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    lib.dp32g030_bus_read.restype = ctypes.c_bool
    lib.dp32g030_bus_write.argtypes = [
        ctypes.POINTER(Cpu),
        ctypes.c_uint32,
        ctypes.c_uint,
        ctypes.c_uint32,
    ]
    lib.dp32g030_bus_write.restype = ctypes.c_bool
    return lib


def firmware(halfwords, sp=SRAM_TOP, entry=CODE_BASE, extra=b""):
    code = b"".join(struct.pack("<H", h & 0xFFFF) for h in halfwords)
    return struct.pack("<II", sp, entry | 1) + code + extra


def to_buf(data):
    return (ctypes.c_uint8 * len(data)).from_buffer_copy(data)


@dataclass
class Result:
    status: int
    r: list
    xpsr: int
    primask: int
    control: int
    halted: bool
    sleeping: bool
    insn_pc: int
    sram: bytes
    flash: bytes

    @property
    def sp(self):
        return self.r[13]

    @property
    def lr(self):
        return self.r[14]

    @property
    def pc(self):
        return self.r[15]

    @property
    def n(self):
        return bool(self.xpsr & FLAG_N)

    @property
    def z(self):
        return bool(self.xpsr & FLAG_Z)

    @property
    def c(self):
        return bool(self.xpsr & FLAG_C)

    @property
    def v(self):
        return bool(self.xpsr & FLAG_V)

    def flags(self):
        return "".join(
            ch if getattr(self, ch.lower()) else "-" for ch in "NZCV"
        )


def run(lib, halfwords, regs=None, flags=0, sram=b"", steps=1000, append_bkpt=True):
    hws = list(halfwords) + ([BKPT] if append_bkpt else [])
    data = firmware(hws)
    ptr = lib.dp32g030_cpu_new(to_buf(data), len(data))
    assert ptr, "dp32g030_cpu_new returned NULL"
    try:
        cpu = ptr.contents
        for idx, val in (regs or {}).items():
            cpu.r[idx] = val
        cpu.xpsr |= flags
        if sram:
            ctypes.memmove(cpu.sram, sram, len(sram))
        status = lib.dp32g030_cpu_run(ctypes.byref(ptr), steps)
        return Result(
            status=status,
            r=list(cpu.r),
            xpsr=cpu.xpsr,
            primask=cpu.primask,
            control=cpu.control,
            halted=bool(cpu.halted),
            sleeping=bool(cpu.sleeping),
            insn_pc=cpu.insn_pc,
            sram=bytes(cpu.sram),
            flash=bytes(cpu.flash),
        )
    finally:
        lib.dp32g030_cpu_free(ptr)


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v
