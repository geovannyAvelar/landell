import struct

import dp32g030 as emu


def image(size):
    return struct.pack("<II", emu.SRAM_TOP, 9) + bytes(size - 8)


def test_soc_by_name(lib):
    assert lib.dp32g030_soc_by_name(b"dp32g030")
    assert not lib.dp32g030_soc_by_name(b"bogus")


def test_cpu_new_soc_size_limits(lib):
    soc = lib.dp32g030_soc_by_name(b"dp32g030")
    ok = image(emu.FLASH_SIZE)
    assert lib.dp32g030_cpu_new_soc(soc, emu.to_buf(ok), len(ok))
    big = image(emu.FLASH_SIZE + 1)
    assert not lib.dp32g030_cpu_new_soc(soc, emu.to_buf(big), len(big))
    small = bytes(7)
    assert not lib.dp32g030_cpu_new_soc(soc, emu.to_buf(small), len(small))


def test_cpu_records_its_soc(lib):
    soc = lib.dp32g030_soc_by_name(b"dp32g030")
    data = image(16)
    cpu = lib.dp32g030_cpu_new_soc(soc, emu.to_buf(data), len(data))
    try:
        assert cpu.contents.soc == soc
    finally:
        lib.dp32g030_cpu_free(cpu)


def test_cpu_new_defaults_to_dp32g030(lib):
    data = image(16)
    cpu = lib.dp32g030_cpu_new(emu.to_buf(data), len(data))
    try:
        assert cpu.contents.soc == lib.dp32g030_soc_by_name(b"dp32g030")
    finally:
        lib.dp32g030_cpu_free(cpu)
