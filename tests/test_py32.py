import ctypes
import struct

import pytest

import dp32g030 as emu

FLASH_BASE = 0x08000000
IMAGE_BASE = 0x08002800
ALIAS_BASE = 0x00000000
FLASH_SIZE_PY32 = 128 * 1024
SRAM_BASE = 0x20000000
SRAM_TOP = SRAM_BASE + 16 * 1024

NOP = 0xBF00
BKPT = 0xBE00


def app_image(code_halfwords=(BKPT,), handlers=None, size=None):
    """Raw app image: vector table (16 words) at offset 0, code at 0x40."""
    handlers = handlers or {}
    code_addr = IMAGE_BASE + 0x40
    table = [SRAM_TOP, code_addr | 1] + [code_addr | 1] * 14
    for exc, addr in handlers.items():
        table[exc] = addr | 1
    data = struct.pack("<16I", *table)
    data += struct.pack(f"<{len(code_halfwords)}H", *code_halfwords)
    if size:
        data += bytes(size - len(data))
    return data


@pytest.fixture
def py32(lib):
    soc = lib.dp32g030_soc_by_name(b"py32f071")
    assert soc, "py32f071 SoC is registered"
    made = []

    def _new(data):
        cpu = lib.dp32g030_cpu_new_soc(soc, emu.to_buf(data), len(data))
        assert cpu
        made.append(cpu)
        return cpu

    yield _new
    for cpu in made:
        lib.dp32g030_cpu_free(cpu)


def rd(lib, cpu, addr, size=4):
    out = ctypes.c_uint32(0xDEADBEEF)
    ok = lib.dp32g030_bus_read(cpu, addr, size, ctypes.byref(out))
    return ok, out.value


def wr(lib, cpu, addr, val, size=4):
    return lib.dp32g030_bus_write(cpu, addr, size, val)


def test_py32_memory_map(lib, py32):
    cpu = py32(app_image())
    assert rd(lib, cpu, IMAGE_BASE) == (True, SRAM_TOP)
    # flash is also visible at 0 (boot remap)
    assert rd(lib, cpu, ALIAS_BASE + 0x2800) == (True, SRAM_TOP)
    assert rd(lib, cpu, FLASH_BASE + FLASH_SIZE_PY32 - 4) == (True, 0xFFFFFFFF)
    assert not rd(lib, cpu, FLASH_BASE + FLASH_SIZE_PY32)[0]
    assert not wr(lib, cpu, IMAGE_BASE, 0)  # flash is read-only to the bus
    assert wr(lib, cpu, SRAM_BASE + 0x100, 0xCAFEBABE)
    assert rd(lib, cpu, SRAM_BASE + 0x100) == (True, 0xCAFEBABE)
    assert not rd(lib, cpu, SRAM_TOP)[0]


def test_py32_image_rejected_when_too_big(lib):
    soc = lib.dp32g030_soc_by_name(b"py32f071")
    room = FLASH_SIZE_PY32 - (IMAGE_BASE - FLASH_BASE)
    ok = app_image(size=room)
    assert lib.dp32g030_cpu_new_soc(soc, emu.to_buf(ok), len(ok))
    big = app_image(size=room + 1)
    assert not lib.dp32g030_cpu_new_soc(soc, emu.to_buf(big), len(big))


def test_py32_boot_state(lib, py32):
    cpu = py32(app_image())
    c = cpu.contents
    assert c.r[13] == SRAM_TOP
    assert c.r[15] == IMAGE_BASE + 0x40
    assert rd(lib, cpu, 0xE000ED08) == (True, IMAGE_BASE)  # VTOR
    assert rd(lib, cpu, 0xE000ED00)[1] == 0x410CC601  # Cortex-M0+


def test_vtor_vectoring(lib, py32):
    handler = IMAGE_BASE + 0x80
    data = app_image([NOP] * 8, handlers={15: handler}, size=0x100)
    cpu = py32(data)
    assert wr(lib, cpu, 0xE000ED04, 1 << 26)  # ICSR.PENDSTSET
    pp = ctypes.pointer(cpu)
    lib.dp32g030_cpu_run(pp, 1)
    c = cpu.contents
    assert c.handler
    assert c.xpsr & 0x3F == 15
    assert handler <= c.r[15] <= handler + 4


def test_dp32g030_vtor_ignored(lib):
    soc = lib.dp32g030_soc_by_name(b"dp32g030")
    data = emu.firmware([BKPT])
    cpu = lib.dp32g030_cpu_new_soc(soc, emu.to_buf(data), len(data))
    try:
        assert wr(lib, cpu, 0xE000ED08, 0x20000000)
        assert rd(lib, cpu, 0xE000ED08) == (True, 0)
        assert rd(lib, cpu, 0xE000ED00)[1] == 0x410CC200  # Cortex-M0
    finally:
        lib.dp32g030_cpu_free(cpu)


@pytest.mark.parametrize(
    "name,base",
    [
        ("GPIOA", 0x50000000),
        ("GPIOB", 0x50000400),
        ("GPIOC", 0x50000800),
        ("GPIOF", 0x50001400),
        ("SPI1", 0x40013000),
        ("USART1", 0x40013800),
        ("I2C1", 0x40005400),
        ("ADC1", 0x40012400),
        ("TIM1", 0x40012C00),
        ("DMA1", 0x40020000),
        ("CRC", 0x40023000),
        ("PWR", 0x40007000),
        ("SYSCFG", 0x40010000),
    ],
)
def test_py32_region_table(lib, py32, name, base):
    cpu = py32(app_image())
    assert wr(lib, cpu, base + 0x38, 0x12345678), name
    assert rd(lib, cpu, base + 0x38) == (True, 0x12345678), name
    # byte and halfword accesses go through the same register file
    assert wr(lib, cpu, base + 0x38, 0xAB, 1)
    assert rd(lib, cpu, base + 0x38) == (True, 0x123456AB), name


def test_py32_unmapped_faults(lib, py32):
    cpu = py32(app_image())
    assert not rd(lib, cpu, 0x60000000)[0]
    assert not rd(lib, cpu, 0x50000C00)[0]  # no GPIOD on this chip
    assert not wr(lib, cpu, 0x40040000, 1)


def test_py32_rcc_boot_state(lib, py32):
    cpu = py32(app_image())
    cr = rd(lib, cpu, 0x40021000)[1]
    assert cr & (1 << 10)  # HSIRDY
    assert cr & (1 << 25)  # PLLRDY: the bootloader left the PLL running


# ---- GPIO ---------------------------------------------------------------

GPIOA = 0x50000000
GPIOF = 0x50001400
MODER, OTYPER, PUPDR, IDR, ODR, BSRR, BRR = 0x00, 0x04, 0x0C, 0x10, 0x14, 0x18, 0x28
PORT_A, PORT_F = 0, 3  # port index used by gpio_set_input/gpio_pins


def mode(pin, m):
    return m << (2 * pin)


@pytest.fixture
def pins(lib, py32):
    cpu = py32(app_image())

    class Env:
        pass

    e = Env()
    e.cpu = cpu
    e.pins = lambda port=PORT_A: lib.dp32g030_gpio_pins(cpu, port)
    e.reg = lambda off, base=GPIOA: rd(lib, cpu, base + off)[1]
    e.put = lambda off, val, base=GPIOA: wr(lib, cpu, base + off, val)
    return e


def test_py32_gpio_odr_bsrr(pins):
    pins.put(MODER, mode(3, 1))  # PA3 output
    pins.put(BSRR, 1 << 3)
    assert pins.reg(ODR) & (1 << 3)
    assert pins.reg(IDR) & (1 << 3)
    assert pins.pins() & (1 << 3)
    pins.put(BSRR, 1 << 19)
    assert not pins.reg(ODR) & (1 << 3)
    assert not pins.reg(IDR) & (1 << 3)


def test_py32_bsrr_reset_wins(pins):
    pins.put(MODER, mode(3, 1))
    pins.put(BSRR, (1 << 3) | (1 << 19))
    assert not pins.reg(ODR) & (1 << 3)


def test_py32_brr(pins):
    pins.put(MODER, mode(2, 1))
    pins.put(BSRR, 1 << 2)
    pins.put(BRR, 1 << 2)
    assert not pins.reg(ODR) & (1 << 2)


def test_py32_gpio_input_pulls_and_host_drive(lib, pins):
    pins.put(MODER, 0)  # all inputs
    assert not pins.reg(IDR) & (1 << 5)  # floating reads 0
    pins.put(PUPDR, mode(5, 1))  # pull-up
    assert pins.reg(IDR) & (1 << 5)
    pins.put(PUPDR, mode(5, 2))  # pull-down
    assert not pins.reg(IDR) & (1 << 5)
    lib.dp32g030_gpio_set_input(pins.cpu, PORT_A, 1 << 5, 1 << 5)
    assert pins.reg(IDR) & (1 << 5)  # the outside world wins over the pull
    lib.dp32g030_gpio_set_input(pins.cpu, PORT_A, 1 << 5, 0)
    assert not pins.reg(IDR) & (1 << 5)


def test_py32_gpio_open_drain_reads_high_when_released(pins):
    pins.put(MODER, mode(4, 1))
    pins.put(OTYPER, 1 << 4)
    pins.put(BSRR, 1 << 4)  # released: external pull-up
    assert pins.reg(IDR) & (1 << 4)
    pins.put(BSRR, 1 << 20)  # driven low
    assert not pins.reg(IDR) & (1 << 4)


def test_py32_gpio_analog_reads_zero(lib, pins):
    # reset state: pins are analog; digital input is disconnected
    lib.dp32g030_gpio_set_input(pins.cpu, PORT_A, 1 << 6, 1 << 6)
    assert not pins.reg(IDR) & (1 << 6)


def test_py32_gpio_change_hook(lib, pins):
    seen = []
    pins.cpu.contents.periph.hooks.gpio_change = emu.GPIO_CB(
        lambda user, port, p, d: seen.append((port, p, d))
    )
    pins.put(MODER, mode(3, 1))
    pins.put(BSRR, 1 << 3)
    port, level, direction = seen[-1]
    assert port == PORT_A
    assert level & (1 << 3)
    assert direction & (1 << 3)


def test_py32_gpio_port_f(pins):
    pins.put(MODER, mode(8, 1), GPIOF)
    pins.put(BSRR, 1 << 8, GPIOF)
    assert pins.pins(PORT_F) & (1 << 8)
    assert not pins.pins(PORT_A) & (1 << 8)
