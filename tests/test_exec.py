import pytest

import dp32g030 as emu
from dp32g030 import FLAG_C, FLAG_N, FLAG_V, FLAG_Z, SRAM_BASE, run

MAX = 0xFFFFFFFF


def check(res, regs=None, flags=None):
    for idx, val in (regs or {}).items():
        assert res.r[idx] == val & MAX, f"r{idx}={res.r[idx]:#x}, want {val & MAX:#x}"
    if flags is not None:
        assert res.flags() == flags


class TestMoveAndCompare:
    def test_movs_imm(self, lib):
        res = run(lib, [0x2080])
        check(res, {0: 0x80}, "----")

    def test_movs_imm_zero_sets_z(self, lib):
        check(run(lib, [0x2000], regs={0: 5}), {0: 0}, "-Z--")

    def test_movs_reg_sets_n(self, lib):
        check(run(lib, [0x000A], regs={1: 0x80000000}), {2: 0x80000000}, "N---")

    def test_movs_keeps_carry(self, lib):
        check(run(lib, [0x2001], flags=FLAG_C), {0: 1}, "--C-")

    def test_mov_reg_keeps_flags(self, lib):
        check(run(lib, [0x4608], regs={1: 0}, flags=FLAG_N | FLAG_C), {0: 0}, "N-C-")

    def test_mov_reg_high(self, lib):
        check(run(lib, [0x46C8], regs={9: 0x1234}), {8: 0x1234})

    @pytest.mark.parametrize(
        "a,b,flags",
        [
            (5, 5, "-ZC-"),
            (5, 3, "--C-"),
            (3, 5, "N---"),
            (0x80000000, 1, "--CV"),
            (1, 0x80000000, "N--V"),
            (MAX, 1, "N-C-"),
        ],
    )
    def test_cmp_reg(self, lib, a, b, flags):
        res = run(lib, [0x4288], regs={0: a, 1: b})
        check(res, {0: a, 1: b}, flags)

    def test_cmp_imm(self, lib):
        check(run(lib, [0x2809], regs={0: 9}), {0: 9}, "-ZC-")

    def test_cmn(self, lib):
        check(run(lib, [0x42C8], regs={0: 1, 1: MAX}), {}, "-ZC-")

    def test_tst_does_not_write(self, lib):
        check(run(lib, [0x4208], regs={0: 0xF0, 1: 0x0F}), {0: 0xF0}, "-Z--")


class TestArithmetic:
    def test_adds_reg(self, lib):
        check(run(lib, [0x1888], regs={1: 3, 2: 4}), {0: 7}, "----")

    def test_adds_overflow(self, lib):
        check(run(lib, [0x3001], regs={0: 0x7FFFFFFF}), {0: 0x80000000}, "N--V")

    def test_adds_carry_and_zero(self, lib):
        check(run(lib, [0x3001], regs={0: MAX}), {0: 0}, "-ZC-")

    def test_adds_imm3(self, lib):
        check(run(lib, [0x1D48], regs={1: 10}), {0: 15}, "----")

    def test_subs_reg_borrow(self, lib):
        check(run(lib, [0x1A88], regs={1: 1, 2: 2}), {0: MAX}, "N---")

    def test_subs_reg_equal(self, lib):
        check(run(lib, [0x1A88], regs={1: 5, 2: 5}), {0: 0}, "-ZC-")

    def test_subs_overflow(self, lib):
        check(run(lib, [0x3801], regs={0: 0x80000000}), {0: 0x7FFFFFFF}, "--CV")

    def test_subs_imm3(self, lib):
        check(run(lib, [0x1FC8], regs={1: 20}), {0: 13}, "--C-")

    @pytest.mark.parametrize(
        "carry,expected,flags",
        [(0, 0x30, "----"), (FLAG_C, 0x31, "----")],
    )
    def test_adcs(self, lib, carry, expected, flags):
        res = run(lib, [0x4148], regs={0: 0x10, 1: 0x20}, flags=carry)
        check(res, {0: expected}, flags)

    @pytest.mark.parametrize(
        "carry,expected", [(FLAG_C, 0x10 - 0x08), (0, 0x10 - 0x08 - 1)]
    )
    def test_sbcs(self, lib, carry, expected):
        res = run(lib, [0x4188], regs={0: 0x10, 1: 0x08}, flags=carry)
        check(res, {0: expected})

    @pytest.mark.parametrize(
        "x,expected,flags", [(5, -5, "N---"), (0, 0, "-ZC-"), (0x80000000, 0x80000000, "N--V")]
    )
    def test_rsbs(self, lib, x, expected, flags):
        check(run(lib, [0x4248], regs={1: x}), {0: expected}, flags)

    def test_muls(self, lib):
        check(run(lib, [0x4348], regs={0: 6, 1: 7}), {0: 42, 1: 7}, "----")

    def test_muls_truncates(self, lib):
        check(run(lib, [0x4348], regs={0: 0x10000, 1: 0x10000}), {0: 0}, "-Z--")

    def test_add_reg_high_keeps_flags(self, lib):
        check(run(lib, [0x4488], regs={8: 5, 1: 6}, flags=FLAG_Z), {8: 11}, "-Z--")

    def test_add_sp_imm(self, lib):
        check(run(lib, [0xA802]), {0: emu.SRAM_TOP + 8})

    def test_add_sp_sp(self, lib):
        check(run(lib, [0xB002]), {13: emu.SRAM_TOP + 8})

    def test_sub_sp_sp(self, lib):
        check(run(lib, [0xB082]), {13: emu.SRAM_TOP - 8})

    def test_adr(self, lib):
        check(run(lib, [0xA002]), {0: ((emu.CODE_BASE + 4) & ~3) + 8})


class TestLogic:
    @pytest.mark.parametrize(
        "opcode,a,b,expected",
        [
            (0x4008, 0xFF0F, 0x0FF0, 0x0F00),
            (0x4048, 0xF0, 0xFF, 0x0F),
            (0x4308, 0xF0, 0x0F, 0xFF),
            (0x4388, 0xFF, 0x0F, 0xF0),
        ],
        ids=["ands", "eors", "orrs", "bics"],
    )
    def test_binary(self, lib, opcode, a, b, expected):
        check(run(lib, [opcode], regs={0: a, 1: b}), {0: expected}, "----")

    def test_mvns(self, lib):
        check(run(lib, [0x43C8], regs={1: 0}), {0: MAX}, "N---")

    def test_ands_zero(self, lib):
        check(run(lib, [0x4008], regs={0: 1, 1: 2}), {0: 0}, "-Z--")

    def test_logic_keeps_carry(self, lib):
        check(run(lib, [0x4008], regs={0: 1, 1: 1}, flags=FLAG_C | FLAG_V), {0: 1}, "--CV")


class TestShifts:
    def test_lsls_imm(self, lib):
        check(run(lib, [0x0108], regs={1: 0x10000001}), {0: 0x10}, "--C-")

    def test_lsls_imm_negative(self, lib):
        check(run(lib, [0x0048], regs={1: 0x40000000}), {0: 0x80000000}, "N---")

    def test_lsrs_imm(self, lib):
        check(run(lib, [0x0848], regs={1: 3}), {0: 1}, "--C-")

    def test_lsrs_imm_32(self, lib):
        check(run(lib, [0x0808], regs={1: 0x80000000}), {0: 0}, "-ZC-")

    def test_asrs_imm(self, lib):
        check(run(lib, [0x1108], regs={1: 0x80000000}), {0: 0xF8000000}, "N---")

    def test_asrs_imm_32_positive(self, lib):
        check(run(lib, [0x1008], regs={1: 0x7FFFFFFF}), {0: 0}, "-Z--")

    def test_asrs_imm_32_negative(self, lib):
        check(run(lib, [0x1008], regs={1: 0x80000000}), {0: MAX}, "N-C-")

    @pytest.mark.parametrize(
        "n,expected,flags",
        [(0, 1, "----"), (1, 2, "----"), (31, 0x80000000, "N---"), (32, 0, "-ZC-"), (33, 0, "-Z--"), (256, 1, "----")],
    )
    def test_lsls_reg(self, lib, n, expected, flags):
        check(run(lib, [0x4088], regs={0: 1, 1: n}), {0: expected}, flags)

    def test_lsls_reg_zero_keeps_carry(self, lib):
        check(run(lib, [0x4088], regs={0: 1, 1: 0}, flags=FLAG_C), {0: 1}, "--C-")

    @pytest.mark.parametrize(
        "n,expected,flags", [(1, 0x40000000, "----"), (31, 1, "----"), (32, 0, "-ZC-"), (40, 0, "-Z--")]
    )
    def test_lsrs_reg(self, lib, n, expected, flags):
        check(run(lib, [0x40C8], regs={0: 0x80000000, 1: n}), {0: expected}, flags)

    @pytest.mark.parametrize(
        "x,n,expected,flags",
        [
            (0x80000000, 4, 0xF8000000, "N---"),
            (0x80000000, 40, MAX, "N-C-"),
            (0x40000000, 40, 0, "-Z--"),
        ],
    )
    def test_asrs_reg(self, lib, x, n, expected, flags):
        check(run(lib, [0x4108], regs={0: x, 1: n}), {0: expected}, flags)

    @pytest.mark.parametrize(
        "n,expected,flags",
        [(1, 0x80000000, "N-C-"), (4, 0x10000000, "----"), (32, 1, "----"), (33, 0x80000000, "N-C-")],
    )
    def test_rors_reg(self, lib, n, expected, flags):
        check(run(lib, [0x41C8], regs={0: 1, 1: n}), {0: expected}, flags)


class TestExtendReverse:
    @pytest.mark.parametrize(
        "opcode,x,expected",
        [
            (0xB248, 0x80, 0xFFFFFF80),
            (0xB248, 0x7F, 0x7F),
            (0xB208, 0x8000, 0xFFFF8000),
            (0xB288, 0x12345678, 0x5678),
            (0xB2C8, 0x1234, 0x34),
            (0xBA08, 0x11223344, 0x44332211),
            (0xBA48, 0x11223344, 0x22114433),
            (0xBAC8, 0x000080FF, 0xFFFFFF80),
            (0xBAC8, 0x000012FF, 0xFFFFFF12),
        ],
        ids=["sxtb-neg", "sxtb-pos", "sxth", "uxth", "uxtb", "rev", "rev16", "revsh-neg", "revsh-ff"],
    )
    def test_ext_rev(self, lib, opcode, x, expected):
        check(run(lib, [opcode], regs={1: x}), {0: expected})


class TestBranch:
    def test_b_forward(self, lib):
        res = run(lib, [0x2001, 0xE000, 0x2002, 0x2109])
        check(res, {0: 1, 1: 9})

    def test_b_backward_countdown(self, lib):
        res = run(lib, [0x2003, 0x3801, 0xD1FD, 0x2105])
        check(res, {0: 0, 1: 5}, "--C-")

    def test_beq_taken(self, lib):
        check(run(lib, [0x2000, 0xD000, 0x2101, 0x2207]), {1: 0, 2: 7})

    def test_beq_not_taken(self, lib):
        check(run(lib, [0x2001, 0xD000, 0x2101, 0x2207]), {1: 1, 2: 7})

    @pytest.mark.parametrize(
        "cond,a,b,taken",
        [
            (0x0, 1, 1, True), (0x0, 1, 2, False),
            (0x1, 1, 2, True), (0x1, 1, 1, False),
            (0x2, 2, 1, True), (0x2, 1, 2, False),
            (0x3, 1, 2, True), (0x3, 2, 1, False),
            (0x4, 1, 2, True), (0x4, 2, 1, False),
            (0x5, 2, 1, True), (0x5, 1, 2, False),
            (0x6, 0x80000000, 1, True), (0x6, 2, 1, False),
            (0x7, 2, 1, True), (0x7, 0x80000000, 1, False),
            (0x8, MAX, 1, True), (0x8, 1, 1, False),
            (0x9, 1, 1, True), (0x9, MAX, 1, False),
            (0xA, 1, 1, True), (0xA, MAX, 1, False),
            (0xB, MAX, 1, True), (0xB, 1, 1, False),
            (0xC, 2, 1, True), (0xC, 1, 1, False),
            (0xD, 1, 1, True), (0xD, 2, 1, False),
        ],
    )
    def test_conditions_after_cmp(self, lib, cond, a, b, taken):
        branch = 0xD000 | (cond << 8)
        res = run(lib, [0x4288, branch, 0x2201, 0x2302], regs={0: a, 1: b})
        check(res, {2: 0 if taken else 1, 3: 2})

    def test_bl_sets_lr_and_jumps(self, lib):
        res = run(lib, [0xF000, 0xF801, 0x2001, 0x2102])
        check(res, {0: 0, 1: 2, 14: (emu.CODE_BASE + 4) | 1})

    def test_bl_and_bx_lr_return(self, lib):
        prog = [0xF000, 0xF802, 0x2209, 0xBE00, 0x2005, 0x4770]
        check(run(lib, prog), {0: 5, 2: 9})

    def test_bl_backward(self, lib):
        prog = [0xE001, 0x2005, 0x4770, 0xF7FF, 0xFFFC, 0x2209]
        res = run(lib, prog)
        check(res, {0: 5, 2: 9})

    def test_blx_reg(self, lib):
        prog = [0x4788, 0x2209, 0xBE00, 0x2005, 0x4770]
        target = (emu.CODE_BASE + 6) | 1
        res = run(lib, prog, regs={1: target})
        check(res, {0: 5, 2: 9, 14: (emu.CODE_BASE + 2) | 1})

    def test_bx_reg(self, lib):
        target = (emu.CODE_BASE + 6) | 1
        res = run(lib, [0x4708, 0x2101, 0xBE00, 0x2202], regs={1: target})
        check(res, {1: target, 2: 2})

    def test_mov_to_pc_branches(self, lib):
        target = emu.CODE_BASE + 6
        res = run(lib, [0x4687, 0x2101, 0xBE00, 0x2202], regs={0: target})
        check(res, {1: 0, 2: 2})


class TestSystem:
    def test_cpsid_cpsie(self, lib):
        assert run(lib, [0xB672]).primask == 1
        assert run(lib, [0xB662], ).primask == 0

    def test_cpsie_clears(self, lib):
        res = run(lib, [0xB672, 0xB662])
        assert res.primask == 0

    def test_wfi_sets_sleeping(self, lib):
        assert run(lib, [0xBF30]).sleeping

    def test_nop_changes_nothing(self, lib):
        res = run(lib, [0xBF00, 0xBF10, 0xBF40], regs={0: 7})
        check(res, {0: 7}, "----")
        assert not res.sleeping

    def test_barriers_are_nops(self, lib):
        res = run(lib, [0xF3BF, 0x8F4F, 0xF3BF, 0x8F5F, 0xF3BF, 0x8F6F, 0x2001])
        check(res, {0: 1})

    def test_msr_mrs_primask(self, lib):
        res = run(lib, [0xF381, 0x8810, 0xF3EF, 0x8010], regs={1: 1})
        check(res, {0: 1})
        assert res.primask == 1

    def test_msr_mrs_xpsr_flags(self, lib):
        res = run(lib, [0xF380, 0x8803, 0xF3EF, 0x8203], regs={0: 0xF0000000})
        assert res.flags() == "NZCV"
        assert res.r[2] == 0xF0000000 | emu.XPSR_THUMB

    def test_mrs_sp(self, lib):
        check(run(lib, [0xF3EF, 0x8008]), {0: emu.SRAM_TOP})

    def test_msr_sp_aligns(self, lib):
        check(run(lib, [0xF380, 0x8808], regs={0: SRAM_BASE + 0x1003}), {13: SRAM_BASE + 0x1000})


class TestMemory:
    def test_str_ldr_word(self, lib, requires_bus):
        res = run(lib, [0x6008, 0x680A], regs={0: 0xDEADBEEF, 1: SRAM_BASE})
        check(res, {2: 0xDEADBEEF})
        assert res.sram[:4] == bytes.fromhex("EFBEADDE")

    def test_str_ldr_offset(self, lib, requires_bus):
        res = run(lib, [0x6048, 0x6849], regs={0: 0x11223344, 1: SRAM_BASE})
        assert res.sram[4:8] == bytes.fromhex("44332211")
        check(res, {1: 0x11223344})

    def test_strb_ldrb(self, lib, requires_bus):
        res = run(lib, [0x7008, 0x780A], regs={0: 0x1234, 1: SRAM_BASE})
        check(res, {2: 0x34})
        assert res.sram[:2] == b"\x34\x00"

    def test_strh_ldrh(self, lib, requires_bus):
        res = run(lib, [0x8008, 0x880A], regs={0: 0x12345678, 1: SRAM_BASE})
        check(res, {2: 0x5678})

    def test_ldrsb_ldrsh_reg(self, lib, requires_bus):
        sram = bytes([0x80, 0x00, 0x00, 0x80])
        res = run(lib, [0x5688, 0x5E8B], regs={1: SRAM_BASE, 2: 0, 3: 2}, sram=sram)
        check(res, {0: 0xFFFFFF80})
        res = run(lib, [0x5E88], regs={1: SRAM_BASE, 2: 2}, sram=sram)
        check(res, {0: 0xFFFF8000})

    def test_reg_offset_forms(self, lib, requires_bus):
        res = run(lib, [0x5088, 0x5888], regs={0: 77, 1: SRAM_BASE, 2: 8})
        check(res, {0: 77})
        assert res.sram[8] == 77

    def test_sp_relative(self, lib, requires_bus):
        res = run(lib, [0xB082, 0x9001, 0x9A01], regs={0: 0xCAFE})
        check(res, {2: 0xCAFE, 13: emu.SRAM_TOP - 8})

    def test_ldr_literal(self, lib, requires_bus):
        res = run(lib, [0x4800, 0xBE00, 0xBEEF, 0xDEAD])
        check(res, {0: 0xDEADBEEF})

    def test_push_pop_roundtrip(self, lib, requires_bus):
        res = run(lib, [0xB403, 0xBC0C], regs={0: 0x11, 1: 0x22})
        check(res, {2: 0x11, 3: 0x22, 13: emu.SRAM_TOP})

    def test_push_layout(self, lib, requires_bus):
        res = run(lib, [0xB503], regs={0: 1, 1: 2, 14: 3})
        check(res, {13: emu.SRAM_TOP - 12})
        off = emu.SRAM_SIZE - 12
        assert res.sram[off : off + 12] == b"".join(v.to_bytes(4, "little") for v in (1, 2, 3))

    def test_pop_pc_returns(self, lib, requires_bus):
        target = (emu.CODE_BASE + 8) | 1
        res = run(lib, [0xB401, 0xBD00, 0x2101, 0xBE00, 0x2202], regs={0: target})
        check(res, {1: 0, 2: 2, 13: emu.SRAM_TOP})

    def test_stm_ldm(self, lib, requires_bus):
        prog = [0xC105, 0xCB30]
        res = run(lib, prog, regs={0: 1, 2: 2, 1: SRAM_BASE, 3: SRAM_BASE})
        check(res, {1: SRAM_BASE + 8, 3: SRAM_BASE + 8, 4: 1, 5: 2})

    def test_ldm_base_in_list_no_writeback(self, lib, requires_bus):
        sram = (0x55).to_bytes(4, "little") + (0x66).to_bytes(4, "little")
        res = run(lib, [0xC903], regs={1: SRAM_BASE}, sram=sram)
        check(res, {0: 0x55, 1: 0x66})
