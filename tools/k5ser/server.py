"""PTY server: Chirp opens the slave path at 38400."""

from __future__ import annotations

import os
import select
import sys
import termios

from .codec import FrameDecoder, encode_frame
from .proto import RadioSession


def _raw(fd: int) -> None:
    attrs = termios.tcgetattr(fd)
    attrs[3] &= ~(termios.ECHO | termios.ICANON | termios.ISIG)
    attrs[0] &= ~(termios.IXON | termios.IXOFF | termios.INLCR | termios.ICRNL)
    attrs[1] &= ~(termios.OPOST)
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def open_pty() -> tuple[int, int, str]:
    """Return (master_fd, slave_fd, slave_name).

    Keep `slave_fd` open for the life of the server so the master does not see
    EOF/HUP before Chirp attaches; Chirp may open the same slave path again.
    """
    master, slave = os.openpty()
    _raw(slave)
    _raw(master)
    return master, slave, os.ttyname(slave)


def serve_fd(session: RadioSession, master_fd: int) -> None:
    """Serve frames on an already-open PTY master until EOF."""
    decoder = FrameDecoder()
    while True:
        r, _, _ = select.select([master_fd], [], [], 1.0)
        if not r:
            continue
        try:
            chunk = os.read(master_fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        for clear in decoder.feed(chunk):
            reply = session.handle(clear)
            if reply is not None:
                os.write(master_fd, encode_frame(reply))


def run_forever(session: RadioSession) -> str:
    master, slave, name = open_pty()
    print(name, flush=True)
    print(f"landell-k5ser: listening on {name} (38400 8N1)", file=sys.stderr)
    try:
        serve_fd(session, master)
    except KeyboardInterrupt:
        pass
    finally:
        os.close(master)
        os.close(slave)
    return name
