import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

GUI = os.environ["LANDELL_GUI"]

# SP, reset vector (thumb, entry at 8), then `bkpt #0`.
IMAGE = struct.pack("<IIH", 0x20004000, 0x09, 0xBE00)


def run(*args):
    return subprocess.run([GUI, *args], capture_output=True, text=True, timeout=30)


def check(cond, msg):
    if not cond:
        print("FAIL:", msg, file=sys.stderr)
        sys.exit(1)


with tempfile.TemporaryDirectory() as d:
    fw = Path(d) / "fw.bin"
    fw.write_bytes(IMAGE)

    for bad in ("0", "17", "abc"):
        r = run("--scale", bad, str(fw))
        check(r.returncode == 1, f"--scale {bad} exit {r.returncode}")
        check("scale" in r.stderr, f"--scale {bad} stderr: {r.stderr!r}")

    r = run(str(Path(d) / "missing.bin"))
    check(r.returncode == 1, f"missing exit {r.returncode}")
    check("cannot read" in r.stderr, f"missing stderr: {r.stderr!r}")

    junk = Path(d) / "junk.bin"
    junk.write_bytes(b"\x00\x01")
    r = run(str(junk))
    check(r.returncode == 1 and "not a valid image" in r.stderr, f"junk: {r}")

    r = run("--soc", "bogus", str(fw))
    check(r.returncode == 1 and "soc" in r.stderr, f"--soc bogus: {r}")
    r = run("--soc", "py32f071", str(fw))  # a DP32G030 image
    check(r.returncode == 1 and "py32f071" in r.stderr, f"py32 vs dp32 image: {r}")

    r = run("--adc", "16=1", str(fw))
    check(r.returncode == 1 and "adc" in r.stderr, f"adc: {r}")

    r = run("-a", "16=1", str(fw))
    check(r.returncode == 1 and "adc" in r.stderr, f"-a bad: {r}")

    png = Path(d) / "short.png"
    r = run("-a", "9=100", "--screenshot", str(png), "--steps", "1000", str(fw))
    check(r.returncode == 0 and png.exists(), f"-a short form: {r}")

    png = Path(d) / "out.png"
    r = run("--screenshot", str(png), "--steps", "1000", "--scale", "2", str(fw))
    check(r.returncode == 0, f"screenshot exit {r.returncode}: {r.stderr}")
    data = png.read_bytes()
    check(data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG")
    w, h = struct.unpack(">II", data[16:24])
    check((w, h) == (256, (64 + 72) * 2), f"size {(w, h)}")

    r = run("--press", "bogus", "--screenshot", str(png), str(fw))
    check(r.returncode == 1 and "press" in r.stderr, f"--press bogus: {r}")

    plain = Path(d) / "plain.png"
    pressed = Path(d) / "pressed.png"
    a = run("--screenshot", str(plain), "--steps", "1000", "--scale", "2", str(fw))
    b = run("--screenshot", str(pressed), "--steps", "1000", "--scale", "2",
            "--press", "menu,ptt", str(fw))
    check(a.returncode == 0 and b.returncode == 0, f"press runs: {a} {b}")
    check(plain.read_bytes() != pressed.read_bytes(), "--press changes the picture")

    # a PY32F071 app image is detected from its reset vector (and with --soc)
    py = Path(d) / "py32.bin"
    py.write_bytes(struct.pack("<IIH", 0x20004000, 0x08002809, 0xBE00))
    for extra in ([], ["--soc", "py32f071"], ["--soc", "auto"]):
        out = Path(d) / "py.png"
        r = run(*extra, "--screenshot", str(out), "--steps", "100", "--scale", "2", str(py))
        check(r.returncode == 0 and out.exists(), f"py32 {extra}: {r}")
