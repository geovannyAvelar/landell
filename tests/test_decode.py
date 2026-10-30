import pytest

from dp32g030 import Op, s32

CASES = [
    ("movs r1,#5", 0x2105, 0, "MOVS_IMM", 2, dict(rd=1, imm=5)),
    ("cmp r2,#9", 0x2A09, 0, "CMP_IMM", 2, dict(rn=2, imm=9)),
    ("adds r3,#7", 0x3307, 0, "ADDS_IMM", 2, dict(rd=3, rn=3, imm=7)),
    ("subs r3,#7", 0x3B07, 0, "SUBS_IMM", 2, dict(rd=3, rn=3, imm=7)),
    ("adds r0,r1,r2", 0x1888, 0, "ADDS_REG", 2, dict(rd=0, rn=1, rm=2)),
    ("subs r0,r1,r2", 0x1A88, 0, "SUBS_REG", 2, dict(rd=0, rn=1, rm=2)),
    ("adds r0,r1,#1", 0x1C48, 0, "ADDS_IMM", 2, dict(rd=0, rn=1, imm=1)),
    ("subs r0,r1,#1", 0x1E48, 0, "SUBS_IMM", 2, dict(rd=0, rn=1, imm=1)),
    ("lsls r0,r1,#1", 0x0048, 0, "LSLS_IMM", 2, dict(rd=0, rn=1, imm=1)),
    ("lsrs r0,r1,#1", 0x0848, 0, "LSRS_IMM", 2, dict(rd=0, rn=1, imm=1)),
    ("lsrs r0,r1,#32", 0x0808, 0, "LSRS_IMM", 2, dict(rd=0, rn=1, imm=32)),
    ("asrs r0,r1,#4", 0x1108, 0, "ASRS_IMM", 2, dict(rd=0, rn=1, imm=4)),
    ("asrs r0,r1,#32", 0x1008, 0, "ASRS_IMM", 2, dict(rd=0, rn=1, imm=32)),
    ("movs r0,r0 (lsls #0)", 0x0000, 0, "MOVS_REG", 2, dict(rd=0, rm=0)),
    ("movs r2,r3", 0x001A, 0, "MOVS_REG", 2, dict(rd=2, rm=3)),
    ("ands r0,r1", 0x4008, 0, "ANDS", 2, dict(rd=0, rn=0, rm=1)),
    ("eors r0,r1", 0x4048, 0, "EORS", 2, dict(rd=0, rn=0, rm=1)),
    ("lsls r0,r1", 0x4088, 0, "LSLS_REG", 2, dict(rd=0, rn=0, rm=1)),
    ("lsrs r0,r1", 0x40C8, 0, "LSRS_REG", 2, dict(rd=0, rn=0, rm=1)),
    ("asrs r0,r1", 0x4108, 0, "ASRS_REG", 2, dict(rd=0, rn=0, rm=1)),
    ("adcs r0,r1", 0x4148, 0, "ADCS", 2, dict(rd=0, rn=0, rm=1)),
    ("sbcs r0,r1", 0x4188, 0, "SBCS", 2, dict(rd=0, rn=0, rm=1)),
    ("rors r0,r1", 0x41C8, 0, "RORS_REG", 2, dict(rd=0, rn=0, rm=1)),
    ("tst r0,r1", 0x4208, 0, "TST", 2, dict(rn=0, rm=1)),
    ("rsbs r0,r1,#0", 0x4248, 0, "RSBS", 2, dict(rd=0, rn=1, imm=0)),
    ("cmp r0,r1", 0x4288, 0, "CMP_REG", 2, dict(rn=0, rm=1)),
    ("cmn r0,r1", 0x42C8, 0, "CMN", 2, dict(rn=0, rm=1)),
    ("orrs r0,r1", 0x4308, 0, "ORRS", 2, dict(rd=0, rn=0, rm=1)),
    ("muls r0,r1,r0", 0x4348, 0, "MULS", 2, dict(rd=0, rn=1, rm=0)),
    ("bics r0,r1", 0x4388, 0, "BICS", 2, dict(rd=0, rn=0, rm=1)),
    ("mvns r0,r1", 0x43C8, 0, "MVNS", 2, dict(rd=0, rm=1)),
    ("add r0,pc", 0x4478, 0, "ADD_REG", 2, dict(rd=0, rn=0, rm=15)),
    ("add r8,r1", 0x4488, 0, "ADD_REG", 2, dict(rd=8, rn=8, rm=1)),
    ("cmp r8,r1", 0x4588, 0, "CMP_REG", 2, dict(rn=8, rm=1)),
    ("mov r0,r1", 0x4608, 0, "MOV_REG", 2, dict(rd=0, rm=1)),
    ("mov r8,r9", 0x46C8, 0, "MOV_REG", 2, dict(rd=8, rm=9)),
    ("bx lr", 0x4770, 0, "BX", 2, dict(rm=14)),
    ("blx r1", 0x4788, 0, "BLX_REG", 2, dict(rm=1)),
    ("ldr r0,[pc,#8]", 0x4802, 0, "LDR_LIT", 2, dict(rd=0, imm=8)),
    ("str r0,[r1,r2]", 0x5088, 0, "STR_REG", 2, dict(rd=0, rn=1, rm=2)),
    ("strh r0,[r1,r2]", 0x5288, 0, "STRH_REG", 2, dict(rd=0, rn=1, rm=2)),
    ("strb r0,[r1,r2]", 0x5488, 0, "STRB_REG", 2, dict(rd=0, rn=1, rm=2)),
    ("ldrsb r0,[r1,r2]", 0x5688, 0, "LDRSB_REG", 2, dict(rd=0, rn=1, rm=2)),
    ("ldr r0,[r1,r2]", 0x5888, 0, "LDR_REG", 2, dict(rd=0, rn=1, rm=2)),
    ("ldrh r0,[r1,r2]", 0x5A88, 0, "LDRH_REG", 2, dict(rd=0, rn=1, rm=2)),
    ("ldrb r0,[r1,r2]", 0x5C88, 0, "LDRB_REG", 2, dict(rd=0, rn=1, rm=2)),
    ("ldrsh r0,[r1,r2]", 0x5E88, 0, "LDRSH_REG", 2, dict(rd=0, rn=1, rm=2)),
    ("str r0,[r1]", 0x6008, 0, "STR_IMM", 2, dict(rd=0, rn=1, imm=0)),
    ("ldr r0,[r1,#4]", 0x6848, 0, "LDR_IMM", 2, dict(rd=0, rn=1, imm=4)),
    ("strb r0,[r1,#1]", 0x7048, 0, "STRB_IMM", 2, dict(rd=0, rn=1, imm=1)),
    ("ldrb r0,[r1,#1]", 0x7848, 0, "LDRB_IMM", 2, dict(rd=0, rn=1, imm=1)),
    ("strh r0,[r1,#2]", 0x8048, 0, "STRH_IMM", 2, dict(rd=0, rn=1, imm=2)),
    ("ldrh r0,[r1,#2]", 0x8848, 0, "LDRH_IMM", 2, dict(rd=0, rn=1, imm=2)),
    ("str r0,[sp,#4]", 0x9001, 0, "STR_SP", 2, dict(rd=0, imm=4)),
    ("ldr r0,[sp,#4]", 0x9801, 0, "LDR_SP", 2, dict(rd=0, imm=4)),
    ("adr r0,#8", 0xA002, 0, "ADR", 2, dict(rd=0, imm=8)),
    ("add r0,sp,#8", 0xA802, 0, "ADD_SP_IMM", 2, dict(rd=0, imm=8)),
    ("add sp,#8", 0xB002, 0, "ADD_SP_SP_IMM", 2, dict(imm=8)),
    ("sub sp,#8", 0xB082, 0, "SUB_SP_SP_IMM", 2, dict(imm=8)),
    ("sxth r0,r1", 0xB208, 0, "SXTH", 2, dict(rd=0, rm=1)),
    ("sxtb r0,r1", 0xB248, 0, "SXTB", 2, dict(rd=0, rm=1)),
    ("uxth r0,r1", 0xB288, 0, "UXTH", 2, dict(rd=0, rm=1)),
    ("uxtb r0,r1", 0xB2C8, 0, "UXTB", 2, dict(rd=0, rm=1)),
    ("push {lr}", 0xB500, 0, "PUSH", 2, dict(reglist=0x4000)),
    ("push {r4-r7,lr}", 0xB5F0, 0, "PUSH", 2, dict(reglist=0x40F0)),
    ("cpsie i", 0xB662, 0, "CPSIE", 2, {}),
    ("cpsid i", 0xB672, 0, "CPSID", 2, {}),
    ("rev r0,r1", 0xBA08, 0, "REV", 2, dict(rd=0, rm=1)),
    ("rev16 r0,r1", 0xBA48, 0, "REV16", 2, dict(rd=0, rm=1)),
    ("revsh r0,r1", 0xBAC8, 0, "REVSH", 2, dict(rd=0, rm=1)),
    ("pop {pc}", 0xBD00, 0, "POP", 2, dict(reglist=0x8000)),
    ("pop {r4-r7,pc}", 0xBDF0, 0, "POP", 2, dict(reglist=0x80F0)),
    ("bkpt #0", 0xBE00, 0, "BKPT", 2, dict(imm=0)),
    ("bkpt #255", 0xBEFF, 0, "BKPT", 2, dict(imm=255)),
    ("nop", 0xBF00, 0, "NOP", 2, {}),
    ("yield", 0xBF10, 0, "YIELD", 2, {}),
    ("wfe", 0xBF20, 0, "WFE", 2, {}),
    ("wfi", 0xBF30, 0, "WFI", 2, {}),
    ("sev", 0xBF40, 0, "SEV", 2, {}),
    ("stmia r1!,{r0,r2}", 0xC105, 0, "STM", 2, dict(rn=1, reglist=0x05)),
    ("ldmia r3!,{r4,r5}", 0xCB30, 0, "LDM", 2, dict(rn=3, reglist=0x30)),
    ("beq .", 0xD0FE, 0, "B_COND", 2, dict(cond=0, imm=-4)),
    ("bne .", 0xD1FE, 0, "B_COND", 2, dict(cond=1, imm=-4)),
    ("bhi +2", 0xD801, 0, "B_COND", 2, dict(cond=8, imm=2)),
    ("blt +2", 0xDB01, 0, "B_COND", 2, dict(cond=0xB, imm=2)),
    ("ble .", 0xDDFE, 0, "B_COND", 2, dict(cond=0xD, imm=-4)),
    ("udf #3", 0xDE03, 0, "UDF", 2, dict(imm=3)),
    ("svc #5", 0xDF05, 0, "SVC", 2, dict(imm=5)),
    ("b .", 0xE7FE, 0, "B", 2, dict(imm=-4)),
    ("b +2", 0xE001, 0, "B", 2, dict(imm=2)),
    ("bl +0", 0xF000, 0xF800, "BL", 4, dict(imm=0)),
    ("bl +4", 0xF000, 0xF802, "BL", 4, dict(imm=4)),
    ("bl -4", 0xF7FF, 0xFFFE, "BL", 4, dict(imm=-4)),
    ("msr primask,r1", 0xF381, 0x8810, "MSR", 4, dict(rn=1, imm=0x10)),
    ("mrs r0,primask", 0xF3EF, 0x8010, "MRS", 4, dict(rd=0, imm=0x10)),
    ("dsb", 0xF3BF, 0x8F4F, "DSB", 4, {}),
    ("dmb", 0xF3BF, 0x8F5F, "DMB", 4, {}),
    ("isb", 0xF3BF, 0x8F6F, "ISB", 4, {}),
    ("udf.w", 0xF7F0, 0xA000, "UDF", 4, {}),
]


@pytest.mark.parametrize(
    "hw1,hw2,op,size,fields",
    [c[1:] for c in CASES],
    ids=[c[0] for c in CASES],
)
def test_decode(lib, hw1, hw2, op, size, fields):
    insn = lib.dp32g030_insn_decode(hw1, hw2)

    assert insn.op == getattr(Op, op)
    assert insn.size == size
    for name, expected in fields.items():
        got = getattr(insn, name)
        if name == "imm":
            assert s32(got) == s32(expected), f"imm: {s32(got)} != {s32(expected)}"
        else:
            assert got == expected, f"{name}: {got} != {expected}"


@pytest.mark.parametrize(
    "hw1,hw2",
    [(0xBB00, 0), (0xB100, 0), (0xFFFF, 0xFFFF), (0xE800, 0), (0xF000, 0x0000)],
    ids=["misc-unassigned", "misc-cbz", "prefix-11111", "prefix-11101", "bl-bad-hw2"],
)
def test_decode_undefined(lib, hw1, hw2):
    assert lib.dp32g030_insn_decode(hw1, hw2).op == Op.UNDEF


@pytest.mark.parametrize("hw1", [0xE800, 0xF000, 0xF7FF, 0xFFFF])
def test_decode_32bit_prefix_size(lib, hw1):
    assert lib.dp32g030_insn_decode(hw1, 0).size == 4


@pytest.mark.parametrize("hw1", [0x0000, 0x2105, 0xBF00, 0xDFFF, 0xE7FF])
def test_decode_16bit_size(lib, hw1):
    assert lib.dp32g030_insn_decode(hw1, 0).size == 2


@pytest.mark.parametrize("hw1", range(0x0000, 0xE800, 0x0101))
def test_decode_never_crashes(lib, hw1):
    insn = lib.dp32g030_insn_decode(hw1, 0)
    assert 0 <= insn.op < Op.COUNT
