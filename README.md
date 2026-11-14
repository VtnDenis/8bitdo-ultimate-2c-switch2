# 8BitDo Ultimate 2C Wired → Switch 2 Pro Controller

[![tests](https://github.com/VtnDenis/8bitdo-ultimate-2c-switch2/actions/workflows/test.yml/badge.svg)](https://github.com/VtnDenis/8bitdo-ultimate-2c-switch2/actions/workflows/test.yml)

Make an 8BitDo Ultimate 2C Wired (USB `2DC8:310A` XInput / `2DC8:301D` DInput) show up
on a Switch 2 as a **wired Switch Pro Controller** (`057E:2009`) by patching its own
firmware. No adapter, no dongle, no hardware modification. Verified end to end, buttons
and both sticks, on a retail Switch 2.

**Status:** experimental, verified on one pad and one console. Reversible — the
bootloader region is never written, so the stock image always comes back. Wired only:
this patch has no radio path, and the Bluetooth 2C already has official Switch 2
support, so it needs none of this.

**Not for you if** you want wireless play, you own the 2C *Bluetooth* model, or you
would rather not flash a controller that already works on PC.

Quick start, if the rest of this page is too slow to read:

```sh
python tools/fetch_fw.py                     # stock 1.09 image -> fw/u2c_109.dat
wsl -e bash scripts/build_blob.sh            # tc32-elf-gcc -> build/blob.bin
python tools/patch.py                        # -> build/patched.dat + build/tail.bin
python tools/verify_image.py                 # must print IMAGE OK
# unplug the pad, hold LB+RB, plug it into the PC, then:
python tools/flash.py flash build/patched.dat build/tail.bin
```

Then on the console: Settings → Controllers and Sensors → **Pro Controller Wired
Communication** = ON, **power cycle fully**, plug the pad into the dock.

## Why this works

- The 2C `.dat` firmware images are **not encrypted** (raw entropy 6.92, versus 7.98
  for the 8BitDo lines that use the `8cryptdo` cipher) and the bootloader enforces
  **no signature** — it only checks a CRC-16 per 46-byte write.
- The Switch 2 still speaks the **Switch 1 wired Pro Controller** protocol, with no
  cryptographic challenge. Only the USB identity and the report format matter.
- The stock build already contains a DInput personality, so no USB stack is
  rewritten: a dozen bytes change the descriptors and three 4-byte `tjl` hooks jump
  into code injected at `0x18500`, in blank flash past the end of the app bank.

## What you need

- An Ultimate 2C **Wired with the DInput personality active**: it must enumerate as
  `2DC8:301D`. If it shows `2DC8:310A` (XInput), power it on while holding `X+Home`
  (8BitDo's documented combo) and plug it in again — the mode persists.
- Windows + [WSL](https://learn.microsoft.com/windows/wsl/) for the `tc32-elf-gcc`
  toolchain, Python 3 with [`hidapi`](https://pypi.org/project/hidapi/).
- A Switch 2 with *System Settings → Controllers and Sensors → **Pro Controller
  Wired Communication** = ON*, then a **full power cycle** (sleep is not enough — a
  console that already rejected a device will not re-scan it until it reboots).

## Console setup

1. Settings → Controllers and Sensors → Pro Controller Wired Communication → **ON**.
2. Power the console **fully off**, then on.
3. Plug the patched pad into the dock (any USB port; the tools never assume one).
4. If it still is not seen, open HOME → Controllers → **Change Grip/Order** to force
   a re-scan.

## How it works

| address | change |
|---|---|
| `0x17984` | DInput device descriptor: `2DC8:301D` → `057E:2009` |
| `0x17988` | `bcdDevice` `0x0001` → `0x0210` |
| `0x179CF` / `0x179D0` | config: bus powered + remote wakeup, 500 mA |
| `0x179E9` / `0x179F0` | both endpoint `bInterval` → 8 |
| `0x179E1` / `0xE82E` | HID report descriptor length → 203 |
| `0xE858` | HID report descriptor pointer → `0x18400` (our tail) |
| `0xAA68`, `0xAA2E` | EP5/EP6 OUT hooks: capture the console's command, answer from the interrupt |
| `0xA45C` | main-loop hook: build and send a `0x30` input report every pass |

Injected code (`src/`):

- `handler.c` — answers the `0x80 01..06` session commands and the `0x01`
  subcommands (`00`, `01`, `02`, `03`, `04`, `08`, `10` SPI, `30`, `31`, `38`,
  `40/41/43/48`, `50`), including the SPI calibration tables the console reads.
- `glue.c` — reads the stock state (`0x84342C` buttons, `0x8433EC/F6/E4/E6` axes,
  `0x84340E/E8` triggers), builds the frame, and bursts it straight into the EP4
  FIFO exactly like the stock packer does.
- `packer.c` — pure 64-byte `0x30` frame builder, unit-tested on the host.

Three details cost real debugging time and are worth knowing if you fork this:

1. Replies must leave the **interrupt context** (pointer to the frame is enough —
   the EP4 copy has to happen while the console is still listening); answering from
   the 5 ms main loop makes the console give up.
2. The identify reply carries the MAC **reversed** relative to the device-info reply
   (GP2040 does `report[4+i] = mac[5-i]`), and the console does compare them.
3. All four axis words are stored **inverted** relative to the Switch wire format:
   `(0xFFFF - v) >> 4`, neutral `0x7FFF` → 2048.

## Build

```sh
wsl -e bash scripts/build_blob.sh       # tc32-elf-gcc 4.5.1 -> build/blob.bin + hook_*.bin
python tools/patch.py                   # -> build/patched.dat + build/tail.bin
python tools/verify_image.py            # offline pre-flight, must print IMAGE OK
```

Host unit tests (no hardware, plain gcc):

```sh
wsl -e bash scripts/test.sh              # 20 packer cases + 10 handler cases
```

`scripts/build_blob.sh` needs the `tc32-elf-*` binaries from
[`flyskywhy/tc32`](https://github.com/flyskywhy/tc32) in `~/tc32/bin`, plus
`~/tc32/alias/as` and `~/tc32/alias/ld` symlinks to `tc32-elf-as` / `tc32-elf-ld`
(the driver shells out to bare `as` / `ld`).

## Flash

The patched firmware no longer exposes the vendor output report the stock build uses
to reboot into the bootloader, so every flash starts physically:

1. Unplug the pad, **hold LB+RB**, plug the USB into the PC → it appears as `2DC8:3208`.
2. `python tools/flash.py flash build/patched.dat build/tail.bin`
   (erase `0x6000..0x19000`, write 74752 B + tail, read back and compare).
3. **Unplug and replug** to boot the app — the bootloader has no software reset.

`python tools/watch_flash.py build/patched.dat build/tail.bin` waits for the
bootloader and flashes the moment it appears.

## Revert

```sh
python tools/patch.py stock                                         # descriptor only
python tools/flash.py flash build/diag.dat build/diag_tail.bin
python tools/restore_stock.py 1.09                                  # or 1.04 = factory dump
```

The bootloader at `0x0..0x6000` is never erased by anything here, so a bad image is
always recoverable with the LB+RB entry. `restore_stock.py 1.04` uses
`build/dump_boot_19000.bin`, the untouched factory image read off the reference pad.

## Test on the PC

```sh
python tools/flash.py id            # in the bootloader: expect `1b 30` (model 0x301B)
python tools/watch_input.py         # live decode: buttons by name, stick values
python tools/trace_read.py          # read back the console's commands from flash (bootloader)
```

## Repository layout

- `fw/` — put the stock image here with `python tools/fetch_fw.py` (not committed:
  it is 8BitDo's copyrighted firmware).
- `src/` — injected C, linker script; `docs/` — RE notes, USB protocol spec, injection map.
- `tools/` — patch, flash, verify, restore, decode helpers.
- `build/` — generated images and the factory dump (gitignored).

## Credits

- [GP2040-CE](https://github.com/OpenStickCommunity/GP2040-CE) — the Switch Pro
  Controller USB protocol, descriptors and subcommand behaviour this firmware follows.
- [dekuNukem/Nintendo_Switch_Reverse_Engineering](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering) — protocol documentation.
- [carlosbrownef-source/8bitdo-ultimate-2c-toolkit](https://github.com/carlosbrownef-source/8bitdo-ultimate-2c-toolkit)
  and [jgus-org/8bitdo-2c-re-flake](https://github.com/jgus-org/8bitdo-2c-re-flake) — flash map, code caves, bootloader flasher.
- [ripdajacker/bytedo-patcher](https://github.com/ripdajacker/bytedo-patcher) — the `.dat` container CRC repair.
- [aeromodes/8cryptdo](https://github.com/aeromodes/8cryptdo) — mapping which firmware
  images are encrypted and which (like this one) are plaintext.

## Licence

MIT, see `LICENSE`. Not affiliated with 8BitDo or Nintendo. Flashing modifies your
controller's firmware: you can always recover with the bootloader entry above, but do
it at your own risk.
