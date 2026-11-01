"""GPIO, UART and plain register-file peripherals."""

import ctypes

import dp32g030 as emu
from test_exc import BKPT, BX_LR, ISER, NOP, V_IRQ0, WFI, go

GPIOA, GPIOB = 0x40060000, 0x40060800
UART0, UART1 = 0x4006B000, 0x4006B800
SYSCON, PORTCON = 0x40000000, 0x400B0000

STR_R1_R0_4 = 0x6041  # STR r1,[r0,#4]
STR_R2_R0_0 = 0x6002  # STR r2,[r0,#0]
LDR_R3_R0_0 = 0x6803  # LDR r3,[r0,#0]


def ldr(rt, rn, off):
    return 0x6800 | ((off // 4) << 6) | (rn << 3) | rt


def str_(rt, rn, off):
    return 0x6000 | ((off // 4) << 6) | (rn << 3) | rt


def test_gpio_output_and_input_mix(lib):
    def setup(ptr):
        lib.dp32g030_gpio_set_input(ptr, 0, 0x10, 0x10)

    res = go(
        lib,
        [STR_R1_R0_4, STR_R2_R0_0, LDR_R3_R0_0, BKPT],
        regs={0: GPIOA, 1: 0xF, 2: 0x5},
        setup=setup,
    )
    assert res.r[3] == 0x15  # outputs 0x5 on pins 0-3, input pin 4 high


def test_gpio_output_data_hidden_while_input(lib):
    res = go(lib, [STR_R2_R0_0, LDR_R3_R0_0, BKPT], regs={0: GPIOA, 2: 0xFF})
    assert res.r[3] == 0  # DIR is 0, so written data does not appear


def test_gpio_ports_are_independent(lib):
    main = [STR_R1_R0_4, STR_R2_R0_0, str_(1, 4, 4), str_(1, 4, 0), ldr(3, 4, 0), BKPT]
    res = go(lib, main, regs={0: GPIOA, 1: 0xFF, 2: 0x3, 4: GPIOB})
    assert res.r[3] == 0xFF


def test_gpio_hook_reports_pin_changes(lib):
    events = []

    cb = emu.GPIO_CB(lambda user, port, pins, d: events.append((port, pins, d)))

    def setup(ptr):
        ptr.contents.periph.hooks.gpio_change = cb

    go(lib, [STR_R1_R0_4, STR_R2_R0_0, BKPT], regs={0: GPIOA, 1: 0xF, 2: 0x5}, setup=setup)
    assert events[-1] == (0, 0x5, 0xF)


def test_gpio_rejects_byte_access(lib):
    res = go(lib, [0x7803, BKPT], regs={0: GPIOA})
    assert res.halted and res.locked_up


def test_regfile_roundtrip_word_half_byte(lib):
    main = [
        str_(1, 0, 8),
        ldr(2, 0, 8),
        0x7A03 - 0x0000,  # LDRB r3,[r0,#8]  (0x7800 | 8<<6 ... below)
        BKPT,
    ]
    main[2] = 0x7800 | (8 << 6) | (0 << 3) | 3
    res = go(lib, main, regs={0: SYSCON, 1: 0x11223344})
    assert res.r[2] == 0x11223344
    assert res.r[3] == 0x44


def test_regfile_subword_write_merges(lib):
    main = [
        str_(1, 0, 0),
        0x7000 | (1 << 6) | (0 << 3) | 2,  # STRB r2,[r0,#1]
        ldr(3, 0, 0),
        BKPT,
    ]
    res = go(lib, main, regs={0: PORTCON, 1: 0x11223344, 2: 0xAA})
    assert res.r[3] == 0x1122AA44


def test_regfile_regions_do_not_alias(lib):
    main = [str_(1, 0, 0), ldr(3, 4, 0), BKPT]
    res = go(lib, main, regs={0: SYSCON, 1: 0xDEAD, 4: PORTCON})
    assert res.r[3] == 0


def test_uart_tx_reaches_host_and_sets_txdone(lib):
    sent = []
    cb = emu.UART_TX_CB(lambda user, n, b: sent.append((n, b)))

    def setup(ptr):
        ptr.contents.periph.hooks.uart_tx = cb

    main = [str_(1, 0, 8), str_(1, 0, 8), ldr(3, 0, 0x14), BKPT]
    res = go(lib, main, regs={0: UART1, 1: 0x41}, setup=setup)
    assert sent == [(1, 0x41), (1, 0x41)]
    assert res.r[3] & (1 << 2)


def test_uart_txdone_is_write_one_to_clear(lib):
    main = [str_(1, 0, 8), str_(2, 0, 0x14), ldr(3, 0, 0x14), BKPT]
    res = go(lib, main, regs={0: UART0, 1: 0x41, 2: 1 << 2})
    assert not res.r[3] & (1 << 2)


def test_uart_rx_fifo_order_and_flags(lib):
    def setup(ptr):
        for b in (0x11, 0x22):
            lib.dp32g030_uart_rx(ptr, 0, b)

    main = [ldr(5, 0, 0x14), ldr(3, 0, 0xC), ldr(4, 0, 0xC), ldr(6, 0, 0x14), BKPT]
    res = go(lib, main, regs={0: UART0}, setup=setup)
    assert (res.r[3], res.r[4]) == (0x11, 0x22)
    assert not res.r[5] & (1 << 10) and (res.r[5] >> 17) & 7 == 2
    assert res.r[6] & (1 << 10)  # RX FIFO empty


def test_uart_rx_overflow_latches(lib):
    def setup(ptr):
        for b in range(9):
            lib.dp32g030_uart_rx(ptr, 0, b)

    main = [ldr(3, 0, 0x14), BKPT]
    res = go(lib, main, regs={0: UART0}, setup=setup)
    assert res.r[3] & (1 << 8)
    assert res.r[3] & (1 << 11)  # FIFO full


def test_uart_fifo_clear(lib):
    def setup(ptr):
        lib.dp32g030_uart_rx(ptr, 0, 0x55)

    main = [str_(1, 0, 0x18), ldr(3, 0, 0x14), BKPT]
    res = go(lib, main, regs={0: UART0, 1: 1 << 6}, setup=setup)
    assert res.r[3] & (1 << 10)


def test_uart_rx_interrupt(lib):
    def setup(ptr):
        lib.dp32g030_uart_rx(ptr, 0, 0x5A)

    main = [str_(1, 0, 0x10), str_(2, 4, 0), NOP, BKPT]
    handlers = {V_IRQ0 + 4 * 13: [ldr(5, 0, 0xC), 0x3701, BX_LR]}
    res = go(lib, main, handlers, regs={0: UART0, 1: 1 << 6, 2: 1 << 13, 4: ISER}, setup=setup)
    assert res.r[7] == 1
    assert res.r[5] == 0x5A


def test_uart_irq_is_level_sensitive(lib):
    # The handler leaves the byte in the FIFO, so the IRQ fires again after return.
    def setup(ptr):
        lib.dp32g030_uart_rx(ptr, 0, 0x5A)

    main = [str_(1, 0, 0x10), str_(2, 4, 0)] + [NOP] * 40 + [BKPT]
    handlers = {V_IRQ0 + 4 * 13: [0x3701, 0x2F03, 0xD100, 0xBF00, BX_LR]}
    res = go(lib, main, handlers, regs={0: UART0, 1: 1 << 6, 2: 1 << 13, 4: ISER}, setup=setup)
    assert res.r[7] > 1


def test_uart_rx_timeout_wakes_wfi(lib):
    def setup(ptr):
        u = ptr.contents.periph.uart[0]
        u.rxto = 3
        u.fifo = 3  # threshold of four bytes
        lib.dp32g030_uart_rx(ptr, 0, 0x77)

    main = [str_(1, 0, 0x10), str_(2, 4, 0), WFI, 0x2609, BKPT]
    handlers = {V_IRQ0 + 4 * 13: [ldr(5, 0, 0x14), str_(5, 0, 0x14), 0x3701, BX_LR]}
    res = go(lib, main, handlers, regs={0: UART0, 1: 1 << 5, 2: 1 << 13, 4: ISER}, setup=setup)
    assert res.r[5] & (1 << 5)
    assert res.r[7] == 1
    assert res.r[6] == 9
    assert res.cycles >= 30


def test_uart_no_timeout_when_above_threshold(lib):
    def setup(ptr):
        ptr.contents.periph.uart[0].rxto = 3
        lib.dp32g030_uart_rx(ptr, 0, 0x77)  # threshold is one byte

    main = [str_(1, 0, 0x10), str_(2, 4, 0), WFI, 0x2609, BKPT]
    handlers = {V_IRQ0 + 4 * 13: [0x3701, BX_LR]}
    res = go(lib, main, handlers, regs={0: UART0, 1: 1 << 5, 2: 1 << 13, 4: ISER}, setup=setup)
    assert res.r[7] == 0 and res.sleeping


def test_peripherals_reset_with_core(lib):
    ptr = lib.dp32g030_cpu_new(emu.to_buf(emu.firmware([BKPT])), 10)
    try:
        cpu = ptr.contents
        cpu.periph.uart[0].baud = 7
        cpu.periph.gpio[0].dir = 1
        cpu.periph.gpio[0].in_ = 4
        lib.dp32g030_uart_rx(ptr, 0, 1)
        lib.dp32g030_cpu_reset(ptr)
        assert cpu.periph.uart[0].baud == 0
        assert cpu.periph.uart[0].rx_count == 0
        assert cpu.periph.gpio[0].dir == 0
        assert cpu.periph.gpio[0].in_ == 4  # pins driven by the host persist
        assert cpu.scs.irq_lines == 0
    finally:
        lib.dp32g030_cpu_free(ptr)
