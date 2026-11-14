"""Patch stock 8BitDo Ultimate 2C firmware 1.09 into a wired Switch Pro Controller.

Image base 0x6000. The .dat payload only covers 0x6000..0x18400, so the Switch HID
report descriptor and the injected code ride in a separate "tail" region that the
flasher writes after the payload.

Input is the *stock* 1.09 image from 8BitDo (`python tools/fetch_fw.py` gets it);
every edit is checked against the expected stock bytes, so a wrong or already
patched input fails loudly instead of producing a half-working image.

  python tools/patch.py          # -> build/patched.dat + build/tail.bin
  python tools/patch.py stock    # descriptor only, no injected code (diagnostic)
"""
import hashlib
import pathlib
import struct
import sys
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "fw" / "u2c_109.dat"
OUT = ROOT / "build"
STOCK_DAT_SHA256 = "87349e603bb92d6ae9ff6defb6f32b10de2f2d3252fe4e442fa7993f982f2a5a"

BASE, LEN, CRC_OFF = 0x6000, 0x12400, 0x12270  # CRC word at payload + 0x12270
DESC_ADDR = 0x18400                            # 0x18400..0x19000 is blank past BANK_A_END
BLOB_ADDR = 0x18500                            # injected code (build/blob.bin)

# Switch Pro Controller USB HID report descriptor, 203 bytes, taken verbatim from
# GP2040-CE (headers/drivers/switchpro/SwitchProDescriptors.h). The report IDs and
# sizes in here are what the console parses; the payloads are not described.
HID_DESC = bytes.fromhex(
    "05 01 15 00 09 04 A1 01 85 30 05 01 05 09 19 01 29 0A 15 00 25 01 75 01 95 0A 55 00 65 00 81 02"
    "05 09 19 0B 29 0E 15 00 25 01 75 01 95 04 81 02 75 01 95 02 81 03 0B 01 00 01 00 A1 00 0B 30 00"
    "01 00 0B 31 00 01 00 0B 32 00 01 00 0B 35 00 01 00 15 00 27 FF FF 00 00 75 10 95 04 81 02 C0 0B"
    "39 00 01 00 15 00 25 07 35 00 46 3B 01 65 14 75 04 95 01 81 02 05 09 19 0F 29 12 15 00 25 01 75"
    "01 95 04 81 02 75 08 95 34 81 03 06 00 FF 85 21 09 01 75 08 95 3F 81 03 85 81 09 02 75 08 95 3F"
    "81 03 85 01 09 03 75 08 95 3F 91 83 85 10 09 04 75 08 95 3F 91 83 85 80 09 05 75 08 95 3F 91 83"
    "85 82 09 06 75 08 95 3F 91 83 C0".replace(" ", ""))
assert len(HID_DESC) == 203


def blob(name):
    return (ROOT / "build" / name).read_bytes()


# (flash address, new bytes, expected stock bytes)
DESCRIPTOR_EDITS = [
    # DInput personality device descriptor @0x1797C: VID 057E, PID 2009, bcdDevice 0x0210
    (0x17984, bytes.fromhex("7e 05 09 20"), bytes.fromhex("c8 2d 1d 30")),
    (0x17988, bytes.fromhex("10 02"), bytes.fromhex("01 00")),
    # HID descriptor inside the config @0x179C8: wDescriptorLength 0x0091 -> 0x00CB
    (0x179E1, len(HID_DESC).to_bytes(2, "little"), bytes.fromhex("91 00")),
    # literal pool of the GET_DESCRIPTOR HID branch @0xE858: report desc -> our tail
    (0xE858, struct.pack("<I", DESC_ADDR), struct.pack("<I", 0x17B74)),
    # config descriptor: bus powered + remote wakeup, 500 mA, 8 ms endpoints
    (0x179CF, bytes.fromhex("a0"), bytes.fromhex("80")),
    (0x179D0, bytes.fromhex("fa"), bytes.fromhex("f0")),
    (0x179E9, bytes.fromhex("08"), bytes.fromhex("04")),
    (0x179F0, bytes.fromhex("08"), bytes.fromhex("01")),
    # same branch @0xE82E: tmovs r3,#0x91 -> tmovs r3,#0xCB
    (0xE82E, bytes([len(HID_DESC) & 0xFF, 0xA3]), bytes.fromhex("91 a3")),
]

# Four-byte tjl hooks into the injected code. The stock bytes are the branch each
# one replaces, so an already patched image is refused rather than double-patched.
HOOK_EDITS = [
    (0xAA68, blob("hook_aa68.bin"), bytes.fromhex("03 90 5a 9e")),  # EP5 OUT -> sw_out_hook
    (0xAA2E, blob("hook_aa2e.bin"), bytes.fromhex("03 90 77 9e")),  # EP6 OUT -> sw_out6_hook
    (0xA45C, blob("hook_a45c.bin"), bytes.fromhex("04 90 8c 9f")),  # main loop -> sw_tick
]


def off(addr):
    return addr - BASE


def build(edits):
    """Return (stock-shaped .dat, tail region written at DESC_ADDR)."""
    dat = SRC.read_bytes()
    if hashlib.sha256(dat).hexdigest() != STOCK_DAT_SHA256:
        raise SystemExit(f"{SRC} is not the stock 1.09 image (run tools/fetch_fw.py)")
    p = bytearray(dat[0x1C:0x1C + LEN])
    for addr, new, old in edits:
        o = off(addr)
        if p[o:o + len(old)] != old:
            raise SystemExit(
                f"stock mismatch at {addr:#x}: {p[o:o + len(old)].hex()} != {old.hex()}")
        p[o:o + len(new)] = new
    struct.pack_into("<I", p, CRC_OFF, (~zlib.crc32(bytes(p[:CRC_OFF]))) & 0xFFFFFFFF)
    code = blob("blob.bin")
    assert DESC_ADDR + len(HID_DESC) <= BLOB_ADDR, "descriptor overlaps code"
    assert BLOB_ADDR + len(code) <= 0x19000, f"blob too big: {len(code)}"
    tail = HID_DESC + b"\0" * (BLOB_ADDR - DESC_ADDR - len(HID_DESC)) + code
    return dat[:0x1C] + bytes(p), tail


def main():
    stock = "stock" in sys.argv[1:]
    edits = DESCRIPTOR_EDITS if stock else DESCRIPTOR_EDITS + HOOK_EDITS
    OUT.mkdir(exist_ok=True)
    img, tail = build(edits)
    name, tail_name = ("diag", "diag_tail.bin") if stock else ("patched", "tail.bin")
    (OUT / f"{name}.dat").write_bytes(img)
    (OUT / tail_name).write_bytes(tail[: len(HID_DESC)] if stock else tail)
    print("variant", "descriptor only (diagnostic)" if stock else "switch")
    print("payload sha256", hashlib.sha256(img[0x1C:]).hexdigest())
    print("crc word", hex(struct.unpack_from("<I", img, 0x1C + CRC_OFF)[0]),
          "expect", hex((~zlib.crc32(img[0x1C:0x1C + CRC_OFF])) & 0xFFFFFFFF))
    for a, n, _ in edits:
        print(f"{a:#08x}", img[0x1C + off(a):0x1C + off(a) + len(n)].hex(" "))
    print("dat", len(img), "tail", len(tail), f"({DESC_ADDR:#x}..{DESC_ADDR + len(tail):#x})")


if __name__ == "__main__":
    main()
