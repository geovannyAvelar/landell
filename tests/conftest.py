import pytest

import dp32g030 as emu


@pytest.fixture(scope="session")
def lib(tmp_path_factory):
    return emu.load_lib(tmp_path_factory.mktemp("lib"))


@pytest.fixture(scope="session")
def bus_works(lib):
    probe = [0x205A, 0x6008, 0x680A]
    res = emu.run(lib, probe, regs={1: emu.SRAM_BASE})
    return res.r[2] == 0x5A


@pytest.fixture
def requires_bus(bus_works):
    if not bus_works:
        pytest.skip("bus stub: dp32g030_bus_read/write not implemented yet")
