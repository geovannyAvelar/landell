"""TIMER_BASE0-2 and PWM_BASE0-1, driven through the bus."""

import ctypes

import dp32g030 as emu
from test_exc import BKPT, BX_LR, ISER, NOP, V_IRQ0, WFI, go
from test_periph import str_

TIMER_BASE0 = 0x40064000
PWM_BASE0 = 0x400B1000


def bw(ptr, lib, addr, val):
    assert lib.dp32g030_bus_write(ptr, addr, 4, val)


def br(ptr, lib, addr):
    out = ctypes.c_uint32()
    assert lib.dp32g030_bus_read(ptr, addr, 4, ctypes.byref(out))
    return out.value


def test_timer_base_counts_and_wraps(lib):
    def setup(ptr):
        bw(ptr, lib, TIMER_BASE0 + 0x30, 5)  # LOW_LOAD
        bw(ptr, lib, TIMER_BASE0 + 0x00, 1)  # LOW_EN

    def inspect(ptr):
        return br(ptr, lib, TIMER_BASE0 + 0x34), br(ptr, lib, TIMER_BASE0 + 0x14)

    res = go(lib, [NOP] * 7 + [BKPT], setup=setup, inspect=inspect)
    cnt, iflags = res.extra
    assert cnt == 2  # 7 ticks: wraps at 5, then 2 more
    assert iflags & 1  # LOW_IF


def test_timer_base_high_and_low_independent(lib):
    def setup(ptr):
        bw(ptr, lib, TIMER_BASE0 + 0x20, 4)  # HIGH_LOAD
        bw(ptr, lib, TIMER_BASE0 + 0x30, 100)  # LOW_LOAD
        bw(ptr, lib, TIMER_BASE0 + 0x00, 3)  # HIGH_EN | LOW_EN

    def inspect(ptr):
        return br(ptr, lib, TIMER_BASE0 + 0x24), br(ptr, lib, TIMER_BASE0 + 0x34)

    res = go(lib, [NOP] * 7 + [BKPT], setup=setup, inspect=inspect)
    hi, lo = res.extra
    assert hi == 3  # 7 ticks against a load of 4: wraps once (4), then 3 more
    assert lo == 7  # load of 100 never reached


def test_timer_base_disabled_does_not_count(lib):
    def inspect(ptr):
        return br(ptr, lib, TIMER_BASE0 + 0x34)

    res = go(lib, [NOP] * 10 + [BKPT], inspect=inspect)
    assert res.extra == 0


def test_timer_base_irq_fires(lib):
    def setup(ptr):
        bw(ptr, lib, TIMER_BASE0 + 0x30, 20)  # LOW_LOAD
        bw(ptr, lib, TIMER_BASE0 + 0x10, 1)  # LOW_IE
        bw(ptr, lib, TIMER_BASE0 + 0x00, 1)  # LOW_EN

    res = go(
        lib,
        [str_(1, 0, 0), WFI, 0x2609, BKPT],
        regs={0: ISER, 1: 1 << 5, 2: TIMER_BASE0, 3: 1},
        handlers={V_IRQ0 + 4 * 5: [str_(3, 2, 0x14), str_(4, 2, 0x00), 0x3701, BX_LR]},
        setup=setup,
    )
    assert res.r[7] >= 1  # LOW_LOAD=2 can match again before EN is cleared
    assert res.r[6] == 9


def test_pwm_base_period_and_comp_interrupts(lib):
    def setup(ptr):
        bw(ptr, lib, PWM_BASE0 + 0x0C, 5)  # PERIOD
        bw(ptr, lib, PWM_BASE0 + 0x20, 2)  # CH0_COMP
        bw(ptr, lib, PWM_BASE0 + 0x00, 1)  # COUNTER_EN

    def inspect(ptr):
        return br(ptr, lib, PWM_BASE0 + 0x18), br(ptr, lib, PWM_BASE0 + 0x14)

    res = go(lib, [NOP] * 7 + [BKPT], setup=setup, inspect=inspect)
    cnt, iflags = res.extra
    # CNT resets to 0xFFFF on reset, so the first tick just wraps it to 0.
    assert cnt == 0
    assert iflags & 1  # CH0_COMP_IF (matched at cnt==2)
    assert iflags & (1 << 3)  # POF_IF


def test_pwm_base_disabled_does_not_count(lib):
    def inspect(ptr):
        return br(ptr, lib, PWM_BASE0 + 0x18)

    res = go(lib, [NOP] * 10 + [BKPT], inspect=inspect)
    assert res.extra == 0xFFFF  # reset value


def test_pwm_base_irq_fires(lib):
    def setup(ptr):
        bw(ptr, lib, PWM_BASE0 + 0x0C, 3)  # PERIOD
        bw(ptr, lib, PWM_BASE0 + 0x10, 1 << 3)  # POF_IE
        bw(ptr, lib, PWM_BASE0 + 0x00, 1)

    res = go(
        lib,
        [str_(1, 0, 0), WFI, 0x2609, BKPT],
        regs={0: ISER, 1: 1 << 9, 2: PWM_BASE0, 3: 1 << 3},
        handlers={V_IRQ0 + 4 * 9: [str_(3, 2, 0x14), 0x3701, BX_LR]},
        setup=setup,
    )
    assert res.r[7] >= 1


def test_wfi_wakes_on_timer_base(lib):
    def setup(ptr):
        bw(ptr, lib, TIMER_BASE0 + 0x30, 50)
        bw(ptr, lib, TIMER_BASE0 + 0x10, 1)
        bw(ptr, lib, TIMER_BASE0 + 0x00, 1)

    res = go(
        lib,
        [str_(1, 0, 0), WFI, 0x2609, BKPT],
        regs={0: ISER, 1: 1 << 5, 2: TIMER_BASE0, 3: 1},
        handlers={V_IRQ0 + 4 * 5: [str_(3, 2, 0x14), 0x3701, BX_LR]},
        setup=setup,
    )
    assert res.r[7] == 1
    assert res.r[6] == 9
    assert res.cycles >= 50
