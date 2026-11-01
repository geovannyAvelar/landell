"""SPI, SARADC and CRC, driven through the bus."""

import ctypes

import pytest

import dp32g030 as emu
from test_exc import BKPT, BX_LR, ISER, NOP, V_IRQ0, go
from test_periph import ldr, str_

SPI0, SARADC, CRC = 0x400B8000, 0x400BA000, 0x40003000


@pytest.fixture
def cpu(lib):
    ptr = lib.dp32g030_cpu_new(emu.to_buf(emu.firmware([BKPT])), 10)
    yield ptr
    lib.dp32g030_cpu_free(ptr)


@pytest.fixture
def rd(lib, cpu):
    def _rd(addr):
        out = ctypes.c_uint32()
        assert lib.dp32g030_bus_read(cpu, addr, 4, ctypes.byref(out))
        return out.value

    return _rd


@pytest.fixture
def wr(lib, cpu):
    def _wr(addr, val):
        assert lib.dp32g030_bus_write(cpu, addr, 4, val)

    return _wr


def test_spi_transfer_uses_host_hook(lib, cpu, rd, wr):
    seen = []

    def xfer(user, n, tx):
        seen.append((n, tx))
        return tx ^ 0xFF

    cb = emu.SPI_CB(xfer)
    cpu.contents.periph.hooks.spi_xfer = cb
    wr(SPI0, (1 << 3) | (1 << 6))  # SPE | MSTR
    wr(SPI0 + 4, 0xA5)
    wr(SPI0 + 4, 0x01)
    assert seen == [(0, 0xA5), (0, 0x01)]
    assert rd(SPI0 + 0x18) >> 6 & 7 == 2
    assert rd(SPI0 + 8) == 0x5A
    assert rd(SPI0 + 8) == 0xFE
    assert rd(SPI0 + 0x18) & 1  # RX FIFO empty


def test_spi_disabled_does_not_transfer(lib, cpu, rd, wr):
    wr(SPI0 + 4, 0x12)
    assert rd(SPI0 + 0x18) & 1


def test_spi_overflow_and_clear(lib, cpu, rd, wr):
    wr(SPI0, (1 << 3) | (1 << 6))
    for _ in range(9):
        wr(SPI0 + 4, 1)
    assert rd(SPI0 + 0x14) & 1  # OVF
    assert rd(SPI0 + 0x14) & 2  # full
    wr(SPI0 + 0x14, 1)
    assert not rd(SPI0 + 0x14) & 1
    wr(SPI0, (1 << 3) | (1 << 6) | (1 << 15))
    assert rd(SPI0 + 0x18) & 1


def test_spi1_is_separate(lib, cpu, rd, wr):
    wr(SPI0, (1 << 3) | (1 << 6))
    wr(SPI0 + 4, 1)
    assert rd(0x400B8800 + 0x18) & 1


def test_spi_interrupt(lib):
    main = [str_(1, 0, 0), str_(2, 0, 0x10), str_(3, 5, 0), str_(4, 0, 4), NOP, BKPT]
    handlers = {V_IRQ0 + 4 * 16: [ldr(6, 0, 8), 0x3701, BX_LR]}
    res = go(lib, main, handlers, regs={0: SPI0, 1: (1 << 3) | (1 << 6), 2: 2, 3: 1 << 16, 4: 0x44, 5: ISER})
    # RX full needs 8 bytes; one byte must not raise the interrupt
    assert res.r[7] == 0


def test_spi_full_interrupt_fires(lib):
    main = [str_(1, 0, 0), str_(2, 0, 0x10), str_(3, 5, 0)] + [str_(4, 0, 4)] * 8 + [NOP, BKPT]
    handlers = {V_IRQ0 + 4 * 16: [ldr(6, 0, 8), 0x3701, BX_LR]}
    res = go(lib, main, handlers, regs={0: SPI0, 1: (1 << 3) | (1 << 6), 2: 2, 3: 1 << 16, 4: 0x44, 5: ISER})
    assert res.r[7] >= 1


def adc_setup(lib, cpu, wr, ch_mask=0x3):
    lib.dp32g030_adc_set_input(cpu, 0, 0x7B2)
    lib.dp32g030_adc_set_input(cpu, 1, 0xFFFF)
    wr(SARADC, (1 << 27) | ch_mask)


def test_adc_conversion_latches_inputs(lib, cpu, rd, wr):
    adc_setup(lib, cpu, wr)
    assert rd(SARADC + 0xC) == 0
    wr(SARADC + 4, 1)
    assert rd(SARADC + 0xC) & (1 << 16)
    assert rd(SARADC + 0x10) == 0x7B2
    assert rd(SARADC + 0x14) == 0xFFF  # clipped to 12 bits
    assert rd(SARADC + 0x18) == 0  # channel 2 not selected


def test_adc_needs_enable(lib, cpu, rd, wr):
    lib.dp32g030_adc_set_input(cpu, 0, 5)
    wr(SARADC, 1)
    wr(SARADC + 4, 1)
    assert rd(SARADC + 0xC) == 0


def test_adc_flag_write_one_to_clear_and_reset(lib, cpu, rd, wr):
    adc_setup(lib, cpu, wr)
    wr(SARADC + 4, 1)
    wr(SARADC + 0xC, 1 << 16)
    assert not rd(SARADC + 0xC) & (1 << 16)
    wr(SARADC + 4, 4)
    assert rd(SARADC + 0x10) == 0


def test_adc_inputs_survive_reset(lib, cpu, rd, wr):
    adc_setup(lib, cpu, wr)
    lib.dp32g030_cpu_reset(cpu)
    wr(SARADC, (1 << 27) | 1)
    wr(SARADC + 4, 1)
    assert rd(SARADC + 0x10) == 0x7B2


def crc_run(wr, rd, cr, iv, data, width_bits=8):
    wr(CRC, cr)
    wr(CRC + 4, iv)
    for b in data:
        wr(CRC + 8, b)
    return rd(CRC + 0xC)


def test_crc16_ccitt_false(lib, cpu, rd, wr):
    cr = 1 | (2 << 7) | (0 << 9)  # enable, 8-bit data, CCITT
    assert crc_run(wr, rd, cr, 0xFFFF, b"123456789") == 0x29B1


def test_crc8_atm(lib, cpu, rd, wr):
    cr = 1 | (2 << 7) | (1 << 9)
    assert crc_run(wr, rd, cr, 0, b"123456789") == 0xF4


def test_crc16_ibm_poly(lib, cpu, rd, wr):
    cr = 1 | (2 << 7) | (2 << 9)
    # CRC-16/UMTS (poly 0x8005, no reflection), init 0
    assert crc_run(wr, rd, cr, 0, b"123456789") == 0xFEE8


def test_crc32_mpeg2(lib, cpu, rd, wr):
    cr = 1 | (2 << 7) | (3 << 9)
    # CRC-32/MPEG-2 (poly 0x04C11DB7, no reflection, no xorout)
    assert crc_run(wr, rd, cr, 0xFFFFFFFF, b"123456789") == 0x0376E6E7


def test_crc_disabled_ignores_data(lib, cpu, rd, wr):
    wr(CRC, (2 << 7))
    wr(CRC + 4, 0x1234)
    wr(CRC + 8, 0x55)
    assert rd(CRC + 0xC) == 0x1234


def test_crc_32bit_word_matches_four_bytes(lib, cpu, rd, wr):
    cr8 = 1 | (2 << 7) | (3 << 9)
    cr32 = 1 | (0 << 7) | (3 << 9)
    a = crc_run(wr, rd, cr8, 0xFFFFFFFF, bytes([0x12, 0x34, 0x56, 0x78]))
    wr(CRC, cr32)
    wr(CRC + 4, 0xFFFFFFFF)
    wr(CRC + 8, 0x12345678)
    assert rd(CRC + 0xC) == a
