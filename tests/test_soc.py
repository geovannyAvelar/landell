import ctypes
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


PY32_RESET_LOW, PY32_RESET_HIGH = 0x08000000, 0x0801FFFF


def py32_image(reset=0x08002D49, size=64):
    return struct.pack("<II", emu.SRAM_TOP, reset) + bytes(size - 8)


def test_cpu_new_auto_detects_py32(lib):
    data = py32_image()
    cpu = lib.dp32g030_cpu_new(emu.to_buf(data), len(data))
    try:
        assert cpu
        assert cpu.contents.soc == lib.dp32g030_soc_by_name(b"py32f071")
    finally:
        lib.dp32g030_cpu_free(cpu)


def test_detection_boundaries(lib):
    py32 = lib.dp32g030_soc_by_name(b"py32f071")
    dp32 = lib.dp32g030_soc_by_name(b"dp32g030")
    for reset, expected in [
        (PY32_RESET_LOW | 1, py32),
        (PY32_RESET_HIGH, py32),
        (PY32_RESET_HIGH + 2, dp32),  # just past the PY32 flash window
        (PY32_RESET_LOW - 1, dp32),
        (0x00000009, dp32),
        (0x20000101, dp32),  # code in SRAM stays a DP32G030 image
    ]:
        data = py32_image(reset)
        assert lib.dp32g030_soc_detect(emu.to_buf(data), len(data)) == expected, hex(reset)


def test_soc_detect_short_image_defaults_to_dp32g030(lib):
    data = bytes(4)
    assert lib.dp32g030_soc_detect(emu.to_buf(data), len(data)) == lib.dp32g030_soc_by_name(b"dp32g030")


def test_py32_rejects_image_with_foreign_reset_vector(lib):
    soc = lib.dp32g030_soc_by_name(b"py32f071")
    data = struct.pack("<II", emu.SRAM_TOP, 9) + bytes(56)  # a DP32G030 image
    assert not lib.dp32g030_cpu_new_soc(soc, emu.to_buf(data), len(data))
    out_of_window = py32_image(0x08040001)
    assert not lib.dp32g030_cpu_new_soc(soc, emu.to_buf(out_of_window), len(out_of_window))


def test_soc_name(lib):
    assert lib.dp32g030_soc_name(lib.dp32g030_soc_by_name(b"py32f071")) == b"py32f071"


def test_board_defaults_py32_preset_battery_and_calibration(lib):
    soc = lib.dp32g030_soc_by_name(b"py32f071")
    data = py32_image()
    cpu = lib.dp32g030_cpu_new_soc(soc, emu.to_buf(data), len(data))
    try:
        lib.dp32g030_soc_defaults(cpu)
        out = (ctypes.c_uint8 * 12)()
        lib.dp32g030_extflash_get(cpu, 0x010140, out, 12)
        cal = struct.unpack("<6H", bytes(out))
        assert cal[3] == 2000  # the voltage divisor must be sane
        assert 1500 <= cal[3] <= 3500
    finally:
        lib.dp32g030_cpu_free(cpu)
