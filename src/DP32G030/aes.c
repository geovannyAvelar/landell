#include "periph.h"

#include <string.h>

/* ---- AES-128 block cipher (FIPS-197), from scratch ------------------ */

static const uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
    0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
    0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
    0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
    0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
    0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
    0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
    0xb0, 0x54, 0xbb, 0x16,
};

static uint8_t inv_sbox[256];
static const uint8_t rcon[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10,
                                 0x20, 0x40, 0x80, 0x1B, 0x36};

static uint8_t gmul(uint8_t a, uint8_t b) {
  uint8_t p = 0;

  for (int i = 0; i < 8; i++) {
    if (b & 1)
      p ^= a;
    uint8_t hi = a & 0x80;
    a <<= 1;
    if (hi)
      a ^= 0x1B;
    b >>= 1;
  }
  return p;
}

static void aes_init_tables(void) {
  static bool done = false;

  if (done)
    return;
  for (int i = 0; i < 256; i++)
    inv_sbox[sbox[i]] = (uint8_t)i;
  done = true;
}

/* 11 round keys of 4 words each, from the 128-bit key. */
static void key_expand(const uint32_t key[4], uint32_t w[44]) {
  for (int i = 0; i < 4; i++)
    w[i] = key[i];
  for (int i = 4; i < 44; i++) {
    uint32_t t = w[i - 1];

    if (i % 4 == 0) {
      t = (t << 8) | (t >> 24); /* RotWord */
      t = ((uint32_t)sbox[(t >> 24) & 0xFF] << 24) |
          ((uint32_t)sbox[(t >> 16) & 0xFF] << 16) |
          ((uint32_t)sbox[(t >> 8) & 0xFF] << 8) |
          (uint32_t)sbox[t & 0xFF];
      t ^= (uint32_t)rcon[i / 4] << 24;
    }
    w[i] = w[i - 4] ^ t;
  }
}

static void add_round_key(uint8_t s[16], const uint32_t w[4]) {
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      s[4 * c + r] ^= (uint8_t)(w[c] >> (24 - 8 * r));
}

static void sub_bytes(uint8_t s[16], bool inv) {
  for (int i = 0; i < 16; i++)
    s[i] = inv ? inv_sbox[s[i]] : sbox[s[i]];
}

static void shift_rows(uint8_t s[16], bool inv) {
  uint8_t t[16];

  memcpy(t, s, 16);
  for (int r = 1; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      int shift = inv ? (c - r + 4) % 4 : (c + r) % 4;
      s[4 * c + r] = t[4 * shift + r];
    }
}

static void mix_columns(uint8_t s[16], bool inv) {
  for (int c = 0; c < 4; c++) {
    uint8_t *a = &s[4 * c];
    uint8_t t[4] = {a[0], a[1], a[2], a[3]};

    if (!inv) {
      a[0] = (uint8_t)(gmul(t[0], 2) ^ gmul(t[1], 3) ^ t[2] ^ t[3]);
      a[1] = (uint8_t)(t[0] ^ gmul(t[1], 2) ^ gmul(t[2], 3) ^ t[3]);
      a[2] = (uint8_t)(t[0] ^ t[1] ^ gmul(t[2], 2) ^ gmul(t[3], 3));
      a[3] = (uint8_t)(gmul(t[0], 3) ^ t[1] ^ t[2] ^ gmul(t[3], 2));
    } else {
      a[0] = (uint8_t)(gmul(t[0], 14) ^ gmul(t[1], 11) ^ gmul(t[2], 13) ^
                       gmul(t[3], 9));
      a[1] = (uint8_t)(gmul(t[0], 9) ^ gmul(t[1], 14) ^ gmul(t[2], 11) ^
                       gmul(t[3], 13));
      a[2] = (uint8_t)(gmul(t[0], 13) ^ gmul(t[1], 9) ^ gmul(t[2], 14) ^
                       gmul(t[3], 11));
      a[3] = (uint8_t)(gmul(t[0], 11) ^ gmul(t[1], 13) ^ gmul(t[2], 9) ^
                       gmul(t[3], 14));
    }
  }
}

static void block_to_bytes(const uint32_t block[4], uint8_t s[16]) {
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      s[4 * i + j] = (uint8_t)(block[i] >> (24 - 8 * j));
}

static void bytes_to_block(const uint8_t s[16], uint32_t block[4]) {
  for (int i = 0; i < 4; i++)
    block[i] = ((uint32_t)s[4 * i] << 24) | ((uint32_t)s[4 * i + 1] << 16) |
               ((uint32_t)s[4 * i + 2] << 8) | (uint32_t)s[4 * i + 3];
}

static void aes128_encrypt(const uint32_t key[4], const uint32_t in[4],
                           uint32_t out[4]) {
  uint32_t w[44];
  uint8_t s[16];

  aes_init_tables();
  key_expand(key, w);
  block_to_bytes(in, s);
  add_round_key(s, &w[0]);
  for (int round = 1; round <= 9; round++) {
    sub_bytes(s, false);
    shift_rows(s, false);
    mix_columns(s, false);
    add_round_key(s, &w[4 * round]);
  }
  sub_bytes(s, false);
  shift_rows(s, false);
  add_round_key(s, &w[40]);
  bytes_to_block(s, out);
}

static void aes128_decrypt(const uint32_t key[4], const uint32_t in[4],
                           uint32_t out[4]) {
  uint32_t w[44];
  uint8_t s[16];

  aes_init_tables();
  key_expand(key, w);
  block_to_bytes(in, s);
  add_round_key(s, &w[40]);
  for (int round = 9; round >= 1; round--) {
    shift_rows(s, true);
    sub_bytes(s, true);
    add_round_key(s, &w[4 * round]);
    mix_columns(s, true);
  }
  shift_rows(s, true);
  sub_bytes(s, true);
  add_round_key(s, &w[0]);
  bytes_to_block(s, out);
}

/* The hardware's "key derivation" step: the last round key, used by its
   equivalent-inverse-cipher decryption fast path. We don't need it for
   decrypt_block above, but firmware reads it back from KEYRx. */
static void aes128_derive_key(const uint32_t key[4], uint32_t derived[4]) {
  uint32_t w[44];

  key_expand(key, w);
  memcpy(derived, &w[40], sizeof derived[0] * 4);
}

/* ---- register model -------------------------------------------------- */

#define AES_CR_ERRC (1u << 8)
#define AES_CR_CCFC (1u << 7)
#define AES_CR_CHMOD_MASK (3u << 5)
#define AES_CR_MODE_MASK (3u << 3)
#define AES_CR_DATATYPE_MASK (3u << 1)
#define AES_CR_EN (1u << 0)
#define AES_SR_WRERR (1u << 2)
#define AES_SR_RDERR (1u << 1)
#define AES_SR_CCF (1u << 0)

#define AES_CHMOD_ECB 0u
#define AES_CHMOD_CBC 1u
#define AES_CHMOD_CTR 2u
#define AES_MODE_ENCRYPT 0u
#define AES_MODE_KEYDERIVE 1u
#define AES_MODE_DECRYPT 2u
#define AES_MODE_KEYDERIVE_DECRYPT 3u

static void aes_ctr_increment(uint32_t iv[4]) {
  for (int i = 3; i >= 0; i--)
    if (++iv[i])
      break;
}

/* Process one 128-bit block once DINR has been written 4 times. */
static void aes_run_block(dp32g030_aes_t *a) {
  unsigned chmod = (a->cr & AES_CR_CHMOD_MASK) >> 5;
  unsigned mode = (a->cr & AES_CR_MODE_MASK) >> 3;
  uint32_t out[4];

  if (chmod == AES_CHMOD_CTR) {
    uint32_t keystream[4];

    aes128_encrypt(a->key, a->iv, keystream);
    for (int i = 0; i < 4; i++)
      out[i] = a->din[i] ^ keystream[i];
    aes_ctr_increment(a->iv);
  } else if (mode == AES_MODE_DECRYPT || mode == AES_MODE_KEYDERIVE_DECRYPT) {
    uint32_t plain[4];

    aes128_decrypt(a->key, a->din, plain);
    if (chmod == AES_CHMOD_CBC) {
      for (int i = 0; i < 4; i++) {
        out[i] = plain[i] ^ a->iv[i];
        a->iv[i] = a->din[i]; /* this block's ciphertext feeds the next */
      }
    } else {
      memcpy(out, plain, sizeof out);
    }
  } else {
    uint32_t block[4];

    if (chmod == AES_CHMOD_CBC)
      for (int i = 0; i < 4; i++)
        block[i] = a->din[i] ^ a->iv[i];
    else
      memcpy(block, a->din, sizeof block);
    aes128_encrypt(a->key, block, out);
    if (chmod == AES_CHMOD_CBC)
      memcpy(a->iv, out, sizeof a->iv);
  }
  memcpy(a->dout, out, sizeof a->dout);
  a->dout_count = 0;
  a->din_count = 0;
  a->sr |= AES_SR_CCF;
}

bool dp32g030_aes_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                       uint32_t off, unsigned size, uint32_t *out) {
  dp32g030_aes_t *a = &c->periph.aes;
  uint32_t v;

  if (size != 4)
    return false;
  switch (off) {
  case 0x00: v = a->cr; break;
  case 0x04: v = a->sr; break;
  case 0x08: v = 0; break; /* write-only */
  case 0x0C:
    if (!(a->sr & AES_SR_CCF) || a->dout_count >= 4) {
      a->sr |= AES_SR_RDERR;
      v = 0;
    } else {
      v = a->dout[a->dout_count++];
      if (a->dout_count == 4)
        a->sr &= ~AES_SR_CCF;
    }
    break;
  case 0x10: v = a->key[0]; break;
  case 0x14: v = a->key[1]; break;
  case 0x18: v = a->key[2]; break;
  case 0x1C: v = a->key[3]; break;
  case 0x20: v = a->iv[0]; break;
  case 0x24: v = a->iv[1]; break;
  case 0x28: v = a->iv[2]; break;
  case 0x2C: v = a->iv[3]; break;
  default: return dp32g030_regfile_read(c, r, off, size, out);
  }
  *out = v;
  return true;
}

bool dp32g030_aes_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                        uint32_t off, unsigned size, uint32_t val) {
  dp32g030_aes_t *a = &c->periph.aes;

  if (size != 4)
    return false;
  switch (off) {
  case 0x00: {
    bool was_en = (a->cr & AES_CR_EN) != 0;

    a->cr = val & (AES_CR_CHMOD_MASK | AES_CR_MODE_MASK | AES_CR_DATATYPE_MASK |
                   AES_CR_EN);
    if (val & AES_CR_ERRC)
      a->sr &= ~(AES_SR_WRERR | AES_SR_RDERR);
    if (val & AES_CR_CCFC)
      a->sr &= ~AES_SR_CCF;
    if (!was_en && (a->cr & AES_CR_EN)) {
      a->din_count = 0;
      a->dout_count = 0;
      unsigned mode = (a->cr & AES_CR_MODE_MASK) >> 3;
      if (mode == AES_MODE_KEYDERIVE) {
        aes128_derive_key(a->key, a->key);
        a->sr |= AES_SR_CCF;
        a->cr &= ~AES_CR_EN; /* auto-cleared by hardware */
      }
    }
    break;
  }
  case 0x04: break; /* read-only */
  case 0x08:
    if (!(a->cr & AES_CR_EN) || a->din_count >= 4) {
      a->sr |= AES_SR_WRERR;
      break;
    }
    a->din[a->din_count++] = val;
    if (a->din_count == 4) {
      unsigned mode = (a->cr & AES_CR_MODE_MASK) >> 3;

      if (mode != AES_MODE_KEYDERIVE)
        aes_run_block(a);
    }
    break;
  case 0x0C: break; /* read-only */
  case 0x10: a->key[0] = val; break;
  case 0x14: a->key[1] = val; break;
  case 0x18: a->key[2] = val; break;
  case 0x1C: a->key[3] = val; break;
  case 0x20: a->iv[0] = val; break;
  case 0x24: a->iv[1] = val; break;
  case 0x28: a->iv[2] = val; break;
  case 0x2C: a->iv[3] = val; break;
  default: return dp32g030_regfile_write(c, r, off, size, val);
  }
  return true;
}
