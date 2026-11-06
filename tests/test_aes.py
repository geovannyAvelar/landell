"""AES128: ECB/CBC/CTR against the NIST SP 800-38A test vectors."""

import ctypes

import dp32g030 as emu

AES = 0x400BD000

CR_EN = 1 << 0
CR_CCFC = 1 << 7
CR_ERRC = 1 << 8


def cr(chmod, mode, en=1):
    return (chmod << 5) | (mode << 3) | (1 if en else 0)


def words(hexstr):
    b = bytes.fromhex(hexstr)
    return [int.from_bytes(b[4 * i : 4 * i + 4], "big") for i in range(4)]


KEY = "2b7e151628aed2a6abf7158809cf4f3c"
BLOCK0 = "6bc1bee22e409f96e93d7e117393172a"
ECB_CT0 = "3ad77bb40d7a3660a89ecaf32466ef97"
CBC_IV = "000102030405060708090a0b0c0d0e0f"
CBC_CT0 = "7649abac8119b246cee98e9b12e9197d"
CTR_IV = "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"
CTR_CT0 = "874d6191b620e3261bef6864990db6ce"


def bw(ptr, lib, addr, val):
    assert lib.dp32g030_bus_write(ptr, addr, 4, val)


def br(ptr, lib, addr):
    out = ctypes.c_uint32()
    assert lib.dp32g030_bus_read(ptr, addr, 4, ctypes.byref(out))
    return out.value


def set_key(ptr, lib, key_hex):
    for i, w in enumerate(words(key_hex)):
        bw(ptr, lib, AES + 0x10 + 4 * i, w)


def set_iv(ptr, lib, iv_hex):
    for i, w in enumerate(words(iv_hex)):
        bw(ptr, lib, AES + 0x20 + 4 * i, w)


def run_block(ptr, lib, block_hex):
    for w in words(block_hex):
        bw(ptr, lib, AES + 0x08, w)
    assert br(ptr, lib, AES + 0x04) & 1  # CCF
    out = [br(ptr, lib, AES + 0x0C) for _ in range(4)]
    return "".join("%08x" % w for w in out)


def new_cpu(lib):
    return lib.dp32g030_cpu_new(emu.to_buf(emu.firmware([0xBE00])), 10)


def test_ecb_encrypt_matches_vector(lib):
    ptr = new_cpu(lib)
    try:
        set_key(ptr, lib, KEY)
        bw(ptr, lib, AES + 0x00, cr(0, 0))  # ECB, encrypt
        assert run_block(ptr, lib, BLOCK0) == ECB_CT0
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_ecb_decrypt_matches_vector(lib):
    ptr = new_cpu(lib)
    try:
        set_key(ptr, lib, KEY)
        bw(ptr, lib, AES + 0x00, cr(0, 2))  # ECB, decrypt
        assert run_block(ptr, lib, ECB_CT0) == BLOCK0
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_cbc_encrypt_matches_vector(lib):
    ptr = new_cpu(lib)
    try:
        set_key(ptr, lib, KEY)
        set_iv(ptr, lib, CBC_IV)
        bw(ptr, lib, AES + 0x00, cr(1, 0))  # CBC, encrypt
        assert run_block(ptr, lib, BLOCK0) == CBC_CT0
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_cbc_decrypt_matches_vector(lib):
    ptr = new_cpu(lib)
    try:
        set_key(ptr, lib, KEY)
        set_iv(ptr, lib, CBC_IV)
        bw(ptr, lib, AES + 0x00, cr(1, 2))  # CBC, decrypt
        assert run_block(ptr, lib, CBC_CT0) == BLOCK0
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_ctr_matches_vector(lib):
    ptr = new_cpu(lib)
    try:
        set_key(ptr, lib, KEY)
        set_iv(ptr, lib, CTR_IV)
        bw(ptr, lib, AES + 0x00, cr(2, 0))  # CTR
        assert run_block(ptr, lib, BLOCK0) == CTR_CT0
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_ctr_decrypt_is_the_same_operation(lib):
    # CTR is XOR with a keystream, so "decrypt" mode must give the same
    # result as encrypt mode for the same key/IV/block.
    ptr = new_cpu(lib)
    try:
        set_key(ptr, lib, KEY)
        set_iv(ptr, lib, CTR_IV)
        bw(ptr, lib, AES + 0x00, cr(2, 2))  # CTR, decrypt
        assert run_block(ptr, lib, BLOCK0) == CTR_CT0
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_key_derivation_sets_ccf_and_autoclears_en(lib):
    ptr = new_cpu(lib)
    try:
        set_key(ptr, lib, KEY)
        bw(ptr, lib, AES + 0x00, cr(0, 1))  # key derivation
        assert br(ptr, lib, AES + 0x04) & 1  # CCF
        assert not br(ptr, lib, AES + 0x00) & CR_EN
        derived = [br(ptr, lib, AES + 0x10 + 4 * i) for i in range(4)]
        assert derived != words(KEY)
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_write_while_disabled_sets_wrerr(lib):
    ptr = new_cpu(lib)
    try:
        bw(ptr, lib, AES + 0x08, 0)  # EN is 0
        assert br(ptr, lib, AES + 0x04) & (1 << 2)  # WRERR
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_read_dout_before_ready_sets_rderr(lib):
    ptr = new_cpu(lib)
    try:
        bw(ptr, lib, AES + 0x00, cr(0, 0))
        br(ptr, lib, AES + 0x0C)
        assert br(ptr, lib, AES + 0x04) & (1 << 1)  # RDERR
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_errc_clears_error_flags(lib):
    ptr = new_cpu(lib)
    try:
        bw(ptr, lib, AES + 0x08, 0)
        assert br(ptr, lib, AES + 0x04) & (1 << 2)
        bw(ptr, lib, AES + 0x00, CR_ERRC)
        assert not br(ptr, lib, AES + 0x04) & (1 << 2)
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_round_trip_through_registers(lib):
    # Encrypt then feed the ciphertext straight back in to decrypt.
    ptr = new_cpu(lib)
    try:
        set_key(ptr, lib, KEY)
        bw(ptr, lib, AES + 0x00, cr(0, 0))
        ct = run_block(ptr, lib, BLOCK0)
        bw(ptr, lib, AES + 0x00, cr(0, 2))
        assert run_block(ptr, lib, ct) == BLOCK0
    finally:
        lib.dp32g030_cpu_free(ptr)
