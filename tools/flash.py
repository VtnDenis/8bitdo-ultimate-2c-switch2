"""Ultimate 2C bootloader flasher (2dc8:3208), hidapi only, no vendor tools.

The pad has no software reset out of the bootloader, so every flash ends with a
physical replug. Enter the bootloader by unplugging, holding LB+RB and plugging
the cable in; the device then shows up as 2dc8:3208.

  python tools/flash.py id                      # identity, expect data bytes 1b 30
  python tools/flash.py flash build/patched.dat build/tail.bin [--dry]
  python tools/flash.py read 0x6000 64          # hex dump of the app bank
  python tools/flash.py toboot                  # only on an unpatched pad (see below)

`toboot` writes the vendor report the stock firmware understands (81 05 00 51 00).
The patched image drops that vendor endpoint, so after flashing it does nothing.
"""
import hid
import pathlib
import sys

VID, PID = 0x2DC8, 0x3208          # bootloader
APP_VID, APP_PID = 0x057E, 0x2009  # what the patched pad enumerates as

BASE, ERASE_END, CHUNK = 0x6000, 0x13000, 46
APP_LEN = 0x12400                  # payload covered by the .dat
TAIL_MAX = 0x19000 - 0x18400       # room after the payload, before CONFIG


def crc16(data):
    c = 0xFFFF
    for x in data:
        c ^= x
        for _ in range(8):
            c = (c >> 1 ^ 0xA001) if c & 1 else c >> 1
    return c


def frame(cmd, addr=0, param=0, data=b"", count=None):
    r = bytearray(64)
    r[0] = 0x05
    r[1:3] = cmd.to_bytes(2, "little")
    r[5:7] = (len(data) if count is None else count).to_bytes(2, "little")
    if data:
        r[7:9] = crc16(data).to_bytes(2, "little")
    r[9:13] = param.to_bytes(4, "little")
    r[13:17] = addr.to_bytes(4, "little")
    r[17:17 + len(data)] = data
    return bytes(r)


def open_boot():
    found = hid.enumerate(VID, PID)
    if not found:
        raise SystemExit("bootloader 2dc8:3208 not found - unplug, hold LB+RB, replug")
    if len(found) > 1:
        raise SystemExit(f"{len(found)} bootloaders found - plug in only the pad you mean to flash")
    dev = hid.device()
    dev.open_path(found[0]["path"])
    return dev


def xfer(dev, req, timeout=1000, tries=3):
    """Write, then wait for the matching reply (the pad answers lazily, so poll reads)."""
    for _ in range(tries):
        if dev.write(b"\0" + req) < 0:   # hidapi: byte 0 is the report ID; this device is unnumbered
            return None
        for _ in range(4):
            a = bytes(dev.read(64, timeout))
            if len(a) >= 18 and a[0] == 0 and a[1] == 5 and a[2] == 0 \
                    and a[4] == req[1] and a[14:18] == req[13:17]:
                return a
    return None


def erase(dev, addr, length):
    a = xfer(dev, frame(4, addr, length), timeout=max(10000, int(300 * length / 0x1000)))
    if not a:
        raise SystemExit(f"erase failed at {addr:#x}")


def write(dev, addr, data, total):
    if not xfer(dev, frame(3, addr, total, data)):
        raise SystemExit(f"write failed at {addr:#x}")


def read(dev, addr, n):
    out = bytearray()
    while len(out) < n:
        k = min(CHUNK, n - len(out))
        a = xfer(dev, frame(5, addr + len(out), k, count=k))
        if not a:
            raise SystemExit(f"read failed at {addr + len(out):#x}")
        out += a[18:18 + k]
    return bytes(out)


def check_image(dat, tail):
    """Refuse anything that is not a 2C image of the expected shape."""
    if len(dat) != 28 + APP_LEN:
        raise SystemExit(f"{len(dat)} B .dat - expected {28 + APP_LEN} (a 2C image)")
    version, addr, payload_len, pid = int.from_bytes(dat[0:4], "little"), \
        int.from_bytes(dat[4:8], "little"), \
        int.from_bytes(dat[8:12], "little"), \
        int.from_bytes(dat[12:16], "little")
    if (addr, payload_len, pid) != (BASE, APP_LEN, 0x301B):
        raise SystemExit(f"unexpected header: addr={addr:#x} len={payload_len:#x} pid={pid:#x}")
    if not 0 < len(tail) <= TAIL_MAX:
        raise SystemExit(f"tail is {len(tail)} B - expected 1..{TAIL_MAX}")
    return version


def flash(dev, image, tail, dry=False):
    print(f"erase {BASE:#x}..{BASE + ERASE_END:#x} ({ERASE_END:#x} bytes)")
    if not dry:
        erase(dev, BASE, ERASE_END)
    n = 0
    for a in range(BASE, BASE + len(image), CHUNK):
        if not dry:
            write(dev, a, image[a - BASE:a - BASE + CHUNK], len(image))
        n += 1
        if n % 200 == 0:
            print(f"  {a - BASE:#07x} / {len(image):#07x}", flush=True)
    for i in range(0, len(tail), CHUNK):
        if not dry:
            write(dev, BASE + APP_LEN + i, tail[i:i + CHUNK], len(image))
    print(f"wrote {n} chunks + tail {len(tail)} B")
    if dry:
        return
    print("verifying...")
    want = image + tail
    got = read(dev, BASE, len(want))
    if got != want:
        bad = next(i for i in range(len(want)) if got[i] != want[i])
        raise SystemExit(f"verify mismatch at {BASE + bad:#x}: {got[bad]:02x} != {want[bad]:02x}")
    print("verify OK - unplug and replug to boot the new firmware")


def toboot():
    """Ask a stock (unpatched) pad to reboot into its own bootloader."""
    for d in hid.enumerate():
        if d["product_id"] not in (0x301D, 0x310A) or d["vendor_id"] != 0x2DC8:
            continue
        dev = hid.device()
        try:
            dev.open_path(d["path"])
        except OSError:
            continue
        r = bytearray(64)
        r[0], r[1], r[2], r[3] = 0x81, 0x05, 0x00, 0x51
        print("sent", dev.write(bytes(r)) > 0, d.get("product_string"))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "id"
    if cmd == "toboot":
        toboot()
        return
    dev = open_boot()
    if cmd == "id":
        a = xfer(dev, frame(8, 0, 4, count=4))
        print("identity reply:", a.hex() if a else "none")
    elif cmd == "read":
        print(read(dev, int(sys.argv[2], 0), int(sys.argv[3], 0)).hex(" "))
    elif cmd == "flash":
        dat = pathlib.Path(sys.argv[2]).read_bytes()
        tail = pathlib.Path(sys.argv[3]).read_bytes()
        version = check_image(dat, tail)
        print(f"image version {version}, {len(tail)} B tail")
        flash(dev, dat[0x1C:0x1C + APP_LEN], tail, dry="--dry" in sys.argv)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
