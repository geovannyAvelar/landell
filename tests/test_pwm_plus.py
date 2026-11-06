"""PWM_PLUS0-1: edge/center-aligned counting, brake and mask overrides."""

import ctypes

import dp32g030 as emu
from test_exc import BKPT, BX_LR, ISER, NOP, V_IRQ0, WFI, go
from test_periph import str_

PWM_PLUS0 = 0x400B4000

CFG_COUNTER_EN = 1 << 0
CFG_CNT_TYPE_DOWN = 1 << 1
CFG_CNT_REP_LOOP = 1 << 2
CFG_OUT_MODE_CENTER = 1 << 3


def bw(ptr, lib, addr, val):
    assert lib.dp32g030_bus_write(ptr, addr, 4, val)


def br(ptr, lib, addr):
    out = ctypes.c_uint32()
    assert lib.dp32g030_bus_read(ptr, addr, 4, ctypes.byref(out))
    return out.value


def test_edge_aligned_up_period_and_comp_interrupts(lib):
    def setup(ptr):
        bw(ptr, lib, PWM_PLUS0 + 0x1C, 5)  # PERIOD
        bw(ptr, lib, PWM_PLUS0 + 0x20, 2)  # CH0_COMP
        bw(ptr, lib, PWM_PLUS0 + 0x00, CFG_COUNTER_EN | CFG_CNT_REP_LOOP)

    def inspect(ptr):
        return br(ptr, lib, PWM_PLUS0 + 0xE0), br(ptr, lib, PWM_PLUS0 + 0x64)

    # 6 ticks from CNT=0: 1,2(=COMP),3,4,5,6(>PERIOD, wraps to 0).
    res = go(lib, [NOP] * 6 + [BKPT], setup=setup, inspect=inspect)
    cnt_st, iflags = res.extra
    assert cnt_st & 0xFFFF == 0
    assert iflags & 1  # UP_CH0COMP_IF
    assert iflags & (1 << 3)  # UP_POF_IF


def test_edge_aligned_down_counts(lib):
    def setup(ptr):
        bw(ptr, lib, PWM_PLUS0 + 0x1C, 5)  # PERIOD
        bw(ptr, lib, PWM_PLUS0 + 0x20, 2)  # CH0_COMP
        bw(ptr, lib, PWM_PLUS0 + 0x00,
           CFG_COUNTER_EN | CFG_CNT_REP_LOOP | CFG_CNT_TYPE_DOWN)

    def inspect(ptr):
        return br(ptr, lib, PWM_PLUS0 + 0xE0), br(ptr, lib, PWM_PLUS0 + 0x64)

    # CNT=0 immediately wraps to PERIOD=5 (tick 1), then counts down
    # 4, 3, 2 (matches COMP=2).
    res = go(lib, [NOP] * 4 + [BKPT], setup=setup, inspect=inspect)
    cnt_st, iflags = res.extra
    assert cnt_st & 0xFFFF == 2
    assert iflags & (1 << 8)  # DOWN_CH0COMP_IF


def test_center_aligned_direction_reverses(lib):
    def setup(ptr):
        bw(ptr, lib, PWM_PLUS0 + 0x1C, 3)  # PERIOD
        bw(ptr, lib, PWM_PLUS0 + 0x00,
           CFG_COUNTER_EN | CFG_CNT_REP_LOOP | CFG_OUT_MODE_CENTER)

    def inspect(ptr):
        return br(ptr, lib, PWM_PLUS0 + 0xE0)

    # cnt climbs 1,2,3 then overflows past PERIOD=3 on the 4th tick,
    # reversing direction and clamping to 3.
    res = go(lib, [NOP] * 4 + [BKPT], setup=setup, inspect=inspect)
    cnt_st = res.extra
    assert cnt_st & (1 << 16)  # CNT_DIR: now counting down
    assert cnt_st & 0xFFFF == 3


def test_single_shot_stops_after_one_cycle(lib):
    def setup(ptr):
        bw(ptr, lib, PWM_PLUS0 + 0x1C, 2)  # PERIOD, no CNT_REP -> single-shot
        bw(ptr, lib, PWM_PLUS0 + 0x00, CFG_COUNTER_EN)

    def inspect(ptr):
        return br(ptr, lib, PWM_PLUS0 + 0x00)

    res = go(lib, [NOP] * 10 + [BKPT], setup=setup, inspect=inspect)
    assert not res.extra & CFG_COUNTER_EN  # hardware cleared it


def test_channel_output_follows_start_and_comp(lib):
    def setup(ptr):
        bw(ptr, lib, PWM_PLUS0 + 0x1C, 9)  # PERIOD
        bw(ptr, lib, PWM_PLUS0 + 0x20, 4)  # CH0_COMP
        bw(ptr, lib, PWM_PLUS0 + 0x04, 1 << 8)  # CH0_START=1
        bw(ptr, lib, PWM_PLUS0 + 0x00, CFG_COUNTER_EN | CFG_CNT_REP_LOOP)

    res = go(
        lib,
        [NOP] * 3 + [BKPT],
        setup=setup,
        inspect=lambda ptr: ptr.contents.periph.pwm_plus[0].ch_level,
    )
    # 3 ticks: CNT=3 (< COMP=4) -> CH0 outputs START (1).
    assert res.extra & 1


def test_brake_forces_output_and_sets_flag(lib):
    def setup(ptr):
        bw(ptr, lib, PWM_PLUS0 + 0x0C, (1 << 16) | (1 << 12) | (1 << 0))
        # CH0_BRAKE=brakein0, BRAKE_LEV[0]=active-high, BRAKE_CH0POL=1
        bw(ptr, lib, PWM_PLUS0 + 0x1C, 9)
        bw(ptr, lib, PWM_PLUS0 + 0x00, CFG_COUNTER_EN | CFG_CNT_REP_LOOP)

    def inspect(ptr):
        lib.dp32g030_pwm_plus_set_brake(ptr, 0, 1)
        iflags = br(ptr, lib, PWM_PLUS0 + 0x64)
        return ptr.contents.periph.pwm_plus[0].ch_level, iflags

    res = go(lib, [NOP] * 3 + [BKPT], setup=setup, inspect=inspect)
    ch_level, iflags = res.extra
    assert ch_level & 1  # CH0 forced to BRAKE_CH0POL (1)
    assert iflags & (1 << 16)  # BRAK0_IF


def test_mask_forces_output_level(lib):
    def setup(ptr):
        bw(ptr, lib, PWM_PLUS0 + 0x10, 1)  # MASK_LEV: CH0_MASK_LEV=1
        bw(ptr, lib, PWM_PLUS0 + 0x88, 1)  # MASK_EN: CH0_MASK_EN
        bw(ptr, lib, PWM_PLUS0 + 0x1C, 9)
        bw(ptr, lib, PWM_PLUS0 + 0x20, 8)  # CH0_COMP high, so raw would be 0
        bw(ptr, lib, PWM_PLUS0 + 0x00, CFG_COUNTER_EN | CFG_CNT_REP_LOOP)

    res = go(
        lib,
        [NOP] * 2 + [BKPT],
        setup=setup,
        inspect=lambda ptr: ptr.contents.periph.pwm_plus[0].ch_level,
    )
    assert res.extra & 1  # forced to MASK_LEV=1 regardless of the comparator


def test_disabled_does_not_count(lib):
    def inspect(ptr):
        return br(ptr, lib, PWM_PLUS0 + 0xE0)

    res = go(lib, [NOP] * 10 + [BKPT], inspect=inspect)
    assert res.extra & 0xFFFF == 0  # reset value, never touched


def test_pwm_plus_irq_fires(lib):
    def setup(ptr):
        bw(ptr, lib, PWM_PLUS0 + 0x1C, 3)  # PERIOD
        bw(ptr, lib, PWM_PLUS0 + 0x60, 1 << 3)  # UP_POF_IE
        bw(ptr, lib, PWM_PLUS0 + 0x00, CFG_COUNTER_EN | CFG_CNT_REP_LOOP)

    res = go(
        lib,
        [str_(1, 0, 0), WFI, 0x2609, BKPT],
        regs={0: ISER, 1: 1 << 11, 2: PWM_PLUS0, 3: 1 << 3, 4: 0},
        handlers={
            V_IRQ0 + 4 * 11: [str_(3, 2, 0x64), str_(4, 2, 0x00), 0x3701, BX_LR]
        },
        setup=setup,
    )
    assert res.r[7] >= 1
    assert res.r[6] == 9
