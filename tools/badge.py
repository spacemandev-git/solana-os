#!/usr/bin/env python3
"""Drive the Solana OS console from the host.

    python3 tools/badge.py "open wifi" "key down" "shot"   # run commands in order
    python3 tools/badge.py --reset ...                      # reset first, wait for boot
    python3 tools/badge.py --out shots/ "shot"              # where screenshots go

Every `shot` command is decoded from the console stream into a PNG
(RGB565 rows, base64, see core/os.c take_screenshot).
"""
import argparse, base64, os, struct, sys, time
import serial

p = argparse.ArgumentParser()
p.add_argument("cmds", nargs="*")
p.add_argument("--port", default="/dev/cu.usbserial-110")
p.add_argument("--reset", action="store_true", help="reset the board and wait for the prompt")
p.add_argument("--out", default="shots")
p.add_argument("--quiet", action="store_true", help="don't echo console output")
a = p.parse_args()

s = serial.Serial()
s.port, s.baudrate, s.timeout = a.port, 115200, 0.1
s.dtr = s.rts = False       # don't hold the chip in reset/boot
s.open()

def read_until(marker, timeout):
    buf, end = b"", time.time() + timeout
    while time.time() < end:
        d = s.read(8192)
        buf += d
        if marker and marker in buf:
            break
    return buf

if a.reset:
    s.rts = True; time.sleep(0.1); s.rts = False
else:
    # On macOS opening the CH340 port can reset the board anyway; if it
    # rebooted, wait for the console before sending anything.
    head = read_until(b"ESP-ROM", 1.0)
    a.reset = b"ESP-ROM" in head or b"rst:" in head
if a.reset:
    out = read_until(b"console ready", 20)
    if not a.quiet:
        sys.stdout.write(out.decode("utf-8", "replace"))
    time.sleep(1.5)     # let the launcher draw

shot_n = 0

def save_shot(text):
    global shot_n
    from PIL import Image
    w = h = None
    rows = {}
    for line in text.splitlines():
        if line.startswith("SHOT-BEGIN"):
            _, w, h = line.split()[:3]
            w, h = int(w), int(h)
        elif line.startswith("SHOT "):
            parts = line.split(" ")
            if len(parts) != 5:
                continue
            x, y, n = int(parts[1]), int(parts[2]), int(parts[3])
            try:
                rows[(x, y)] = (n, base64.b64decode(parts[4]))
            except Exception:
                pass
    if not w:
        print("no screenshot in output", file=sys.stderr)
        return
    img = Image.new("RGB", (w, h))
    px = img.load()
    for (x0, y), (n, data) in rows.items():
        for i in range(min(n, len(data) // 2)):
            v = struct.unpack_from("<H", data, i * 2)[0]
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            px[x0 + i, y] = (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)
    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, f"shot_{int(time.time())}_{shot_n}.png")
    shot_n += 1
    img.save(path)
    print(f"saved {path} ({len(rows)} rows)")

for c in a.cmds:
    if c.startswith("sleep "):
        time.sleep(float(c.split()[1]))
        continue
    s.write((c + "\r\n").encode())
    if c == "shot":
        out = read_until(b"SHOT-END", 60).decode("utf-8", "replace")
        save_shot(out)
        continue
    out, idle, end = b"", 0, time.time() + 30
    while time.time() < end and idle < 8:
        d = s.read(4096)
        out += d
        idle = 0 if d else idle + 1
    if not a.quiet:
        sys.stdout.write(out.decode("utf-8", "replace"))
