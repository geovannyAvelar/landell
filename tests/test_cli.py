import os
import re
import struct
import subprocess

import pytest

import dp32g030 as emu

BIN = os.environ.get("LANDELL_BIN")

pytestmark = pytest.mark.skipif(not BIN, reason="LANDELL_BIN not set (run via meson test)")


def landell(*args):
    return subprocess.run([BIN, *map(str, args)], capture_output=True, text=True, timeout=30)


@pytest.fixture
def image(tmp_path):
    def _make(halfwords, name="fw.bin", **kw):
        path = tmp_path / name
        path.write_bytes(emu.firmware(halfwords, **kw))
        return path

    return _make


def reg(out, name):
    m = re.search(rf"\b{name}\s*=\s*([0-9a-f]{{8}})", out)
    assert m, f"{name} not found in:\n{out}"
    return int(m.group(1), 16)


def test_help():
    res = landell("--help")
    assert res.returncode == 0
    assert "usage:" in res.stdout
    assert "--steps" in res.stdout


def test_short_help():
    assert landell("-h").returncode == 0


def test_no_arguments_is_usage_error():
    res = landell()
    assert res.returncode == 1
    assert "usage:" in res.stderr


def test_unknown_option():
    res = landell("--bogus")
    assert res.returncode == 1
    assert "unknown option" in res.stderr


def test_two_files_rejected(image):
    a, b = image([0x2001], "a.bin"), image([0x2001], "b.bin")
    res = landell(a, b)
    assert res.returncode == 1
    assert "more than one" in res.stderr


def test_missing_file(tmp_path):
    res = landell(tmp_path / "nope.bin")
    assert res.returncode == 1
    assert "cannot read" in res.stderr


def test_image_too_short(tmp_path):
    path = tmp_path / "short.bin"
    path.write_bytes(b"abcd")
    res = landell(path)
    assert res.returncode == 1
    assert "not a valid image" in res.stderr


def test_image_too_large(tmp_path):
    path = tmp_path / "big.bin"
    path.write_bytes(bytes(emu.FLASH_SIZE + 1))
    res = landell(path)
    assert res.returncode == 1
    assert "not a valid image" in res.stderr


def test_runs_until_bkpt(image):
    res = landell(image([0x2005, 0x3003, 0xBE00]))
    assert res.returncode == 0
    assert res.stdout.startswith("halted")
    assert reg(res.stdout, "r0") == 8
    assert reg(res.stdout, "sp") == emu.SRAM_TOP
    assert reg(res.stdout, "pc") == emu.CODE_BASE + 6
    assert reg(res.stdout, "xpsr") == emu.XPSR_THUMB


def test_flags_are_printed(image):
    res = landell(image([0x2000, 0xBE00]))
    assert "[-Z--]" in res.stdout


def test_dumps_all_low_registers(image):
    res = landell(image([0x2111, 0x2222, 0x2733, 0xBE00]))
    assert reg(res.stdout, "r1") == 0x11
    assert reg(res.stdout, "r2") == 0x22
    assert reg(res.stdout, "r7") == 0x33
    for n in range(13):
        reg(res.stdout, f"r{n}")


@pytest.mark.parametrize("flag", ["-s", "--steps"])
def test_step_limit(image, flag):
    res = landell(flag, 5, image([0xE7FE]))
    assert res.returncode == 0
    assert res.stdout.startswith("step limit reached")
    assert reg(res.stdout, "pc") == emu.CODE_BASE


def test_step_limit_counts_instructions(image):
    res = landell("-s", 3, image([0x3001] * 10))
    assert reg(res.stdout, "r0") == 3


def test_steps_accepts_hex(image):
    res = landell("-s", "0x4", image([0x3001] * 10))
    assert reg(res.stdout, "r0") == 4


def test_options_after_file(image):
    res = landell(image([0x3001] * 10), "-s", 2)
    assert reg(res.stdout, "r0") == 2


@pytest.mark.parametrize("value", ["0", "-1", "abc", "12x", ""])
def test_invalid_steps(image, value):
    res = landell("-s", value, image([0x2001]))
    assert res.returncode == 1
    assert "invalid value for --steps" in res.stderr


def test_steps_missing_value():
    res = landell("-s")
    assert res.returncode == 1
    assert "invalid value for --steps" in res.stderr


def test_undefined_instruction(image):
    res = landell(image([0xFFFF, 0xFFFF]))
    assert res.returncode == 3
    assert "undefined instruction" in res.stdout
    assert reg(res.stdout, "pc") == emu.CODE_BASE


def test_fetch_outside_flash_is_fault(tmp_path):
    path = tmp_path / "bad.bin"
    path.write_bytes(struct.pack("<II", emu.SRAM_TOP, emu.SRAM_BASE | 1) + bytes(8))
    res = landell(path)
    assert res.returncode == 2
    assert "fault" in res.stdout


def test_memory_access_through_bus(image):
    prog = [0x2101, 0x0749, 0x205A, 0x6008, 0x680A, 0xBE00]
    res = landell(image(prog))
    assert res.returncode == 0
    assert reg(res.stdout, "r1") == emu.SRAM_BASE
    assert reg(res.stdout, "r2") == 0x5A


def test_uart_option_echoes_tx(tmp_path):
    # r0 = UART1 base (literal pool), r1 = 'A', then STR r1,[r0,#8] twice
    path = tmp_path / "uart.bin"
    code = [0x4802, 0x2141, 0x6081, 0x6081, emu.BKPT, 0xBF00]
    path.write_bytes(emu.firmware(code, extra=struct.pack("<I", 0x4006B800)))
    res = landell("-u", 1, path)
    assert res.stdout.startswith("AA")
    assert landell("-u", 0, path).stdout.startswith("halted")


def test_uart_option_rejects_bad_index(image):
    fw = image([emu.BKPT])
    assert landell("-u", 3, fw).returncode == 1


def test_soc_option_rejects_unknown_chip(image):
    res = landell("--soc", "bogus", image([0x2001]))
    assert res.returncode == 1
    assert "soc" in res.stderr


def test_soc_py32f071_rejects_a_dp32g030_image(image):
    res = landell("--soc", "py32f071", image([0x2001]))
    assert res.returncode == 1
    assert "py32f071" in res.stderr


def test_soc_dp32g030_explicit_runs(image):
    res = landell("--soc", "dp32g030", image([0x2001, 0xBE00]))
    assert res.returncode == 0


def test_soc_auto_picks_py32f071_from_the_reset_vector(tmp_path):
    path = tmp_path / "py32.bin"
    # app image: SP, reset 0x08002809 (entry at 0x08002808), then `movs r0,#1; bkpt`
    path.write_bytes(struct.pack("<II", emu.SRAM_TOP, 0x08002809) + struct.pack("<HH", 0x2001, 0xBE00))
    res = landell(path)
    assert res.returncode == 0, res.stderr
    assert reg(res.stdout, "r0") == 1
    assert reg(res.stdout, "pc") == 0x0800280C  # just past the bkpt


def test_soc_py32f071_error_says_which_image_it_wants(tmp_path):
    path = tmp_path / "boot.bin"
    path.write_bytes(struct.pack("<II", emu.SRAM_TOP, 0x08000109) + bytes(56))
    res = landell("--soc", "py32f071", path)
    assert res.returncode == 1
    assert "0x08002800" in res.stderr  # raw app image linked there
