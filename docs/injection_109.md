# Code injection into 8BitDo Ultimate 2C firmware 1.09 (Telink B80 / TC32)

Derived only from the three cloned repos plus our own verified hardware patches.
Symbols: `TK` = a clone of `carlosbrownef-source/8bitdo-ultimate-2c-toolkit`,
`RF` = `...\8bitdo-2c-re-flake`, `MAC` = `...\8bitdo-macos`.
All addresses are **flash/image addresses** unless written `run` or `file`.

---

## 0. The two address spaces, and why it matters

| Space | Relation | Use |
|---|---|---|
| image (flash) | — | what the linker is told (`packer.ld`, `--defsym`) |
| run (RAM) | `run = image - 0x6000` for image `0x6000-0x8500` | the first 0x2500 B of the payload are **copied to RAM 0x0-0x2500 and executed there** (`TK\docs\firmware-notes.md:8`) |
| `.dat` file (1.06 updater) | `file = fw - 0x6000 + 0x1C` | only for the old `.dat` container; the 1.09 bootloader takes payload bytes directly |

`tjl`/`tj` are **PC-relative**. A hook or cave must therefore be assembled *at the address
where it will execute*, and every symbol you branch to must be named in the same space
(`MAC\docs\developing-patches.md:271-311`; `RF\NOTES.md:129` — "a shim's `tjl`/`tjex` targets
and pool words are absolute, so the linker script's section address is load-bearing").
Getting this wrong is a hard fault, not a warning.

---

## 1. Injecting a C function

### 1.1 Linker script — `TK\firmware-src\packer.ld`

The script *is* the placement plan: each output section's VMA is the address the code will
run at, and `AT()` gives the image address when the two differ (RAM-resident code).

| Section | VMA (run) | LMA (image) | Capacity | Purpose |
|---|---|---|---|---|
| `.packer` | 0xB908 | = | 0xBAD4-0xB908 = 460 B | stock BT report packer slot; `pack_xbox` must be **first** (`:40-41`) |
| `.noop` / `.rumble` | 0x18390 | = | 0x70 B (`:9-10,42-43`) | output-report write callback; alternatives, so `.rumble` overlaps `.noop` |
| `.led` | 0x1738 | 0x7738 | 0x9C B (`:11,44`) | RAM code reusing dead run 0x1738-0x17d4 |
| `.hook`,`.ringhook`,`.vhooka/b`,`.pollhook`,`.hgamea/b`,`.hbuzza/b`,`.hidle`,`.hstickl/r`,`.hscan`,`.hidletask`,`.hunplug`,`.hsweep`,`.resthook`,`.hstall`,`.hmeter` | see table §2 | = | **exactly 4 B each** (`ASSERT SIZEOF==4`, `:45,49,53`) | the `tjl` overrides |
| `.hota` | 0x178CC | = | 4 B | `.word no_write + 1`, a thumb-style function pointer, *not* a jump (`:27`) |
| `.battlog` | 0x9964 | = | 0x348 B (`:32,50`) | dead soft-float subtraction routine, used as a cave |
| `.wdog` | 0x20 | 0x6020 | 0x94 B (`:33,51`) | RAM-resident |
| `.stall` | 0x248C | 0x848C | 0x74 B (`:34,52`) | RAM-resident |
| `.sram` | 0x1210 | 0x7210 | 0xC0 B (`:35,46`) | RAM-resident |
| `.ringtail` | 0x18400 | = | 0x18FF0-0x18400 (`:36,47-48`) | everything else: `rumble_tail`, `*(.text*)`, `*(.rodata*)` |
| `.rest` | — | — | must be **empty** | catch-all; a stray symbol aborts the link with `unplaced code or data` (`:54`) |

Two consequences that bite us:

* `.ringtail { rumble_tail *(.text*) *(.rodata*) }` means **any new function added to
  `packer.c` without its own section in `packer.ld` lands at 0x18400** — exactly where our
  203-byte Switch Pro descriptor now lives. Either give the new code its own section or move
  our descriptor.
* `.packer` is the *live* stock BT packer, not blank space: writing there replaces BT input
  behaviour and must reproduce a valid report.

### 1.2 Compile / assemble / link (`TK\firmware-src\build.sh`)

```sh
# compile (per .c file: packer.c, settings.c, rest.c)          build.sh:12-14
tc32-elf-gcc -Os -Wall -Werror -ffreestanding -fno-builtin \
    -ffunction-sections -fdata-sections -c packer.c -o "$out/packer.o"

# assemble                                                      build.sh:15
tc32-elf-as hook.s -o "$out/hook.o"

# link                                                          build.sh:16-20
tc32-elf-ld -N -T packer.ld --no-check-sections \
  --defsym ble_notify=0x1458c --defsym after_notify=0x1abc \
  --defsym motor_set=0xfa68   --defsym motor_tick=0xfaa0 \
  --defsym led_driver=0xd36c  --defsym save_settings=0xd538 \
  --defsym stock_chords=0xcfa4 --defsym stock_vendor_a=0xf56c --defsym stock_vendor_b=0xf594 \
  --defsym flash_unlock=0xbd00 --defsym flash_erase=0xfe98 \
  --defsym stock_crc=0xaf80 --defsym stock_limit=0x890 --defsym stock_isqrt=0x84c \
  --defsym stock_scan=0xc3a8 --defsym stock_save_mappings=0xd7d4 --defsym stock_idle=0xd190 \
  --defsym stock_connected_bt=0xa28c --defsym stock_connected_24g=0xe088 \
  --defsym stock_da58=0xda58 --defsym stock_reboot=0xd056 --defsym stock_sweep=0xb388 \
  --defsym stock_send=0xeb7c --defsym stock_clear=0x103f8 \
  --defsym __udivsi3=0x850c --defsym __divsi3=0x8504 \
  packer.o settings.o rest.o hook.o -o packer.elf
```

`-N` + `--no-check-sections` make the linker emit one flat load image with the odd VMAs/LMAs
above and no complaint about the `.noop`/`.rumble` overlap (`TK\firmware-src\README.md:45`).

Split into one raw binary per section (`build.sh:21`):

```sh
for s in packer noop rumble led hook ringtail ringhook vhooka vhookb pollhook hgamea hgameb \
         hbuzza hbuzzb hidle hstickl hstickr hscan hidletask hunplug hota hsweep resthook \
         hstall hmeter battlog wdog stall sram; do
  tc32-elf-objcopy -O binary -j ".$s" packer.elf "$out/$s.bin"
done
tc32-elf-objdump -d packer.elf > packer.dis
```

Placement is then a plain byte copy: `objcopy` output for `.hook` is 4 bytes that go at
0xA43C; `.ringtail`'s output starts at 0x18400; `.led`'s output goes at **0x7738** (its LMA),
`.wdog` at 0x6020, `.sram` at 0x7210, `.stall` at 0x848C.

### 1.3 Free regions on stock 1.09, **given our patches**

Ours already occupy: 0x18400-0x184CA (Switch Pro descriptor), and — via code edits, not space —
0xE82E, 0xE858, 0x17984, 0x17988, 0x179E1. Nothing in the three repos contains those edits;
they are our own and must be re-applied to any 1.09 base image.

| Region | Size | State on stock 1.09 | Notes |
|---|---|---|---|
| 0x18274-0x183FF | 396 B | zero tail of bank A ("zero tail after the image", `TK\firmware-src\README.md:9`; image ends at 0x18400, `RF\findings\verify-btff.md:55`) | free for us; taken only by the toolkit's `bt_xbox` map (283 B) + `noop` |
| 0x184CB-0x18FEF | 0xB25 B | blank, erased and written with bank A, same write parameter (`TK\docs\firmware-notes.md:33`) | **the main free tail after our descriptor** |
| 0x18FF0-0x18FFF | 16 B | toolkit uses 0x18FF0/1/2 as feature flags (`TK\u2c_customfw\firmware\recipes.py:26-28`) | only bytes 0x18FF3-0x18FFF are unconditionally free |
| 0x9964-0x9CAC | 0x348 B | dead soft-float routine, used as a cave (`packer.ld:32`) | in the middle of bank A; small |
| 0xB908-0xBAD4 | 460 B | **live** stock BT report packer | replace, do not squat |
| 0x20-0xB4 → image 0x6020 | 0x94 B | dead RAM code | run at 0x20 |
| 0xA90 / 0xABC → image 0x6A90 / 0x6ABC | 4 B each | unused halfword sites (stock calls at 0x890) | `packer.ld:24-25` |
| 0x1210-0x12D0 → image 0x7210 | 0xC0 B | dead RAM code | `packer.ld:35` |
| 0x1738-0x17D4 → image 0x7738 | 0x9C B | dead RAM code | `packer.ld:11` |
| 0x248C-0x2500 → image 0x848C | 0x74 B | dead RAM code | `packer.ld:34` |
| 0x19000-0x3FFFF | 0x27000 | blank 0xff, **not** written by a normal flash | needs extra-sector support (`TK\u2c_customfw\ops\plan.py:35-38` allows sectors ≥0x19000 and <0x73000); nothing references it; code there is reachable by `tjl` from the app |
| 0x40000-0x523FF | — | app bank B (second slot) | not free |
| 0x73000-0x7FFFF | — | config, MAC 0x76000, 0x77000 edition flag, 0x79000 boot record, 0x7A000 app settings (`TK\docs\firmware-notes.md:49`), 0x7C000/0x7D000 BT pairings (0x7D000 sector erased at every start if not blank) | never |
| 0x3F000 | 52 B | update record — version +0x0C, pending magic +0x18, staging addr +0x1C, pending len +0x20, CRC +0x30 (`firmware-notes.md:15-19`) | never |

**Do not** treat 0x1734C (BT report map), 0x1779C (its GATT entry), 0x17764, 0x17984, 0xE82E,
0xE858 as free — they are live structures we have already edited.

---

## 2. The hook mechanism (`TK\firmware-src\hook.s`)

One section per hook, containing **one instruction**:

```asm
	.section .hook, "ax"
	tjl	main_tick
```

`tjl` is a 32-bit instruction (it prints as one 4-byte objdump line,
`MAC\docs\developing-patches.md:376-377`). The hook file is *not* 2-byte: each site gets
exactly 4 bytes, asserted at link time. The verified site list:

| Site | Site address | Stock target | New target |
|---|---|---|---|
| `.hook` | 0xA43C | `tjl 0xfaa0` motor_tick | `main_tick` (`hook.s:3-4`) |
| `.ringhook` | 0xD4F0 | `tjl 0xd36c` LED driver | `ring_tick` (`:7-8`) |
| `.vhooka` / `.vhookb` | 0xF83C / 0xF842 | 0xF56C / 0xF594 | `vendor_a` / `vendor_b` (`:11-14`) |
| `.pollhook` | 0xA484 | `tjl 0xcfa4` Start+Select check | `main_poll` (`:17-18`) |
| `.hgamea` / `.hgameb` | 0xDCE6 / 0xEAF4 | motor calls | `game_motor` (`:21-24`) |
| `.hbuzza` / `.hbuzzb` | 0xA0F8 / 0xDAA8 | connect buzz | `connect_buzz` (`:25-28`) |
| `.hstickl` / `.hstickr` | 0x6A90 / 0x6ABC (**image**) → run 0xA90 / 0xABC | analog read | `stick_left` / `stick_right` (`:31-34`) |
| `.hscan` | 0xC910 | 0xC3A8 scan | `scan_hook` (`:37-38`) |
| `.hidletask` / `.hunplug` | 0xA480 / 0xEA88 | idle / unplug | `idle_task` / `unplug_task` (`:41-44`) |
| `.hsweep` | 0xA2DC | stick sweep | `two_sweeps` (`:61-62`) |
| `.resthook` | 0xA484 | (Bluetooth branch) | `rest_main_poll` (`:66-67`) |
| `.hstall` | 0xA4AE | `tjl 0xfa68` motor_set | `stall_hook`, still calls it (`:65,70`) |
| `.hmeter` / `.hidle` | 0xB8E8 / 0xD1C8 | — | `meter_level` / `idle_limit` (`:77-84`) |
| `.hota` | 0x178CC | — | `.word no_write + 1` (`:45-46`) |

### 2.1 Bytes before/after

There is **no copy-pasteable byte pair** in the repos, and there cannot be: the encoding is a
displacement, so the four bytes change with the address of the site. The one raw encoding on
record is `tjl 0x8350` at 0x14BA = `00 90 49 9f` (`RF\NOTES.md:230`). Procedure:

1. Read the 4 stock bytes at the site and disassemble them (`tc32-elf-objdump -D -b binary
   -m tc32`) to confirm the instruction and its target.
2. Assemble the replacement at that exact VMA — the toolkit does it by giving each hook
   section the site address in `packer.ld`; `MAC` does it with `-Ttext <site>`.
3. Write the 4 resulting bytes over the 4 stock bytes. Bytes 4..N of a longer stock
   instruction span are **not** touched by the toolkit: the stock `tjl` is the whole call.
4. If you must cover a longer span (a setup sequence plus the call), overwrite the call with
   `tjl cave` and pad with `nop` (0x46C0, `c0 46`, `RF\findings\verify-iap.md:40`) up to the
   end of the span, and end the cave with `tjl <address after the hook>` — never a bare
   return, because `lr` points back into the cave (`MAC\docs\developing-patches.md:404-414`).

### 2.2 How the wrapper calls the original

The wrapper is entered with the **call site's** register state, so `r0-r3` still hold the
original arguments. It does its own work, then tail-calls the stock function with the same
arguments and returns its result:

```c
void main_tick(void) { /* ... */ motor_tick(); }                  /* packer.c:152-161  */
void main_poll(void) { settings_poll(); stock_chords(); }          /* packer.c:261-277  */
void ring_tick(...)  { /* ... */ led_driver(mode, pin, counter); } /* packer.c:389-392  */
void vendor_a(u8 *p) { ring_vendor(p); stock_vendor_a(p); }        /* packer.c:424-434  */
```

A tail call (not a call+return) keeps the stack and `lr` exactly as the stock code expects.
Nothing is saved or restored because the wrapper adds no frame of its own; `pack_xbox` uses
`u8 r[16]` on the stack only inside a leaf that returns normally (`packer.c:61-82`).

### 2.3 Alignment / range

* Alignment: **undocumented**. Every hook section is 4-byte aligned in practice (`tjl` is a
  32-bit instruction); `packer.ld` only asserts size, not alignment. Assemble at the site
  address and the assembler will place it correctly.
* Range: **undocumented, no stated limit**. Evidence of what works: 0xA43C → 0x18400 is
  +0xDFC4; `MAC` jumps 0x5B38 → 0x12340 (+0xC808) and back 0x12340 → 0x5B44 (-0xC7FC). So at
  least ±0xE000 (57 KB) is fine in both directions, which covers every region in §1.3. Do not
  infer a 16-bit limit; verify any new site with `objdump`.
* `tjl` takes an immediate only. `tjl rX` does not assemble; use `tjex rX` for an indirect
  call (`MAC\docs\developing-patches.md:364-381`).

---

## 3. `packer.c` — what it needs to be linkable

`TK\firmware-src\packer.c` (434 lines, one C file, free-standing).

* **Entry point must be first in `.packer`**: `pack_xbox` is listed first in the section and
  `ASSERT(pack_xbox == 0xb908)` (`packer.ld:8,40-41`).
* **Externals** — every one resolved by `--defsym` in `build.sh:17-19`, declared in the C file
  (`packer.c:30-31,103-106,202-208`): `ble_notify` 0x1458C, `after_notify` 0x1ABC, `motor_set`
  0xFA68, `motor_tick` 0xFAA0, `led_driver` 0xD36C, `save_settings` 0xD538, `stock_chords`
  0xCFA4, `stock_vendor_a` 0xF56C, `stock_vendor_b` 0xF594, `stock_crc` 0xAF80, `stock_scan`
  0xC3A8, `__udivsi3` 0x850C, `__divsi3` 0x8504. No libc, no `memcpy`, no startup code.
* **Memory it touches** is all hard-coded, never declared as data (`packer.c:20-26,194-231`):
  buttons 0x84342C, sticks 0x8433EC (LX) / 0x8433F6 (LY) / 0x8433E4 (RX) / 0x8433E6 (RY),
  triggers 0x84340E (LT) / 0x8433E8 (RT), ring state 0x841640 (struct ≤ 0x40 B so the settings
  record at 0x841680 survives), ring level 0x8416D0, battery 0x843415, latched 0x843423, saved
  level 0x843929, alerts flag 0x18FF0, timer 0x800740, raw buttons 0x84344C, GPIO 0x80050D /
  0x80051D / 0x80052D.
* **Position independence** comes from having no absolute data at all: state lives at fixed
  addresses, and only call targets become literal-pool words, which the linker emits for the
  section's VMA. That is why the section addresses in `packer.ld` are load-bearing.
* **Report path**: build 16 bytes on the stack, then `ble_notify(0x40, 0x1d, r, 16)` and
  `after_notify(ret)` (`packer.c:61-82`). Report ID 1 is *not* in the payload — it is declared
  in the GATT report reference.
* **Embedding**: the `.bin` is written verbatim at the recipe address; `packer.bin` is
  `ljust(0x1cc, 0xff)` into 0xB908-0xBAD4 (`TK\u2c_customfw\firmware\recipes.py:74-93`). It is
  never linked into, or merged with, the stock image, and never wrapped in a `.dat`.
* Host-side unit test: `tc32-elf-gcc` is not needed — `python3 firmware-src/test.py` compiles
  with `-DHOST` and checks the reports against a model built from `report-map.hex`
  (`TK\firmware-src\README.md:49-51`).

---

## 4. Report map / descriptor swap

**Terminology check.** Two different descriptors are involved and the repos keep them apart:

* **USB** HID descriptor — what we already patched at 0x17984 (device descriptor), 0x179E1
  (`wDescriptorLength`), 0xE858 (its pointer literal), 0xE82E (the 145→203 length immediate),
  with the descriptor itself at 0x18400. None of the three repos documents these; they are ours.
* **Bluetooth** GATT Report Map (0x2A4B). Stock 1.09: **116 bytes at 0x1734C**, served by the
  attribute table entry at **0x1779C** whose `attrLen` is at **0x177A0** and value pointer at
  **0x177A8** (`RF\findings\verify-btff.md:24-27`). 0x18274 is *not* the stock map — it is the
  blank tail the toolkit copies a new map into.

The swap is therefore **three edits and no code** (`TK\u2c_customfw\firmware\recipes.py:74-93`):

```
Edit(0x18274, report-map.bin)        # 283 bytes, ends 0x1838F
Edit(0x177A0, u32(len(map)))         # attribute length
Edit(0x177A8, u32(0x18274))          # value pointer
Edit(0x18390, noop.bin)              # output-report write callback for report 3
Edit(0x17764, u32(0x18391))          # output write callback, thumb bit set
Edit(0x18098, 03 02)                 # output report reference, report ID 3
Edit(0x172C0, c4 03)                 # appearance: gamepad
Edit(0x172DC / 0x17488, name)        # GATT name "8BitDo Ultimate 2C (X/S)" + its length
Edit(0x17254 / 0xA214, scan response) # [len+1, 9] + name; length immediate
Edit(0x173C0, 02 5e 04 13 0b 09 05)  # PnP ID 045e:0b13, version 0509
```

**Boot or enumeration?** Nothing in the repos documents a boot-time parse, and the evidence
says the map is *served from the attribute on request*: the toolkit changes only the length
word, the pointer word and the bytes behind it — no code — and it works (`TK\docs\validation.md:12`:
"the exact 283-byte Xbox descriptor" is read back, 115 reports/s). A boot-time parser would
have had to be re-run or re-pointed in code. Treat "parsed when the GATT client reads 0x2A4B"
as the working model, flagged as inference rather than a documented fact.

**Size.** Larger *and* smaller both work, because only the pointer and the length word change:

* larger: 116 → 283 B, placed in the blank tail (`recipes.py:74-93`) — proven on hardware;
* smaller: 116 → 98 B on 1.06 (`MAC\docs\firmware-patch.md:52-64`) — proven.

The constraints are (a) the map must fit somewhere blank, and (b) **the packer must emit
exactly the fields the map declares**: "a mismatch truncates reports — the BLE stack drops
whatever the map does not declare — and controls silently die" (`MAC\docs\firmware-patch.md:95-97`).
Changing identity or map needs a forget-and-re-pair, otherwise the host may keep the old
cached descriptor (`TK\docs\tweaks.md:5`).

The stock 116-byte map is a near-miss of the Xbox layout (same stick/trigger byte offsets,
8-bit fields, hat 0..7), which is why the 1.06 cave could reuse it (`firmware-patch.md:92-94`).

---

## 5. Build commands, end to end

### 5.1 Firmware blobs (`TK`)

```sh
# toolchain: flyskywhy/tc32, macos branch fdb1e7e (x86-64/Rosetta);
# identical bytes to Linux commit 8ec375bb  (TK\firmware-src\README.md:35-43)
export PATH=$TC32/bin:$PATH
firmware-src/build.sh /tmp/u2c-blobs          # gcc/as/ld/objcopy/objdump, as in §1.2
PYTHONPATH=. python3 tools/make_constants.py ultimate-2c-controller-v1.09.dat
```

`build.sh` order: compile 3 `.c` → assemble `hook.s` → link → `objcopy` 30 sections →
`objdump -d` → `report-map.hex`→`report-map.bin` (`build.sh:23`) → copy `.bin` into
`u2c_customfw/firmware/blobs/` + `sha256sum *.bin > MANIFEST` (`build.sh:24-29`).
With `EXTRA_CFLAGS` set it stops before replacing the shipped blobs (`:24-26`).

### 5.2 Image (`TK`)

```sh
python3 -m u2c_customfw ...        # build(): requires len==0x12400 and sha256==STOCK_SHA256,
                                   # verifies each edit region's hash, then applies the edits
```

`image.py` constants that matter: `BASE=0x6000`, `BANK_A_SIZE=0x12400`, `BANK_A_END=0x18400`,
**`APP_END=0x19000`**, `APP_SIZE=0x13000`, `SUPPORTED_VERSION=1.09`,
`STOCK_SHA256=dfd443c77f831008aa453ee2c1c9cb6871c36f8947c548e61d4387fba0a9da70`.

### 5.3 Writing the device (1.09 bootloader, `2DC8:3208`)

64-byte HID reports, no report ID (`TK\docs\firmware-notes.md:27`):

```
[0]=0x05 [1:3]=cmd [5:7]=len u16 [7:9]=CRC16/MODBUS [9:13]=param [13:17]=addr u32 [17:]=data
```

Order used by `TK\u2c_customfw\ops\plan.py:51-70`:

```python
boot.erase(ch, 0x6000, 0x19000-0x6000)                    # cmd 4, 4 KB units
for a in range(0x6000, 0x18400, 46):
    boot.write_chunk(ch, a, 0x12400, blk)                  # cmd 3, param = BANK_A_SIZE
for a in range(0x18400, 0x19000, 46):                      # the tail, incl. our 0x18400 data
    if any(b != 0xff for b in blk):
        boot.write_chunk(ch, a, 0x12400, blk)              # same param ("worked here on hardware")
read back 0x13000 bytes and compare
```

`RF` reaches the same bootloader with a different command set (`00` start, `c1`, `c3`
block-addr, `04`, `03`, `c4` finish, `07` reset) and the **same** CRC-16/MODBUS over
`data[18:18+len]` (poly 0xA001, init 0xFFFF, verified on 1643/1643 writes, `RF\NOTES.md:71-92`).
Our own hidapi flasher already speaks this.

---

## 6. Gotchas specific to 1.09

1. **No whole-image checksum on the 1.09 path.** Each request carries CRC-16/MODBUS over its 46
   payload bytes; there is no zlib CRC over the image and no signature (`RF\NOTES.md:71-92`, 141;
   `RF\findings\verify-btff.md:52` says "no signature / CRC-only" is *not* proven for the
   controller — the IAP RE was of the dongle). The `zlib.crc32` recipe in
   `MAC\docs\developing-patches.md:457-472` belongs to the 1.06 `.dat` container, not to 1.09.
2. **Update record at 0x3F000** (52 B): version +0x0C, pending magic +0x18, staging address
   0x40000 +0x1C, pending length +0x20, CRC +0x30. A patch is refused while a pending field is
   set (`firmware-notes.md:15-19`; `TK\SAFEGUARDS.md:11`).
3. **Dual bank.** App-mode flashing writes the *inactive* bank and the bootloader swaps on
   reboot; boot-mode flashing writes physical 0x6000 (`RF\findings\flash-read-and-bootloader.md:34-36`).
   Observed: after an app-mode flash, bank A stayed stock and bank B held the 54-byte diff. A
   bad app-mode flash bricks because the crashing bank becomes active; recovery is a boot-mode
   flash. **Implication for us: our 0x18400 descriptor lives inside bank A; if the swap
   mechanism does not copy 0x18400-0x19000, a bank swap loses it.** Not documented — verify by
   reading both banks back.
4. **0x18400-0x19000 is written by the normal flash path**, with the same parameter as bank A,
   and 0x19000 is only where an 8BitDo update stops erasing, not a bootloader limit
   (`firmware-notes.md:33`).
5. **Settings sector 0x73000**: 26 bytes the firmware owns (`TK\u2c_customfw\firmware\image.py`,
   `SETTINGS=0x73000`); the toolkit's own 35-byte `U2CS` record lives at 0x7A000
   (`firmware-notes.md:49`). Never write 0x73000+ in a normal patch: MAC 0x76000, edition flag
   0x77000, pairing sectors 0x7C000/0x7D000 (the second is erased at every start if not blank,
   `firmware-notes.md:15-19`).
6. **The first 0x2500 bytes are copied to RAM 0x0-0x2500 and run there** (`firmware-notes.md:8`):
   anything you patch in image 0x6000-0x8500 has *two* addresses, and code that references it
   must use the run address.
7. **`.ringtail` is a catch-all at 0x18400** (`packer.ld:36`) — our descriptor collides with any
   unplaced function. Give new code its own section, or move the descriptor into 0x184CB+.
8. **Assembler quirks** (`MAC\docs\developing-patches.md:364-381`, `RF\NOTES.md:122-129`):
   flag-setting ALU ops assemble *without* the `s` (`tand`, `tor`, `tshftl`, `tshftr`) though
   objdump prints `tands`/…; `tmovs` keeps its `s`; 3-operand arithmetic requires the
   destination to overlap a source; `;` comments are rejected ("junk at end of line"); the
   entry-point warning "cannot find entry symbol _start" is harmless.
9. **`--defsym` names firmware addresses; the section address is what makes the branch
   correct.** Link at file-offset space with file-offset symbols, or at image space with image
   symbols — never mix (`developing-patches.md:271-311`; `RF\NOTES.md:129`).
10. **Identity changes need a re-pair** (`TK\docs\tweaks.md:5`), and the packer, the report map
    and the GATT report reference must agree or reports are dropped silently
    (`MAC\docs\firmware-patch.md:95-97`).

---

## 7. Explicitly undocumented

* `tjl` displacement range and the alignment requirement of a hook site — no repo states them;
  only the encodings and the ±0xE000 evidence above.
* Whether the BT report map is parsed at boot or at first GATT read (inference: served from
  the attribute).
* Whether a bank swap copies 0x18400-0x19000.
* Whether the 3208 bootloader verifies anything beyond per-chunk CRC (candidate: the 4 bytes
  after the size word at 0x6018, `RF\findings\flash-angle-4.md:141`).
