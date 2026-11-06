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
