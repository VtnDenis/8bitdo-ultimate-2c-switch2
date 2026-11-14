"""Fetch the stock 1.09 firmware for the Ultimate 2C from 8BitDo's public API.

The image is 8BitDo's copyrighted firmware: it is not committed to this repo,
download it here instead. The sha256 is checked, so a republished or truncated
file fails loudly instead of producing a bad patch.

  python tools/fetch_fw.py
"""
import hashlib
import json
import pathlib
import urllib.request

API = "http://dl.8bitdo.com:8080/firmware/select"
ROOT = pathlib.Path(__file__).resolve().parent.parent
DEST = ROOT / "fw" / "u2c_109.dat"
SHA256 = "87349e603bb92d6ae9ff6defb6f32b10de2f2d3252fe4e442fa7993f982f2a5a"
TYPE_ULTIMATE_2C = 75      # 77 is the wired SKU, same 1.06 image
VERSION = "1.09"


def post(type_id):
    req = urllib.request.Request(
        API, data=b"", headers={"Beta": "1", "Type": str(type_id)})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode())


def main():
    # the API returns versions as floats (1.090000033378601), so compare rounded
    entry = next((e for e in post(TYPE_ULTIMATE_2C)["list"]
                  if round(float(e["version"]), 2) == 1.09), None)
    if entry is None:
        raise SystemExit(f"8BitDo no longer lists {VERSION} for type {TYPE_ULTIMATE_2C}")
    url = "http://dl.8bitdo.com:8080" + entry["filePathName"]
    print(f"downloading {VERSION} ({entry['fileSize']} B) from {url}")
    with urllib.request.urlopen(url, timeout=60) as r:
        data = r.read()
    got = hashlib.sha256(data).hexdigest()
    if got != SHA256:
        raise SystemExit(f"sha256 {got} != expected {SHA256}")
    DEST.parent.mkdir(exist_ok=True)
    DEST.write_bytes(data)
    print(f"wrote {DEST} ({len(data)} B, sha256 ok)")


if __name__ == "__main__":
    main()
