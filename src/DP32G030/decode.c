#include "decode.h"

#include <stddef.h>
#include <stdint.h>

static const dp32g030_op_t dp32g030_alu_ops[16] = {
    OP_ANDS, OP_EORS,     OP_LSLS_REG, OP_LSRS_REG, OP_ASRS_REG, OP_ADCS,
    OP_SBCS, OP_RORS_REG, OP_TST,      OP_RSBS,     OP_CMP_REG,  OP_CMN,
    OP_ORRS, OP_MULS,     OP_BICS,     OP_MVNS,
};

static const dp32g030_op_t dp32g030_ldst_reg_ops[8] = {
    OP_STR_REG, OP_STRH_REG, OP_STRB_REG, OP_LDRSB_REG,
    OP_LDR_REG, OP_LDRH_REG, OP_LDRB_REG, OP_LDRSH_REG,
};

#define DP32G030_BITS(x, hi, lo)                                               \
  (((uint32_t)(x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))

static inline int32_t dp32g030_sext(uint32_t v, unsigned bits) {
  uint32_t m = 1u << (bits - 1);
  return (int32_t)((v ^ m) - m);
}

static void dp32g030_decode_shift_imm(uint16_t h, dp32g030_insn_t *in) {
  uint32_t imm5 = DP32G030_BITS(h, 10, 6);
  uint32_t kind = h >> 11;

  in->rd = DP32G030_BITS(h, 2, 0);
  in->rn = DP32G030_BITS(h, 5, 3);
  in->imm = imm5;

  if (kind == 0x00) {
    if (imm5 == 0) {
      in->op = OP_MOVS_REG;
      in->rm = in->rn;
    } else {
      in->op = OP_LSLS_IMM;
    }
    return;
  }

  in->op = (kind == 0x01) ? OP_LSRS_IMM : OP_ASRS_IMM;
  if (imm5 == 0)
    in->imm = 32;
}

static void dp32g030_decode_add_sub(uint16_t h, dp32g030_insn_t *in) {
  in->rd = DP32G030_BITS(h, 2, 0);
  in->rn = DP32G030_BITS(h, 5, 3);

  if (DP32G030_BITS(h, 10, 10)) {
    in->imm = DP32G030_BITS(h, 8, 6);
    in->op = DP32G030_BITS(h, 9, 9) ? OP_SUBS_IMM : OP_ADDS_IMM;
  } else {
    in->rm = DP32G030_BITS(h, 8, 6);
    in->op = DP32G030_BITS(h, 9, 9) ? OP_SUBS_REG : OP_ADDS_REG;
  }
}

static void dp32g030_decode_movs_imm(uint16_t h, dp32g030_insn_t *in) {
  in->op = OP_MOVS_IMM;
  in->rd = DP32G030_BITS(h, 10, 8);
  in->imm = DP32G030_BITS(h, 7, 0);
}

static void dp32g030_decode_cmp_imm(uint16_t h, dp32g030_insn_t *in) {
  in->op = OP_CMP_IMM;
  in->rn = DP32G030_BITS(h, 10, 8);
  in->imm = DP32G030_BITS(h, 7, 0);
}

static void dp32g030_decode_add_sub_imm8(uint16_t h, dp32g030_insn_t *in) {
  in->op = ((h >> 11) == 0x06) ? OP_ADDS_IMM : OP_SUBS_IMM;
  in->rd = in->rn = DP32G030_BITS(h, 10, 8);
  in->imm = DP32G030_BITS(h, 7, 0);
}

static void dp32g030_decode_alu(uint16_t h, dp32g030_insn_t *in) {
  uint32_t a = DP32G030_BITS(h, 2, 0);
  uint32_t b = DP32G030_BITS(h, 5, 3);

  in->op = dp32g030_alu_ops[DP32G030_BITS(h, 9, 6)];

  switch (in->op) {
  case OP_TST:
  case OP_CMP_REG:
  case OP_CMN:
    in->rn = a;
    in->rm = b;
    break;
  case OP_RSBS:
    in->rd = a;
    in->rn = b;
    in->imm = 0;
    break;
  case OP_MVNS:
    in->rd = a;
    in->rm = b;
    break;
  case OP_MULS:
    in->rd = a;
    in->rn = b;
    in->rm = a;
    break;
  default:
    in->rd = in->rn = a;
    in->rm = b;
    break;
  }
}

static void dp32g030_decode_special(uint16_t h, dp32g030_insn_t *in) {
  uint32_t r = (DP32G030_BITS(h, 7, 7) << 3) | DP32G030_BITS(h, 2, 0);

  in->rm = DP32G030_BITS(h, 6, 3);

  switch (DP32G030_BITS(h, 9, 8)) {
  case 0:
    in->op = OP_ADD_REG;
    in->rd = in->rn = r;
    break;
  case 1:
    if (DP32G030_BITS(h, 9, 6) != 4) {
      in->op = OP_CMP_REG;
      in->rn = r;
    }
    break;
  case 2:
    in->op = OP_MOV_REG;
    in->rd = r;
    break;
  case 3:
    in->op = DP32G030_BITS(h, 7, 7) ? OP_BLX_REG : OP_BX;
    break;
  }
}

static void dp32g030_decode_data_proc(uint16_t h, dp32g030_insn_t *in) {
  if (DP32G030_BITS(h, 10, 10))
    dp32g030_decode_special(h, in);
  else
    dp32g030_decode_alu(h, in);
}

static void dp32g030_decode_ldr_lit(uint16_t h, dp32g030_insn_t *in) {
  in->op = OP_LDR_LIT;
  in->rd = DP32G030_BITS(h, 10, 8);
  in->imm = DP32G030_BITS(h, 7, 0) << 2;
}

static void dp32g030_decode_ldst_reg(uint16_t h, dp32g030_insn_t *in) {
  in->op = dp32g030_ldst_reg_ops[DP32G030_BITS(h, 11, 9)];
  in->rd = DP32G030_BITS(h, 2, 0);
  in->rn = DP32G030_BITS(h, 5, 3);
  in->rm = DP32G030_BITS(h, 8, 6);
}

static void dp32g030_decode_ldst_imm(uint16_t h, dp32g030_insn_t *in) {
  static const dp32g030_op_t ops[6] = {
      OP_STR_IMM,  OP_LDR_IMM,  OP_STRB_IMM,
      OP_LDRB_IMM, OP_STRH_IMM, OP_LDRH_IMM,
  };
  static const uint8_t shift[6] = {2, 2, 0, 0, 1, 1};
  unsigned k = (h >> 11) - 0x0C;

  in->op = ops[k];
  in->rd = DP32G030_BITS(h, 2, 0);
  in->rn = DP32G030_BITS(h, 5, 3);
  in->imm = DP32G030_BITS(h, 10, 6) << shift[k];
}

static void dp32g030_decode_ldst_sp(uint16_t h, dp32g030_insn_t *in) {
  in->op = ((h >> 11) == 0x12) ? OP_STR_SP : OP_LDR_SP;
  in->rd = DP32G030_BITS(h, 10, 8);
  in->imm = DP32G030_BITS(h, 7, 0) << 2;
}

static void dp32g030_decode_adr_add_sp(uint16_t h, dp32g030_insn_t *in) {
  in->op = ((h >> 11) == 0x14) ? OP_ADR : OP_ADD_SP_IMM;
  in->rd = DP32G030_BITS(h, 10, 8);
  in->imm = DP32G030_BITS(h, 7, 0) << 2;
}

static void dp32g030_decode_misc(uint16_t h, dp32g030_insn_t *in) {
  if ((h & 0xFF00) == 0xB000) {
    in->op = (h & 0x80) ? OP_SUB_SP_SP_IMM : OP_ADD_SP_SP_IMM;
    in->imm = DP32G030_BITS(h, 6, 0) << 2;
    return;
  }

  switch (h & 0xFFC0) {
  case 0xB200:
    in->op = OP_SXTH;
    goto ext;
  case 0xB240:
    in->op = OP_SXTB;
    goto ext;
  case 0xB280:
    in->op = OP_UXTH;
    goto ext;
  case 0xB2C0:
    in->op = OP_UXTB;
    goto ext;
  case 0xBA00:
    in->op = OP_REV;
    goto ext;
  case 0xBA40:
    in->op = OP_REV16;
    goto ext;
  case 0xBAC0:
    in->op = OP_REVSH;
    goto ext;
  default:
    break;
  }

  if ((h & 0xFE00) == 0xB400) {
    in->op = OP_PUSH;
    in->reglist = DP32G030_BITS(h, 7, 0) | (DP32G030_BITS(h, 8, 8) << 14);
    return;
  }
  if ((h & 0xFE00) == 0xBC00) {
    in->op = OP_POP;
    in->reglist = DP32G030_BITS(h, 7, 0) | (DP32G030_BITS(h, 8, 8) << 15);
    return;
  }
  if ((h & 0xFF00) == 0xBE00) {
    in->op = OP_BKPT;
    in->imm = DP32G030_BITS(h, 7, 0);
    return;
  }

  switch (h) {
  case 0xB662:
    in->op = OP_CPSIE;
    break;
  case 0xB672:
    in->op = OP_CPSID;
    break;
  case 0xBF00:
    in->op = OP_NOP;
    break;
  case 0xBF10:
    in->op = OP_YIELD;
    break;
  case 0xBF20:
    in->op = OP_WFE;
    break;
  case 0xBF30:
    in->op = OP_WFI;
    break;
  case 0xBF40:
    in->op = OP_SEV;
    break;
  default:
    break;
  }
  return;

ext:
  in->rd = DP32G030_BITS(h, 2, 0);
  in->rm = DP32G030_BITS(h, 5, 3);
}

static void dp32g030_decode_ldm_stm(uint16_t h, dp32g030_insn_t *in) {
  in->op = ((h >> 11) == 0x18) ? OP_STM : OP_LDM;
  in->rn = DP32G030_BITS(h, 10, 8);
  in->reglist = DP32G030_BITS(h, 7, 0);
}

static void dp32g030_decode_cond_branch(uint16_t h, dp32g030_insn_t *in) {
  in->cond = DP32G030_BITS(h, 11, 8);

  if (in->cond == 0xE) {
    in->op = OP_UDF;
    in->imm = DP32G030_BITS(h, 7, 0);
  } else if (in->cond == 0xF) {
    in->op = OP_SVC;
    in->imm = DP32G030_BITS(h, 7, 0);
  } else {
    in->op = OP_B_COND;
    in->imm = (uint32_t)(dp32g030_sext(DP32G030_BITS(h, 7, 0), 8) * 2);
  }
}

static void dp32g030_decode_branch(uint16_t h, dp32g030_insn_t *in) {
  in->op = OP_B;
  in->imm = (uint32_t)(dp32g030_sext(DP32G030_BITS(h, 10, 0), 11) * 2);
}

static void dp32g030_decode32(uint16_t h1, uint16_t h2, dp32g030_insn_t *in) {
  in->size = 4;

  if ((h1 & 0xF800) == 0xF000 && (h2 & 0xD000) == 0xD000) {
    uint32_t s = DP32G030_BITS(h1, 10, 10);
    uint32_t j1 = DP32G030_BITS(h2, 13, 13);
    uint32_t j2 = DP32G030_BITS(h2, 11, 11);
    uint32_t i1 = !(j1 ^ s);
    uint32_t i2 = !(j2 ^ s);
    uint32_t v = (s << 24) | (i1 << 23) | (i2 << 22) |
                 (DP32G030_BITS(h1, 9, 0) << 12) |
                 (DP32G030_BITS(h2, 10, 0) << 1);
    in->op = OP_BL;
    in->imm = (uint32_t)dp32g030_sext(v, 25);
  } else if ((h1 & 0xFFF0) == 0xF380 && (h2 & 0xFF00) == 0x8800) {
    in->op = OP_MSR;
    in->rn = DP32G030_BITS(h1, 3, 0);
    in->imm = DP32G030_BITS(h2, 7, 0);
  } else if (h1 == 0xF3EF && (h2 & 0xF000) == 0x8000) {
    in->op = OP_MRS;
    in->rd = DP32G030_BITS(h2, 11, 8);
    in->imm = DP32G030_BITS(h2, 7, 0);
  } else if (h1 == 0xF3BF && (h2 & 0xFFF0) == 0x8F40) {
    in->op = OP_DSB;
  } else if (h1 == 0xF3BF && (h2 & 0xFFF0) == 0x8F50) {
    in->op = OP_DMB;
  } else if (h1 == 0xF3BF && (h2 & 0xFFF0) == 0x8F60) {
    in->op = OP_ISB;
  } else if ((h1 & 0xFFF0) == 0xF7F0 && (h2 & 0xF000) == 0xA000) {
    in->op = OP_UDF;
  }
}

dp32g030_insn_t dp32g030_insn_decode(uint16_t h, uint16_t h2) {
  dp32g030_insn_t in = {0};
  in.size = 2;

  switch (h >> 11) {
  case 0x00:
  case 0x01:
  case 0x02:
    dp32g030_decode_shift_imm(h, &in);
    break;
  case 0x03:
    dp32g030_decode_add_sub(h, &in);
    break;
  case 0x04:
    dp32g030_decode_movs_imm(h, &in);
    break;
  case 0x05:
    dp32g030_decode_cmp_imm(h, &in);
    break;
  case 0x06:
  case 0x07:
    dp32g030_decode_add_sub_imm8(h, &in);
    break;
  case 0x08:
    dp32g030_decode_data_proc(h, &in);
    break;
  case 0x09:
    dp32g030_decode_ldr_lit(h, &in);
    break;
  case 0x0A:
  case 0x0B:
    dp32g030_decode_ldst_reg(h, &in);
    break;
  case 0x0C:
  case 0x0D:
  case 0x0E:
  case 0x0F:
  case 0x10:
  case 0x11:
    dp32g030_decode_ldst_imm(h, &in);
    break;
  case 0x12:
  case 0x13:
    dp32g030_decode_ldst_sp(h, &in);
    break;
  case 0x14:
  case 0x15:
    dp32g030_decode_adr_add_sp(h, &in);
    break;
  case 0x16:
  case 0x17:
    dp32g030_decode_misc(h, &in);
    break;
  case 0x18:
  case 0x19:
    dp32g030_decode_ldm_stm(h, &in);
    break;
  case 0x1A:
  case 0x1B:
    dp32g030_decode_cond_branch(h, &in);
    break;
  case 0x1C:
    dp32g030_decode_branch(h, &in);
    break;
  default:
    dp32g030_decode32(h, h2, &in);
    break;
  }

  return in;
}
