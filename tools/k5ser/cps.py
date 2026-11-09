"""CPS-facing notes and helpers.

Official Quansheng CPS uses the same ABCD/XOR/CRC framing as Chirp for EEPROM
clone. Extra CPS commands already answered by `RadioSession`:

- ``0x052F`` lock → hello (``0x0515``)
- ``0x052D`` set password → ``0x052E`` stub ack
- ``0x05DD`` reset → no reply

BK4819 crystal / ADC CPS probes (``0x051F``..``0x052A``) are not implemented
yet; extend `proto.RadioSession.handle` when a CPS capture is available.

USB gadget (CDC-ACM with Quansheng VID/PID) is only needed if CPS rejects a
PTY — see docs/k5ser.md.
"""
