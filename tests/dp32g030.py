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
FLASH_MAX = 128 * 1024  # backing store shared by every SoC
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


class Scs(ctypes.Structure):
    _fields_ = [
        ("pending", ctypes.c_uint64),
        ("active", ctypes.c_uint64),
        ("irq_lines", ctypes.c_uint32),
        ("nvic_enabled", ctypes.c_uint32),
        ("prio", ctypes.c_uint8 * 48),
        ("scr", ctypes.c_uint32),
        ("st_ctrl", ctypes.c_uint32),
        ("st_load", ctypes.c_uint32),
        ("st_val", ctypes.c_uint32),
    ]


UART_TX_CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint8)
GPIO_CB = ctypes.CFUNCTYPE(
    None, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint32, ctypes.c_uint32
)


class Gpio(ctypes.Structure):
    _fields_ = [
        ("out", ctypes.c_uint32),
        ("dir", ctypes.c_uint32),
        ("in_", ctypes.c_uint32),
        ("driven", ctypes.c_uint32),
    ]


class Uart(ctypes.Structure):
    _fields_ = [
        ("ctrl", ctypes.c_uint32),
        ("baud", ctypes.c_uint32),
        ("ie", ctypes.c_uint32),
        ("fifo", ctypes.c_uint32),
        ("fc", ctypes.c_uint32),
        ("rxto", ctypes.c_uint32),
        ("sticky", ctypes.c_uint32),
        ("rx", ctypes.c_uint8 * 8),
        ("rx_head", ctypes.c_uint),
        ("rx_count", ctypes.c_uint),
        ("rxto_deadline", ctypes.c_uint64),
    ]


SPI_CB = ctypes.CFUNCTYPE(ctypes.c_uint8, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint8)
I2C_START_CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint)
I2C_WRITE_CB = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint8)
I2C_READ_CB = ctypes.CFUNCTYPE(ctypes.c_uint8, ctypes.c_void_p, ctypes.c_uint, ctypes.c_bool)
I2C_STOP_CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint)
BK4819_FREQ_CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint32)
BK4819_TONE_CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint)
BK4819_GPIO_CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint8)
LCD_UPDATE_CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p)


class Spi(ctypes.Structure):
    _fields_ = [
        ("cr", ctypes.c_uint32),
        ("ie", ctypes.c_uint32),
        ("sticky", ctypes.c_uint32),
        ("rx", ctypes.c_uint8 * 8),
        ("rx_head", ctypes.c_uint),
        ("rx_count", ctypes.c_uint),
    ]


class Adc(ctypes.Structure):
    _fields_ = [
        ("cfg", ctypes.c_uint32),
        ("ie", ctypes.c_uint32),
        ("ifl", ctypes.c_uint32),
        ("exttrig", ctypes.c_uint32),
        ("calib_offset", ctypes.c_uint32),
        ("calib_kd", ctypes.c_uint32),
        ("input", ctypes.c_uint32 * 16),
        ("result", ctypes.c_uint32 * 16),
    ]


class Crc(ctypes.Structure):
    _fields_ = [("cr", ctypes.c_uint32), ("iv", ctypes.c_uint32), ("state", ctypes.c_uint32)]


class TimerBase(ctypes.Structure):
    _fields_ = [
        ("en", ctypes.c_uint32),
        ("div", ctypes.c_uint32),
        ("ie", ctypes.c_uint32),
        ("iflags", ctypes.c_uint32),
        ("high_load", ctypes.c_uint32),
        ("high_cnt", ctypes.c_uint32),
        ("low_load", ctypes.c_uint32),
        ("low_cnt", ctypes.c_uint32),
        ("presc", ctypes.c_uint32),
    ]


class PwmBase(ctypes.Structure):
    _fields_ = [
        ("en", ctypes.c_uint32),
        ("div", ctypes.c_uint32),
        ("con", ctypes.c_uint32),
        ("period", ctypes.c_uint32),
        ("ie", ctypes.c_uint32),
        ("iflags", ctypes.c_uint32),
        ("cnt", ctypes.c_uint32),
        ("comp", ctypes.c_uint32 * 3),
        ("presc", ctypes.c_uint32),
    ]


class TimerPlus(ctypes.Structure):
    _fields_ = [
        ("en", ctypes.c_uint32),
        ("div", ctypes.c_uint32),
        ("ctr", ctypes.c_uint32),
        ("ie", ctypes.c_uint32),
        ("iflags", ctypes.c_uint32),
        ("high_load", ctypes.c_uint32),
        ("high_cnt", ctypes.c_uint32),
        ("high_cval", ctypes.c_uint32),
        ("low_load", ctypes.c_uint32),
        ("low_cnt", ctypes.c_uint32),
        ("low_cval", ctypes.c_uint32),
        ("hall_val", ctypes.c_uint32),
        ("ext_level", ctypes.c_uint32 * 2),
        ("presc", ctypes.c_uint32),
    ]


class PwmPlus(ctypes.Structure):
    _fields_ = [
        ("cfg", ctypes.c_uint32),
        ("gen", ctypes.c_uint32),
        ("clksrc", ctypes.c_uint32),
        ("brake_cfg", ctypes.c_uint32),
        ("mask_lev", ctypes.c_uint32),
        ("period", ctypes.c_uint32),
        ("comp", ctypes.c_uint32 * 3),
        ("dt", ctypes.c_uint32 * 3),
        ("trig_comp", ctypes.c_uint32),
        ("trig_cfg", ctypes.c_uint32),
        ("ie", ctypes.c_uint32),
        ("iflags", ctypes.c_uint32),
        ("mask_en", ctypes.c_uint32),
        ("cnt", ctypes.c_uint32),
        ("counting_down", ctypes.c_bool),
        ("brake_level", ctypes.c_uint32),
        ("ch_level", ctypes.c_uint32),
        ("presc", ctypes.c_uint32),
    ]


class Rtc(ctypes.Structure):
    _fields_ = [
        ("cfg", ctypes.c_uint32),
        ("ie", ctypes.c_uint32),
        ("iflags", ctypes.c_uint32),
        ("pre", ctypes.c_uint32),
        ("tr", ctypes.c_uint32),
        ("dr", ctypes.c_uint32),
        ("ar", ctypes.c_uint32),
        ("sec", ctypes.c_uint8),
        ("min", ctypes.c_uint8),
        ("hour", ctypes.c_uint8),
        ("weekday", ctypes.c_uint8),
        ("date", ctypes.c_uint8),
        ("month", ctypes.c_uint8),
        ("year", ctypes.c_uint8),
        ("cnt", ctypes.c_uint32),
        ("presc", ctypes.c_uint32),
    ]


class Iwdt(ctypes.Structure):
    _fields_ = [
        ("load", ctypes.c_uint32),
        ("ctrl", ctypes.c_uint32),
        ("iflags", ctypes.c_uint32),
        ("cnt", ctypes.c_uint32),
        ("expired_once", ctypes.c_bool),
    ]


class Wwdt(ctypes.Structure):
    _fields_ = [
        ("load", ctypes.c_uint32),
        ("ctrl", ctypes.c_uint32),
        ("iflags", ctypes.c_uint32),
        ("value", ctypes.c_uint32),
        ("presc", ctypes.c_uint32),
    ]


class Aes(ctypes.Structure):
    _fields_ = [
        ("cr", ctypes.c_uint32),
        ("sr", ctypes.c_uint32),
        ("key", ctypes.c_uint32 * 4),
        ("iv", ctypes.c_uint32 * 4),
        ("din", ctypes.c_uint32 * 4),
        ("dout", ctypes.c_uint32 * 4),
        ("din_count", ctypes.c_uint),
        ("dout_count", ctypes.c_uint),
    ]


class Iic(ctypes.Structure):
    _fields_ = [
        ("ccfg", ctypes.c_uint32),
        ("ctrans", ctypes.c_uint32),
        ("ie", ctypes.c_uint32),
        ("iflags", ctypes.c_uint32),
        ("sctrl", ctypes.c_uint32),
        ("saddr", ctypes.c_uint32),
        ("rxdata", ctypes.c_uint8),
        ("txdata", ctypes.c_uint8),
        ("busy", ctypes.c_bool),
    ]


class Lcd(ctypes.Structure):
    _fields_ = [
        ("ram", (ctypes.c_uint8 * 132) * 8),
        ("page", ctypes.c_uint8),
        ("col", ctypes.c_uint8),
        ("start_line", ctypes.c_uint8),
        ("contrast", ctypes.c_uint8),
        ("pending_contrast", ctypes.c_bool),
        ("display_on", ctypes.c_bool),
        ("seg_reverse", ctypes.c_bool),
        ("com_reverse", ctypes.c_bool),
        ("inverse", ctypes.c_bool),
        ("all_on", ctypes.c_bool),
    ]


class Bk4819(ctypes.Structure):
    _fields_ = [
        ("regs", ctypes.c_uint16 * 128),
        ("selected", ctypes.c_bool),
        ("prev_scn", ctypes.c_bool),
        ("prev_scl", ctypes.c_bool),
        ("prev_sda_in", ctypes.c_bool),
        ("addr_phase", ctypes.c_bool),
        ("reading", ctypes.c_bool),
        ("bitpos", ctypes.c_uint),
        ("addr", ctypes.c_uint8),
        ("shift", ctypes.c_uint16),
        ("squelch_open", ctypes.c_bool),
        ("ctcss_tone_in", ctypes.c_uint32),
        ("cdcss_code_in", ctypes.c_uint32),
        ("cdcss_present_in", ctypes.c_bool),
        ("ctcss_found", ctypes.c_bool),
        ("cdcss_found", ctypes.c_bool),
        ("cdcss_low", ctypes.c_uint16),
        ("cdcss_high", ctypes.c_uint16),
        ("vox_active", ctypes.c_bool),
        ("fsk_rx", ctypes.c_uint16 * 8),
        ("fsk_rx_head", ctypes.c_uint),
        ("fsk_rx_count", ctypes.c_uint),
        ("tone_hz", ctypes.c_uint),
    ]


class Hooks(ctypes.Structure):
    _fields_ = [
        ("user", ctypes.c_void_p),
        ("uart_tx", UART_TX_CB),
        ("gpio_change", GPIO_CB),
        ("spi_xfer", SPI_CB),
        ("i2c_start", I2C_START_CB),
        ("i2c_write", I2C_WRITE_CB),
        ("i2c_read", I2C_READ_CB),
        ("i2c_stop", I2C_STOP_CB),
        ("bk4819_freq_change", BK4819_FREQ_CB),
        ("bk4819_tone_change", BK4819_TONE_CB),
        ("bk4819_gpio_change", BK4819_GPIO_CB),
        ("lcd_update", LCD_UPDATE_CB),
    ]


NREGIONS = 30


class Periph(ctypes.Structure):
    _fields_ = [
        ("regfile", (ctypes.c_uint32 * 512) * NREGIONS),
        ("gpio", Gpio * 3),
        ("uart", Uart * 3),
        ("spi", Spi * 2),
        ("adc", Adc),
        ("crc", Crc),
        ("timer_base", TimerBase * 3),
        ("timer_plus", TimerPlus * 2),
        ("pwm_base", PwmBase * 2),
        ("pwm_plus", PwmPlus * 2),
        ("rtc", Rtc),
        ("iwdt", Iwdt),
        ("wwdt", Wwdt),
        ("aes", Aes),
        ("iic", Iic * 2),
        ("bk4819", Bk4819),
        ("lcd", Lcd),
        ("hooks", Hooks),
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
        ("flash", ctypes.c_uint8 * FLASH_MAX),
        ("sram", ctypes.c_uint8 * SRAM_SIZE),
        ("cycles", ctypes.c_uint64),
        ("msp", ctypes.c_uint32),
        ("psp", ctypes.c_uint32),
        ("using_psp", ctypes.c_bool),
        ("handler", ctypes.c_bool),
        ("locked_up", ctypes.c_bool),
        ("event", ctypes.c_bool),
        ("reset_req", ctypes.c_bool),
        ("soc", ctypes.c_void_p),
        ("scs", Scs),
        ("periph", Periph),
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
    lib.dp32g030_soc_by_name.argtypes = [ctypes.c_char_p]
    lib.dp32g030_soc_by_name.restype = ctypes.c_void_p
    lib.dp32g030_soc_detect.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
    lib.dp32g030_soc_detect.restype = ctypes.c_void_p
    lib.dp32g030_soc_name.argtypes = [ctypes.c_void_p]
    lib.dp32g030_soc_name.restype = ctypes.c_char_p
    lib.dp32g030_soc_defaults.argtypes = [ctypes.POINTER(Cpu)]
    lib.dp32g030_cpu_new_soc.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
    ]
    lib.dp32g030_cpu_new_soc.restype = ctypes.POINTER(Cpu)
    lib.dp32g030_extflash_set.argtypes = [
        ctypes.POINTER(Cpu), ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32,
    ]
    lib.dp32g030_extflash_get.argtypes = [
        ctypes.POINTER(Cpu), ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32,
    ]
    lib.dp32g030_soc_tick.argtypes = [ctypes.POINTER(Cpu), ctypes.c_uint64]
    lib.dp32g030_soc_next_event.argtypes = [ctypes.POINTER(Cpu)]
    lib.dp32g030_soc_next_event.restype = ctypes.c_uint64
    lib.dp32g030_has_backlight.argtypes = [ctypes.POINTER(Cpu)]
    lib.dp32g030_has_backlight.restype = ctypes.c_bool
    lib.dp32g030_backlight_level.argtypes = [ctypes.POINTER(Cpu)]
    lib.dp32g030_backlight_level.restype = ctypes.c_uint8
    lib.dp32g030_cpu_free.argtypes = [ctypes.POINTER(Cpu)]
    lib.dp32g030_cpu_free.restype = None
    lib.dp32g030_gpio_set_input.argtypes = [
        ctypes.POINTER(Cpu), ctypes.c_uint, ctypes.c_uint32, ctypes.c_uint32,
    ]
    lib.dp32g030_gpio_dir.argtypes = [ctypes.POINTER(Cpu), ctypes.c_uint]
    lib.dp32g030_gpio_dir.restype = ctypes.c_uint32
    lib.dp32g030_gpio_pins.argtypes = [ctypes.POINTER(Cpu), ctypes.c_uint]
    lib.dp32g030_gpio_pins.restype = ctypes.c_uint32
    lib.dp32g030_uart_rx.argtypes = [ctypes.POINTER(Cpu), ctypes.c_uint, ctypes.c_uint8]
    lib.py32_usart_rx.argtypes = [ctypes.POINTER(Cpu), ctypes.c_uint, ctypes.c_uint8]
    lib.dp32g030_adc_set_input.argtypes = [ctypes.POINTER(Cpu), ctypes.c_uint, ctypes.c_uint32]
    lib.dp32g030_cpu_reset.argtypes = [ctypes.POINTER(Cpu)]
    lib.dp32g030_timer_plus_set_ext.argtypes = [
        ctypes.POINTER(Cpu), ctypes.c_uint, ctypes.c_uint, ctypes.c_bool,
    ]
    lib.dp32g030_timer_plus_set_hall.argtypes = [
        ctypes.POINTER(Cpu), ctypes.c_uint, ctypes.c_uint32,
    ]
    lib.dp32g030_pwm_plus_set_brake.argtypes = [
        ctypes.POINTER(Cpu), ctypes.c_uint, ctypes.c_uint32,
    ]
    lib.dp32g030_bk4819_set_signal.argtypes = [
        ctypes.POINTER(Cpu), ctypes.c_uint16, ctypes.c_uint8, ctypes.c_uint8,
    ]
    lib.dp32g030_bk4819_frequency.argtypes = [ctypes.POINTER(Cpu)]
    lib.dp32g030_bk4819_frequency.restype = ctypes.c_uint32
    lib.dp32g030_bk4819_tone_hz.argtypes = [ctypes.POINTER(Cpu)]
    lib.dp32g030_bk4819_tone_hz.restype = ctypes.c_uint
    lib.dp32g030_bk4819_squelch_open.argtypes = [ctypes.POINTER(Cpu)]
    lib.dp32g030_bk4819_squelch_open.restype = ctypes.c_bool
    lib.dp32g030_bk4819_set_ctcss_tone.argtypes = [ctypes.POINTER(Cpu), ctypes.c_uint32]
    lib.dp32g030_bk4819_set_cdcss_code.argtypes = [
        ctypes.POINTER(Cpu), ctypes.c_uint32, ctypes.c_bool,
    ]
    lib.dp32g030_bk4819_signal_dtmf.argtypes = [ctypes.POINTER(Cpu), ctypes.c_char]
    lib.dp32g030_bk4819_signal_dtmf.restype = ctypes.c_bool
    lib.dp32g030_bk4819_set_mic_level.argtypes = [ctypes.POINTER(Cpu), ctypes.c_uint16]
    lib.dp32g030_bk4819_fsk_rx_push.argtypes = [ctypes.POINTER(Cpu), ctypes.c_uint16]
    lib.dp32g030_bk4819_fsk_rx_push.restype = ctypes.c_bool
    lib.dp32g030_bk4819_fsk_rx_finish.argtypes = [ctypes.POINTER(Cpu)]
    lib.dp32g030_lcd_get_pixel.argtypes = [ctypes.POINTER(Cpu), ctypes.c_uint, ctypes.c_uint]
    lib.dp32g030_lcd_get_pixel.restype = ctypes.c_bool
    lib.dp32g030_lcd_ram.argtypes = [ctypes.POINTER(Cpu), ctypes.c_uint, ctypes.c_uint]
    lib.dp32g030_lcd_ram.restype = ctypes.c_uint8
    lib.dp32g030_lcd_is_on.argtypes = [ctypes.POINTER(Cpu)]
    lib.dp32g030_lcd_is_on.restype = ctypes.c_bool
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
    cycles: int = 0
    msp: int = 0
    psp: int = 0
    using_psp: bool = False
    handler: bool = False
    locked_up: bool = False
    pending: int = 0
    active: int = 0
    extra: object = None

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
    return run_image(lib, firmware(hws), regs, flags, sram, steps)


def run_image(lib, data, regs=None, flags=0, sram=b"", steps=1000, setup=None, inspect=None):
    ptr = lib.dp32g030_cpu_new(to_buf(data), len(data))
    assert ptr, "dp32g030_cpu_new returned NULL"
    try:
        cpu = ptr.contents
        for idx, val in (regs or {}).items():
            cpu.r[idx] = val
        cpu.xpsr |= flags
        if sram:
            ctypes.memmove(cpu.sram, sram, len(sram))
        if setup:
            setup(ptr)
        status = lib.dp32g030_cpu_run(ctypes.byref(ptr), steps)
        extra = inspect(ptr) if inspect else None
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
            cycles=cpu.cycles,
            msp=cpu.msp,
            psp=cpu.psp,
            using_psp=bool(cpu.using_psp),
            handler=bool(cpu.handler),
            locked_up=bool(cpu.locked_up),
            pending=cpu.scs.pending,
            active=cpu.scs.active,
            extra=extra,
        )
    finally:
        lib.dp32g030_cpu_free(ptr)


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v
