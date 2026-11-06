"""RTC, IWDT and WWDT, driven through the bus."""

import ctypes

import dp32g030 as emu
from test_exc import BKPT, BX_LR, ISER, NOP, V_IRQ0, WFI, go
from test_periph import str_

RTC, IWDT, WWDT = 0x40069000, 0x4006A000, 0x4006A800

CFG_LOAD_EN, CFG_ALM_EN, CFG_RTC_EN = 1 << 2, 1 << 1, 1 << 0


def bw(ptr, lib, addr, val):
    assert lib.dp32g030_bus_write(ptr, addr, 4, val)


def br(ptr, lib, addr):
    out = ctypes.c_uint32()
    assert lib.dp32g030_bus_read(ptr, addr, 4, ctypes.byref(out))
    return out.value


def bcd2(v):
    return ((v // 10) << 4) | (v % 10)


def tr(week, hour, minute, sec):
    return (week << 24) | (bcd2(hour) << 16) | (bcd2(minute) << 8) | bcd2(sec)


def dr(year, month, date):
    return (bcd2(year) << 16) | (bcd2(month) << 8) | bcd2(date)


def ar(week_mask, hour, minute, sec):
    return (week_mask << 24) | (bcd2(hour) << 16) | (bcd2(minute) << 8) | bcd2(sec)


def tstr_min(tstr):
    return ((tstr >> 12) & 0xF) * 10 + ((tstr >> 8) & 0xF)


def tstr_sec(tstr):
    return ((tstr >> 4) & 0xF) * 10 + (tstr & 0xF)


# ---- RTC -----------------------------------------------------------


def test_rtc_seconds_tick_and_interrupt(lib):
    def setup(ptr):
        bw(ptr, lib, RTC + 0x10, 4)  # PRE_ROUND=4 -> period=5
        bw(ptr, lib, RTC + 0x00, CFG_RTC_EN)

    def inspect(ptr):
        return br(ptr, lib, RTC + 0x20), br(ptr, lib, RTC + 0x08)

    res = go(lib, [NOP] * 5 + [BKPT], setup=setup, inspect=inspect)
    tstr, iflags = res.extra
    assert tstr_sec(tstr) == 1  # 1 second elapsed
    assert iflags & 1  # SEC_IF
    assert iflags & (1 << 5)  # MS_IF (half and full boundary both crossed)


def test_rtc_minute_rollover(lib):
    def setup(ptr):
        bw(ptr, lib, RTC + 0x14, tr(0, 0, 0, 59))
        bw(ptr, lib, RTC + 0x00, CFG_LOAD_EN)
        bw(ptr, lib, RTC + 0x10, 0)  # period=1: every core clock is a second
        bw(ptr, lib, RTC + 0x00, CFG_RTC_EN)

    def inspect(ptr):
        return br(ptr, lib, RTC + 0x20), br(ptr, lib, RTC + 0x08)

    res = go(lib, [NOP] * 1 + [BKPT], setup=setup, inspect=inspect)
    tstr, iflags = res.extra
    assert tstr_min(tstr) == 1  # minute advanced
    assert tstr_sec(tstr) == 0  # second wrapped to 0
    assert iflags & (1 << 1)  # MIN_IF


def test_rtc_load_valid_time_and_date(lib):
    def setup(ptr):
        bw(ptr, lib, RTC + 0x14, tr(3, 13, 45, 7))
        bw(ptr, lib, RTC + 0x18, dr(26, 11, 6))
        bw(ptr, lib, RTC + 0x00, CFG_LOAD_EN)

    def inspect(ptr):
        rtc = ptr.contents.periph.rtc
        return rtc.weekday, rtc.hour, rtc.min, rtc.sec, rtc.year, rtc.month, rtc.date, (
            br(ptr, lib, RTC + 0x08) & (1 << 8)
        )

    res = go(lib, [NOP, BKPT], setup=setup, inspect=inspect)
    weekday, hour, minute, sec, year, month, date, time_err = res.extra
    assert (weekday, hour, minute, sec) == (3, 13, 45, 7)
    assert (year, month, date) == (26, 11, 6)
    assert not time_err


def test_rtc_load_invalid_sets_time_err(lib):
    def setup(ptr):
        bw(ptr, lib, RTC + 0x14, tr(0, 25, 0, 0))  # hour=25, invalid
        bw(ptr, lib, RTC + 0x00, CFG_LOAD_EN)

    def inspect(ptr):
        return br(ptr, lib, RTC + 0x08) & (1 << 8), ptr.contents.periph.rtc.hour

    res = go(lib, [NOP, BKPT], setup=setup, inspect=inspect)
    time_err, hour = res.extra
    assert time_err
    assert hour == 0  # rejected; current time left unchanged


def test_rtc_alarm_fires_once_and_clears(lib):
    def setup(ptr):
        bw(ptr, lib, RTC + 0x14, tr(0, 0, 0, 58))
        bw(ptr, lib, RTC + 0x1C, ar(0, 0, 0, 59))  # one-shot (week_mask=0)
        bw(ptr, lib, RTC + 0x00, CFG_LOAD_EN)
        bw(ptr, lib, RTC + 0x10, 0)  # 1 core clock per second
        bw(ptr, lib, RTC + 0x00, CFG_RTC_EN | CFG_ALM_EN)

    def inspect(ptr):
        return br(ptr, lib, RTC + 0x08), br(ptr, lib, RTC + 0x00)

    res = go(lib, [NOP, BKPT], setup=setup, inspect=inspect)
    iflags, cfg = res.extra
    assert iflags & (1 << 4)  # ALM_IF
    assert not cfg & CFG_ALM_EN  # one-shot alarm auto-cleared


def test_rtc_disabled_does_not_tick(lib):
    def inspect(ptr):
        return br(ptr, lib, RTC + 0x20)

    res = go(lib, [NOP] * 10 + [BKPT], inspect=inspect)
    assert res.extra == 0


def test_rtc_irq_fires(lib):
    def setup(ptr):
        bw(ptr, lib, RTC + 0x10, 9)  # period=10
        bw(ptr, lib, RTC + 0x04, 1)  # SEC_IE
        bw(ptr, lib, RTC + 0x00, CFG_RTC_EN)

    res = go(
        lib,
        [str_(1, 0, 0), WFI, 0x2609, BKPT],
        regs={0: ISER, 1: 1 << 2, 2: RTC, 3: 1, 4: 0},
        handlers={V_IRQ0 + 4 * 2: [str_(3, 2, 0x08), str_(4, 2, 0x00), 0x3701, BX_LR]},
        setup=setup,
    )
    assert res.r[7] >= 1
    assert res.r[6] == 9


# ---- IWDT ------------------------------------------------------------


def test_iwdt_first_expiry_sets_if_and_reloads(lib):
    def setup(ptr):
        bw(ptr, lib, IWDT + 0x00, 5)  # LOAD
        bw(ptr, lib, IWDT + 0x08, 1)  # EN

    def inspect(ptr):
        w = ptr.contents.periph.iwdt
        return br(ptr, lib, IWDT + 0x0C), w.cnt, w.expired_once, w.ctrl

    res = go(lib, [NOP] * 5 + [BKPT], setup=setup, inspect=inspect)
    iflags, cnt, expired, ctrl = res.extra
    assert iflags & 1
    assert cnt == 5
    assert expired
    assert ctrl & 1  # no reset happened


def test_iwdt_second_expiry_without_feed_resets(lib):
    def setup(ptr):
        bw(ptr, lib, IWDT + 0x00, 2)
        bw(ptr, lib, IWDT + 0x08, 1)

    def inspect(ptr):
        return ptr.contents.periph.iwdt.ctrl

    res = go(lib, [NOP] * 6 + [BKPT], setup=setup, inspect=inspect)
    assert res.extra == 0  # reset cleared everything back to defaults


def test_iwdt_feed_clears_expired_flag(lib):
    data = emu.firmware([NOP] * 20 + [BKPT])
    ptr = lib.dp32g030_cpu_new(emu.to_buf(data), len(data))
    try:
        bw(ptr, lib, IWDT + 0x00, 5)
        bw(ptr, lib, IWDT + 0x08, 1)
        assert lib.dp32g030_cpu_run(ctypes.byref(ptr), 5) == emu.EMU_OK
        assert ptr.contents.periph.iwdt.expired_once
        bw(ptr, lib, IWDT + 0x10, 0x55)  # FEED
        assert not ptr.contents.periph.iwdt.expired_once
        assert lib.dp32g030_cpu_run(ctypes.byref(ptr), 5) == emu.EMU_OK
        assert ptr.contents.periph.iwdt.expired_once
        assert ptr.contents.periph.iwdt.ctrl & 1  # still no reset
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_iwdt_disabled_does_not_count(lib):
    def inspect(ptr):
        return ptr.contents.periph.iwdt.cnt

    res = go(lib, [NOP] * 10 + [BKPT], inspect=inspect)
    assert res.extra == 0xFFFF


def test_iwdt_irq_fires(lib):
    def setup(ptr):
        # LOAD must leave enough margin for exception entry/exit (~20
        # cycles) plus the handler itself, or the dog starves a second
        # time (unfed) before the handler returns, resetting the core.
        bw(ptr, lib, IWDT + 0x00, 1000)
        bw(ptr, lib, IWDT + 0x08, 3)  # INTEN | EN

    res = go(
        lib,
        [str_(1, 0, 0), WFI, 0x2609, BKPT],
        regs={0: ISER, 1: 1 << 1, 2: IWDT, 3: 1},
        handlers={V_IRQ0 + 4 * 1: [str_(3, 2, 0x0C), 0x3701, BX_LR]},
        setup=setup,
    )
    assert res.r[7] >= 1
    assert res.r[6] == 9


# ---- WWDT ------------------------------------------------------------


def test_wwdt_window_interrupt_fires(lib):
    def setup(ptr):
        bw(ptr, lib, WWDT + 0x00, (10 << 8) | 100)  # RST_LOAD=10, INT_LOAD=100
        bw(ptr, lib, WWDT + 0x08, 1 | (1 << 3))  # EN | INTEN

    res = go(
        lib,
        [str_(1, 0, 0), WFI, 0x2609, BKPT],
        regs={0: ISER, 1: 1 << 0, 2: WWDT, 3: 1, 4: 0},
        handlers={V_IRQ0: [str_(3, 2, 0x0C), str_(4, 2, 0x08), 0x3701, BX_LR]},
        setup=setup,
        steps=2000,
    )
    assert res.r[7] >= 1
    assert res.r[6] == 9


def test_wwdt_pre_reset_interrupt_fires(lib):
    def setup(ptr):
        bw(ptr, lib, WWDT + 0x00, (10 << 8) | 100)
        bw(ptr, lib, WWDT + 0x08, 1 | (1 << 4))  # EN | PRERSTINTEN

    res = go(
        lib,
        [str_(1, 0, 0), WFI, 0x2609, BKPT],
        regs={0: ISER, 1: 1 << 0, 2: WWDT, 3: 1 << 1, 4: 0},
        handlers={V_IRQ0: [str_(3, 2, 0x0C), str_(4, 2, 0x08), 0x3701, BX_LR]},
        setup=setup,
        steps=2000,
    )
    assert res.r[7] >= 1
    assert res.r[6] == 9


def test_wwdt_reset_at_rst_load(lib):
    def setup(ptr):
        bw(ptr, lib, WWDT + 0x00, (10 << 8) | 100)
        bw(ptr, lib, WWDT + 0x08, 1)  # EN, no interrupts

    def inspect(ptr):
        return ptr.contents.periph.wwdt.ctrl, ptr.contents.periph.wwdt.value

    res = go(lib, [WFI, 0x2609, BKPT], setup=setup, inspect=inspect, steps=2000)
    ctrl, value = res.extra
    assert ctrl == 0  # reset wiped it back to defaults
    assert value == 0x7F


def test_wwdt_feed_inside_window_reloads(lib):
    ptr = lib.dp32g030_cpu_new(emu.to_buf(emu.firmware([BKPT])), 10)
    try:
        bw(ptr, lib, WWDT + 0x00, (10 << 8) | 100)
        ptr.contents.periph.wwdt.value = 50  # inside the window (100 > 50 > 10)
        bw(ptr, lib, WWDT + 0x10, 0x55)  # FEED
        assert ptr.contents.periph.wwdt.value == 0x7F
        assert not ptr.contents.periph.wwdt.ctrl  # EN was never set; still no reset
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_wwdt_feed_outside_window_resets(lib):
    def setup(ptr):
        bw(ptr, lib, WWDT + 0x00, (10 << 8) | 100)
        bw(ptr, lib, WWDT + 0x08, 1)
        ptr.contents.periph.wwdt.value = 5  # below RST_LOAD=10

    main = [str_(5, 0, 0x10), BKPT]  # STR r5,[r0,#0x10] -> write FEED

    def inspect(ptr):
        return ptr.contents.periph.wwdt.ctrl

    res = go(lib, main, setup=setup, inspect=inspect, regs={0: WWDT, 5: 0x55})
    assert res.extra == 0  # fed outside the window -> reset


def test_wwdt_disabled_does_not_count(lib):
    def inspect(ptr):
        return ptr.contents.periph.wwdt.value

    res = go(lib, [NOP] * 10 + [BKPT], inspect=inspect)
    assert res.extra == 0x7F
