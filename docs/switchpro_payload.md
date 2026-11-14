# What `src/packer.c` and `src/handler.c` put on the wire

Byte-exact record of the two files, with the reason for each byte. Everything below was checked
against the GP2040-CE sources (see §7) or against `switch_pro_usb_spec.md`, not from memory.
Anything I could not verify is marked **[UNVERIFIED]**.

Build (freestanding, no libc, no `stdint.h`):

```
tc32-elf-gcc -Os -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -c src/packer.c
tc32-elf-gcc -Os -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -c src/handler.c
```

`unsigned long` is 32-bit on tc32 (ILP32), which the `u32` typedef relies on.

---

## 1. Input report `0x30` — `pack_report()`

64 bytes, always. The caller may pass an uninitialised buffer: bytes 13..63 are cleared inside.

| Off | Value | Why |
|---|---|---|
| 0 | `30` | Report ID. Declared by `0x85 0x30` in the report descriptor. |
| 1 | rolling counter | `s_tick++`, wraps mod 256. GP2040 sends `last_report_counter++` per packet. The console uses it only to detect stalled/frozen input, so any monotonic byte works; it must change when the payload is re-sent. |
| 2 | `80` | Low nibble = connection info `0` (wired Pro Controller, GP2040's `connectionInfo = 0`), high nibble = battery `8` (full, `batteryLevel = 0x08`). Byte order is fixed by `SwitchInputReport`'s bitfields: `connectionInfo : 4` first, then `batteryLevel : 4`. |
| 3 | buttons | `Y`=0x01 `X`=0x02 `B`=0x04 `A`=0x08 `SR`=0x10 `SL`=0x20 `R`=0x40 `ZR`=0x80. SR/SL have no physical button on the 2C and stay 0. |
| 4 | buttons | `Minus`=0x01 `Plus`=0x02 `RStick`=0x04 `LStick`=0x08 `Home`=0x10 `Capture`=0x20 bit 6 unused, `ChargingGrip`=0x80. GP2040 sets ChargingGrip for every Pro Controller (`controllerType == SWITCH_TYPE_PRO_CONTROLLER`), so we do too; `CHARGING_GRIP 0` disables it. |
| 5 | buttons | `Down`=0x01 `Up`=0x02 `Right`=0x04 `Left`=0x08 `SR`=0x10 `SL`=0x20 `L`=0x40 `ZL`=0x80. The d-pad is four independent bits here, not a hat. |
| 6-8 | left stick | 12-bit packed: `d[0]=x&0xFF`, `d[1]=((x>>8)&0x0F)\|((y&0x0F)<<4)`, `d[2]=y>>4`. Centre 2048/2048 = `00 08 80`. |
| 9-11 | right stick | same packing |
| 12 | `09` | Vibrator / rumble-ack byte. GP2040 hardcodes `0x09` after every `process()`. Real HW cycles `0x70`/`0xC0`/`0xB0`; the console does not read it. |
| 13-48 | `00` × 36 | IMU: 3 frames × (accel XYZ, gyro XYZ) as int16 LE. All zero = "no motion". GP2040 also sends 36 zero bytes and the console accepts input, which is the proof this region is not gating. |
| 49-63 | `00` × 15 | Zero padding to the 64-byte endpoint size (`SwitchProReport.padding[15]`). |

Resting frame, no buttons held, sticks centred (`s_tick` = 0, `CHARGING_GRIP` = 1):

```
30 00 80 00 80 00 00 08 80 00 08 80 09 + 00 × 51
```

Note byte 4 is `80`, not `00`, because of ChargingGrip. GP2040's "all-zero" struct dump in the spec
is taken before that bit is set.

### Stick orientation

`struct input_state` holds 0..4095 **already in report orientation** (0 = up/left, 4095 =
down/right, 2048 = centre). GP2040 negates Y (`leftStick.setY(-ly)`) because its gamepad layer is
Y-down; doing the negation here instead would bake an assumption about the firmware's ADC sign into
the injectable blob. Whoever fills `g_input` (`packer.c:g_input`) applies the sign. Values are
clamped to 0..4095, not masked, so a bogus 0x1000 becomes full deflection instead of wrapping to 0.

### Button mapping is a placeholder

`parse_buttons()` (`src/packer.c`) reads one 32-bit word from `BTN_WORD_ADDR` (default `0x84342C`)
and maps bit indices `BIT_Y` … `BIT_RIGHT` to wire bits. Only the indices are guesses; the wire
positions are not. Every index carries a `TODO(map)` marker. Until the button-scan routine is read
out of the disassembly, buttons will be scrambled.

---

## 2. Session channel `0x80` → reply `0x81`

`usb_out_handler()` matches `in[0] == 0x80`, switches on `in[1]`, and always answers with report ID
`0x81`, byte 1 echoed, 64 bytes. Unrecognised `0x80` commands (`0x91`/`0x92` raw UART) return 0 and
are not answered.

| Host sends | Reply (first 10 bytes, rest `00`) | Why |
|---|---|---|
| `80 01` | `81 01 00 03 7C BB 8A 00 00 01` | IDENTIFY. Byte 2 `00` (GP2040 sends 0), byte 3 = controller type `0x03` = `SWITCH_TYPE_PRO_CONTROLLER` (verified from the enum), bytes 4-9 = MAC in wire order. A wrong type byte makes the console treat the device as a Joy-Con. |
| `80 02` | `81 02` | HANDSHAKE. Legal once per session. |
| `80 03` | `81 03` | Baud rate 3 Mbit. Reply only; there is no UART to reconfigure over USB. |
| `80 04` | `81 04` | DISABLE USB TIMEOUT. The gate: after this the console stays in USB mode and expects `0x30` frames. |
| `80 05` | `81 05` | ENABLE USB TIMEOUT (BT fallback re-armed). |
| `80 06` | `81 06` | RESET. |

MAC byte order: GP2040 stores `{0x7c, 0xbb, 0x8a, r, r, r}` and then *reverses* it for identify
(`report[4+i] = mac[5-i]`) but copies it unreversed for device info — the two replies disagree with
each other. We use one fixed array, `7C BB 8A 00 00 01`, in the same order in both, which matches
the fixed-first layout of the real-HW identify sample in the spec (`81 01 00 02 57 30 EA 8A BB 7C`).

**Deviation from GP2040:** GP2040 answers `0x04`/`0x05` with report ID **`0x30`**, i.e.
`30 04` / `30 05` (its `handleConfigReport` sets `report[0] = REPORT_OUTPUT_30` for those two
cases). That looks like a latent bug, and the spec marks the real hardware's answer
**[UNVERIFIED]**. This implementation replies `0x81 04` because that is the framing
decompiled DK-USB notes describe for the whole `0x80` channel. If the console stalls right after
`80 04`, the first thing to change is `reply_81()` → `0x30` for `0x04`/`0x05`; the session
never reaches the subcommands without that ack being accepted, so it will be obvious.

---

## 3. Subcommand channel `0x01` → reply `0x21`

`in[0] == 0x01`. Bytes 1 = packet counter, 2-9 = rumble (ignored), 10 = subcommand, 11.. = args.
Every reply is 64 bytes and starts with the same header:

| Off | Value | Why |
|---|---|---|
| 0 | `21` | Report ID |
| 1 | `pack_tick()` | Same counter value the next `0x30` will carry, like GP2040's `report[1] = last_report_counter` |
| 2-11 | controller state | Battery/connection + 3 button bytes + both sticks. `pack_input_block()` is shared with `pack_report()`, so the reply and the `0x30` stream can never disagree. |
| 12 | `09` | Vibrator |
| 13 | ACK | `80` simple, `82` device info, `90` SPI, `B0` lights, `C0` IMU, `D0` voltage |
| 14 | subcommand | echoed, except `0x02` which also echoes `0x02` |
| 15-63 | payload | zero-filled unless the subcommand writes here |

| Sub | Name | ACK | Payload from `[15]` |
|---|---|---|---|
| `00` | GET_CONTROLLER_STATE | `80` | `03` |
| `02` | REQUEST_DEVICE_INFO | `82` | `04 91 03 02 7C BB 8A 00 00 01 01 02` — 12 bytes at `[15..26]`: fw major `04`, fw minor `91`, type `03`, `02`, MAC ×6, `01`, `02` |
| `03` | SET_MODE | `80` | `in[11]` echoed (console sends `30` = full input) |
| `10` | SPI_READ | `90` | `in[11] in[12] in[13] in[14] in[15]` = addr LE32 + len, then `len` bytes at `[20..]`; §4 |
| `30` | SET_PLAYER_LIGHTS | `80` | — (bitfield stored in `s_player`) |
| `31` | GET_PLAYER_LIGHTS | `B0` | `s_player` |
| `38` | SET_HOME_LIGHT | `80` | `00` (accepted, not animated) |
| `40` | TOGGLE_IMU | `80` | `00` (no IMU in the report) |
| `41` | IMU sensitivity | `80` | — |
| `43` | READ_IMU raw regs | `C0` | `in[11] in[12]` (address, size echoed) |
| `48` | ENABLE_VIBRATION | `80` | `00` |
| `50` | GET_VOLTAGE | `D0` | `83 06` (`0x0683` ≈ 1.667 V, GP2040 constant) |
| other | — | `80` | `03` |

`0x01`/`0x04`/`0x08`/`0x21`/`0x22`/`0x33` all land in the default branch (`ack` + `03`), which is
what GP2040 does for everything it does not special-case. Not implemented at all: OUT reports
`0x10` and `0x82` (firmware update) and report `0x00` — `usb_out_handler()` returns 0 for them.

SPI read length: `len > 44` returns 1 **without sending a reply**, matching GP2040's
`reportData[15] > sizeof(report) - 20` guard. `len ≤ 44` fills `[20..63]` exactly.

---

## 4. SPI flash image

Requests look like `01 <ctr> <rumble ×8> 10 <addr LE32> <len>`. Served from two `static const`
tables; any other bank, and any offset past a table, returns `0xFF` — byte-identical to
GP2040's `readSPIFlash()`, which only has data for banks `0x6000` and `0x8000`.

### Bank `0x6000` (`k_spi6000`, 176 bytes, covers `0x6000..0x60AF`)

```
0x6000  FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF
0x6010  FF FF 03 A0 FF FF FF FF FF FF FF 02 FF FF FF FF
0x6020  E3 FF 39 FF ED 01 00 40 00 40 00 40 09 00 EA FF
0x6030  A1 FF 3B 34 3B 34 3B 34 FF FF FF FF FF A4 46 6A
0x6040  00 08 80 A4 46 6A 00 08 80 A4 46 6A A4 46 6A FF
0x6050  1B 1B 1D FF FF FF EC 00 8C EC 00 8C 01 FF FF FF
0x6060  FF ×16
0x6070  FF ×16
0x6080  50 FD 00 00 C6 0F 0F 30 61 AE 90 D9 D4 14 54 41
0x6090  15 54 C7 79 9C 33 36 63 0F 30 61 AE 90 D9 D4 14
0x60A0  54 41 15 54 C7 79 9C 33 36 63 FF FF FF FF FF FF
```

| Region | Offsets | Content | Read by the console? |
|---|---|---|---|
| serial number | `0000-000F` | all `FF` = "no serial" | no |
| device type | `0012` | `03` = Pro Controller | maybe (`0x6012`, len 1) |
| unknown | `0013` | `A0` | — |
| color info | `001B` | `02` | — |
| 6-axis factory cal | `0020-0037` | 24 bytes | yes, `0x6020` len `18` |
| left stick cal | `003D-0045` | `A4 46 6A 00 08 80 A4 46 6A` | yes, `0x603D` len `09` |
| right stick cal | `0046-004E` | `00 08 80 A4 46 6A A4 46 6A` | yes, `0x6046` len `09` |
| body / button / grip RGB | `0050`, `0053`, `0056`, `0059` | `1B1B1D`, `FFFFFF`, `EC008C`, `EC008C` | cosmetically |
| region | `005C` | `01` | — |
| stick device params | `0080-0085` | 6 bytes | rarely |
| stick params 1 | `0086-0097` | 18 bytes | rarely |
| stick params 2 | `0098-00A9` | 18 bytes | rarely |

The calibration decodes (12-bit packing, same as the stick bytes in `0x30`) to centre `2048,2048`,
max above centre `1700,1700`, min below centre `1700,1700` for both sticks — so rest is 2048 and
full deflection is 348/3748. That is deliberately conservative and is exactly what GP2040 serves.
The console sanity-checks calibration and falls back to internal defaults if it looks implausible,
so these values are safe but not measured.

Note `0x6080` starts *stick device parameters* at `0x6080`, params 1 at `0x6086` and params 2 at
`0x6098`. GP2040's `SwitchFactoryConfig` struct declares `unknown06[36]` where its initialiser only
supplies 35 bytes, so the struct view and the byte array disagree by one; the **byte array** is what
gets served, and it matches DK-SPI's map. The table above follows the byte array.

### Bank `0x8000` (`k_spi8000`, 63 bytes)

```
0x8000  FF ×16
0x8010  B2 A1 A4 46 6A 00 08 80 A4 46 6A B2 A1 00 08 80
0x8020  A4 46 6A A4 46 6A FF FF FF FF FF FF FF FF FF FF
0x8030  FF FF FF FF FF FF 00 00 00 00 00 00 00 00 00
```

`B2 A1` is the "calibration present" magic, once before each stick block. User calibration is read
at `0x8010` with len `0x16`. The nine `00` bytes at `0x8036..0x803E` are there because GP2040's
`userCalibrationData` is declared `[0x3F]` and its initialiser stops at `0x8036`; keeping them makes
the reply byte-identical rather than "0xFF everywhere past the data".

---

## 5. Session order, what is mandatory, and confidence

What the console actually does, in order, from `switch_pro_usb_spec.md` §6 (which was itself
extracted from DK-USB and GP2040's state machine):

```
enumeration                      host reads device / config / report descriptors
   ↓
device → 81 01 00 03 <mac>       optional, GP2040 volunteers it before being asked
host  → 80 01                     IDENTIFY
host  → 80 02                     HANDSHAKE            (once per session)
host  → 80 03                     BAUD 3 Mbit
host  → 80 04                     DISABLE USB TIMEOUT
host  → 01 02                     REQUEST_DEVICE_INFO
host  → 01 03 30                  SET_MODE
host  → 01 10 20 60 00 00 18      SPI_READ (several)
host  → 01 30 <bits>              SET_PLAYER_LIGHTS
loop:  device pushes 0x30 every ≤5 ms
```

| Step | Strictly mandatory? | Confidence | Basis |
|---|---|---|---|
| `80 01` → `81 01 00 03 <mac>` | **yes** | high | The type byte is the only thing that makes the console treat the device as a Pro Controller rather than a Joy-Con; `0x03` comes from the `SwitchControllerType` enum, and the reply shape from `sendIdentify()` plus a real-HW capture in the spec. |
| `80 02` → `81 02` | probably yes | medium | The console sends it; GP2040 answers it unconditionally. DK-USB only says it is legal **once per session** and that `80 03` must follow it. Whether the console aborts if the ack is missing is not documented anywhere I could read. Cheapest to just answer. |
| `80 03` → `81 03` | no | low-medium | It switches the *Bluetooth* UART to 3 Mbit. Over USB there is no UART to switch, and the spec's §7 lists "is `80 03` required over USB" as unverified. Answering costs nothing, so it is answered. |
| `80 04` → ack | **yes, this is the gate** | high that it gates, medium that `0x81 04` is the accepted shape | GP2040 sets `isReady` here and only then starts streaming `0x30`; the spec calls it the step that keeps the controller out of BT fallback. The reply ID is the one open question — see §2. |
| `01 02` device info | **yes** | high | 12-byte structure with fw `04 91`, type `03`; GP2040 implements it and the console reads it to identify the pad. |
| `01 03` SET_MODE | **yes** | high | The console sets the input mode to `0x30`; GP2040 echoes it. |
| `01 10` SPI reads | **yes** | high | A missing `0x90` reply stalls input setup indefinitely (spec §6); the calibration values are what the console sanity-checks. |
| `01 30` player lights | yes-ish | medium | GP2040 acks it; the console sends it every connection. A bare ack is enough. |
| `01 40/41/43/48`, `01 38`, `01 50` | **no** | high | GP2040 acks and ignores IMU, home light and voltage, and sends 36 zero IMU bytes while the console still accepts input — that is the direct proof. |
| `01 04/08/21/22/33` | no | high | Default branch (ack + `03`) covers them; Chromium's `0x33` probe is explicitly ignorable. |
| pushing `0x30` | **yes** | high | Nothing to send = the BT fallback `80 04` disabled. Cadence: GP2040 force-sends every 5 ms if the payload changed or the keepalive expired, even though `bInterval` is 8 ms. |
| unsolicited `81 01` at init | no | high | Safe (the controller re-sends identify on connection-state change), but this package does not send it; answering `80 01` is enough. |

`0x30` must be 64 bytes on the wire, and the endpoint is declared 64 bytes, so the length is fixed
by the descriptor rather than by a choice.

---

## 6. Assumptions

1. OUT reports arrive as 64-byte interrupt transfers with the report ID in byte 0. Both code paths
   rely on this; a control-transfer SET_REPORT would put `0` in byte 0 and be ignored (returns 0).
2. `send_in(buf, 64)` is called at most once per handler invocation, and the caller copies or
   transmits before the next call — `s_reply` is a single static buffer.
3. `usb_out_handler()` is not re-entered. If the firmware can call it from an interrupt while
   another call is mid-reply, the replies must be queued instead (GP2040 queues out of the OUT
   callback for exactly this reason). **[UNVERIFIED how this firmware's EP callback is dispatched]**
4. The 32-bit button word at `0x84342C` is read atomically (single aligned load) and is not mutated
   by an ISR mid-read. True on tc32 for an aligned word access.
5. `g_input` axes are already report-oriented 12-bit values; this file applies no sign.
6. The MAC is arbitrary. Any locally-administered value works; GP2040 randomises the last three
   bytes and the console does not validate them.
7. No IMU. Reporting zeroes is known-good (GP2040).
8. `0x30` byte 12 (`0x09`) is cosmetic; the console ignores it.
9. Wiring: `packer.c` exports `pack_report`, `pack_input_block`, `pack_tick`, `parse_buttons` and the
   mutable `g_input`; `handler.c` imports `pack_input_block`/`pack_tick` and the firmware's
   `send_in`. The two files are meant to be compiled together; neither defines `send_in`.

---

## 7. Open TODOs

| # | TODO | Needs |
|---|---|---|
| 1 | `TODO(map)`: bit indices `BIT_Y` … `BIT_RIGHT` in `packer.c` are placeholders. Buttons will be scrambled until the button-scan routine is read out of the disassembly. | disassembly of the stock 1.09 button scan, one bit per button |
| 2 | `TODO(map)`: axis globals. `g_input` must be filled from the firmware's ADC values; addresses unknown. | global addresses for LX/LY/RX/RY, plus their sign convention and centre |
| 3 | RAM placement. `s_tick`, `s_reply`, `s_player` and `g_input` live in `.bss`/`.data`. Injecting this as a raw blob into a flash code cave leaves them without storage. | either a small reserved RAM block, or point them at reclaimable firmware RAM |
| 4 | `80 04`/`80 05` reply ID: `0x81` here vs `0x30` in GP2040. Only matters if the console stalls at the gate. | one test on real hardware |
| 5 | `0x91`/`0x92` raw UART passthrough on channel `0x80` is not implemented (returns 0). | only if something other than the console is driven |
| 6 | OUT reports `0x10`/`0x82` and `0x00` are ignored. Those are the firmware-update path. | only if firmware update over USB is wanted |
| 7 | Nothing in this package was compiled — no toolchain on this machine. Review is by eye. | `tc32-elf-gcc` build + a `-Wall -Wextra` pass |

---

## 8. Sources

- `OpenStickCommunity/GP2040-CE`, `src/drivers/switchpro/SwitchProDriver.cpp` — `process()`,
  `sendIdentify()`, `handleConfigReport()`, `handleFeatureReport()`, `readSPIFlash()`.
- same repo, `headers/drivers/switchpro/SwitchProDriver.h` — `factoryConfigData[]`,
  `userCalibrationData[]`, `SwitchDeviceInfo`, the calibration accessors.
- same repo, `headers/drivers/switchpro/SwitchProDescriptors.h` — `SwitchProReport` (64 bytes),
  `SwitchInputReport` (10 bytes), `SwitchFactoryConfig`, `SwitchUserCalibration`,
  `SwitchControllerType` (`SWITCH_TYPE_PRO_CONTROLLER = 0x03`), `SwitchReportID`, `SwitchCommands`,
  the report descriptor.
- `dekuNukem/Nintendo_Switch_Reverse_Engineering` — `USB-HID-Notes.md`, `bluetooth_hid_notes.md`,
  `spi_flash_notes.md`, `bluetooth_hid_subcommands_notes.md`.
- `switch_pro_usb_spec.md` (local, verified summary of the above).
