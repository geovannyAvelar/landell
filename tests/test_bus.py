import ctypes

import pytest

import dp32g030 as emu
from dp32g030 import FLASH_SIZE, SRAM_BASE, SRAM_SIZE, SRAM_TOP


@pytest.fixture
def cpu(lib):
    data = emu.firmware([0xBE00], extra=bytes([0, 0, 0xAA, 0xBB, 0xCC, 0xDD]))
    ptr = lib.dp32g030_cpu_new(emu.to_buf(data), len(data))
    yield ptr
    lib.dp32g030_cpu_free(ptr)


@pytest.fixture
def read(lib, cpu):
    def _read(addr, size):
        out = ctypes.c_uint32(0xDEADBEEF)
        ok = lib.dp32g030_bus_read(cpu, addr, size, ctypes.byref(out))
        return ok, out.value

    return _read


@pytest.fixture
def write(lib, cpu):
    return lambda addr, size, val: lib.dp32g030_bus_write(cpu, addr, size, val)


class TestSram:
    @pytest.mark.parametrize("size,val", [(1, 0xAB), (2, 0xABCD), (4, 0xDEADBEEF)])
    def test_roundtrip(self, read, write, size, val):
        assert write(SRAM_BASE + 0x100, size, val)
        assert read(SRAM_BASE + 0x100, size) == (True, val)

    def test_little_endian(self, cpu, write):
        assert write(SRAM_BASE, 4, 0x11223344)
        assert bytes(cpu.contents.sram[:4]) == bytes.fromhex("44332211")

    def test_partial_reads_of_word(self, read, write):
        write(SRAM_BASE, 4, 0x11223344)
        assert read(SRAM_BASE, 1) == (True, 0x44)
        assert read(SRAM_BASE + 1, 1) == (True, 0x33)
        assert read(SRAM_BASE + 2, 2) == (True, 0x1122)

    def test_narrow_write_keeps_neighbours(self, read, write):
        write(SRAM_BASE, 4, 0xFFFFFFFF)
        write(SRAM_BASE + 1, 1, 0x00)
        assert read(SRAM_BASE, 4) == (True, 0xFFFF00FF)

    def test_write_truncates_value(self, read, write):
        write(SRAM_BASE, 4, 0)
        write(SRAM_BASE, 1, 0x12345678)
        assert read(SRAM_BASE, 4) == (True, 0x78)

    def test_last_word_and_byte(self, read, write):
        assert write(SRAM_TOP - 4, 4, 0xCAFEBABE)
        assert read(SRAM_TOP - 4, 4) == (True, 0xCAFEBABE)
        assert read(SRAM_TOP - 1, 1) == (True, 0xCA)

    @pytest.mark.parametrize("addr,size", [(SRAM_TOP, 1), (SRAM_TOP - 2, 4), (SRAM_TOP + 4, 4)])
    def test_past_end_faults(self, read, write, addr, size):
        assert read(addr, size)[0] is False
        assert write(addr, size, 1) is False

    def test_just_below_base_faults(self, read):
        assert read(SRAM_BASE - 4, 4)[0] is False


class TestFlash:
    def test_read_appended_bytes(self, read):
        assert read(12, 4) == (True, 0xDDCCBBAA)

    def test_read_vector_table(self, read):
        assert read(0, 4) == (True, SRAM_TOP)
        assert read(4, 4) == (True, emu.CODE_BASE | 1)

    def test_read_erased_flash(self, read):
        assert read(FLASH_SIZE - 4, 4) == (True, 0xFFFFFFFF)
        assert read(FLASH_SIZE - 1, 1) == (True, 0xFF)

    def test_read_past_end_faults(self, read):
        assert read(FLASH_SIZE, 1)[0] is False
        assert read(FLASH_SIZE - 2, 4)[0] is False

    @pytest.mark.parametrize("size", [1, 2, 4])
    def test_write_faults(self, cpu, write, size):
        before = bytes(cpu.contents.flash[:16])
        assert write(0x10, size, 0) is False
        assert bytes(cpu.contents.flash[:16]) == before


class TestRules:
    @pytest.mark.parametrize("addr,size", [(SRAM_BASE + 1, 2), (SRAM_BASE + 2, 4), (SRAM_BASE + 3, 4), (1, 2), (2, 4)])
    def test_unaligned_faults(self, read, write, addr, size):
        assert read(addr, size)[0] is False
        assert write(addr, size, 0) is False

    @pytest.mark.parametrize("size", [0, 3, 5, 8])
    def test_bad_size_faults(self, read, write, size):
        assert read(SRAM_BASE, size)[0] is False
        assert write(SRAM_BASE, size, 0) is False

    @pytest.mark.parametrize("addr", [0x10000, 0x1FFFFFFC, 0x40000000, 0x40008000, 0xE000E010, 0xFFFFFFFC])
    def test_unmapped_faults(self, read, write, addr):
        assert read(addr, 4)[0] is False
        assert write(addr, 4, 1) is False

    def test_failed_read_leaves_out_untouched(self, read):
        assert read(0x40000000, 4) == (False, 0xDEADBEEF)

    def test_flash_and_sram_do_not_alias(self, read, write):
        write(SRAM_BASE, 4, 0x12345678)
        assert read(0, 4) == (True, SRAM_TOP)


class TestThroughCpu:
    def test_unaligned_ldr_faults(self, lib):
        res = emu.run(lib, [0x6808, 0x2301], regs={1: SRAM_BASE + 1})
        assert res.halted
        assert res.r[3] == 0

    def test_unmapped_str_faults(self, lib):
        res = emu.run(lib, [0x6008, 0x2301], regs={0: 1, 1: 0x40000000})
        assert res.halted
        assert res.r[3] == 0

    def test_store_to_flash_faults(self, lib):
        res = emu.run(lib, [0x6008, 0x2301], regs={0: 1, 1: 0x100})
        assert res.r[3] == 0

    def test_valid_access_continues(self, lib):
        res = emu.run(lib, [0x6008, 0x2301], regs={0: 1, 1: SRAM_BASE})
        assert res.r[3] == 1
