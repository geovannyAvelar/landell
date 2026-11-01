import struct

import pytest

import dp32g030 as emu


def new(lib, data):
    return lib.dp32g030_cpu_new(emu.to_buf(data), len(data))


def test_new_rejects_null(lib):
    assert not lib.dp32g030_cpu_new(None, 16)


@pytest.mark.parametrize("size", [0, 4, 7])
def test_new_rejects_short_image(lib, size):
    assert not new(lib, bytes(size)) if size else not lib.dp32g030_cpu_new(None, 0)


def test_new_rejects_oversized_image(lib):
    assert not new(lib, bytes(emu.FLASH_SIZE + 1))


def test_new_accepts_full_flash(lib):
    ptr = new(lib, struct.pack("<II", emu.SRAM_TOP, 9) + bytes(emu.FLASH_SIZE - 8))
    assert ptr
    lib.dp32g030_cpu_free(ptr)


def test_initial_state(lib):
    data = emu.firmware([0x2001], sp=0x20003000, entry=0x10)
    ptr = new(lib, data)
    try:
        cpu = ptr.contents
        assert cpu.r[13] == 0x20003000
        assert cpu.r[15] == 0x10
        assert cpu.xpsr == emu.XPSR_THUMB
        assert cpu.primask == 0
        assert not cpu.halted
        assert not cpu.sleeping
        assert all(cpu.r[i] == 0 for i in range(13))
        assert bytes(cpu.flash[: len(data)]) == data
        assert set(cpu.flash[len(data) :]) == {0xFF}
        assert set(cpu.sram) == {0}
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_entry_bit0_is_cleared(lib):
    ptr = new(lib, struct.pack("<II", emu.SRAM_TOP, 0x0D))
    try:
        assert ptr.contents.r[15] == 0x0C
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_run_stops_on_bkpt(lib):
    res = emu.run(lib, [0x2001])
    assert res.status == emu.EMU_OK
    assert res.halted
    assert res.pc == emu.CODE_BASE + 4
    assert res.insn_pc == emu.CODE_BASE + 2


def test_run_step_limit_without_halt(lib):
    res = emu.run(lib, [0xE7FE], steps=50, append_bkpt=False)
    assert res.status == emu.EMU_OK
    assert not res.halted
    assert res.pc == emu.CODE_BASE


def test_run_step_count_is_exact(lib):
    res = emu.run(lib, [0x3001] * 10, steps=4, append_bkpt=False)
    assert res.r[0] == 4
    assert not res.halted


def test_run_undefined_instruction(lib):
    res = emu.run(lib, [0xFFFF, 0xFFFF], append_bkpt=False)
    assert res.status == emu.EMU_UNDEF


def test_run_entry_outside_flash_faults(lib):
    ptr = new(lib, struct.pack("<II", emu.SRAM_TOP, 0x10000000 | 1) + bytes(8))
    try:
        import ctypes

        assert lib.dp32g030_cpu_run(ctypes.byref(ptr), 10) == emu.EMU_FAULT
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_run_falls_off_end_of_flash(lib):
    nops = [0xBF00] * ((emu.FLASH_SIZE - emu.CODE_BASE) // 2)
    data = emu.firmware(nops)
    assert len(data) == emu.FLASH_SIZE
    ptr = new(lib, data)
    try:
        import ctypes

        assert lib.dp32g030_cpu_run(ctypes.byref(ptr), 100000) == emu.EMU_FAULT
    finally:
        lib.dp32g030_cpu_free(ptr)
