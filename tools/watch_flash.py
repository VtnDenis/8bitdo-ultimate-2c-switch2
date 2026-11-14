"""Wait for the pad in bootloader mode (2dc8:3208), then flash the given images.

The bootloader has no software reset, so this is the only way back into it:
unplug, hold LB+RB, replug. Usage:

    python tools/watch_flash.py build/patched.dat build/tail.bin [minutes]
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import hid
import flash

ROOT = Path(__file__).parent.parent


def main() -> int:
    dat = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/patched.dat").read_bytes()
    tail = Path(sys.argv[2] if len(sys.argv) > 2 else ROOT / "build/tail.bin").read_bytes()
    minutes = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0
    version = flash.check_image(dat, tail)   # refuse anything that is not a 2C image
    image = dat[0x1C:0x1C + flash.APP_LEN]
    print(f"image version {version}, {len(tail)} B tail", flush=True)

    print(f"waiting for 2dc8:3208 (up to {minutes:.0f} min) ...", flush=True)
    end = time.time() + minutes * 60
    while time.time() < end:
        if hid.enumerate(0x2DC8, 0x3208):
            print("found, flashing", flush=True)
            dev = flash.open_boot()
            flash.flash(dev, image, tail)
            print("done - unplug and replug to boot the new firmware", flush=True)
            return 0
        time.sleep(0.5)
    print("timeout", flush=True)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
