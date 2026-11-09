# landell-k5ser — UV-K5 programming serial

Two ways to talk Chirp (and later CPS) to a UV-K5:

1. **Live emulator (preferred)** — `landell-gui --k5ser` bridges a PTY to the
   firmware's programming UART (`UART1` on `dp32g030`, `USART1` on
   `py32f071`). Chirp talks to the running image; EEPROM/flash is whatever
   that firmware implements.
2. **Standalone file** — `python -m k5ser` serves an 8 KiB `eeprom.bin` with
   no CPU (offline experiments).

## Chirp + landell-gui

```sh
build/gui/landell-gui --k5ser --soc py32f071 firmware/f4hwn.fusion.v6.3.0.bin
# stderr: k5ser: Chirp port /dev/pts/N …
# stdout: /dev/pts/N
```

In Chirp: Download from radio → Port = that `/dev/pts/N`, Quansheng UV-K5
(or the matching OEM/custom driver). Baud is 38400 inside the protocol.

Wait until the radio has finished booting (main screen) before cloning so
USART1 is up.

## Standalone (no emulator)

```sh
PYTHONPATH=tools python3 -m k5ser --eeprom eeprom.bin
# prints e.g. /dev/pts/3
```

Default `--fw` is `k5_2.01.26` (an OEM prefix Chirp accepts). Override with
`--fw` if you need another string.

## Protocol surface

| Cmd | Reply | Role |
|-----|-------|------|
| `0x0514` hello | `0x0515` | firmware string |
| `0x051B` EEPROM read | `0x051C` | up to `0x80` bytes |
| `0x051D` EEPROM write | `0x051E` | persist to `--eeprom` |
| `0x05DD` reset | none | no-op |
| `0x052F` lock | `0x0515` | CPS; sets locked flag + hello |
| `0x052D` password | `0x052E` | CPS stub ack |

EEPROM size is `0x2000` (Chirp `MEM_SIZE`).

## CPS follow-up

Official Quansheng CPS uses the same ABCD/XOR/CRC framing and the same
EEPROM read/write commands. Extra CPS traffic (password / lock / BK4819
crystal tweaks) is stubbed so the session does not die; full behavioural
fidelity is not claimed yet.

If CPS refuses a PTY and insists on a USB CDC device, add a `configfs`
gadget that exposes CDC-ACM with the Quansheng VID/PID and point it at the
same `RadioSession` — that needs a USB Device Controller (`/sys/class/udc`).

## Notes

- Fusion answers hello as `F4HWN v6.3.0`. Stock Chirp OEM approval may reject
  that string; use a custom/unsupported UV-K5 driver entry, or extend Chirp's
  `k5_approve_firmware` prefixes.
- On-chip `USBD` is still a stub; programming uses USART1 (same framing Chirp
  sends over a USB-serial cable on the real radio).
