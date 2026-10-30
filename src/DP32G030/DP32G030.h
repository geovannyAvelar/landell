#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DP32G030_FLASH_BASE 0x00000000u
#define DP32G030_FLASH_SIZE (64 * 1024)
#define DP32G030_SRAM_BASE 0x20000000u
#define DP32G030_SRAM_SIZE (16 * 1024)

typedef enum {
  OP_UNDEF = 0,
  OP_LSLS_IMM,
  OP_LSRS_IMM,
  OP_ASRS_IMM,
  OP_ADDS_REG,
  OP_SUBS_REG,
  OP_ADDS_IMM,
  OP_SUBS_IMM,
  OP_MOVS_IMM,
  OP_MOVS_REG,
  OP_MOV_REG,
  OP_CMP_IMM,
  OP_CMP_REG,
  OP_CMN,
  OP_ANDS,
  OP_EORS,
  OP_ORRS,
  OP_BICS,
  OP_MVNS,
  OP_TST,
  OP_ADCS,
  OP_SBCS,
  OP_RSBS,
  OP_MULS,
  OP_LSLS_REG,
  OP_LSRS_REG,
  OP_ASRS_REG,
  OP_RORS_REG,
  OP_ADD_REG,
  OP_ADD_SP_IMM,
  OP_ADD_SP_SP_IMM,
  OP_SUB_SP_SP_IMM,
  OP_ADR,
  OP_LDR_IMM,
  OP_STR_IMM,
  OP_LDRB_IMM,
  OP_STRB_IMM,
  OP_LDRH_IMM,
  OP_STRH_IMM,
  OP_LDR_SP,
  OP_STR_SP,
  OP_LDR_LIT,
  OP_LDR_REG,
  OP_STR_REG,
  OP_LDRB_REG,
  OP_STRB_REG,
  OP_LDRH_REG,
  OP_STRH_REG,
  OP_LDRSB_REG,
  OP_LDRSH_REG,
  OP_LDM,
  OP_STM,
  OP_PUSH,
  OP_POP,
  OP_B_COND,
  OP_B,
  OP_BL,
  OP_BX,
  OP_BLX_REG,
  OP_SXTB,
  OP_SXTH,
  OP_UXTB,
  OP_UXTH,
  OP_REV,
  OP_REV16,
  OP_REVSH,
  OP_NOP,
  OP_YIELD,
  OP_WFE,
  OP_WFI,
  OP_SEV,
  OP_SVC,
  OP_BKPT,
  OP_UDF,
  OP_CPSID,
  OP_CPSIE,
  OP_MRS,
  OP_MSR,
  OP_DMB,
  OP_DSB,
  OP_ISB,
  OP_COUNT,
} dp32g030_op_t;

typedef enum {
  EMU_OK = 0,
  EMU_FAULT,
  EMU_UNDEF,
} dp32g030_status_t;

typedef struct {
  dp32g030_op_t op;
  uint8_t rd, rn, rm;
  uint32_t imm;
  uint8_t cond;
  uint16_t reglist;
  uint8_t size;
} dp32g030_insn_t;

typedef struct {
  uint32_t r[16];
  uint32_t insn_pc;
  uint32_t xpsr;
  uint32_t primask;
  uint32_t control;
  bool halted;
  bool sleeping;
  uint8_t flash[DP32G030_FLASH_SIZE];
  uint8_t sram[DP32G030_SRAM_SIZE];
} dp32g030_cpu_t;

dp32g030_cpu_t *dp32g030_cpu_new(const uint8_t *fw, size_t size);
uint8_t dp32g030_cpu_run(dp32g030_cpu_t **c, size_t size);
void dp32g030_cpu_free(dp32g030_cpu_t *c);
