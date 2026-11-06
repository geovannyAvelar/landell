"""IIC0-1 master mode, driven through the bus and a host hook."""

import ctypes

import dp32g030 as emu

IIC0, IIC1 = 0x400B9000, 0x400B9800

CCFG_MASTER_EN = (1 << 1) | (1 << 0)

MCTRL_STA, MCTRL_RD, MCTRL_WR, MCTRL_STO = 1 << 0, 1 << 1, 1 << 2, 1 << 3

IF_RXF, IF_TXF, IF_RXOVF, IF_RXNE, IF_TXE = 1 << 4, 1 << 3, 1 << 2, 1 << 1, 1 << 0


def bw(ptr, lib, addr, val):
    assert lib.dp32g030_bus_write(ptr, addr, 4, val)


def br(ptr, lib, addr):
    out = ctypes.c_uint32()
    assert lib.dp32g030_bus_read(ptr, addr, 4, ctypes.byref(out))
    return out.value


def new_cpu(lib):
    return lib.dp32g030_cpu_new(emu.to_buf(emu.firmware([0xBE00])), 10)


def test_master_write_byte_captures_ack(lib):
    ptr = new_cpu(lib)
    try:
        seen = []

        def on_write(user, n, byte):
            seen.append((n, byte))
            return True  # ACK

        cb = emu.I2C_WRITE_CB(on_write)
        ptr.contents.periph.hooks.i2c_write = cb

        bw(ptr, lib, IIC0 + 0x00, CCFG_MASTER_EN)
        bw(ptr, lib, IIC0 + 0x10, 0xA0)  # TXDATA: address byte
        bw(ptr, lib, IIC0 + 0x20, MCTRL_STA | MCTRL_WR)
        assert seen == [(0, 0xA0)]
        assert not br(ptr, lib, IIC0 + 0x08) & (1 << 1)  # RX_ACK: 0 = ACKed
        assert br(ptr, lib, IIC0 + 0x18) & IF_TXF
        assert br(ptr, lib, IIC0 + 0x18) & IF_TXE
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_master_write_nack_is_reported(lib):
    ptr = new_cpu(lib)
    try:
        ptr.contents.periph.hooks.i2c_write = emu.I2C_WRITE_CB(lambda u, n, b: False)
        bw(ptr, lib, IIC0 + 0x00, CCFG_MASTER_EN)
        bw(ptr, lib, IIC0 + 0x10, 0x55)
        bw(ptr, lib, IIC0 + 0x20, MCTRL_WR)
        assert br(ptr, lib, IIC0 + 0x08) & (1 << 1)  # RX_ACK: 1 = NACKed
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_master_read_byte_sends_requested_ack(lib):
    ptr = new_cpu(lib)
    try:
        seen = []

        def on_read(user, n, ack):
            seen.append((n, ack))
            return 0x7B

        ptr.contents.periph.hooks.i2c_read = emu.I2C_READ_CB(on_read)
        bw(ptr, lib, IIC0 + 0x00, CCFG_MASTER_EN)
        bw(ptr, lib, IIC0 + 0x08, 0)  # TX_ACK=0 -> send ACK
        bw(ptr, lib, IIC0 + 0x20, MCTRL_RD)
        assert seen == [(0, True)]
        assert br(ptr, lib, IIC0 + 0x18) & (IF_RXF | IF_RXNE)
        assert br(ptr, lib, IIC0 + 0x0C) == 0x7B
        assert not br(ptr, lib, IIC0 + 0x18) & IF_RXNE  # cleared on read
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_master_read_nack_when_requested(lib):
    ptr = new_cpu(lib)
    try:
        seen = []
        ptr.contents.periph.hooks.i2c_read = emu.I2C_READ_CB(
            lambda u, n, ack: (seen.append(ack), 0)[1]
        )
        bw(ptr, lib, IIC0 + 0x00, CCFG_MASTER_EN)
        bw(ptr, lib, IIC0 + 0x08, 1)  # TX_ACK=1 -> send NACK
        bw(ptr, lib, IIC0 + 0x20, MCTRL_RD)
        assert seen == [False]
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_read_overflow_when_not_drained(lib):
    ptr = new_cpu(lib)
    try:
        values = iter([1, 2])
        ptr.contents.periph.hooks.i2c_read = emu.I2C_READ_CB(
            lambda u, n, ack: next(values)
        )
        bw(ptr, lib, IIC0 + 0x00, CCFG_MASTER_EN)
        bw(ptr, lib, IIC0 + 0x20, MCTRL_RD)
        bw(ptr, lib, IIC0 + 0x20, MCTRL_RD)  # RXDATA never read in between
        assert br(ptr, lib, IIC0 + 0x18) & IF_RXOVF
        assert br(ptr, lib, IIC0 + 0x0C) == 1  # the first byte, not overwritten
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_start_and_stop_drive_busy(lib):
    ptr = new_cpu(lib)
    try:
        events = []
        ptr.contents.periph.hooks.i2c_start = emu.I2C_START_CB(
            lambda u, n: events.append("start")
        )
        ptr.contents.periph.hooks.i2c_stop = emu.I2C_STOP_CB(
            lambda u, n: events.append("stop")
        )
        bw(ptr, lib, IIC0 + 0x00, CCFG_MASTER_EN)
        bw(ptr, lib, IIC0 + 0x20, MCTRL_STA)
        assert br(ptr, lib, IIC0 + 0x04) & 1  # BUSY
        bw(ptr, lib, IIC0 + 0x20, MCTRL_STO)
        assert not br(ptr, lib, IIC0 + 0x04) & 1
        assert events == ["start", "stop"]
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_disabled_ignores_mctrl(lib):
    ptr = new_cpu(lib)
    try:
        called = []
        ptr.contents.periph.hooks.i2c_write = emu.I2C_WRITE_CB(
            lambda u, n, b: (called.append(b), True)[1]
        )
        bw(ptr, lib, IIC0 + 0x10, 0x11)
        bw(ptr, lib, IIC0 + 0x20, MCTRL_WR)  # EN=0: no-op
        assert called == []
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_iic1_is_independent_of_iic0(lib):
    ptr = new_cpu(lib)
    try:
        seen = []
        ptr.contents.periph.hooks.i2c_write = emu.I2C_WRITE_CB(
            lambda u, n, b: (seen.append((n, b)), True)[1]
        )
        bw(ptr, lib, IIC1 + 0x00, CCFG_MASTER_EN)
        bw(ptr, lib, IIC1 + 0x10, 0x42)
        bw(ptr, lib, IIC1 + 0x20, MCTRL_WR)
        assert seen == [(1, 0x42)]
        assert br(ptr, lib, IIC0 + 0x18) & IF_TXE  # untouched, reset default
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_master_irq_fires_on_txf(lib):
    ptr = new_cpu(lib)
    try:
        ptr.contents.periph.hooks.i2c_write = emu.I2C_WRITE_CB(lambda u, n, b: True)
        bw(ptr, lib, IIC0 + 0x00, CCFG_MASTER_EN)
        bw(ptr, lib, IIC0 + 0x14, IF_TXF)  # IE: TXF
        bw(ptr, lib, IIC0 + 0x20, MCTRL_WR)
        assert ptr.contents.scs.irq_lines & (1 << 18)
        bw(ptr, lib, IIC0 + 0x18, IF_TXF)  # clear
        assert not ptr.contents.scs.irq_lines & (1 << 18)
    finally:
        lib.dp32g030_cpu_free(ptr)


def test_slave_registers_are_stored_but_inert(lib):
    ptr = new_cpu(lib)
    try:
        bw(ptr, lib, IIC0 + 0x30, 0x05)  # SCTRL
        bw(ptr, lib, IIC0 + 0x34, 0x1234)  # SADDR
        assert br(ptr, lib, IIC0 + 0x30) == 0x05
        assert br(ptr, lib, IIC0 + 0x34) == 0x1234
    finally:
        lib.dp32g030_cpu_free(ptr)
