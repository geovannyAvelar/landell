"""CLI: python -m k5ser --eeprom eeprom.bin [--fw STR]"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Allow `python tools/k5ser` and `python -m k5ser` from repo tools/.
if __package__ is None or __package__ == "":
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from k5ser import DEFAULT_FW
    from k5ser.eeprom import Eeprom
    from k5ser.proto import RadioSession
    from k5ser.server import run_forever
else:
    from . import DEFAULT_FW
    from .eeprom import Eeprom
    from .proto import RadioSession
    from .server import run_forever


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="k5ser",
        description="UV-K5 programming serial PTY for Chirp (then CPS).",
    )
    p.add_argument(
        "--eeprom",
        default="eeprom.bin",
        help="8 KiB EEPROM image path (created as 0xFF if missing)",
    )
    p.add_argument(
        "--fw",
        default=DEFAULT_FW,
        help=f"firmware version string reported to Chirp (default {DEFAULT_FW})",
    )
    args = p.parse_args(argv)
    session = RadioSession(Eeprom(args.eeprom), firmware=args.fw)
    run_forever(session)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
