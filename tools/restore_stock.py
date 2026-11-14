"""Put the pad back to a stock image: python tools/restore_stock.py [1.04|1.09]

1.09 = 8BitDo's published image (fw/u2c_109.dat, fetched by tools/fetch_fw.py).
1.04 = the factory build dumped off this pad (build/dump_boot_19000.bin), kept
       because it is a build 8BitDo never published.
Both erase 0x6000..0x19000, so the patched descriptor and code at 0x18400 go too.
"""
import hashlib
import pathlib
import sys

import flash

ROOT = pathlib.Path(__file__).resolve().parent.parent
IMAGES = {
    "1.04": (ROOT / "build" / "dump_boot_19000.bin", 0x12400,
             "0acb5890738ae03b8ee74ca0ee429cc4cebd82fdd2d772b5ac34d7e51ee3b452"),
    "1.09": (ROOT / "fw" / "u2c_109.dat", 0x1C,
             "dfd443c77f831008aa453ee2c1c9cb6871c36f8947c548e61d4387fba0a9da70"),
}


def main():
    which = (sys.argv[1] if len(sys.argv) > 1 else "1.09").strip()
    if which not in IMAGES:
        raise SystemExit(__doc__)
    path, skip, want = IMAGES[which]
    data = path.read_bytes()
    payload = data[skip:skip + 0x12400] if which == "1.04" else data[0x1C:0x1C + 0x12400]
    got = hashlib.sha256(payload).hexdigest()
    if got != want:
        raise SystemExit(f"{path}: sha256 {got} != {want}")
    dev = flash.open_boot()
    flash.flash(dev, payload, b"")
    print("restored", which)


if __name__ == "__main__":
    main()
