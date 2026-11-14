"""Pre-flight check of build/patched.dat + build/tail.bin, before it goes near the pad.

python tools/verify_image.py
Exits non-zero on the first inconsistency. No hardware needed.
"""
import hashlib
import pathlib
import struct
import sys
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
DAT = ROOT / "build" / "patched.dat"
TAIL = ROOT / "build" / "tail.bin"
BASE, PAYLOAD, CRC_OFF, DESC_ADDR, BLOB_ADDR = 0x6000, 0x12400, 0x12270, 0x18400, 0x18500
HDR = 0x1C

fails = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails.append(what)


dat = DAT.read_bytes()
tail = TAIL.read_bytes()
po = lambda a: a - BASE                 # flash address -> payload index
fo = lambda a: a - BASE + HDR           # flash address -> .dat index

check(len(dat) == HDR + PAYLOAD, f".dat length {len(dat)}")
check(struct.unpack("<I", dat[0:4])[0] == 109, "version field = 109")
check(struct.unpack("<I", dat[8:12])[0] == PAYLOAD, "payload length field")
check(struct.unpack("<I", dat[12:16])[0] == 0x301B, "pid field = 0x301B")

payload = dat[HDR:HDR + PAYLOAD]
check(len(payload) == PAYLOAD, f"payload size {len(payload)}")

# --- every edit, read back from the built image -------------------------------
EDITS = {
    0x17984: "7e 05 09 20",      # VID 057E, PID 2009
    0x17988: "10 02",            # bcdDevice 0x0210
    0x179E1: "cb 00",            # HID wDescriptorLength 203
    0xE82E: "cb a3",             # tmovs r3,#203
    0x179CF: "a0",               # bmAttributes: bus powered + remote wakeup
    0x179D0: "fa",               # bMaxPower 500 mA
    0x179E9: "08",               # EP 0x84 bInterval
    0x179F0: "08",               # EP 0x05 bInterval
}
for addr, hexstr in EDITS.items():
    want = bytes.fromhex(hexstr)
    got = payload[po(addr):po(addr) + len(want)]
    check(got == want, f"edit @{addr:#06x} = {want.hex(' ')} (got {got.hex(' ')})")

ptr = struct.unpack("<I", payload[po(0xE858):po(0xE858) + 4])[0]
check(ptr == DESC_ADDR, f"descriptor pointer {ptr:#x} -> {DESC_ADDR:#x}")

blob_end = BLOB_ADDR + len(tail) - (BLOB_ADDR - DESC_ADDR)
for site, name in ((0xAA68, "vendor parser call (EP5)"), (0xAA2E, "vendor parser call (EP6)"),
                   (0xA45C, "main loop")):
    hw1, hw2 = struct.unpack("<HH", payload[po(site):po(site) + 4])
    delta = ((hw1 & 0x7FF) << 12) | ((hw2 & 0x7FF) << 1)
    if delta & 0x800000:
        delta -= 0x1000000
    target = site + 4 + delta
    check(hw1 & 0xF800 == 0x9000 and hw2 & 0xF800 == 0x9800 and BLOB_ADDR <= target < blob_end,
          f"hook @{site:#06x} ({name}) -> {target:#x} inside blob")

# --- container CRC ------------------------------------------------------------
crc_dat = HDR + CRC_OFF                     # CRC word index inside the .dat
want_crc = ~zlib.crc32(dat[HDR:crc_dat]) & 0xFFFFFFFF
got_crc = struct.unpack("<I", dat[crc_dat:crc_dat + 4])[0]
check(got_crc == want_crc, f"container CRC {got_crc:#010x} == {want_crc:#010x}")

# --- tail = descriptor @0x18400 + blob @0x18500 ------------------------------
desc_len = struct.unpack("<H", dat[fo(0x179E1):fo(0x179E1) + 2])[0]
check(desc_len == 203, f"descriptor length {desc_len}")
desc = tail[:desc_len]
check(all(b == 0 for b in tail[desc_len:BLOB_ADDR - DESC_ADDR]), "descriptor padding is zero")
check(desc[0:3] == bytes.fromhex("05 01 15") and bytes.fromhex("09 04") in desc[:12],
      "descriptor opens with Generic Desktop/Gamepad")
check(desc.endswith(b"\xc0"), "descriptor ends with End Collection")
for rid, what in ((b"\x85\x30", "report 0x30 input"), (b"\x85\x81", "report 0x81"),
                  (b"\x85\x80", "report 0x80 output"), (b"\x85\x01", "report 0x01 rumble")):
    check(rid in desc, f"descriptor declares {what}")
blob = tail[BLOB_ADDR - DESC_ADDR:]
check(len(blob) > 1000 and BLOB_ADDR + len(blob) <= 0x19000, f"blob size {len(blob)}")
check(blob[:2] == bytes.fromhex("10 65") or blob[:2] != b"\xff\xff", "blob is not blank")
print(f"   sha256 dat {hashlib.sha256(dat).hexdigest()[:16]}  tail {hashlib.sha256(tail).hexdigest()[:16]}")

print(f"\n{len(fails)} failure(s)" if fails else "\nIMAGE OK")
sys.exit(1 if fails else 0)
