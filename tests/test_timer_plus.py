"""TIMER_PLUS0-1: timing, count, capture and HALL modes."""

import ctypes

import dp32g030 as emu
from test_exc import BKPT, BX_LR, ISER, NOP, V_IRQ0, WFI, go
from test_periph import str_

TIMER_PLUS0 = 0x40067000

MODE_TIMING, MODE_COUNT, MODE_CAPTURE, MODE_HALL = 0, 1, 2, 3


def bw(ptr, lib, addr, val):
    assert lib.dp32g030_bus_write(ptr, addr, 4, val)


def br(ptr, lib, addr):
    out = ctypes.c_uint32()
    assert lib.dp32g030_bus_read(ptr, addr, 4, ctypes.byref(out))
    return out.value


def test_timing_mode_counts_like_timer_base(lib):
    def setup(ptr):
        bw(ptr, lib, TIMER_PLUS0 + 0x30, 5)  # LOW_GOAL
        bw(ptr, lib, TIMER_PLUS0 + 0x00, 1)  # LOW_EN (ctr defaults to timing)

    def inspect(ptr):
        return br(ptr, lib, TIMER_PLUS0 + 0x34), br(ptr, lib, TIMER_PLUS0 + 0x14)

    res = go(lib, [NOP] * 7 + [BKPT], setup=setup, inspect=inspect)
    cnt, iflags = res.extra
    assert cnt == 2
    assert iflags & 1  # LOW_TO_IF


def test_count_mode_counts_external_edges(lib):
    ptr = lib.dp32g030_cpu_new(emu.to_buf(emu.firmware([BKPT])), 10)
    try:
        bw(ptr, lib, TIMER_PLUS0 + 0x30, 100)  # LOW_GOAL, far away
        # LOW_MODE=COUNT, LOW_EXT_SEL=0, LOW_EXT_EDGE=00 (rising)
        bw(ptr, lib, TIMER_PLUS0 + 0x08, MODE_COUNT)
        bw(ptr, lib, TIMER_PLUS0 + 0x00, 1)  # LOW_EN
        for _ in range(3):
            lib.dp32g030_timer_plus_set_ext(ptr, 0, 0, True)
            lib.dp32g030_timer_plus_set_ext(ptr, 0, 0, False)
        assert br(ptr, lib, TIMER_PLUS0 + 0x34) == 3
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_count_mode_respects_edge_polarity(lib):
    ptr = lib.dp32g030_cpu_new(emu.to_buf(emu.firmware([BKPT])), 10)
    try:
        bw(ptr, lib, TIMER_PLUS0 + 0x30, 100)
        edge_falling = 1 << 5
        bw(ptr, lib, TIMER_PLUS0 + 0x08, MODE_COUNT | edge_falling)
        bw(ptr, lib, TIMER_PLUS0 + 0x00, 1)
        lib.dp32g030_timer_plus_set_ext(ptr, 0, 0, True)  # rising: ignored
        assert br(ptr, lib, TIMER_PLUS0 + 0x34) == 0
        lib.dp32g030_timer_plus_set_ext(ptr, 0, 0, False)  # falling: counted
        assert br(ptr, lib, TIMER_PLUS0 + 0x34) == 1
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_capture_mode_latches_free_running_count(lib):
    def setup(ptr):
        bw(ptr, lib, TIMER_PLUS0 + 0x30, 1000)  # LOW_GOAL, far away
        bw(ptr, lib, TIMER_PLUS0 + 0x08, MODE_CAPTURE)
        bw(ptr, lib, TIMER_PLUS0 + 0x00, 1)

    def inspect(ptr):
        lib.dp32g030_timer_plus_set_ext(ptr, 0, 0, True)
        cval = br(ptr, lib, TIMER_PLUS0 + 0x38)
        iflags = br(ptr, lib, TIMER_PLUS0 + 0x14)
        return cval, iflags

    res = go(lib, [NOP] * 5 + [BKPT], setup=setup, inspect=inspect)
    cval, iflags = res.extra
    assert cval == 5
    assert iflags & (1 << 1)  # LOW_PR_IF


def test_hall_edges_set_flags_and_raw_value(lib):
    ptr = lib.dp32g030_cpu_new(emu.to_buf(emu.firmware([BKPT])), 10)
    try:
        lib.dp32g030_timer_plus_set_hall(ptr, 0, 0b001)
        assert br(ptr, lib, TIMER_PLUS0 + 0x40) == 1
        assert br(ptr, lib, TIMER_PLUS0 + 0x14) & (1 << 16)  # HALL0_R_IF
        lib.dp32g030_timer_plus_set_hall(ptr, 0, 0b000)
        assert br(ptr, lib, TIMER_PLUS0 + 0x14) & (1 << 17)  # HALL0_F_IF
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_disabled_does_not_count(lib):
    def inspect(ptr):
        return br(ptr, lib, TIMER_PLUS0 + 0x34)

    res = go(lib, [NOP] * 10 + [BKPT], inspect=inspect)
    assert res.extra == 0


def test_timer_plus_irq_fires(lib):
    def setup(ptr):
        bw(ptr, lib, TIMER_PLUS0 + 0x30, 20)
        bw(ptr, lib, TIMER_PLUS0 + 0x10, 1)  # LOW_TO_IE
        bw(ptr, lib, TIMER_PLUS0 + 0x00, 1)

    res = go(
        lib,
        [str_(1, 0, 0), WFI, 0x2609, BKPT],
        regs={0: ISER, 1: 1 << 7, 2: TIMER_PLUS0, 3: 1, 4: 0},
        handlers={V_IRQ0 + 4 * 7: [str_(3, 2, 0x14), str_(4, 2, 0x00), 0x3701, BX_LR]},
        setup=setup,
    )
    assert res.r[7] >= 1
    assert res.r[6] == 9
