"""BK4819: the GPIOC bit-bang protocol and the RX measurement path.

Mirrors the real firmware's BK4819_WriteRegister/BK4819_ReadRegister bit
sequences (see driver/bk4819.c in the uv-k5 custom firmware) so the tests
exercise the same protocol timing firmware actually uses.
"""

import ctypes

import dp32g030 as emu

GPIOC = 0x40061000
SCN, SCL, SDA = 1 << 0, 1 << 1, 1 << 2


def bw(ptr, lib, addr, val):
    assert lib.dp32g030_bus_write(ptr, addr, 4, val)


def br(ptr, lib, addr):
    out = ctypes.c_uint32()
    assert lib.dp32g030_bus_read(ptr, addr, 4, ctypes.byref(out))
    return out.value


def new_cpu(lib):
    return lib.dp32g030_cpu_new(emu.to_buf(emu.firmware([0xBE00])), 10)


class Link:
    """A from-scratch re-implementation of the firmware's bit-bang driver."""

    def __init__(self, ptr, lib):
        self.ptr, self.lib = ptr, lib
        self.out = 0
        self.dir = SCN | SCL | SDA
        bw(ptr, lib, GPIOC + 0x04, self.dir)
        self._set(SCN | SCL | SDA)

    def _set(self, bits):
        self.out |= bits
        bw(self.ptr, self.lib, GPIOC + 0x00, self.out)

    def _clear(self, bits):
        self.out &= ~bits
        bw(self.ptr, self.lib, GPIOC + 0x00, self.out)

    def _sda_dir(self, as_input):
        self.dir = (self.dir & ~SDA) if as_input else (self.dir | SDA)
        bw(self.ptr, self.lib, GPIOC + 0x04, self.dir)

    def _check_sda(self):
        return (br(self.ptr, self.lib, GPIOC + 0x00) & SDA) != 0

    def _write_bits(self, value, nbits):
        self._clear(SCL)
        for i in range(nbits):
            bit = (value >> (nbits - 1 - i)) & 1
            self._set(SDA) if bit else self._clear(SDA)
            self._set(SCL)
            self._clear(SCL)

    def _read_bits(self, nbits):
        self._sda_dir(True)
        value = 0
        for _ in range(nbits):
            value = (value << 1) | (1 if self._check_sda() else 0)
            self._set(SCL)
            self._clear(SCL)
        self._sda_dir(False)
        return value

    def write_register(self, addr, data):
        self._set(SCN)
        self._clear(SCL)
        self._clear(SCN)
        self._write_bits(addr & 0x7F, 8)
        self._write_bits(data, 16)
        self._set(SCN)
        self._set(SCL)
        self._set(SDA)

    def read_register(self, addr):
        self._set(SCN)
        self._clear(SCL)
        self._clear(SCN)
        self._write_bits((addr & 0x7F) | 0x80, 8)
        value = self._read_bits(16)
        self._set(SCN)
        self._set(SCL)
        self._set(SDA)
        return value


def test_write_then_read_back_a_register(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.write_register(0x36, 0xABCD)
        assert link.read_register(0x36) == 0xABCD
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_write_without_chip_select_is_ignored(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        # Toggle SCL without ever lowering SCN: no transaction starts.
        link._write_bits(0x3637, 24)
        assert link.read_register(0x37) == 0
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_two_consecutive_transactions(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.write_register(0x01, 0x1111)
        link.write_register(0x05, 0x2222)
        assert link.read_register(0x01) == 0x1111
        assert link.read_register(0x05) == 0x2222
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_frequency_write_notifies_host(lib):
    ptr = new_cpu(lib)
    try:
        seen = []
        ptr.contents.periph.hooks.bk4819_freq_change = emu.BK4819_FREQ_CB(
            lambda user, freq: seen.append(freq)
        )
        link = Link(ptr, lib)
        link.write_register(0x38, 0x5678)  # low 16 bits
        link.write_register(0x39, 0x1234)  # high 16 bits
        assert seen == [0x00005678, 0x12345678]
        assert lib.dp32g030_bk4819_frequency(ptr) == 0x12345678
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_squelch_opens_above_threshold(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.write_register(0x78, (150 << 8) | 140)  # open=150, close=140
        assert not lib.dp32g030_bk4819_squelch_open(ptr)
        lib.dp32g030_bk4819_set_signal(ptr, 160, 0, 0)
        assert lib.dp32g030_bk4819_squelch_open(ptr)
        assert link.read_register(0x02) & (1 << 3)  # SQUELCH_FOUND
        assert not link.read_register(0x02) & (1 << 3)  # cleared on read
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_squelch_has_hysteresis(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.write_register(0x78, (150 << 8) | 140)
        lib.dp32g030_bk4819_set_signal(ptr, 160, 0, 0)
        assert lib.dp32g030_bk4819_squelch_open(ptr)
        lib.dp32g030_bk4819_set_signal(ptr, 145, 0, 0)  # below open, above close
        assert lib.dp32g030_bk4819_squelch_open(ptr)  # stays open
        lib.dp32g030_bk4819_set_signal(ptr, 135, 0, 0)  # below close
        assert not lib.dp32g030_bk4819_squelch_open(ptr)
        assert link.read_register(0x02) & (1 << 2)  # SQUELCH_LOST
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_set_signal_reports_rssi_glitch_noise(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        lib.dp32g030_bk4819_set_signal(ptr, 321, 42, 100)
        assert link.read_register(0x67) == 321
        assert link.read_register(0x63) == 42
        assert link.read_register(0x65) == 100
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_ctcss_tone_match_sets_found(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.write_register(0x07, 2065)  # CTC1 mode, ~100.0 Hz
        assert not link.read_register(0x02) & (1 << 7)
        lib.dp32g030_bk4819_set_ctcss_tone(ptr, 1000)  # 100.0 Hz
        assert link.read_register(0x02) & (1 << 7)  # CTCSS_FOUND
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_ctcss_mismatch_does_not_set_found(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.write_register(0x07, 2065)  # ~100.0 Hz
        lib.dp32g030_bk4819_set_ctcss_tone(ptr, 1500)  # 150.0 Hz: way off
        assert not link.read_register(0x02) & (1 << 7)
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_ctcss_lost_when_tone_disappears(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.write_register(0x07, 2065)
        lib.dp32g030_bk4819_set_ctcss_tone(ptr, 1000)
        assert link.read_register(0x02) & (1 << 7)
        lib.dp32g030_bk4819_set_ctcss_tone(ptr, 0)
        assert link.read_register(0x02) & (1 << 6)  # CTCSS_LOST
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_cdcss_match_sets_found(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        code = 0x1A2B3C
        link.write_register(0x07, 2 << 13)  # CDCSS mode
        link.write_register(0x08, code & 0xFFF)  # low 12 bits
        link.write_register(0x08, 0x8000 | ((code >> 12) & 0xFFF))  # high 12 bits
        lib.dp32g030_bk4819_set_cdcss_code(ptr, code, True)
        assert link.read_register(0x02) & (1 << 9)  # CDCSS_FOUND
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_cdcss_mismatch_does_not_set_found(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        code = 0x1A2B3C
        link.write_register(0x07, 2 << 13)
        link.write_register(0x08, code & 0xFFF)
        link.write_register(0x08, 0x8000 | ((code >> 12) & 0xFFF))
        lib.dp32g030_bk4819_set_cdcss_code(ptr, code ^ 1, True)  # one bit off
        assert not link.read_register(0x02) & (1 << 9)
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_cdcss_requires_cdcss_mode(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        code = 0x1A2B3C
        link.write_register(0x07, 0)  # CTC1 mode, not CDCSS
        link.write_register(0x08, code & 0xFFF)
        link.write_register(0x08, 0x8000 | ((code >> 12) & 0xFFF))
        lib.dp32g030_bk4819_set_cdcss_code(ptr, code, True)
        assert not link.read_register(0x02) & (1 << 9)
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_cdcss_lost_when_signal_disappears(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        code = 0x1A2B3C
        link.write_register(0x07, 2 << 13)
        link.write_register(0x08, code & 0xFFF)
        link.write_register(0x08, 0x8000 | ((code >> 12) & 0xFFF))
        lib.dp32g030_bk4819_set_cdcss_code(ptr, code, True)
        assert link.read_register(0x02) & (1 << 9)
        lib.dp32g030_bk4819_set_cdcss_code(ptr, code, False)
        assert link.read_register(0x02) & (1 << 8)  # CDCSS_LOST
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_dtmf_digit_sets_code_and_found(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.write_register(0x24, (1 << 5) | (1 << 4))  # ENABLE | SELECT_DTMF
        assert lib.dp32g030_bk4819_signal_dtmf(ptr, b"7")
        assert (link.read_register(0x0B) >> 8) & 0xF == 7
        assert link.read_register(0x02) & (1 << 11)  # DTMF_5TONE_FOUND
        assert not link.read_register(0x02) & (1 << 11)  # cleared on read
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_dtmf_letters_and_symbols_map_correctly(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.write_register(0x24, (1 << 5) | (1 << 4))
        for digit, code in [(b"A", 10), (b"D", 13), (b"*", 14), (b"#", 15)]:
            assert lib.dp32g030_bk4819_signal_dtmf(ptr, digit)
            assert (link.read_register(0x0B) >> 8) & 0xF == code
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_dtmf_ignored_when_decoder_disabled(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        assert not lib.dp32g030_bk4819_signal_dtmf(ptr, b"5")
        assert not link.read_register(0x02) & (1 << 11)
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_dtmf_ignored_when_selected_for_selcall(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.write_register(0x24, 1 << 5)  # ENABLE but SELECT=SELCALL
        assert not lib.dp32g030_bk4819_signal_dtmf(ptr, b"5")
        assert not link.read_register(0x02) & (1 << 11)
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_dtmf_rejects_unknown_character(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.write_register(0x24, (1 << 5) | (1 << 4))
        assert not lib.dp32g030_bk4819_signal_dtmf(ptr, b"x")
        assert not link.read_register(0x02) & (1 << 11)
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_vox_activates_above_enable_threshold(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.write_register(0x46, 200)  # enable threshold
        link.write_register(0x79, 100)  # disable threshold
        link.write_register(0x31, 1 << 2)  # VOX enable
        assert not link.read_register(0x02) & (1 << 5)
        lib.dp32g030_bk4819_set_mic_level(ptr, 250)
        assert link.read_register(0x02) & (1 << 5)  # VOX_FOUND
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_vox_has_hysteresis(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.write_register(0x46, 200)
        link.write_register(0x79, 100)
        link.write_register(0x31, 1 << 2)
        lib.dp32g030_bk4819_set_mic_level(ptr, 250)
        assert link.read_register(0x02) & (1 << 5)
        lib.dp32g030_bk4819_set_mic_level(ptr, 150)  # below enable, above disable
        assert ptr.contents.periph.bk4819.vox_active  # stays active
        lib.dp32g030_bk4819_set_mic_level(ptr, 50)  # below disable
        assert not ptr.contents.periph.bk4819.vox_active
        assert link.read_register(0x02) & (1 << 4)  # VOX_LOST
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_vox_ignored_when_disabled(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.write_register(0x46, 200)
        link.write_register(0x79, 100)
        lib.dp32g030_bk4819_set_mic_level(ptr, 250)
        assert not ptr.contents.periph.bk4819.vox_active
        assert not link.read_register(0x02) & (1 << 5)
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_chip_gpio_output_change_notifies_host(lib):
    ptr = new_cpu(lib)
    try:
        seen = []
        ptr.contents.periph.hooks.bk4819_gpio_change = emu.BK4819_GPIO_CB(
            lambda user, state: seen.append(state)
        )
        link = Link(ptr, lib)
        link.write_register(0x33, 0x40)  # GPIO0 (RX enable) on
        link.write_register(0x33, 0x60)  # + GPIO1 (PA enable) on
        assert seen == [0x40, 0x60]
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_transaction_aborted_by_chip_select_rise_is_discarded(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link._set(SCN)
        link._clear(SCL)
        link._clear(SCN)
        link._write_bits(0x7F, 8)  # address only, no data
        link._set(SCN)  # abort before the 16 data bits
        link._set(SCL)
        link._set(SDA)
        assert link.read_register(0x7F) == 0
    finally:
        lib.dp32g030_cpu_free(ptr)
