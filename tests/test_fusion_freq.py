"""Reproduce F4HWN Fusion frequency stickiness on the PY32 board."""
from __future__ import annotations

import ctypes
from pathlib import Path

import pytest

import dp32g030 as emu

FUSION = Path("/tmp/f4hwn.fusion.v6.3.0.bin")

# GUI keypad matrix: driven PB6..PB3, sensed PB15..PB12.
MATRIX = [
    ["menu", "1", "4", "7"],
    ["up", "2", "5", "8"],
    ["down", "3", "6", "9"],
    ["exit", "star", "0", "f"],
]


class Py32Keypad:
    def __init__(self, lib, cpu):
        self.lib = lib
        self.cpu = cpu
        self.down = set()
        self.refreshing = False

    def set(self, name: str, pressed: bool):
        if pressed:
            self.down.add(name)
        else:
            self.down.discard(name)
        self.refresh()

    def refresh(self):
        if self.refreshing:
            return
        self.refreshing = True
        level = self.lib.dp32g030_gpio_pins(self.cpu, 1)
        direction = self.lib.dp32g030_gpio_dir(self.cpu, 1)
        cols = 0xF
        for r in range(4):
            pin = 1 << (6 - r)
            driven_low = (direction & pin) and not (level & pin)
            if not driven_low:
                continue
            for c in range(4):
                if MATRIX[r][c] in self.down:
                    cols &= ~(1 << c)
        if "side1" in self.down:
            cols &= ~1
        if "side2" in self.down:
            cols &= ~2
        mask = value = 0
        for c in range(4):
            pin = 1 << (15 - c)
            mask |= pin
            if cols & (1 << c):
                value |= pin
        self.lib.dp32g030_gpio_set_input(self.cpu, 1, mask, value)
        ptt = 1 << 10
        self.lib.dp32g030_gpio_set_input(
            self.cpu, 1, ptt, 0 if "ptt" in self.down else ptt
        )
        self.refreshing = False

    def on_gpio(self, port, _before, _after):
        if port == 1:
            self.refresh()


def flash_u32(lib, cpu, offset: int) -> int:
    buf = (ctypes.c_uint8 * 4)()
    lib.dp32g030_extflash_get(cpu, offset, buf, 4)
    return int.from_bytes(bytes(buf), "little")


@pytest.mark.skipif(not FUSION.exists(), reason="Fusion firmware not downloaded")
def test_fusion_up_changes_and_saves_frequency(lib):
    raw = FUSION.read_bytes()
    data = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw)
    soc = lib.dp32g030_soc_by_name(b"py32f071")
    assert soc
    cpu = lib.dp32g030_cpu_new_soc(soc, data, len(raw))
    assert cpu

    freqs: list[int] = []
    pad = Py32Keypad(lib, cpu)

    def on_freq(_user, freq):
        freqs.append(freq)

    freq_cb = emu.BK4819_FREQ_CB(on_freq)
    gpio_cb = emu.GPIO_CB(lambda u, port, a, b: pad.on_gpio(port, a, b))
    cpu.contents.periph.hooks.bk4819_freq_change = freq_cb
    cpu.contents.periph.hooks.gpio_change = gpio_cb
    pad.refresh()
    lib.dp32g030_soc_defaults(cpu)

    # Boot to the main screen (Fusion needs a long run).
    status = lib.dp32g030_cpu_run(ctypes.byref(cpu), 400_000_000)
    assert status == 0, f"boot status {status}"
    assert freqs, "BK4819 never received a frequency"
    boot_raw = freqs[-1]
    # BAND6_400MHz == 5: Fusion VFO A for that band is at flash 0x90A0.
    band6_vfo_a = 0x90A0
    assert flash_u32(lib, cpu, band6_vfo_a) == 0xFFFFFFFF

    # Hold UP, then release so the deferred SaveChannel runs.
    pad.set("up", True)
    status = lib.dp32g030_cpu_run(ctypes.byref(cpu), 20_000_000)
    assert status == 0
    mid_raw = lib.dp32g030_bk4819_frequency(cpu)
    assert mid_raw != boot_raw, "UP did not change the tuned frequency"
    pad.set("up", False)
    status = lib.dp32g030_cpu_run(ctypes.byref(cpu), 50_000_000)
    assert status == 0

    saved = flash_u32(lib, cpu, band6_vfo_a)
    assert saved == mid_raw, (
        f"VFO flash {band6_vfo_a:#x}={saved:#x} did not keep mid frequency "
        f"{mid_raw:#x} (boot was {boot_raw:#x})"
    )
    # Survive a longer idle (ConfigureChannel reload + dual-watch toggles).
    status = lib.dp32g030_cpu_run(ctypes.byref(cpu), 100_000_000)
    assert status == 0
    assert flash_u32(lib, cpu, band6_vfo_a) == mid_raw, "saved VFO was erased later"

    lib.dp32g030_cpu_free(cpu)
