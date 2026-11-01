"""Exceptions, NVIC, SCB and SysTick."""

import struct

import pytest

import dp32g030 as emu

TOP = emu.SRAM_TOP
CODE = 0x100
NOP, BKPT, BX_LR = 0xBF00, 0xBE00, 0x4770
WFI = 0xBF30
CPSID, CPSIE = 0xB672, 0xB662

SYST_CSR, SYST_RVR = 0xE000E010, 0xE000E014
ISER, ISPR, IPR0 = 0xE000E100, 0xE000E200, 0xE000E400
ICSR, AIRCR = 0xE000ED04, 0xE000ED0C

V_RESET, V_HARDFAULT, V_SVC, V_PENDSV, V_SYSTICK, V_IRQ0 = 4, 12, 44, 56, 60, 64
H0, H1 = 0x180, 0x1C0  # handler addresses


def image(main, handlers=None):
    """Vector table at 0, main at CODE, `handlers` maps vector offset -> halfwords."""
    data = bytearray(0x200)
    struct.pack_into("<II", data, 0, TOP, CODE | 1)
    addr = H0
    for vec, hws in (handlers or {}).items():
        struct.pack_into("<I", data, vec, addr | 1)
        for i, h in enumerate(hws):
            struct.pack_into("<H", data, addr + 2 * i, h)
        addr += 0x40
    for i, h in enumerate(main):
        struct.pack_into("<H", data, CODE + 2 * i, h)
    return bytes(data)


def go(lib, main, handlers=None, **kw):
    return emu.run_image(lib, image(main, handlers), **kw)


def test_svc_runs_handler_and_returns(lib):
    res = go(lib, [0xDF01, 0x2307, BKPT], {V_SVC: [0x2755, BX_LR]})
    assert res.r[7] == 0x55
    assert res.r[3] == 7
    assert res.sp == TOP
    assert not res.handler
    assert res.xpsr & 0x3F == 0
    assert res.active == 0


def test_return_restores_caller_saved_registers(lib):
    res = go(
        lib,
        [0xDF00, BKPT],
        {V_SVC: [0x2009, 0x2109, 0x2209, 0x2309, BX_LR]},
        regs={0: 1, 1: 2, 2: 3, 3: 4, 12: 5, 14: 0x1234},
    )
    assert res.r[:4] == [1, 2, 3, 4]
    assert res.r[12] == 5
    assert res.lr == 0x1234


def test_return_restores_flags(lib):
    res = go(lib, [0xDF00, BKPT], {V_SVC: [0x2000, BX_LR]}, flags=emu.FLAG_C | emu.FLAG_V)
    assert res.flags() == "--CV"


def test_stack_frame_layout(lib):
    res = go(
        lib,
        [0xDF00, BKPT],
        {V_SVC: [BKPT]},
        regs={0: 10, 1: 11, 2: 12, 3: 13, 12: 14, 14: 15},
        flags=emu.FLAG_Z,
    )
    base = TOP - 0x20 - emu.SRAM_BASE
    frame = struct.unpack_from("<8I", res.sram, base)
    assert frame[:6] == (10, 11, 12, 13, 14, 15)
    assert frame[6] == CODE + 2  # return address: after the SVC
    assert frame[7] & emu.FLAG_Z and frame[7] & emu.XPSR_THUMB
    assert res.sp == TOP - 0x20
    assert res.lr == 0xFFFFFFF9
    assert res.handler
    assert res.xpsr & 0x3F == 11


def test_stack_is_realigned_on_entry(lib):
    res = go(lib, [0xDF00, BKPT], {V_SVC: [BKPT]}, regs={13: TOP - 4})
    assert res.sp == TOP - 0x28
    frame = struct.unpack_from("<8I", res.sram, res.sp - emu.SRAM_BASE)
    assert frame[7] & (1 << 9)


def test_realigned_frame_is_unwound(lib):
    res = go(lib, [0xDF00, 0x2301, BKPT], {V_SVC: [BX_LR]}, regs={13: TOP - 4})
    assert res.sp == TOP - 4
    assert res.r[3] == 1


def test_irq_taken_when_enabled_and_pended(lib):
    res = go(
        lib,
        [0x6026, 0x602E, NOP, BKPT],
        {V_IRQ0: [0x2777, BX_LR]},
        regs={4: ISER, 5: ISPR, 6: 1},
    )
    assert res.r[7] == 0x77


def test_disabled_irq_is_not_taken(lib):
    res = go(
        lib,
        [0x602E, NOP, BKPT],
        {V_IRQ0: [0x2777, BX_LR]},
        regs={5: ISPR, 6: 1},
    )
    assert res.r[7] == 0
    assert res.pending == 1 << 16


def test_primask_blocks_irq_until_cpsie(lib):
    res = go(
        lib,
        [0x6026, CPSID, 0x602E, NOP, BKPT],
        {V_IRQ0: [0x2777, BX_LR]},
        regs={4: ISER, 5: ISPR, 6: 1},
    )
    assert res.r[7] == 0
    res = go(
        lib,
        [0x6026, CPSID, 0x602E, NOP, CPSIE, NOP, BKPT],
        {V_IRQ0: [0x2777, BX_LR]},
        regs={4: ISER, 5: ISPR, 6: 1},
    )
    assert res.r[7] == 0x77


NEST_REGS = {0: IPR0, 2: 3, 3: ISPR, 4: ISER, 5: 1, 6: 2, 7: 0}
NEST_MAIN = [0x6001, 0x6022, 0x601D, NOP, NOP, BKPT]
NEST_H0 = [0x013F, 0x3701, 0x601E, 0x013F, 0x3702, BX_LR]
NEST_H1 = [0x013F, 0x3705, BX_LR]


def test_higher_priority_irq_preempts(lib):
    # IRQ0 priority 0xC0, IRQ1 priority 0x40
    res = go(lib, NEST_MAIN, {V_IRQ0: NEST_H0, V_IRQ0 + 4: NEST_H1}, regs={**NEST_REGS, 1: 0x40C0})
    assert res.r[7] == 0x152


def test_equal_priority_irq_does_not_preempt(lib):
    res = go(lib, NEST_MAIN, {V_IRQ0: NEST_H0, V_IRQ0 + 4: NEST_H1}, regs={**NEST_REGS, 1: 0})
    assert res.r[7] == 0x125


def test_lower_numbered_irq_wins_a_tie(lib):
    main = [0x6001, 0x6022, 0x601D, NOP, BKPT]
    h = {V_IRQ0: [0x013F, 0x3701, BX_LR], V_IRQ0 + 4: [0x013F, 0x3702, BX_LR]}
    res = go(lib, main, h, regs={0: IPR0, 1: 0, 2: 3, 3: ISPR, 4: ISER, 5: 3, 7: 0})
    assert res.r[7] == 0x12


def test_pendsv_via_icsr(lib):
    res = go(lib, [0x6001, NOP, BKPT], {V_PENDSV: [0x2733, BX_LR]}, regs={0: ICSR, 1: 1 << 28})
    assert res.r[7] == 0x33


def test_svc_with_primask_escalates_to_hardfault(lib):
    res = go(lib, [CPSID, 0xDF00, BKPT], {V_SVC: [0x2701, BX_LR], V_HARDFAULT: [0x27AA, BKPT]})
    assert res.r[7] == 0xAA


def test_hardfault_handler_runs_with_faulting_pc(lib):
    res = go(
        lib,
        [NOP, 0x6808, BKPT],
        {V_HARDFAULT: [0x27AA, BKPT]},
        regs={1: 0x50000000},
    )
    assert res.r[7] == 0xAA
    assert res.xpsr & 0x3F == 3
    assert res.lr == 0xFFFFFFF9
    frame = struct.unpack_from("<8I", res.sram, res.sp - emu.SRAM_BASE)
    assert frame[6] == CODE + 2


def test_fault_in_hardfault_handler_locks_up(lib):
    res = go(
        lib,
        [0x6808, BKPT],
        {V_HARDFAULT: [0x6808, BKPT]},
        regs={1: 0x50000000},
    )
    assert res.halted and res.locked_up


def test_fault_without_handler_locks_up(lib):
    res = go(lib, [0x6808, BKPT], regs={1: 0x50000000})
    assert res.halted and res.locked_up


def test_bx_exc_return_in_thread_mode_faults(lib):
    res = go(lib, [0x4700 | (0 << 3), BKPT], {V_HARDFAULT: [0x27AA, BKPT]}, regs={0: 0xFFFFFFF9})
    assert res.r[7] == 0xAA


MSR_PSP_R0 = [0xF380, 0x8809]
MSR_CONTROL_R1 = [0xF381, 0x8814]
ISB = [0xF3BF, 0x8F6F]
PSP_TOP = emu.SRAM_BASE + 0x2000


def test_exception_from_psp_thread_uses_msp_and_psp_frame(lib):
    main = MSR_PSP_R0 + MSR_CONTROL_R1 + ISB + [0xDF00, BKPT]
    res = go(lib, main, {V_SVC: [BKPT]}, regs={0: PSP_TOP, 1: 2})
    assert res.lr == 0xFFFFFFFD
    assert res.handler and not res.using_psp
    assert res.psp == PSP_TOP - 0x20
    assert res.sp == TOP
    frame = struct.unpack_from("<8I", res.sram, PSP_TOP - 0x20 - emu.SRAM_BASE)
    assert frame[6] == CODE + 4 * 3 + 2


def test_return_to_psp_thread(lib):
    main = MSR_PSP_R0 + MSR_CONTROL_R1 + ISB + [0xDF00, 0x2303, BKPT]
    res = go(lib, main, {V_SVC: [BX_LR]}, regs={0: PSP_TOP, 1: 2})
    assert res.r[3] == 3
    assert res.using_psp and not res.handler
    assert res.sp == PSP_TOP
    assert res.msp == TOP
    assert res.control == 2


def test_mrs_msp_psp_follow_active_stack(lib):
    MRS_R2_MSP = [0xF3EF, 0x8208]
    MRS_R3_PSP = [0xF3EF, 0x8309]
    main = MSR_PSP_R0 + MSR_CONTROL_R1 + ISB + MRS_R2_MSP + MRS_R3_PSP + [BKPT]
    res = go(lib, main, regs={0: PSP_TOP, 1: 2})
    assert res.r[2] == TOP
    assert res.r[3] == PSP_TOP


def test_control_write_ignored_in_handler_mode(lib):
    main = [0xDF00, BKPT]
    res = go(lib, main + [], {V_SVC: MSR_CONTROL_R1 + [BKPT]}, regs={1: 2})
    assert res.control == 0 and not res.using_psp


def test_systick_interrupt(lib):
    res = go(
        lib,
        [0x6028, 0x6021] + [NOP] * 60 + [BKPT],
        {V_SYSTICK: [0x3701, 0x6022, BX_LR]},
        regs={0: 30, 1: 7, 2: 0, 4: SYST_CSR, 5: SYST_RVR},
    )
    assert res.r[7] == 1


def test_systick_polling_countflag(lib):
    # LOAD=3, CSR=5 (enable, core clock, no interrupt); read CSR until COUNTFLAG
    main = [0x6028, 0x6021, NOP, NOP, NOP, NOP, 0x6823, BKPT]
    res = go(lib, main, regs={0: 3, 1: 5, 4: SYST_CSR, 5: SYST_RVR})
    assert res.r[3] & (1 << 16)
    assert res.r[3] & 5 == 5


def test_systick_countflag_clears_on_read(lib):
    main = [0x6028, 0x6021] + [NOP] * 25 + [0x6823, 0x6826, BKPT]
    res = go(lib, main, regs={0: 20, 1: 5, 4: SYST_CSR, 5: SYST_RVR})
    assert res.r[3] & (1 << 16)
    assert not res.r[6] & (1 << 16)


def test_systick_disabled_does_not_fire(lib):
    res = go(
        lib,
        [0x6028, NOP, NOP, NOP, BKPT],
        {V_SYSTICK: [0x3701, BX_LR]},
        regs={0: 1, 5: SYST_RVR},
    )
    assert res.r[7] == 0


def test_wfi_sleeps_until_systick(lib):
    res = go(
        lib,
        [0x6028, 0x6021, WFI, 0x2609, BKPT],
        {V_SYSTICK: [0x3701, 0x6022, BX_LR]},
        regs={0: 100, 1: 7, 2: 0, 4: SYST_CSR, 5: SYST_RVR},
    )
    assert res.r[7] == 1
    assert res.r[6] == 9
    assert res.cycles >= 100


def test_wfi_with_no_wake_source_stops_cleanly(lib):
    res = go(lib, [WFI, 0x2609, BKPT])
    assert res.status == emu.EMU_OK
    assert res.sleeping and not res.halted
    assert res.r[6] == 0


def test_wfi_wakes_on_masked_pending_irq(lib):
    res = go(lib, [0x6026, CPSID, 0x602E, WFI, 0x2609, BKPT], {V_IRQ0: [0x2777, BX_LR]},
             regs={4: ISER, 5: ISPR, 6: 1})
    assert res.r[6] == 9
    assert res.r[7] == 0


def test_sev_wfe_does_not_sleep(lib):
    res = go(lib, [0xBF40, 0xBF20, 0x2609, BKPT])
    assert res.r[6] == 9 and not res.sleeping


def test_wfe_without_event_sleeps(lib):
    assert go(lib, [0xBF20, BKPT]).sleeping


def test_sysresetreq_restarts(lib):
    res = go(lib, [0x6001, BKPT], regs={0: AIRCR, 1: 0x05FA0004, 13: 0x20001000}, steps=2)
    assert res.sp == TOP  # SP reloaded from the vector table


def test_aircr_needs_vectkey(lib):
    res = go(lib, [0x6001, NOP, BKPT], regs={0: AIRCR, 1: 0x00000004, 13: 0x20001000})
    assert res.sp == 0x20001000


def test_scs_rejects_byte_access(lib):
    res = go(lib, [0x7808, BKPT], {V_HARDFAULT: [0x27AA, BKPT]}, regs={1: SYST_CSR})
    assert res.r[7] == 0xAA


def test_ipr_roundtrip_keeps_two_bits(lib):
    res = go(lib, [0x6001, 0x6802, BKPT], regs={0: IPR0, 1: 0xFFFFFFFF})
    assert res.r[2] == 0xC0C0C0C0


def test_cycle_counts(lib):
    res = go(lib, [NOP, NOP, 0x6808, BKPT], regs={1: emu.SRAM_BASE})
    assert res.cycles == 1 + 1 + 2


def test_taken_branch_costs_three(lib):
    res = go(lib, [0xE000, NOP, BKPT])  # B +0 -> next instruction
    assert res.cycles == 3
