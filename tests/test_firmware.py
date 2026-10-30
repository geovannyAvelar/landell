import os
import shutil
import subprocess
from pathlib import Path

import pytest

import dp32g030 as emu

BIN = os.environ.get("LANDELL_BIN")
FIRMWARE_DIR = emu.ROOT / "firmware"
CROSS = os.environ.get("CROSS", "arm-none-eabi-")

pytestmark = [
    pytest.mark.skipif(not BIN, reason="LANDELL_BIN not set (run via meson test)"),
    pytest.mark.skipif(shutil.which(f"{CROSS}gcc") is None, reason=f"{CROSS}gcc not found"),
]

EXPECTED_R0 = (285 << 16) | 610


@pytest.fixture(scope="module")
def firmware_bin(tmp_path_factory):
    build = tmp_path_factory.mktemp("firmware")
    subprocess.run(
        ["make", "-C", str(FIRMWARE_DIR), f"BUILD={build}", f"CROSS={CROSS}"],
        check=True,
        capture_output=True,
    )
    return build / "firmware.bin"


def landell(*args):
    return subprocess.run([BIN, *map(str, args)], capture_output=True, text=True, timeout=30)


def test_firmware_computes_expected_result(firmware_bin):
    res = landell(firmware_bin)
    assert res.returncode == 0
    assert res.stdout.startswith("halted")
    assert f"r0  = {EXPECTED_R0:08x}" in res.stdout


def test_firmware_image_is_small(firmware_bin):
    assert 8 < Path(firmware_bin).stat().st_size < 2048


def test_firmware_through_library(lib, firmware_bin):
    data = Path(firmware_bin).read_bytes()
    ptr = lib.dp32g030_cpu_new(emu.to_buf(data), len(data))
    try:
        import ctypes

        status = lib.dp32g030_cpu_run(ctypes.byref(ptr), 100000)
        cpu = ptr.contents
        assert status == emu.EMU_OK
        assert cpu.halted
        assert cpu.r[0] == EXPECTED_R0
        assert cpu.r[13] <= emu.SRAM_TOP
    finally:
        lib.dp32g030_cpu_free(ptr)


def symbol(elf, name):
    out = subprocess.run(
        [f"{CROSS}nm", str(elf)], check=True, capture_output=True, text=True
    ).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise AssertionError(f"symbol {name} not found")


def test_firmware_initialises_data_and_sorts(lib, firmware_bin):
    elf = Path(firmware_bin).with_suffix(".elf")
    data_off = symbol(elf, "data") - emu.SRAM_BASE
    fib_off = symbol(elf, "fib") - emu.SRAM_BASE

    image = Path(firmware_bin).read_bytes()
    ptr = lib.dp32g030_cpu_new(emu.to_buf(image), len(image))
    try:
        import ctypes

        lib.dp32g030_cpu_run(ctypes.byref(ptr), 100000)
        sram = bytes(ptr.contents.sram)

        def words(off, n):
            return [int.from_bytes(sram[off + 4 * i : off + 4 * i + 4], "little") for i in range(n)]

        assert words(data_off, 8) == [1, 2, 3, 4, 6, 7, 8, 9]
        assert words(fib_off, 16) == [0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610]
    finally:
        lib.dp32g030_cpu_free(ptr)
