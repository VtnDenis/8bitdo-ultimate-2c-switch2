# Switch 1 Pro Controller over USB HID — implementation spec

Extracted from source, not from memory. Every byte below is copied from a file; anything I could
not verify is marked **[UNVERIFIED]**.

Sources (all fetched at `main`/`master`, 2026-11-14):

| Ref | File |
|---|---|
| GP-D | `github.com/OpenStickCommunity/GP2040-CE`, `headers/drivers/switchpro/SwitchProDescriptors.h` |
| GP-H | same repo, `headers/drivers/switchpro/SwitchProDriver.h` |
| GP-C | same repo, `src/drivers/switchpro/SwitchProDriver.cpp` |
| DK-USB | `github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering`, `USB-HID-Notes.md` |
| DK-BT | same repo, `bluetooth_hid_notes.md` |
| DK-SPI | same repo, `spi_flash_notes.md` |
| DK-SUB | same repo, `bluetooth_hid_subcommands_notes.md` |

GP2040-CE is a working device-mode implementation (TinyUSB), so its constants are the reference
for "what a real console accepts". Its quirks are called out where they matter.

---

## 1. USB descriptors

### 1.1 Device descriptor — 18 bytes (GP-D:370-387)

```
12 01 00 02 00 00 00 40 7E 05 09 20 10 02 01 02 03 01
```

| Field | Value | Note |
|---|---|---|
| bcdUSB | `0x0200` | full speed |
| bDeviceClass/Sub/Proto | 0/0/0 | class is in the interface |
| bMaxPacketSize0 | 64 | |
| idVendor | `0x057E` | Nintendo |
| idProduct | `0x2009` | Pro Controller (SNES `0x2017`, N64 `0x2019`, Genesis `0x201E`) |
| bcdDevice | `0x0210` | 2.10. Real HW also reports 2.10 (community `dmesg` logs) |
| iManufacturer / iProduct / iSerial | 1 / 2 / 3 | |
| bNumConfigurations | 1 | |

GP-D's comment says "bcdDevice 4.10" — the comment is wrong, the bytes are `0x0210`. 8BitDo's
Switch-mode clone reports `0x0200`; both are accepted, so this field is not load-bearing.

### 1.2 Configuration descriptor tree — 41 bytes (GP-D:399-448)

```
09 02 29 00 01 01 00 A0 FA
09 04 00 00 02 03 00 00 00
09 21 11 01 00 01 22 CB 00
07 05 81 03 40 00 08
07 05 01 03 40 00 08
```

* config: `wTotalLength=0x0029`, 1 interface, `bmAttributes=0xA0` (bus powered + remote wakeup), `bMaxPower=0xFA` (500 mA)
* interface 0: 2 endpoints, class 3 (HID), subclass 0, protocol 0, no interface string
* HID descriptor: `bcdHID=0x0111`, 1 descriptor, type `0x22`, `wDescriptorLength=0x00CB` = **203**
* EP `0x81` IN interrupt, `wMaxPacketSize=64`, `bInterval=8`
* EP `0x01` OUT interrupt, `wMaxPacketSize=64`, `bInterval=8`

GP-D also carries a 73-byte 2-interface variant (`switch_pro_pair_configuration_descriptor`,
GP-D:450-517) for Joy-Con pairs — not needed for a Pro Controller. Its unused standalone
`switch_pro_hid_descriptor[]` (GP-D:388) says `0x00CB` in the comment but `0x00 56` in bytes; ignore it,
use the copy inside the config descriptor above.

### 1.3 HID report descriptor — 203 bytes (GP-D:519-…)

```
05 01 15 00 09 04 A1 01 85 30 05 01 05 09 19 01 29 0A 15 00 25 01 75 01 95 0A 55 00 65 00 81 02
05 09 19 0B 29 0E 15 00 25 01 75 01 95 04 81 02 75 01 95 02 81 03 0B 01 00 01 00 A1 00 0B 30 00
01 00 0B 31 00 01 00 0B 32 00 01 00 0B 35 00 01 00 15 00 27 FF FF 00 00 75 10 95 04 81 02 C0 0B
39 00 01 00 15 00 25 07 35 00 46 3B 01 65 14 75 04 95 01 81 02 05 09 19 0F 29 12 15 00 25 01 75
01 95 04 81 02 75 08 95 34 81 03 06 00 FF 85 21 09 01 75 08 95 3F 81 03 85 81 09 02 75 08 95 3F
81 03 85 01 09 03 75 08 95 3F 91 83 85 10 09 04 75 08 95 3F 91 83 85 80 09 05 75 08 95 3F 91 83
85 82 09 06 75 08 95 3F 91 83 C0
```

Decoded (byte counts are what your parser must produce):

| Item | Value |
|---|---|
| Usage Page / Usage | Generic Desktop (0x01) / Joystick (0x04), Application collection |
| Input report 0x30 | 10 buttons (1..10), 4 buttons (11..14), 2 bits padding, 4×16-bit axes (X,Y,Z,Rz = `0x010030,31,32,35`), hat `0x010039` 4 bits logical 0..7 physical 0..315, 4 buttons (15..18), 52 bytes constant → 63 bytes + ID |
| Input report 0x21 | vendor page 0xFF00, usage 0x01, 63 bytes constant |
| Input report 0x81 | usage 0x02, 63 bytes constant |
| Output report 0x01 | usage 0x03, 63 bytes |
| Output report 0x10 | usage 0x04, 63 bytes |
| Output report 0x80 | usage 0x05, 63 bytes |
| Output report 0x82 | usage 0x06, 63 bytes |

Note the 16-bit axes are *declared* with `Logical Maximum 65534` but only 12 bits are ever used
(the low nibble of the second byte is the top 4 bits of X and the high nibble is the low 4 bits of Y).
Every host that matters does the 12-bit unpack itself.

### 1.4 String descriptors (GP-D:352-367)

| Index | GP2040-CE content | Real-hardware content |
|---|---|---|
| 0 | `{0x09,0x04}` (langid 0x0409) | same |
| 1 | `"Open Stick Community"` | **[UNVERIFIED]** — 8BitDo's clone advertises `"Nintendo.Co.Ltd."` |
| 2 | `"GP2040-CE (Pro Controller)"` | `"Pro Controller"` (seen in `dmesg` on 057E:2009) |
| 3 | `"000000000001"` (12 chars) | `"000000000001"` (seen in `dmesg`, identical) |

Standard HID string encoding: `bLength=2*n+2`, `bDescriptorType=0x03`, then UTF-16LE.
GP2040 builds it in `getStringDescriptor()` at `headers/drivers/shared/driverhelper.h`. The console
does not validate these strings; only iProduct/iSerial=3 must exist because the device descriptor
points at them.

---

## 2. Input report 0x30 — 64 bytes

Byte 0 = report ID, byte 1 = rolling timestamp (GP2040 sends `last_report_counter++` per packet),
then the 10-byte standard input block, then vibrator, then 36 bytes IMU, then padding:

| Off | Field |
|---|---|
| 0 | `0x30` |
| 1 | timer, free-running, wraps at 256 |
| 2 | high nibble = battery (8 full, 6 med, 4 low, 2 critical, 0 empty), low nibble = connection info (`0` = Pro/wired). GP2040 sends `0x80` (battery 8, conn 0) |
| 3 | Y `0x01`, X `0x02`, B `0x04`, A `0x08`, SR `0x10`, SL `0x20`, R `0x40`, ZR `0x80` |
| 4 | Minus `0x01`, Plus `0x02`, RStick `0x04`, LStick `0x08`, Home `0x10`, Capture `0x20`, `0x40` unused, ChargingGrip `0x80` |
| 5 | Down `0x01`, Up `0x02`, Right `0x04`, Left `0x08`, SR `0x10`, SL `0x20`, L `0x40`, ZL `0x80` |
| 6-8 | left stick, 12-bit packed |
| 9-11 | right stick, 12-bit packed |
| 12 | vibrator / rumble ack byte. GP2040 hardcodes `0x09`; real HW sends `0x70`/`0xC0`/`0xB0` |
| 13-48 | IMU: 3 frames × (accel XYZ, gyro XYZ), 6 × int16 LE per frame. GP2040 sends all zero |
| 49-63 | zero padding |

Stick packing (GP-D:110-123, and DK-BT "Standard input report - Stick data"):

```c
uint16_t x = data[0] | ((data[1] & 0x0F) << 8);   // 0..4095
uint16_t y = (data[1] >> 4) | (data[2] << 4);     // 0..4095
```

Center = 2048 → bytes `00 08 80`. Do not send 0x7FF: real and emulated controllers both use
0..4095 with center 2048. GP2040 clamps to `[center-min, center+max]` from its calibration table
and negates Y (`switchReport.inputs.leftStick.setY(-ly)`, GP-C:389).

All-zero resting state GP2040 would emit (before calibration clamping):

```
30 00 80 00 00 00 00 08 80 00 08 80 09 | 00 × 36 | 00 × 15
```

The dpad is *not* a hat: it is four independent bits in byte 5.

**The report descriptor does not describe this payload — do not parse with it.** Nintendo's
descriptor packs 2 bytes of buttons (16 bits) + 8 bytes of axes + 1 byte of hat/buttons + 52 const,
which puts "axis X" at packet bytes 4-5 and the hat at packet byte 12 (the vibrator byte). The real
layout above starts one byte later because byte 2 is the battery/connection byte, which the
descriptor has no field for. The console (and Linux `hid-nintendo`) parse raw byte offsets, never
the HID field map; the descriptor's only jobs are declaring report IDs 0x30/0x21/0x81/0x01/0x10/0x80/0x82
and their sizes. Copy it byte-for-byte for compatibility, then parse by §2.

---

## 3. Output reports and subcommand frames

### 3.1 Wire framing

All OUT reports are 64 bytes. Byte 0 is the report ID. The console sends them as **interrupt OUT
transfers**, not HID control SET_REPORT, so byte 0 is the report ID on the wire (TinyUSB passes the
raw buffer to `tud_hid_set_report_cb` with `report_id = 0`, which is why GP2040 can read
`buffer[0]` as the ID — `src/class/hid/hid_device.c`, `hidd_xfer_cb`).
There are two independent command channels:

**A. report ID `0x80` — USB/session config (NOT subcommands).** Byte 1 is the "USB command"
(0x01 identify, 0x02 handshake, 0x03 baud, 0x04 disable timeout, 0x05 enable timeout, 0x06 reset,
0x91/0x92 raw UART). Reply is report ID `0x81` with the same byte 1 (DK-USB).

**B. report ID `0x01` — rumble + subcommand.** Byte 1 = packet counter (0..0xF), bytes 2-9 = 8 bytes
of rumble (neutral `00 01 40 40 00 01 40 40`), byte 10 = subcommand ID, bytes 11.. = args
(DK-BT "OUTPUT 0x01"). `0x10` = rumble only, no subcommand. Reply is report ID `0x21`.

**Reply framing for 0x21** (DK-BT "Standard input report", corroborated by GP-C:575-577):

| Off | Content |
|---|---|
| 0 | `0x21` |
| 1 | timer (echo/increment) |
| 2 | battery/connection, same encoding as 0x30 |
| 3-5 | current buttons |
| 6-11 | current sticks |
| 12 | vibrator |
| 13 | ACK byte: `0x80` simple ack, `0x90` ack+data (SPI), `0xA0`/`0xB0`/`0xC0`/`0xD0` typed acks, `0x00` = NACK |
| 14 | subcommand ID, echoed |
| 15-49 | reply payload, max 35 bytes |

GP2040 implements exactly this: `report[0]=0x21`, `report[1]=last_report_counter`,
`memcpy(report+2, &switchReport.inputs, 10)`, then writes `report[13]`/`[14]`/`[15..]` (GP-C:575-577).

### 3.2 The `0x80` set — what the console sends at plug-in

| Console OUT | GP2040 reply | Meaning |
|---|---|---|
| `80 01` | `81 01 00 <type> <mac BE ×6>` | IDENTIFY. Byte 2 = `0x00`, byte 3 = controller type (`0x03` = Pro Controller), bytes 4-9 = MAC in big-endian. Sample from real HW: `81 01 00 02 57 30 EA 8A BB 7C` (type 2 = right Joy-Con) |
| `80 02` | `81 02` | HANDSHAKE. **Only legal once per session** (DK-USB) |
| `80 03` | `81 03` | switch UART to 3 Mbit. Only legal after `80 02`; a second `80 02` is allowed after it |
| `80 04` | `30 04` in GP2040 **[UNVERIFIED vs real HW]** | DISABLE USB TIMEOUT — the one that makes the controller stop reverting to Bluetooth. GP2040 flips `isReady=true` here and starts streaming 0x30 |
| `80 05` | `30 05` | re-enable timeout (BT fallback) |

GP2040 additionally sends an **unsolicited** `81 01 ...` identify right after init
(`SwitchProDriver::process`, GP-C:470-480, via `sendIdentify()` GP-C:499-509) and again whenever
`80 01` arrives. Emitting it unprompted is safe — DK-USB notes the controller re-sends identify
on connection-state changes.

MAC: GP2040 stores it little-endian at boot and reverses it on the wire
(`report[4+i] = deviceInfo.macAddress[5-i]`, GP-C:506-508), with the first three bytes fixed at
`7C BB 8A` and the last three random. Any locally-administered MAC works.

### 3.3 The `0x01` subcommand set

`ACK` column = byte 13 of the 0x21 reply, `ID` = byte 14, `DATA` = bytes 15.. as described.

| Sub | Name | ACK | Data |
|---|---|---|---|
| `0x00` | GET_CONTROLLER_STATE | `0x80` | — |
| `0x01` | BT pair request | `0x81` | `0x03` (GP2040 stub) |
| `0x02` | REQUEST_DEVICE_INFO | `0x82` | 12 bytes: fw major `0x04`, fw minor `0x91`, type `0x03`, `0x02`, MAC BE 6, `0x01`, `0x02` |
| `0x03` | SET_MODE | `0x80` | echo the mode byte (`0x30` = full input) |
| `0x04` | TRIGGER_BUTTONS | `0x83` | — (real HW: 7×uint16LE, units of 10 ms) |
| `0x08` | SET_SHIPMENT | `0x80` | — (console sends `08 00` after every connection) |
| `0x10` | **SPI READ** | `0x90` | echo addr(4)+len(1), then `len` bytes — see §4 |
| `0x21`/`0x22` | NFC/IR config/state | `0x80` | — |
| `0x30` | SET_PLAYER_LIGHTS | `0x80` | — (arg: low nibble solid, high nibble flashing) |
| `0x31` | GET_PLAYER_LIGHTS | `0xB0` | 1 byte same bitfield |
| `0x33` | unknown (Chromium probe) | `0x80` | `0x03` — GP-C:709 says Chromium sends it to detect a Switch controller; can be ignored |
| `0x38` | SET_HOME_LIGHT | `0x80` | `0x00` (GP2040 doesn't animate) |
| `0x40` | TOGGLE_IMU | `0x80` | `0x00` |
| `0x41` | IMU sensitivity | `0x80` | — |
| `0x43` | READ_IMU (raw regs) | `0xC0` | echo addr,size |
| `0x48` | ENABLE_VIBRATION | `0x80` | `0x00` |
| `0x50` | GET_VOLTAGE | `0xD0` | `83 06` (0x0683 ≈ 1.667 V, GP2040 constant) |
| other | — | `0x80` | echo sub ID, then `0x03` (GP2040 default branch, GP-C:767-773) |

**Your question's frame numbering is off**: `0x80 0x10`, `0x80 0x30`, `0x80 0x33`, `0x80 0x40`,
`0x80 0x48` do not exist. SPI read / player lights / home light / IMU / vibration are all
subcommands inside report `0x01` (byte 10), not `0x80` commands. Only 0x01..0x06, 0x91, 0x92 live
under `0x80`.

---

## 4. SPI flash content — the only data the console actually reads

Request: `01 <ctr> <rumble ×8> 10 <addr LE32> <len>`. Max len per DK-SUB is `0x1D`; GP2040 allows up
to 44 (64-byte report minus 20 bytes of header, GP-C:659).

GP2040 keeps two constant arrays and serves any read whose 256-byte bank is `0x6000` or `0x8000`;
everything else returns `0xFF` (`readSPIFlash`, GP-C:799-817). That is the whole "calibration"
strategy: **fixed synthetic values, no measurement, no per-device data.**

| Address | Len | Meaning | GP2040 source |
|---|---|---|---|
| `0x6020` | `0x18` | 6-axis factory calibration | GP-H:107-113 |
| `0x603D` | `0x09` | left stick factory cal | GP-H:117-118 |
| `0x6046` | `0x09` | right stick factory cal | GP-H:120-122 |
| `0x6050`/`0x6053`/`0x6056`/`0x6059` | 3 each | body/button/left-grip/right-grip RGB | GP-H:125-134 |
| `0x8010` | `0x16` | user stick calibration (magic `B2 A1` ×2) | GP-H:176-190 |

Exact bytes returned (offsets are relative to the requested address):

```
0x6020 (24 B): E3 FF 39 FF ED 01 00 40 00 40 00 40 09 00 EA FF A1 FF 3B 34 3B 34 3B 34
0x603D (21 B): A4 46 6A 00 08 80 A4 46 6A | 00 08 80 A4 46 6A A4 46 6A | FF
0x8010 (36 B): B2 A1 | A4 46 6A 00 08 80 A4 46 6A | B2 A1 | 00 08 80 A4 46 6A A4 46 6A | FF×16
0x6080 (24 B): 50 FD 00 00 C6 0F | 0F 30 61 AE 90 D9 D4 14 54 41 15 54 C7 79 9C 33 36 63 | 0F 30 61 AE ...
```

Decoded stick calibration from those bytes (DK-SPI algorithm, confirmed against GP-D:126-159):

| Value | Left | Right |
|---|---|---|
| center X,Y | 2048, 2048 | 2048, 2048 |
| max above center X,Y | 1700, 1700 | 1700, 1700 |
| min below center X,Y | 1700, 1700 | 1700, 1700 |

So rest = 2048 and full deflection = 348/3748 — deliberately conservative. The Switch sanity-checks
calibration and falls back to an internal default if it looks wrong (DK-SPI, end of file), so
plausible-but-fake values are fine; `FF`-filled is also fine for regions you don't care about.

Also in the 0x6000 bank (read by some hosts, not required): `0x6000` serial (16 bytes), `0x6012`
device type `0x03`, `0x6013` `0xA0`, `0x601B` color-info `0x02`. GP2040 returns `FF`×16 at 0x6000
(i.e. "no serial number") and `03 A0` at 0x6012.

Offsets `0x6086..0x6096` are "stick device parameters 1"; GP2040's array places `0x63` at
`0x6097` where DK-SPI's table puts the start of params 2 at `0x6098` — **[UNVERIFIED]**, an
off-by-one that nobody appears to read. Don't copy that tail verbatim.

---

## 5. Minimum viable set for the console to accept it

Ordered by what actually gates progress, based on GP2040's state machine (`isInitialized` →
first identify, `isReady` → first 0x30 stream, GP-C:445-480, 536-546):

1. **Enumeration**: the 18-byte device descriptor, the 41-byte config tree with the 203-byte report
   descriptor, and both 64-byte interrupt endpoints. Required: reports `0x30` (in), `0x21` (in),
   `0x01` (out), `0x80` (out). `0x81`/`0x82`/`0x10` are declared but never used by GP2040.
2. **`80 01` → `81 01 00 03 <mac>`**. Without a correct type byte the console treats it as a Joy-Con.
3. **`80 02` → `81 02`** (once), **`80 03` → `81 03`**.
4. **`80 04` → any reply**. This is the gate: the console keeps the controller in USB mode only after
   this, and this is where the device may start pushing `0x30`.
5. **`01 02` → device info**, **`01 03` → mode ack**, **`01 10` → SPI reads**, **`01 30` → lights ack**.
6. IMU (`01 40/41/43/48`), home light (`01 38`), rumble (`0x10`), voltage (`01 50`) are all
   acknowledge-and-ignore in GP2040 and are **not** required — proof: GP2040 sends 36 zero bytes of
   IMU data and the console still accepts input.
7. `01 04`, `01 08`, `01 21/22`, `01 33` can be answered with a bare ack; GP2040's default branch
   (ack + `0x03`) covers anything you miss.

Not gating: the strings (any iProduct works), `bcdDevice`, the MAC's first three bytes, the
identify's byte 2 (`0x00` in both sources), and the 0x30 report's IMU/rumble bytes.

---

## 6. Order and timing

```
enumeration              host reads device/config/report descriptors
   ↓
device → 81 01 ...       optional, GP2040 does it proactively and again on request
host  → 80 01            IDENTIFY        → 81 01 00 03 <mac BE>
host  → 80 02            HANDSHAKE       → 81 02          (once per session)
host  → 80 03            BAUD            → 81 03
host  → 80 04            DISABLE TIMEOUT → ack, now "ready"
host  → 01 02  (…10 bytes…)             → 21 .. 82 02 <device info>
host  → 01 03 30                        → 21 .. 80 03 30
host  → 01 10 20 60 00 00 18            → 21 .. 90 10 <addr> <len> <24 bytes>
host  → 01 30 <bitfield>                → 21 .. 80 30
loop: device pushes 0x30 every ≤5 ms (GP-C:9, SWITCH_PRO_KEEPALIVE_TIMER)
```

* `bInterval = 8` ms in the descriptor, but GP2040 force-sends a `0x30` every **5 ms** whenever the
  payload changed or the keepalive expired (GP-C:431-469). The console tolerates far slower, but
  sending nothing is what triggers the BT fallback that `80 04` disabled.
* `80 02` may be sent **once per session**; `80 03` only after it (DK-USB). Resetting
  `isInitialized`/`isReady` on USB bus reset is what GP2040 does in `switchpro_reset` (GP-C:33-36).
* Replies are queued, not sent from the OUT callback: GP2040 sets `isReportQueued` and sends on the
  next `process()` tick, at most one per 5 ms (GP-C:430-438). Doing it inline in the OUT handler also
  works, but keep it to one IN transfer at a time.
* Answer a subcommand within the same session; the console retries, but a missing `0x90` SPI reply
  stalls input setup indefinitely.

---

## 7. Things I could not verify from source

* The exact reply the *real* controller gives to `80 04`/`80 05` (GP2040 replies `30 04`/`30 05`,
  which looks like a bug but is known to work). Safe bet: reply `81 04`/`81 05`, or nothing.
* Whether the genuine `iManufacturer` string is `"Nintendo"` or `"Nintendo.Co.Ltd."` — only a
  clone's string and a `dmesg` product name were observed.
* Whether the console requires `80 03` (baud) before `80 04`; DK-USB only states the ordering
  constraint, not that it is mandatory over USB.
* The `0x6080` tail layout off-by-one noted in §4.
* Switch 2 Pro (`src/drivers/switch2pro/`, idProduct `0x2069`, bcdDevice `0x0201`, bDeviceClass
  `0xEF/02/01` IAD) exists and is a different protocol — not covered here.
