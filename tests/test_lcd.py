"""LCD (ST7565-compatible), driven over the real SPI0 peripheral plus
GPIOB pin 9 (A0), the same way the real UV-K5 driver does.
"""

import ctypes

import dp32g030 as emu

GPIOB = 0x40060800
SPI0 = 0x400B8000
A0 = 1 << 9


def bw(ptr, lib, addr, val):
    assert lib.dp32g030_bus_write(ptr, addr, 4, val)


def br(ptr, lib, addr):
    out = ctypes.c_uint32()
    assert lib.dp32g030_bus_read(ptr, addr, 4, ctypes.byref(out))
    return out.value


def new_cpu(lib):
    return lib.dp32g030_cpu_new(emu.to_buf(emu.firmware([0xBE00])), 10)


class Link:
    """Mirrors ST7565_WriteByte/DrawLine: A0 low for a command byte,
    A0 high for a data byte, sent over SPI0 (master, enabled)."""

    def __init__(self, ptr, lib):
        self.ptr, self.lib = ptr, lib
        bw(ptr, lib, GPIOB + 0x04, A0)  # A0 pin as output
        bw(ptr, lib, SPI0 + 0x00, (1 << 3) | (1 << 6))  # SPE | MSTR

    def cmd(self, byte):
        bw(self.ptr, self.lib, GPIOB + 0x00, 0)
        bw(self.ptr, self.lib, SPI0 + 0x04, byte)

    def data(self, byte):
        bw(self.ptr, self.lib, GPIOB + 0x00, A0)
        bw(self.ptr, self.lib, SPI0 + 0x04, byte)

    def set_page_col(self, page, col):
        self.cmd(0xB0 | page)
        self.cmd(0x10 | ((col >> 4) & 0xF))
        self.cmd(0x00 | (col & 0xF))


def test_data_write_sets_ram_and_autoincrements(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.set_page_col(0, 0)
        link.data(0xAA)
        link.data(0x55)
        assert lib.dp32g030_lcd_ram(ptr, 0, 0) == 0xAA
        assert lib.dp32g030_lcd_ram(ptr, 0, 1) == 0x55
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_page_and_column_addressing(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.set_page_col(3, 82)
        link.data(0x7F)
        assert lib.dp32g030_lcd_ram(ptr, 3, 82) == 0x7F
        assert lib.dp32g030_lcd_ram(ptr, 3, 81) == 0
        assert lib.dp32g030_lcd_ram(ptr, 2, 82) == 0
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_display_on_off(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        assert not lib.dp32g030_lcd_is_on(ptr)
        link.cmd(0xAF)
        assert lib.dp32g030_lcd_is_on(ptr)
        link.cmd(0xAE)
        assert not lib.dp32g030_lcd_is_on(ptr)
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_get_pixel_reflects_ram_bits(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.cmd(0xAF)  # display on
        link.set_page_col(0, 0)
        link.data(0b00000011)  # rows 0 and 1 lit
        assert lib.dp32g030_lcd_get_pixel(ptr, 0, 0)
        assert lib.dp32g030_lcd_get_pixel(ptr, 0, 1)
        assert not lib.dp32g030_lcd_get_pixel(ptr, 0, 2)
        assert not lib.dp32g030_lcd_get_pixel(ptr, 1, 0)  # column 1 untouched
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_get_pixel_blank_while_display_off(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.set_page_col(0, 0)
        link.data(0xFF)
        assert not lib.dp32g030_lcd_get_pixel(ptr, 0, 0)  # display is off
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_seg_reverse_mirrors_columns(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.cmd(0xAF)
        link.cmd(0xA1)  # SEG direction: reverse
        link.set_page_col(0, 0)
        link.data(0x01)  # column 0 in RAM, row 0 lit
        assert not lib.dp32g030_lcd_get_pixel(ptr, 0, 0)
        assert lib.dp32g030_lcd_get_pixel(ptr, 131, 0)  # mirrored to the far edge
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_com_reverse_mirrors_rows(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.cmd(0xAF)
        link.cmd(0xC8)  # COM direction: reverse
        link.set_page_col(0, 0)
        link.data(0x01)  # row 0 in RAM
        assert not lib.dp32g030_lcd_get_pixel(ptr, 0, 0)
        assert lib.dp32g030_lcd_get_pixel(ptr, 0, 63)
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_inverse_display_flips_pixels(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.cmd(0xAF)
        link.set_page_col(0, 0)
        link.data(0x00)
        assert not lib.dp32g030_lcd_get_pixel(ptr, 0, 0)
        link.cmd(0xA7)  # inverse display on
        assert lib.dp32g030_lcd_get_pixel(ptr, 0, 0)
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_all_pixels_on_overrides_ram(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.cmd(0xAF)
        link.set_page_col(0, 0)
        link.data(0x00)
        assert not lib.dp32g030_lcd_get_pixel(ptr, 0, 0)
        link.cmd(0xA5)  # all pixels on
        assert lib.dp32g030_lcd_get_pixel(ptr, 0, 0)
        assert lib.dp32g030_lcd_get_pixel(ptr, 131, 63)
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_start_line_scrolls_vertically(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.cmd(0xAF)
        link.set_page_col(0, 0)
        link.data(0x01)  # logical row 0 lit
        assert lib.dp32g030_lcd_get_pixel(ptr, 0, 0)
        link.cmd(0x40 | 1)  # start line = 1: scroll up by one row
        assert not lib.dp32g030_lcd_get_pixel(ptr, 0, 0)
        assert lib.dp32g030_lcd_get_pixel(ptr, 0, 63)
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_contrast_is_a_two_byte_command(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.cmd(0x81)  # SET_EV
        link.cmd(31)  # the contrast level itself, also sent with A0 low
        assert ptr.contents.periph.lcd.contrast == 31
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_software_reset_clears_state(lib):
    ptr = new_cpu(lib)
    try:
        link = Link(ptr, lib)
        link.cmd(0xAF)
        link.set_page_col(2, 5)
        link.data(0xFF)
        link.cmd(0xE2)  # software reset
        assert not lib.dp32g030_lcd_is_on(ptr)
        assert lib.dp32g030_lcd_ram(ptr, 2, 5) == 0
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_update_hook_fires_on_every_byte(lib):
    ptr = new_cpu(lib)
    try:
        seen = []
        ptr.contents.periph.hooks.lcd_update = emu.LCD_UPDATE_CB(
            lambda user: seen.append(True)
        )
        link = Link(ptr, lib)
        link.cmd(0xAF)
        link.data(0x01)
        assert len(seen) == 2
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_disabled_spi_does_not_reach_the_lcd(lib):
    ptr = new_cpu(lib)
    try:
        bw(ptr, lib, GPIOB + 0x04, A0)
        bw(ptr, lib, GPIOB + 0x00, 0)
        bw(ptr, lib, SPI0 + 0x04, 0xAF)  # SPI0 not enabled: should be dropped
        assert not lib.dp32g030_lcd_is_on(ptr)
    finally:
        lib.dp32g030_cpu_free(ptr)
