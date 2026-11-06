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


# ---- SPI + LCD tap ------------------------------------------------------

SPI1 = 0x40013000
SPI_CR1, SPI_CR2, SPI_SR, SPI_DR = 0x00, 0x04, 0x08, 0x0C
CR1_MSTR, CR1_SPE = 1 << 2, 1 << 6
SR_RXNE, SR_TXE = 1 << 0, 1 << 1


def lcd_setup(lib, cpu):
    """SPI1 enabled as master, PA6 (A0) output, display driven through DR."""
    wr(lib, cpu, GPIOA + MODER, mode(6, 1))
    wr(lib, cpu, SPI1 + SPI_CR1, CR1_MSTR | CR1_SPE)

    def send(byte, data):
        wr(lib, cpu, GPIOA + BSRR, (1 << 6) if data else (1 << 22))
        wr(lib, cpu, SPI1 + SPI_DR, byte, 1)

    return send


def test_py32_spi_lcd_command_and_data(lib, py32):
    cpu = py32(app_image())
    send = lcd_setup(lib, cpu)
    send(0xAF, data=False)  # display on
    assert lib.dp32g030_lcd_is_on(cpu)
    send(0xB1, data=False)  # page 1
    send(0x00, data=False)  # column low nibble 0
    send(0x10, data=False)  # column high nibble 0
    send(0xFF, data=True)
    assert lib.dp32g030_lcd_ram(cpu, 1, 0) == 0xFF
    assert lib.dp32g030_lcd_ram(cpu, 0, 0) == 0


def test_py32_spi_disabled_does_not_clock(lib, py32):
    cpu = py32(app_image())
    send = lcd_setup(lib, cpu)
    wr(lib, cpu, SPI1 + SPI_CR1, CR1_MSTR)  # SPE off
    send(0xAF, data=False)
    assert not lib.dp32g030_lcd_is_on(cpu)


def test_py32_spi_hook_and_status(lib, py32):
    cpu = py32(app_image())
    seen = []

    def xfer(user, spi, tx):
        seen.append((spi, tx))
        return 0x5A

    cb = emu.SPI_CB(xfer)
    cpu.contents.periph.hooks.spi_xfer = cb
    wr(lib, cpu, SPI1 + SPI_CR1, CR1_MSTR | CR1_SPE)
    assert rd(lib, cpu, SPI1 + SPI_SR)[1] & SR_TXE
    assert not rd(lib, cpu, SPI1 + SPI_SR)[1] & SR_RXNE
    wr(lib, cpu, SPI1 + SPI_DR, 0x12, 1)
    assert seen == [(1, 0x12)]
    sr = rd(lib, cpu, SPI1 + SPI_SR)[1]
    assert sr & SR_TXE and sr & SR_RXNE
    assert rd(lib, cpu, SPI1 + SPI_DR)[1] == 0x5A
    assert not rd(lib, cpu, SPI1 + SPI_SR)[1] & SR_RXNE  # reading DR clears it


# ---- ADC (STM32F1-style) --------------------------------------------------

ADC1 = 0x40012400
ADC_SR, ADC_CR1, ADC_CR2, ADC_SQR3, ADC_DR = 0x00, 0x04, 0x08, 0x38, 0x50
CR2_ADON, CR2_CONT, CR2_CAL, CR2_SWSTART = 1 << 0, 1 << 1, 1 << 2, 1 << 22
SR_EOC = 1 << 1


def test_py32_adc_calibration_self_clears(lib, py32):
    cpu = py32(app_image())
    wr(lib, cpu, ADC1 + ADC_CR2, CR2_ADON | CR2_CAL)
    cr2 = rd(lib, cpu, ADC1 + ADC_CR2)[1]
    assert cr2 & CR2_ADON
    assert not cr2 & CR2_CAL  # the firmware waits for this to clear


def test_py32_adc_single_conversion(lib, py32):
    cpu = py32(app_image())
    lib.dp32g030_adc_set_input(cpu, 5, 1234)
    wr(lib, cpu, ADC1 + ADC_SQR3, 5)  # first conversion: channel 5
    wr(lib, cpu, ADC1 + ADC_CR2, CR2_ADON)
    assert not rd(lib, cpu, ADC1 + ADC_SR)[1] & SR_EOC
    wr(lib, cpu, ADC1 + ADC_CR2, CR2_ADON | CR2_SWSTART)
    assert rd(lib, cpu, ADC1 + ADC_SR)[1] & SR_EOC
    assert not rd(lib, cpu, ADC1 + ADC_CR2)[1] & CR2_SWSTART  # self-clears
    assert rd(lib, cpu, ADC1 + ADC_DR)[1] == 1234
    assert not rd(lib, cpu, ADC1 + ADC_SR)[1] & SR_EOC  # reading DR clears EOC


def test_py32_adc_needs_adon(lib, py32):
    cpu = py32(app_image())
    lib.dp32g030_adc_set_input(cpu, 0, 99)
    wr(lib, cpu, ADC1 + ADC_CR2, CR2_SWSTART)  # ADON off
    assert not rd(lib, cpu, ADC1 + ADC_SR)[1] & SR_EOC


def test_py32_adc_continuous_reconverts_after_dr_read(lib, py32):
    cpu = py32(app_image())
    lib.dp32g030_adc_set_input(cpu, 2, 10)
    wr(lib, cpu, ADC1 + ADC_SQR3, 2)
    wr(lib, cpu, ADC1 + ADC_CR2, CR2_ADON | CR2_CONT | CR2_SWSTART)
    assert rd(lib, cpu, ADC1 + ADC_DR)[1] == 10
    lib.dp32g030_adc_set_input(cpu, 2, 20)
    assert rd(lib, cpu, ADC1 + ADC_SR)[1] & SR_EOC  # converted again
    assert rd(lib, cpu, ADC1 + ADC_DR)[1] == 20


# ---- SPI interrupts -------------------------------------------------------

SPI2 = 0x40003800
CR2_RXNEIE, CR2_TXEIE = 1 << 6, 1 << 7
IRQ_SPI1, IRQ_SPI2 = 25, 26


def irq_line(cpu, n):
    return bool(cpu.contents.scs.irq_lines & (1 << n))


def test_py32_spi_txe_interrupt(lib, py32):
    cpu = py32(app_image())
    wr(lib, cpu, SPI2 + SPI_CR1, CR1_MSTR | CR1_SPE)
    assert not irq_line(cpu, IRQ_SPI2)
    wr(lib, cpu, SPI2 + SPI_CR2, CR2_TXEIE)
    assert irq_line(cpu, IRQ_SPI2)  # TXE is always set
    assert not irq_line(cpu, IRQ_SPI1)
    wr(lib, cpu, SPI2 + SPI_CR2, 0)
    assert not irq_line(cpu, IRQ_SPI2)


def test_py32_spi_rxne_interrupt(lib, py32):
    cpu = py32(app_image())
    wr(lib, cpu, SPI1 + SPI_CR1, CR1_MSTR | CR1_SPE)
    wr(lib, cpu, SPI1 + SPI_CR2, CR2_RXNEIE)
    assert not irq_line(cpu, IRQ_SPI1)
    wr(lib, cpu, SPI1 + SPI_DR, 0x01, 1)
    assert irq_line(cpu, IRQ_SPI1)
    rd(lib, cpu, SPI1 + SPI_DR)  # reading DR clears RXNE
    assert not irq_line(cpu, IRQ_SPI1)


# ---- DMA ----------------------------------------------------------------

DMA1 = 0x40020000
DMA_ISR, DMA_IFCR = 0x00, 0x04
CCR_EN, CCR_TCIE, CCR_DIR, CCR_PINC, CCR_MINC = 1 << 0, 1 << 1, 1 << 4, 1 << 6, 1 << 7
CCR_MEM2MEM = 1 << 14


def ch(n, reg):  # channel n (1..7); reg: 0 CCR, 4 CNDTR, 8 CPAR, 12 CMAR
    return DMA1 + 0x08 + 0x14 * (n - 1) + reg


def tcif(n):
    return 1 << (4 * (n - 1) + 1)


def dma_setup(lib, cpu, n, ccr, count, par, mar):
    wr(lib, cpu, ch(n, 4), count)
    wr(lib, cpu, ch(n, 8), par)
    wr(lib, cpu, ch(n, 12), mar)
    wr(lib, cpu, ch(n, 0), ccr)


def test_py32_dma_mem2mem(lib, py32):
    cpu = py32(app_image())
    src, dst = SRAM_BASE + 0x100, SRAM_BASE + 0x200
    for i, b in enumerate(b"\x11\x22\x33\x44"):
        wr(lib, cpu, src + i, b, 1)
    dma_setup(lib, cpu, 1, CCR_EN | CCR_MEM2MEM | CCR_PINC | CCR_MINC, 4, src, dst)
    assert rd(lib, cpu, dst) == (True, 0x44332211)
    assert rd(lib, cpu, ch(1, 4))[1] == 0  # CNDTR counted down
    assert rd(lib, cpu, DMA1 + DMA_ISR)[1] & tcif(1)
    assert not rd(lib, cpu, ch(1, 0))[1] & CCR_EN  # EN clears on completion


def test_py32_dma_16bit_words(lib, py32):
    cpu = py32(app_image())
    src, dst = SRAM_BASE + 0x100, SRAM_BASE + 0x200
    wr(lib, cpu, src, 0xAABBCCDD)
    wr(lib, cpu, src + 4, 0x11223344)
    size16 = (1 << 8) | (1 << 10)  # PSIZE = MSIZE = 16 bit
    dma_setup(lib, cpu, 2, CCR_EN | CCR_MEM2MEM | CCR_PINC | CCR_MINC | size16, 4, src, dst)
    assert rd(lib, cpu, dst) == (True, 0xAABBCCDD)
    assert rd(lib, cpu, dst + 4) == (True, 0x11223344)


def test_py32_dma_tc_interrupt_and_flag_clear(lib, py32):
    cpu = py32(app_image())
    src, dst = SRAM_BASE + 0x100, SRAM_BASE + 0x200
    dma_setup(lib, cpu, 1, CCR_EN | CCR_TCIE | CCR_MEM2MEM | CCR_PINC | CCR_MINC, 2, src, dst)
    assert irq_line(cpu, 9)  # channel 1
    wr(lib, cpu, DMA1 + DMA_IFCR, 1 << 1)  # CTCIF1
    assert not irq_line(cpu, 9)
    assert not rd(lib, cpu, DMA1 + DMA_ISR)[1] & tcif(1)


@pytest.mark.parametrize("n,irq", [(2, 10), (3, 10), (4, 11), (7, 11)])
def test_py32_dma_irq_grouping(lib, py32, n, irq):
    cpu = py32(app_image())
    dma_setup(lib, cpu, n, CCR_EN | CCR_TCIE | CCR_MEM2MEM | CCR_PINC | CCR_MINC, 1,
              SRAM_BASE + 0x100, SRAM_BASE + 0x200)
    assert irq_line(cpu, irq)


def test_py32_dma_spi_full_duplex(lib, py32):
    cpu = py32(app_image())
    seen = []

    def xfer(user, spi, tx):
        seen.append((spi, tx))
        return (tx + 1) & 0xFF

    cb = emu.SPI_CB(xfer)
    cpu.contents.periph.hooks.spi_xfer = cb
    wr(lib, cpu, SPI2 + SPI_CR1, CR1_MSTR | CR1_SPE)
    txbuf, rxbuf = SRAM_BASE + 0x100, SRAM_BASE + 0x200
    for i, b in enumerate((0x10, 0x20, 0x30)):
        wr(lib, cpu, txbuf + i, b, 1)
    # RX channel first (as a driver does), then TX which clocks the bus
    dma_setup(lib, cpu, 2, CCR_EN | CCR_MINC, 3, SPI2 + SPI_DR, rxbuf)
    assert seen == []  # nothing moves until TX starts
    dma_setup(lib, cpu, 3, CCR_EN | CCR_DIR | CCR_MINC, 3, SPI2 + SPI_DR, txbuf)
    assert seen == [(2, 0x10), (2, 0x20), (2, 0x30)]
    assert [rd(lib, cpu, rxbuf + i, 1)[1] for i in range(3)] == [0x11, 0x21, 0x31]
    isr = rd(lib, cpu, DMA1 + DMA_ISR)[1]
    assert isr & tcif(2) and isr & tcif(3)


# ---- BK4819 on the PY32 board (CSN = PF9, SCL = PB8, SDA = PB9) ----------

GPIOB_BASE = 0x50000400
BK_CSN, BK_SCL, BK_SDA = (GPIOF, 1 << 9), (GPIOB_BASE, 1 << 8), (GPIOB_BASE, 1 << 9)


class BkLink:
    """The firmware's bit-bang driver, over the PY32 GPIO registers."""

    def __init__(self, lib, cpu):
        self.lib, self.cpu = lib, cpu
        for (base, bit) in (BK_CSN, BK_SCL, BK_SDA):
            pin = bit.bit_length() - 1
            moder = rd(lib, cpu, base + MODER)[1]
            wr(lib, cpu, base + MODER, (moder & ~(3 << (2 * pin))) | mode(pin, 1))
        self._set(BK_CSN)
        self._set(BK_SCL)
        self._set(BK_SDA)

    def _set(self, pin):
        wr(self.lib, self.cpu, pin[0] + BSRR, pin[1])

    def _clear(self, pin):
        wr(self.lib, self.cpu, pin[0] + BSRR, pin[1] << 16)

    def _sda_input(self, as_input):
        base, bit = BK_SDA
        pin = bit.bit_length() - 1
        moder = rd(self.lib, self.cpu, base + MODER)[1] & ~(3 << (2 * pin))
        wr(self.lib, self.cpu, base + MODER, moder | (0 if as_input else mode(pin, 1)))

    def _write_bits(self, value, nbits):
        self._clear(BK_SCL)
        for i in range(nbits):
            (self._set if (value >> (nbits - 1 - i)) & 1 else self._clear)(BK_SDA)
            self._set(BK_SCL)
            self._clear(BK_SCL)

    def _read_bits(self, nbits):
        self._sda_input(True)
        value = 0
        for _ in range(nbits):
            bit = rd(self.lib, self.cpu, BK_SDA[0] + IDR)[1] & BK_SDA[1]
            value = (value << 1) | (1 if bit else 0)
            self._set(BK_SCL)
            self._clear(BK_SCL)
        self._sda_input(False)
        return value

    def write_register(self, addr, data):
        self._set(BK_CSN)
        self._clear(BK_SCL)
        self._clear(BK_CSN)
        self._write_bits(addr & 0x7F, 8)
        self._write_bits(data, 16)
        self._set(BK_CSN)
        self._set(BK_SCL)

    def read_register(self, addr):
        self._set(BK_CSN)
        self._clear(BK_SCL)
        self._clear(BK_CSN)
        self._write_bits((addr & 0x7F) | 0x80, 8)
        value = self._read_bits(16)
        self._set(BK_CSN)
        self._set(BK_SCL)
        return value


def test_py32_bk4819_register_roundtrip(lib, py32):
    cpu = py32(app_image())
    link = BkLink(lib, cpu)
    link.write_register(0x30, 0xBEEF)
    assert link.read_register(0x30) == 0xBEEF
    link.write_register(0x30, 0x1234)
    assert link.read_register(0x30) == 0x1234


def test_py32_bk4819_idle_register_0c_bit0_clear(lib, py32):
    # BK4819_Init loops while REG_0C bit 0 is set: the chip must answer
    cpu = py32(app_image())
    link = BkLink(lib, cpu)
    assert link.read_register(0x0C) & 1 == 0


# ---- External SPI NOR flash (PY25Q16: SPI2, CS on PA3, 2 MiB) ------------

CS_PIN = 1 << 3  # PA3, active low


class FlashLink:
    """Drives the flash through SPI2 byte exchanges and the CS pin."""

    def __init__(self, lib, cpu):
        self.lib, self.cpu = lib, cpu
        wr(lib, cpu, GPIOA + MODER, mode(3, 1))
        wr(lib, cpu, SPI2 + SPI_CR1, CR1_MSTR | CR1_SPE)
        self.cs(False)

    def cs(self, selected):
        wr(self.lib, self.cpu, GPIOA + BSRR, (CS_PIN << 16) if selected else CS_PIN)

    def xfer(self, byte):
        wr(self.lib, self.cpu, SPI2 + SPI_DR, byte, 1)
        return rd(self.lib, self.cpu, SPI2 + SPI_DR, 1)[1]

    def command(self, cmd, addr=None, data=(), read=0):
        self.cs(True)
        self.xfer(cmd)
        if addr is not None:
            for shift in (16, 8, 0):
                self.xfer((addr >> shift) & 0xFF)
        for b in data:
            self.xfer(b)
        out = [self.xfer(0xFF) for _ in range(read)]
        self.cs(False)
        return out

    def status(self):
        return self.command(0x05, read=1)[0]

    def write_enable(self):
        self.command(0x06)

    def read(self, addr, n):
        return self.command(0x03, addr, read=n)

    def program(self, addr, data):
        self.write_enable()
        self.command(0x02, addr, data)

    def erase_sector(self, addr):
        self.write_enable()
        self.command(0x20, addr)


def test_py32_flash_erased_reads_ff(lib, py32):
    cpu = py32(app_image())
    f = FlashLink(lib, cpu)
    assert f.read(0x010000, 4) == [0xFF] * 4
    assert f.read(0x1FFFFC, 4) == [0xFF] * 4  # last bytes of the 2 MiB part


def test_py32_flash_program_and_read(lib, py32):
    cpu = py32(app_image())
    f = FlashLink(lib, cpu)
    f.program(0x00A000, [0x12, 0x34, 0x56])
    assert f.read(0x00A000, 3) == [0x12, 0x34, 0x56]
    assert f.read(0x009FFF, 2) == [0xFF, 0x12]  # neighbours untouched


def test_py32_flash_program_needs_write_enable(lib, py32):
    cpu = py32(app_image())
    f = FlashLink(lib, cpu)
    f.command(0x02, 0x1000, [0x00])  # no WEL
    assert f.read(0x1000, 1) == [0xFF]
    assert f.status() & 0x02 == 0  # WEL clear


def test_py32_flash_status_wel_and_wip(lib, py32):
    cpu = py32(app_image())
    f = FlashLink(lib, cpu)
    assert f.status() == 0
    f.write_enable()
    assert f.status() == 0x02  # WEL set, WIP never busy
    f.command(0x04)  # write disable
    assert f.status() == 0
    f.write_enable()
    f.command(0x02, 0x2000, [0x55])
    assert f.status() == 0  # WEL clears after a program


def test_py32_flash_program_only_clears_bits(lib, py32):
    cpu = py32(app_image())
    f = FlashLink(lib, cpu)
    f.program(0x3000, [0xF0])
    f.program(0x3000, [0x3C])
    assert f.read(0x3000, 1) == [0x30]  # 0xF0 & 0x3C


def test_py32_flash_sector_erase(lib, py32):
    cpu = py32(app_image())
    f = FlashLink(lib, cpu)
    f.program(0x5000, [0x00, 0x00])
    f.program(0x5FFE, [0x00, 0x00])
    f.program(0x6000, [0x00])
    f.erase_sector(0x5123)  # any address inside the 4 KiB sector
    assert f.read(0x5000, 2) == [0xFF, 0xFF]
    assert f.read(0x5FFE, 2) == [0xFF, 0xFF]
    assert f.read(0x6000, 1) == [0x00]  # the next sector survives


def test_py32_flash_page_program_wraps_in_page(lib, py32):
    cpu = py32(app_image())
    f = FlashLink(lib, cpu)
    f.program(0x70FE, [1, 2, 3, 4])  # 256-byte page: wraps to 0x7000
    assert f.read(0x70FE, 2) == [1, 2]
    assert f.read(0x7000, 2) == [3, 4]
    assert f.read(0x7100, 1) == [0xFF]


def test_py32_flash_ignores_traffic_while_deselected(lib, py32):
    cpu = py32(app_image())
    f = FlashLink(lib, cpu)
    f.cs(False)
    f.xfer(0x06)  # CS high: this is not a command
    assert f.status() == 0


def test_py32_flash_host_api(lib, py32):
    cpu = py32(app_image())
    data = (ctypes.c_uint8 * 3)(7, 8, 9)
    lib.dp32g030_extflash_set(cpu, 0x010000, data, 3)
    f = FlashLink(lib, cpu)
    assert f.read(0x010000, 3) == [7, 8, 9]
    out = (ctypes.c_uint8 * 3)()
    f.program(0x010010, [0xAA, 0xBB, 0xCC])
    lib.dp32g030_extflash_get(cpu, 0x010010, out, 3)
    assert list(out) == [0xAA, 0xBB, 0xCC]


def test_py32_flash_contents_survive_reset(lib, py32):
    cpu = py32(app_image())
    f = FlashLink(lib, cpu)
    f.program(0x8000, [0x42])
    lib.dp32g030_cpu_reset(cpu)
    f2 = FlashLink(lib, cpu)
    assert f2.read(0x8000, 1) == [0x42]


def test_py32_flash_via_dma(lib, py32):
    """The firmware's bulk read: command and address by byte, data by DMA."""
    cpu = py32(app_image())
    f = FlashLink(lib, cpu)
    f.program(0x4000, list(range(32)))
    f.cs(True)
    for b in (0x03, 0x00, 0x40, 0x00):
        f.xfer(b)
    rxbuf, dummy = SRAM_BASE + 0x300, SRAM_BASE + 0x2F0
    wr(lib, cpu, dummy, 0xFF, 1)
    dma_setup(lib, cpu, 4, CCR_EN | CCR_MINC, 32, SPI2 + SPI_DR, rxbuf)  # RD
    dma_setup(lib, cpu, 5, CCR_EN | CCR_DIR, 32, SPI2 + SPI_DR, dummy)  # WR
    f.cs(False)
    assert [rd(lib, cpu, rxbuf + i, 1)[1] for i in range(32)] == list(range(32))


# ---- review fixes --------------------------------------------------------


def test_py32_adc_channel_out_of_range_reads_zero(lib, py32):
    cpu = py32(app_image())
    for ch in range(20):
        lib.dp32g030_adc_set_input(cpu, ch, 1234)
    wr(lib, cpu, ADC1 + ADC_SQR3, 25)  # no such channel
    wr(lib, cpu, ADC1 + ADC_CR2, CR2_ADON | CR2_SWSTART)
    assert rd(lib, cpu, ADC1 + ADC_DR)[1] == 0


def test_py32_dma_bus_error_sets_flags_and_disables_channel(lib, py32):
    cpu = py32(app_image())
    src = SRAM_BASE + 0x100
    dma_setup(lib, cpu, 1, CCR_EN | CCR_TCIE | CCR_MEM2MEM | CCR_PINC | CCR_MINC,
              4, src, 0x60000000)  # destination does not exist
    isr = rd(lib, cpu, DMA1 + DMA_ISR)[1]
    assert isr & (1 << 3)  # TEIF1
    assert isr & 1  # GIF1
    assert not isr & tcif(1)  # it did not complete
    assert not rd(lib, cpu, ch(1, 0))[1] & CCR_EN
    wr(lib, cpu, DMA1 + DMA_IFCR, 0xF)  # clear channel 1 flags
    # an unrelated DMA register write must not retry the dead channel
    wr(lib, cpu, ch(2, 4), 1)
    assert rd(lib, cpu, DMA1 + DMA_ISR)[1] & 0xF == 0


def test_py32_host_driven_inputs_survive_a_reset(lib, py32):
    cpu = py32(app_image())
    wr(lib, cpu, GPIOA + MODER, 0)
    lib.dp32g030_gpio_set_input(cpu, PORT_A, 1 << 5, 1 << 5)
    assert rd(lib, cpu, GPIOA + IDR)[1] & (1 << 5)
    lib.dp32g030_cpu_reset(cpu)  # the firmware reboots itself
    wr(lib, cpu, GPIOA + MODER, 0)
    assert rd(lib, cpu, GPIOA + IDR)[1] & (1 << 5)  # the outside world is still there


def test_py32_rejects_bootloader_slot_images(lib):
    soc = lib.dp32g030_soc_by_name(b"py32f071")
    for reset in (0x08000109, 0x080027FF):  # below the app base
        data = struct.pack("<II", SRAM_TOP, reset) + bytes(56)
        assert not lib.dp32g030_cpu_new_soc(soc, emu.to_buf(data), len(data)), hex(reset)
    ok = struct.pack("<II", SRAM_TOP, 0x08002801) + bytes(56)  # first app address
    assert lib.dp32g030_cpu_new_soc(soc, emu.to_buf(ok), len(ok))


def test_gpio_dir_dispatches_per_soc(lib, py32):
    cpu = py32(app_image())
    assert lib.dp32g030_gpio_dir(cpu, PORT_A) == 0  # everything analog or input
    wr(lib, cpu, GPIOA + MODER, mode(3, 1) | mode(7, 1))
    assert lib.dp32g030_gpio_dir(cpu, PORT_A) == (1 << 3) | (1 << 7)
    soc = lib.dp32g030_soc_by_name(b"dp32g030")
    data = emu.firmware([BKPT])
    dp = lib.dp32g030_cpu_new_soc(soc, emu.to_buf(data), len(data))
    try:
        wr(lib, dp, 0x40061004, 0x24)  # GPIOC DIR
        assert lib.dp32g030_gpio_dir(dp, 2) == 0x24
    finally:
        lib.dp32g030_cpu_free(dp)


# ---- basic timers TIM6 / TIM7 -------------------------------------------

TIM7 = 0x40001400
TIM6 = 0x40001000
TIM_CR1, TIM_DIER, TIM_SR, TIM_EGR, TIM_CNT, TIM_PSC, TIM_ARR = 0x00, 0x0C, 0x10, 0x14, 0x24, 0x28, 0x2C
CR1_CEN, CR1_UDIS, CR1_URS, CR1_OPM, CR1_ARPE = 1 << 0, 1 << 1, 1 << 2, 1 << 3, 1 << 7
DIER_UIE, DIER_UDE = 1 << 0, 1 << 8
SR_UIF = 1 << 0
NO_EVENT = 2**64 - 1


def advance(lib, cpu, cycles, chunk=1):
    """Run the cycle-driven blocks for `cycles` core clocks, like cpu_run does."""
    left = cycles
    while left:
        n = min(chunk, left)
        cpu.contents.cycles += n
        lib.dp32g030_soc_tick(cpu, n)
        left -= n


def tim(lib, cpu, off, base=TIM7):
    return rd(lib, cpu, base + off)[1]


def test_py32_tim7_counts_and_sets_uif(lib, py32):
    cpu = py32(app_image())
    wr(lib, cpu, TIM7 + TIM_ARR, 9)
    wr(lib, cpu, TIM7 + TIM_CR1, CR1_CEN)
    advance(lib, cpu, 9)
    assert tim(lib, cpu, TIM_CNT) == 9
    assert not tim(lib, cpu, TIM_SR) & SR_UIF
    advance(lib, cpu, 1)  # CNT == ARR overflows to 0: update event
    assert tim(lib, cpu, TIM_CNT) == 0
    assert tim(lib, cpu, TIM_SR) & SR_UIF
    wr(lib, cpu, TIM7 + TIM_SR, 0)  # UIF is cleared by writing 0
    assert not tim(lib, cpu, TIM_SR) & SR_UIF
    advance(lib, cpu, 10)
    assert tim(lib, cpu, TIM_SR) & SR_UIF  # the next period


def test_py32_tim7_stopped_does_not_count(lib, py32):
    cpu = py32(app_image())
    wr(lib, cpu, TIM7 + TIM_ARR, 9)
    advance(lib, cpu, 100)
    assert tim(lib, cpu, TIM_CNT) == 0 and not tim(lib, cpu, TIM_SR) & SR_UIF


def test_py32_tim7_prescaler(lib, py32):
    cpu = py32(app_image())
    wr(lib, cpu, TIM7 + TIM_PSC, 3)  # one count every 4 cycles
    wr(lib, cpu, TIM7 + TIM_ARR, 1)  # period (1+1)*(3+1) = 8 cycles
    wr(lib, cpu, TIM7 + TIM_CR1, CR1_CEN)
    advance(lib, cpu, 7)
    assert not tim(lib, cpu, TIM_SR) & SR_UIF
    advance(lib, cpu, 1)
    assert tim(lib, cpu, TIM_SR) & SR_UIF


def test_py32_tim7_big_steps_match_single_cycles(lib, py32):
    a, b = py32(app_image()), py32(app_image())
    for cpu in (a, b):
        wr(lib, cpu, TIM7 + TIM_ARR, 374)
        wr(lib, cpu, TIM7 + TIM_CR1, CR1_CEN)
    advance(lib, a, 10_000, chunk=1)
    advance(lib, b, 10_000, chunk=10_000)  # a WFI skip is one big tick
    assert tim(lib, a, TIM_CNT) == tim(lib, b, TIM_CNT) == 10_000 % 375


def test_py32_tim7_interrupt_line(lib, py32):
    cpu = py32(app_image())
    wr(lib, cpu, TIM7 + TIM_ARR, 4)
    wr(lib, cpu, TIM7 + TIM_DIER, DIER_UIE)
    wr(lib, cpu, TIM7 + TIM_CR1, CR1_CEN)
    assert not irq_line(cpu, 18)
    advance(lib, cpu, 5)
    assert irq_line(cpu, 18)  # TIM7 global interrupt
    wr(lib, cpu, TIM7 + TIM_SR, 0)
    assert not irq_line(cpu, 18)


def test_py32_tim6_uses_its_own_irq_and_registers(lib, py32):
    cpu = py32(app_image())
    wr(lib, cpu, TIM6 + TIM_ARR, 2)
    wr(lib, cpu, TIM6 + TIM_DIER, DIER_UIE)
    wr(lib, cpu, TIM6 + TIM_CR1, CR1_CEN)
    advance(lib, cpu, 3)
    assert irq_line(cpu, 17)  # TIM6 / LPTIM1 / DAC
    assert tim(lib, cpu, TIM_SR, TIM6) & SR_UIF
    assert tim(lib, cpu, TIM_SR, TIM7) == 0  # TIM7 is a different timer


def test_py32_tim7_arr_preload(lib, py32):
    cpu = py32(app_image())
    wr(lib, cpu, TIM7 + TIM_ARR, 9)
    wr(lib, cpu, TIM7 + TIM_CR1, CR1_CEN | CR1_ARPE)
    advance(lib, cpu, 5)
    wr(lib, cpu, TIM7 + TIM_ARR, 2)  # takes effect at the next update
    advance(lib, cpu, 5)  # first update at cycle 10 with the old period
    assert tim(lib, cpu, TIM_SR) & SR_UIF
    wr(lib, cpu, TIM7 + TIM_SR, 0)
    advance(lib, cpu, 2)
    assert not tim(lib, cpu, TIM_SR) & SR_UIF
    advance(lib, cpu, 1)  # new period: 3 cycles
    assert tim(lib, cpu, TIM_SR) & SR_UIF


def test_py32_tim7_arr_without_preload_applies_at_once(lib, py32):
    cpu = py32(app_image())
    wr(lib, cpu, TIM7 + TIM_ARR, 100)
    wr(lib, cpu, TIM7 + TIM_CR1, CR1_CEN)
    advance(lib, cpu, 5)
    wr(lib, cpu, TIM7 + TIM_ARR, 7)
    advance(lib, cpu, 3)
    assert tim(lib, cpu, TIM_SR) & SR_UIF


def test_py32_tim7_ug_reloads_and_urs_masks_the_flag(lib, py32):
    cpu = py32(app_image())
    wr(lib, cpu, TIM7 + TIM_ARR, 50)
    wr(lib, cpu, TIM7 + TIM_CR1, CR1_CEN)
    advance(lib, cpu, 20)
    wr(lib, cpu, TIM7 + TIM_EGR, 1)  # UG
    assert tim(lib, cpu, TIM_CNT) == 0
    assert tim(lib, cpu, TIM_SR) & SR_UIF
    wr(lib, cpu, TIM7 + TIM_SR, 0)
    wr(lib, cpu, TIM7 + TIM_CR1, CR1_CEN | CR1_URS)
    wr(lib, cpu, TIM7 + TIM_EGR, 1)
    assert not tim(lib, cpu, TIM_SR) & SR_UIF  # URS: UG alone does not flag


def test_py32_tim7_one_pulse_stops_at_the_update(lib, py32):
    cpu = py32(app_image())
    wr(lib, cpu, TIM7 + TIM_ARR, 4)
    wr(lib, cpu, TIM7 + TIM_CR1, CR1_CEN | CR1_OPM)
    advance(lib, cpu, 5)
    assert tim(lib, cpu, TIM_SR) & SR_UIF
    assert not tim(lib, cpu, TIM_CR1) & CR1_CEN
    wr(lib, cpu, TIM7 + TIM_SR, 0)
    advance(lib, cpu, 20)
    assert not tim(lib, cpu, TIM_SR) & SR_UIF


def test_py32_tim7_udis_blocks_updates(lib, py32):
    cpu = py32(app_image())
    wr(lib, cpu, TIM7 + TIM_ARR, 4)
    wr(lib, cpu, TIM7 + TIM_CR1, CR1_CEN | CR1_UDIS)
    advance(lib, cpu, 20)
    assert not tim(lib, cpu, TIM_SR) & SR_UIF


def test_py32_tim7_next_event_for_wfi(lib, py32):
    cpu = py32(app_image())
    assert lib.dp32g030_soc_next_event(cpu) == NO_EVENT
    wr(lib, cpu, TIM7 + TIM_ARR, 99)
    wr(lib, cpu, TIM7 + TIM_CR1, CR1_CEN)
    assert lib.dp32g030_soc_next_event(cpu) == NO_EVENT  # nobody listens
    wr(lib, cpu, TIM7 + TIM_DIER, DIER_UIE)
    assert lib.dp32g030_soc_next_event(cpu) == 100
    advance(lib, cpu, 30)
    assert lib.dp32g030_soc_next_event(cpu) == 70


# ---- request-paced DMA (timer update -> DMA) and circular mode -----------

SYSCFG = 0x40010000
SYSCFG_CFGR3, SYSCFG_CFGR4 = 0x08, 0x0C
TIM7_UP, TIM6_UP = 0x27, 0x26
CCR_CIRC = 1 << 5
SIZE32 = (2 << 8) | (2 << 10)  # PSIZE = MSIZE = 32 bit


def map_dma_channel(lib, cpu, n, request):
    if n <= 4:
        reg, shift = SYSCFG_CFGR3, 8 * (n - 1)
    else:
        reg, shift = SYSCFG_CFGR4, 8 * (n - 5)
    cur = rd(lib, cpu, SYSCFG + reg)[1]
    wr(lib, cpu, SYSCFG + reg, (cur & ~(0x3F << shift)) | (request << shift))


def start_tim7(lib, cpu, arr, udis=False):
    wr(lib, cpu, TIM7 + TIM_ARR, arr)
    wr(lib, cpu, TIM7 + TIM_DIER, DIER_UDE)
    wr(lib, cpu, TIM7 + TIM_CR1, CR1_CEN)


def words(lib, cpu, addr, n):
    return [rd(lib, cpu, addr + 4 * i)[1] for i in range(n)]


def test_py32_dma_timer_paced_circular(lib, py32):
    cpu = py32(app_image())
    buf, sink = SRAM_BASE + 0x100, SRAM_BASE + 0x400
    for i, w in enumerate((0xA0, 0xA1, 0xA2, 0xA3)):
        wr(lib, cpu, buf + 4 * i, w)
    map_dma_channel(lib, cpu, 7, TIM7_UP)
    dma_setup(lib, cpu, 7, CCR_EN | CCR_DIR | CCR_CIRC | CCR_MINC | SIZE32, 4, sink, buf)
    assert rd(lib, cpu, sink) == (True, 0)  # nothing moves until a request
    start_tim7(lib, cpu, 9)
    advance(lib, cpu, 9)
    assert rd(lib, cpu, sink) == (True, 0)
    for expected in (0xA0, 0xA1, 0xA2, 0xA3, 0xA0, 0xA1):  # one update every 10 cycles
        advance(lib, cpu, 10)
        assert rd(lib, cpu, sink) == (True, expected)  # ... and it wraps around
    assert rd(lib, cpu, ch(7, 0))[1] & CCR_EN  # circular channels stay enabled
    assert rd(lib, cpu, DMA1 + DMA_ISR)[1] & tcif(7)  # a lap completed


def test_py32_dma_timer_paced_normal_mode_completes(lib, py32):
    cpu = py32(app_image())
    buf, sink = SRAM_BASE + 0x100, SRAM_BASE + 0x400
    for i in range(3):
        wr(lib, cpu, buf + 4 * i, 0x10 + i)
    map_dma_channel(lib, cpu, 5, TIM7_UP)
    dma_setup(lib, cpu, 5, CCR_EN | CCR_DIR | CCR_MINC | SIZE32, 3, sink, buf)
    start_tim7(lib, cpu, 4)
    advance(lib, cpu, 5 * 3)
    assert rd(lib, cpu, DMA1 + DMA_ISR)[1] & tcif(5)
    assert not rd(lib, cpu, ch(5, 0))[1] & CCR_EN
    assert rd(lib, cpu, ch(5, 4))[1] == 0
    wr(lib, cpu, sink, 0xFFFF)
    advance(lib, cpu, 50)
    assert rd(lib, cpu, sink)[1] == 0xFFFF  # no transfers after completion


def test_py32_dma_paced_channel_ignores_other_requests(lib, py32):
    cpu = py32(app_image())
    buf, sink = SRAM_BASE + 0x100, SRAM_BASE + 0x400
    wr(lib, cpu, buf, 0x77)
    map_dma_channel(lib, cpu, 7, TIM6_UP)  # TIM6, but only TIM7 runs
    dma_setup(lib, cpu, 7, CCR_EN | CCR_DIR | CCR_MINC | SIZE32, 1, sink, buf)
    start_tim7(lib, cpu, 4)
    advance(lib, cpu, 30)
    assert rd(lib, cpu, sink)[1] == 0


def test_py32_dma_paced_needs_udde_and_enable(lib, py32):
    cpu = py32(app_image())
    buf, sink = SRAM_BASE + 0x100, SRAM_BASE + 0x400
    wr(lib, cpu, buf, 0x55)
    map_dma_channel(lib, cpu, 7, TIM7_UP)
    dma_setup(lib, cpu, 7, CCR_EN | CCR_DIR | CCR_CIRC | CCR_MINC | SIZE32, 1, sink, buf)
    wr(lib, cpu, TIM7 + TIM_ARR, 4)
    wr(lib, cpu, TIM7 + TIM_CR1, CR1_CEN)  # UDE not set
    advance(lib, cpu, 30)
    assert rd(lib, cpu, sink)[1] == 0
    wr(lib, cpu, TIM7 + TIM_DIER, DIER_UDE)
    advance(lib, cpu, 5)
    assert rd(lib, cpu, sink)[1] == 0x55
    wr(lib, cpu, ch(7, 0), CCR_DIR | CCR_CIRC | CCR_MINC | SIZE32)  # EN cleared
    wr(lib, cpu, sink, 0)
    advance(lib, cpu, 30)
    assert rd(lib, cpu, sink)[1] == 0


def test_py32_dma_paced_tc_interrupt(lib, py32):
    cpu = py32(app_image())
    buf, sink = SRAM_BASE + 0x100, SRAM_BASE + 0x400
    map_dma_channel(lib, cpu, 7, TIM7_UP)
    dma_setup(lib, cpu, 7, CCR_EN | CCR_TCIE | CCR_DIR | CCR_MINC | SIZE32, 2, sink, buf)
    start_tim7(lib, cpu, 2)
    advance(lib, cpu, 3)
    assert not irq_line(cpu, 11)
    advance(lib, cpu, 3)  # second request: transfer complete
    assert irq_line(cpu, 11)


def test_py32_dma_paced_bus_error_disables_the_channel(lib, py32):
    cpu = py32(app_image())
    map_dma_channel(lib, cpu, 7, TIM7_UP)
    dma_setup(lib, cpu, 7, CCR_EN | CCR_DIR | CCR_CIRC | CCR_MINC | SIZE32, 4,
              0x60000000, SRAM_BASE + 0x100)  # peripheral address is unmapped
    start_tim7(lib, cpu, 2)
    advance(lib, cpu, 3)
    isr = rd(lib, cpu, DMA1 + DMA_ISR)[1]
    assert isr & (1 << (4 * 6 + 3))  # TEIF7
    assert not rd(lib, cpu, ch(7, 0))[1] & CCR_EN


def test_py32_dma_spi_mapped_channels_still_run_at_once(lib, py32):
    cpu = py32(app_image())
    map_dma_channel(lib, cpu, 4, 0x15)  # SPI2_RD-ish codes are not timer requests
    src, dst = SRAM_BASE + 0x100, SRAM_BASE + 0x200
    wr(lib, cpu, src, 0x11223344)
    dma_setup(lib, cpu, 1, CCR_EN | CCR_MEM2MEM | CCR_PINC | CCR_MINC, 4, src, dst)
    assert rd(lib, cpu, dst) == (True, 0x11223344)


# ---- backlight (PF8 duty over time) --------------------------------------

GPIOF_BASE = 0x50001400
BL_PIN = 1 << 8
WINDOW = 800_000  # core clocks per measurement window


def backlight_pin_output(lib, cpu):
    wr(lib, cpu, GPIOF_BASE + MODER, mode(8, 1))


def bl_set(lib, cpu, high):
    wr(lib, cpu, GPIOF_BASE + BSRR, BL_PIN if high else BL_PIN << 16)


def test_backlight_present_only_where_the_board_has_one(lib, py32):
    cpu = py32(app_image())
    assert lib.dp32g030_has_backlight(cpu)
    soc = lib.dp32g030_soc_by_name(b"dp32g030")
    data = emu.firmware([BKPT])
    dp = lib.dp32g030_cpu_new_soc(soc, emu.to_buf(data), len(data))
    try:
        assert not lib.dp32g030_has_backlight(dp)
    finally:
        lib.dp32g030_cpu_free(dp)


def test_backlight_duty_follows_the_time_the_pin_is_high(lib, py32):
    cpu = py32(app_image())
    backlight_pin_output(lib, cpu)
    bl_set(lib, cpu, True)  # high at cycle 0
    cpu.contents.cycles = 200_000
    bl_set(lib, cpu, False)
    cpu.contents.cycles = WINDOW
    assert lib.dp32g030_backlight_level(cpu) == 64  # 200k / 800k of 256


def test_backlight_always_high_and_always_low(lib, py32):
    cpu = py32(app_image())
    backlight_pin_output(lib, cpu)
    cpu.contents.cycles = 3 * WINDOW
    assert lib.dp32g030_backlight_level(cpu) == 0  # never driven
    bl_set(lib, cpu, True)
    cpu.contents.cycles = 5 * WINDOW
    assert lib.dp32g030_backlight_level(cpu) == 255  # full duty caps at 255
    bl_set(lib, cpu, False)
    cpu.contents.cycles = 7 * WINDOW
    assert lib.dp32g030_backlight_level(cpu) == 0


def test_backlight_estimate_before_the_first_window_closes(lib, py32):
    cpu = py32(app_image())
    backlight_pin_output(lib, cpu)
    bl_set(lib, cpu, True)
    cpu.contents.cycles = 100_000
    assert lib.dp32g030_backlight_level(cpu) == 255  # high so far


def test_backlight_edge_straddling_windows(lib, py32):
    cpu = py32(app_image())
    backlight_pin_output(lib, cpu)
    cpu.contents.cycles = WINDOW - 100_000
    bl_set(lib, cpu, True)  # high for the last 100k of window 1 ...
    cpu.contents.cycles = WINDOW + 300_000
    bl_set(lib, cpu, False)  # ... and the first 300k of window 2
    cpu.contents.cycles = 2 * WINDOW
    assert lib.dp32g030_backlight_level(cpu) == 96  # 300k / 800k of 256 (window 2)


def test_backlight_ignores_other_pins_and_ports(lib, py32):
    cpu = py32(app_image())
    wr(lib, cpu, GPIOF_BASE + MODER, mode(7, 1) | mode(9, 1))
    wr(lib, cpu, GPIOF_BASE + BSRR, (1 << 7) | (1 << 9))
    wr(lib, cpu, GPIOA + MODER, mode(8, 1))
    wr(lib, cpu, GPIOA + BSRR, 1 << 8)  # PA8 is not the backlight
    cpu.contents.cycles = 2 * WINDOW
    assert lib.dp32g030_backlight_level(cpu) == 0


def test_backlight_pwm_through_timer_and_dma_like_the_firmware(lib, py32):
    cpu = py32(app_image())
    backlight_pin_output(lib, cpu)
    levels, high = 32, 8  # 8/32 duty
    buf = SRAM_BASE + 0x100
    for i in range(levels):
        wr(lib, cpu, buf + 4 * i, BL_PIN if i < high else BL_PIN << 16)
    map_dma_channel(lib, cpu, 7, TIM7_UP)
    dma_setup(lib, cpu, 7, CCR_EN | CCR_DIR | CCR_CIRC | CCR_MINC | SIZE32, levels,
              GPIOF_BASE + BSRR, buf)
    wr(lib, cpu, TIM7 + TIM_ARR, 374)  # 48 MHz / 4 kHz / 32 levels - 1
    wr(lib, cpu, TIM7 + TIM_DIER, DIER_UDE)
    wr(lib, cpu, TIM7 + TIM_CR1, CR1_CEN)
    advance(lib, cpu, 3 * WINDOW, chunk=375)
    assert abs(lib.dp32g030_backlight_level(cpu) - 64) <= 6
    # the firmware dims it: 24/32 duty
    for i in range(levels):
        wr(lib, cpu, buf + 4 * i, BL_PIN if i < 24 else BL_PIN << 16)
    advance(lib, cpu, 3 * WINDOW, chunk=375)
    assert abs(lib.dp32g030_backlight_level(cpu) - 192) <= 6
