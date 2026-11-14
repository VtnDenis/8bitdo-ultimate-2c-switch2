# 8BitDo Ultimate 2C — input-report packers, firmware 1.09

Target: `u2c_109.dat`, payload = file bytes `[0x1c : 0x1c+0x12400]` (74 752 B), loaded at **0x6000**
(image address = objdump address, payload offset = addr − 0x6000).
CPU: Telink **TC32** (16-bit, Thumb-like). Disassembly: `tc32-elf-objdump -D -b binary -m tc32 --adjust-vma=0x6000`.
All 32-bit constants below were read as little-endian words out of the file, not guessed from the
(rescaled) literal pools.

---

## 0. Executive summary

| # | Function | Address | Mode gate (proven) | Wire bytes | Sender |
|---|----------|---------|--------------------|-----------|--------|
| A | default / wireless packer | `0xF0B0` | `byte[0x842E38]==0` **and** `byte[0x843468]==0` | 20 B, starts `14` | `0xEBFC` |
| B | wired DInput packer | `0xF158` | `byte[0x842E38]==1` | 10 B, starts `01` (report ID 1) | `0xEDE8` |
| C | vendor packer (mode 2) | `0xF238` | `byte[0x843468]!=0` **and** `byte[0x8434D9]==2` | 64 B, starts `02 02 66` | `0xF1F4` → `0xEB04` |

All three are called from one task, `0xF378`, every main-loop pass (`0xA45C: 9004 9f8c tjl 0xf378`),
in the order **A → B → C**, stopping at the first one that returns non-zero:

```
0xF396: 97ff 9e8b   tjl 0xF0B0   ; A
0xF39A: a800        tcmp r0,#0
0xF39C: c04a        tjeq 0xF434   ; A returned 0 → try B
0xF434: 97ff 9e90   tjl 0xF158   ; B
0xF43C: 97ff 9efc   tjl 0xF238   ; C
```

So the "mode" is not a dispatch table, it is a fall-through chain: each packer returns 1 when it
queued a packet into the USB FIFO and 0 when its gate failed or the FIFO was busy.

---

## 1. The shared 20-byte intermediate

Each packer builds the *same* 20-byte structure on its own stack frame at `sp+4` and passes it to
its sender as five 32-bit words (`r0..r3` + one stack word). The offsets below are byte offsets
from that buffer (`b`):

| off | width | source | note |
|-----|-------|--------|------|
| 0..2 | 3 B | **never written** in A and B (stack garbage); 0 in C | senders never read them |
| 3..6 | 32 | `word[0x84342C]` (button word), LE | `b[3]`=bit0 … `b[6]`=bit24..31 |
| 7..8 | 16 | `hw[0x8433EC]` | axis 1 |
| 9..10 | 16 | `hw[0x8433F6]` | axis 2 |
| 11..12 | 16 | `hw[0x8433E4]` | axis 3 |
| 13..14 | 16 | `hw[0x8433E6]` | axis 4 |
| 15 | 8 | low byte of `hw[0x84340E]` | trigger LT |
| 16 | 8 | low byte of `hw[0x8433E8]` | trigger RT |
| 17..19 | 3 B | garbage in A/B, 0 in C | |

### 1.1 How the bytes are actually emitted (idiom worth knowing for hooks)

There is no `memcpy`; the compiler emits byte-store + shifted word-store pairs. Packer A
(`0xF0CA` onwards) is the reference:

```
F0CC: tloadr r2,[pc,#108]   ; = 0x84342C
F0CE: tloadrb r1,[r2,#0]    ; btn bit0..7
F0D0: tstorerb r1,[r3,#3]   ; b[3]
F0D2: tloadr r1,[r2,#0]     ; whole word again
F0D4: tshftrs r1,r1,#8      ; >>8
F0D6: tloadrb r2,[r3,#7]    ; keep b[7] (not yet written)
F0D8: tshftls r2,r2,#24
F0DA: tors r2,r1
F0DC: tstorer r2,[r3,#4]    ; b[4..6] = btn>>8, b[7] preserved
```

The 16-bit pairs are packed two-at-a-time as `(hw_hi << 16) | hw_lo`, stored as one byte then as
one shifted word, which lands them contiguously:

```
F0DE: tloadr r2,[pc,#96]    ; = 0x8433F6
F0E0: tloadrh r2,[r2,#0]
F0E2: tshftls r2,r2,#16
F0E4: tloadr r1,[pc,#92]    ; = 0x8433EC
F0E6: tloadrh r1,[r1,#0]
F0E8: tors r2,r1            ; 0x8433F6<<16 | 0x8433EC
F0EA: tstorerb r2,[r3,#7]   ; b[7] = low(0x8433EC)
F0EC: tshftrs r2,r2,#8
F0EE: tloadrb r1,[r3,#11]
F0F0: tshftls r1,r1,#24
F0F2: tors r2,r1
F0F4: tstorer r2,[r3,#8]    ; b[8..10] = rest of EC, F6 ; b[11] preserved
```

`0xF0F6`–`0xF11E` repeat the pattern for `0x8433E6|0x8433E4` and for the two byte values
`(hw[0x8433E8]<<8)|hw[0x84340E]` → `b[15]=low(0x84340E)`, `b[16]=low(0x8433E8)`.

Literal pools (all confirmed by reading the file):

* packer A `0xF134..0xF154` = `00842E38, 00843468, 0084342C, 008433F6, 008433EC, 008433E6, 008433E4, 008433E8, 0084340E`
* packer B `0xF1D4..0xF1F0` = `00842E38, 0084342C, 008433F6, 008433EC, 008433E6, 008433E4, 008433E8, 0084340E`
* packer C `0xF2B0..0xF2D0` = `00843468, 008434D9, 0084342C, 008433F6, 008433EC, 008433E6, 008433E4, 008433E8, 0084340E`

Note packer C has **no** `0x842E38` literal and packer C gates on `0x843468` (the same byte
packer A gates on, but with the opposite polarity), not on the transport byte.

---

## 2. Packer A — `0xF0B0` (wireless / default, 20-byte report)

```
F0B0: 6500        tpush {lr}
F0B2: 6086        tsub  sp,#24
F0B4: 0b1f        tloadr r3,[pc,#124]   ; 0x842E38  (transport)
F0B6: 481b        tloadrb r3,[r3,#0]
F0B8: a000        tmovs r0,#0
F0BA: ab00        tcmp  r3,#0
F0BC: c103        tjne  0xF0C6           ; transport != 0 → return 0
F0BE: 0b1e        tloadr r3,[pc,#120]   ; 0x843468
F0C0: 481b        tloadrb r3,[r3,#0]
F0C2: ab00        tcmp  r3,#0
F0C4: c001        tjeq  0xF0CA           ; 0x843468 == 0 → build report
F0C6: 6006        tadd  sp,#24
F0C8: 6d00        tpop  {pc}
```

Build (`0xF0CA`–`0xF11E`) as in §1.1. Then the 5-word call:

```
F120: 591a        tloadr r2,[r3,#16]   ; word at b[16..19]
F122: 3200        tstorer r2,[sp,#0]   ; 5th argument (stack)
F124: 3801        tloadr r0,[sp,#4]    ; b[0..3]
F126: 5859        tloadr r1,[r3,#4]    ; b[4..7]
F128: 589a        tloadr r2,[r3,#8]    ; b[8..11]
F12A: 58db        tloadr r3,[r3,#12]   ; b[12..15]
F12C: 97ff 9d66   tjl   0xEBFC
```

**Report size handed to the sender: 20 bytes** (5 words).

### 2.1 Sender `0xEBFC` — final 20-byte packet

```
EBFC: 6084 6530 608a   tsub sp,#16 ; tpush {r4,r5,lr} ; tsub sp,#40   (frame F = S-68)
EC02..EC08             r0..r3 → F+52, F+56, F+60, F+64
EC0A: 0b70        tloadr r3,[pc,#448]  ; 0x842E38
EC0C: 481b        tloadrb r3,[r3,#0]
EC0E: a000        tmovs  r0,#0
EC10: ab00        tcmp   r3,#0
EC12: c104        tjne   0xEC1E          ; transport != 0 → return 0
EC14: 0a6e        tloadr r2,[pc,#440]  ; 0x8434D8 ("host ready")
EC16: 4812        tloadrb r2,[r2,#0]
EC1A: aa02        tcmp   r2,#2
EC1C: c004        tjeq   0xEC28          ; != 2 → return 0
```

Then:

* zero `F+0..F+18`, `F+0 = 20` (`EC3C: a214 tmovs r2,#20` / `EC3E: 402a tstorerb r2,[r5,#0]`).
* rebuild the button word: `r1 = byte[arg0+3]`, `r2 = arg1 << 8`, `r2 |= r1` (`EC42..EC4A`).
* 15 bit tests OR a XInput-style mask into the halfword at `F+38` (`EC4C: b426 tadds r4,#38`).
* axes: `r1 = arg2<<8 | byte[arg0+7]` → low16 = `hw 0x8433EC`, high16 = `hw 0x8433F6`;
  `0x7FFF` (literal `0xEDD8`) minus each → `F+32`, `F+28`, then `memcpy(F+5,...,2)`,
  `memcpy(F+7,...,2)`. Same for `arg3<<8 | byte[arg0+11]` → `F+24`, `F+20` → `F+9`, `F+11`.
  ```
  ED16: f408  tshftls r0,r1,#16
  ED18: fc00  tshftrs r0,r0,#16     ; sign-extend low 16
  ED1A: 0b2f  tloadr  r3,[pc,#188]  ; 0x7FFF
  ED1C: ea18  tsubs   r0,r3,r0      ; 0x7FFF - v
  ```
* `memcpy(F+3, F+67, 2)` → `b[3],b[4]` = `b[15],b[16]` of the intermediate (triggers).
* `memcpy(F+1, F+38, 2)` → buttons.
* tail:
  ```
  ED86: 97fc 9c83  tjl   0xB690      ; 0xB690 -> 0xB64C(byte[0x843415]) : battery level 0..10
  ED8A: a310       tmovs r3,#16
  ED8C: 0318       tors  r0,r3
  ED8E: 4368       tstorerb r0,[r5,#13]
  ED90: a31b       tmovs r3,#27      ; ED92 -> F+14
  ED94: a330       tmovs r3,#48      ; ED96 -> F+15
  ED98: a36d       tmovs r3,#109     ; ED9A -> F+16
  ```

Resulting 20-byte payload (`F+0..F+19`):

| off | value |
|-----|-------|
| 0 | `0x14` (20) |
| 1..2 | button mask, LE (see §4) |
| 3 | `low(hw[0x84340E])` — LT |
| 4 | `low(hw[0x8433E8])` — RT |
| 5..6 | `0x7FFF − hw[0x8433EC]` |
| 7..8 | `0x7FFF − hw[0x8433F6]` |
| 9..10 | `0x7FFF − hw[0x8433E4]` |
| 11..12 | `0x7FFF − hw[0x8433E6]` |
| 13 | `battery(0..10) \| 0x10` |
| 14..16 | `1B 30 6D` (constant) |
| 17..18 | 0 |
| 19 | not written (residual stack) |

**Interpretation (inference, not proven):** bytes 1..12 are byte-for-byte the XInput gamepad block
(`buttons, LT, RT, LX, LY, RX, RY`, 12 bytes) which in a real XInput packet sits at offset 3 after
`00 14 01`; here the `00 14 01` header is collapsed into the single length byte `0x14` at offset 0
and everything shifts down by 2. Bytes 13..16 are 8BitDo-private (battery + `1B 30 6D`).
Two facts make this an inference, not a proof: (a) no host-side capture was available,
(b) nothing in the firmware decodes `1B 30 6D`. Settling it: capture the dongle's EP 0x84 traffic
with `usbmon`/Wireshark while pressing buttons, or send this exact 20-byte frame to the dongle.

### 2.2 Transport of A (`0xED9C`–`0xEDC8`)

```
ED9C: 0b0f        tloadr r3,[pc,#60]  ; 0x800124
ED9E: 481b        tloadrb r3,[r3,#0]
EDA0: a201        tmovs  r2,#1
EDA2: a000        tmovs  r0,#0
EDA4: 0013        tands  r3,r2
EDA6: c000        tjeq   0xEDAA        ; busy bit0 → return 0 (via 0xEC1E)
EDAA: 0a0d        tloadr r2,[pc,#52]  ; 0x800114
EDAC: 4013        tstorerb r3,[r2,#0]  ; r3 == 0 here
EDAE: 090d        tloadr r1,[pc,#52]  ; 0x80011C (FIFO)
EDB0: 400b        tstorerb r3,[r1,#0]
EDB6: 1caa        tloadrb r2,[r5,r2]  ; r5 = F
EDB8: 400a        tstorerb r2,[r1,#0]
EDBE: ab13        tcmp   r3,#19         ; 20 iterations
EDC2..EDC6        byte[0x800124] = 1
EDC8: a001        tmovs  r0,#1
```

i.e. it pushes 20 bytes into the USB FIFO at `0x80011C` and raises the "packet ready" flag
`0x800124` bit 0. No report-ID byte is written for this frame.

---

## 3. Packer B — `0xF158` (wired DInput, report ID 1)

```
F158: 6500        tpush {lr}
F15A: 6086        tsub  sp,#24
F15C: 0b1d        tloadr r3,[pc,#116]  ; 0x842E38
F15E: 481b        tloadrb r3,[r3,#0]
F160: a000        tmovs r0,#0
F162: ab01        tcmp  r3,#1
F164: c001        tjeq  0xF16A          ; transport == 1 → build
F166: 6006        tadd  sp,#24
F168: 6d00        tpop  {pc}
```

Body `0xF16A`–`0xF1BE` is identical to packer A (same pool order), then

```
F1C0: 591a        tloadr r2,[r3,#16]
F1C2: 3200        tstorer r2,[sp,#0]
F1C4..F1CA        r0..r3 = b[0..3], b[4..7], b[8..11], b[12..15]
F1CC: 97ff 9e0c   tjl   0xEDE8
```

**Report size handed to the sender: 20 bytes.**

### 3.1 Sender `0xEDE8` — 10-byte packet `01 + 9`

Frame `F`; `memset(F,0,63)`; builds a 9-byte payload at `F+0..F+8`:

* buttons: 15 tests OR into `hw[F+66]`, then `tstorerh` to `F+0` (see §4.2 for the bit order).
* hat: `F+2` is initially `15` (`EF02: a20f tmovs r2,#15`); a branch chain then loads one of eight
  codes and stores it to `[r4,#2]`:

  | target | code | direction |
  |--------|-----:|-----------|
  | `0xEF44` | 1 | up-right (bits 6+9) |
  | `0xEF40` | 2 | right (bit 6) |
  | `0xEFAE` | 3 | down-right (bits 6+8) |
  | `0xEFD4` | 4 | down (bit 8) |
  | `0xEFB4` | 5 | down-left (bits 7+8) |
  | `0xEFCE` | 6 | left (bit 7) |
  | `0xEFC0` | 7 | up-left (bits 7+9) |
  | `0xEFBA` | 0 | up (bit 9) |
  | *(none)* | 15 | released |

  Standard hat encoding (0=N,1=NE,2=E,3=SE,4=S,5=SW,6=W,7=NW,15=idle), and the dpad bit assignment
  it implies (bit6=right, bit7=left, bit8=down, bit9=up) independently confirms §5.1.
* axes `F+3..F+6` = **high byte** of each 16-bit axis:

```
EF48: f42b  tshftls r3,r5,#16
EF4A: fe1b  tshftrs r3,r3,#24    ; r3 = bits 8..15 of axis1 (0x8433EC)
EF4C: a27f  tmovs   r2,#127
EF4E: ab7f  tcmp    r3,#127
EF50: c002  tjeq    0xEF58        ; 127 stays 127
EF52: 03da  tmovns  r2,r3
EF54/EF56   lsl/asr #24          ; sign-extend to 8 bit
EF58: fe2d  tshftrs r5,r5,#24    ; r5 = high byte of 0x8433F6
EF6A: 40e2  tstorerb r2,[r4,#3]
EF6C: 4125  tstorerb r5,[r4,#4]
EF6E: 4163  tstorerb r3,[r4,#5]
EF72: 41a7  tstorerb r7,[r4,#6]
```

* `F+7 = low(hw[0x8433E8])`, `F+8 = low(hw[0x84340E])` (`EE3A`/`EE3E`) — i.e. Accelerator gets
  RT and Brake gets LT, matching the descriptor order `X,Y,Z,Rz,Accelerator,Brake`.
* transport `0xEF74`–`0xEF9E`: same busy flag `0x800124`; writes report ID `1` to `0x80011C`
  (`EF82/EF86`), copies 9 bytes (`ab09 tcmp r3,#9`), sets `0x800124 = 1`.

Wire: `01` `bd bd` `hat` `X` `Y` `Z` `Rz` `RT` `LT` (10 bytes) — matches the HID report
descriptor `0x17B74` (report ID 1: 15 buttons + 1 pad bit, 4-bit hat + 4 pad bits, 4 × 8-bit axes,
2 × 8-bit Accelerator/Brake; `wDescriptorLength = 0x0091`, EP `0x84` IN 64 B / 4 ms, EP `0x05`
OUT 64 B / 1 ms at `0x17C10`).

---

## 4. Packer C — `0xF238` (mode 2, 64-byte vendor report)

```
F238: 6500        tpush {lr}
F23A: 6086        tsub  sp,#24
F23C: 0b1c        tloadr r3,[pc,#112]  ; 0x843468
F23E: 481b        tloadrb r3,[r3,#0]
F240: ab00        tcmp  r3,#0
F242: c033        tjeq  0xF2AC          ; 0x843468 == 0 → return 0
F244: 0b1b        tloadr r3,[pc,#108]  ; 0x8434D9
F246: 481b        tloadrb r3,[r3,#0]
F248: a000        tmovs r0,#0
F24A: ab02        tcmp  r3,#2
F24C: c001        tjeq  0xF252          ; != 2 → return 0
F254: a200        tmovs r2,#0
F256: 3201        tstorer r2,[sp,#4]    ; b[0..3]  = 0
F258: 3205        tstorer r2,[sp,#20]   ; b[16..19] = 0
```

Then the same field copy (its own pool), and

```
F29C: 591a        tloadr r2,[r3,#16]
F29E: 3200        tstorer r2,[sp,#0]
F2A0..F2A6        r0..r3 = the four words
F2A8: 97ff 9fa4   tjl   0xF1F4
```

**Report size handed to the sender: 20 bytes** (the sender then wraps it into 64).

### 4.1 Sender `0xF1F4`

```
F1F4: 6084 6510 6090   tsub sp,#16 ; tpush {r4,lr} ; tsub sp,#64
F204: r0=sp, r1=0, r2=64  →  tjl 0x103F8   (memset)
F20E: a302  tmovs r3,#2
F210: 4023  tstorerb r3,[r4,#0]   ; [0] = 2
F212: 4063  tstorerb r3,[r4,#1]   ; [1] = 2
F214: a366  tmovs r3,#102
F216: 40a3  tstorerb r3,[r4,#2]   ; [2] = 0x66
F218: r0=sp+3, r1=sp+72, r2=20 → tjl 0x1040C (memcpy of the 20-byte intermediate)
F224: r0=sp, r1=64          → tjl 0xEB04
```

Wire: `02 02 66` + the 20-byte intermediate + 41 zero bytes = **64 bytes**.
`0x02` is the report ID of the vendor HID descriptor at `0x17C28`
(`06 7a ff 09 01 a1 01 85 02 09 02 a1 00 09 03 15 00 25 ff 75 08 95 3f 81 02 …`, 63-byte input).

### 4.2 Transport `0xEB04` / `0xEB7C`

```
EB04: gate byte[0x8434D9] == 2      (pool 0xEB6C)
      if byte[0x842E38] == 1 → r5 = 4, status port 0x800124
      else                   → r5 = 3, status port 0x800123   (pools 0xEB70/0xEB74/0xEB78)
      busy bit0 → return 0
      clear 0x800113/0x800114, data port 0x800118, copy len bytes, set 0x800120 = 1
```

`0xEB7C` is the same routine with `len` truncated to 8 bits first; it is the one used by all the
vendor command responses (`0xF356, 0xF4EA, 0xF518, 0xF536, 0xF62E, 0xF67C, 0xF6FC, 0xF784, 0xF822`).

---

## 5. The 32-bit button word `0x84342C`

The packers only *read* it; it is assembled elsewhere (literal-pool references to `0x84342C` exist
at `0x6D68, 0xBAB8, 0xC258, 0xC3A4, 0xC938, 0xCA3C, 0xCE44, 0xD09C, 0xD188, 0xD21C`, i.e. about
ten separate readers, mostly "is button N held" polls such as
`0xD078/0xD07A: tloadr r3,[r6,#0] ; tshftls r3,r3,#3 ; tshftrs r3,r3,#31` = test bit 28).

### 5.1 Derivation from `0xEBFC` (XInput mask)

Each test is `tshftls r3,r2,#N` followed by `c5xx tjpl` — the body runs when **bit (31−N)** of the
button word is set — and ORs the given mask into `hw[F+38]`:

| btn bit | instr | mask ORed | XInput bit | meaning |
|--------:|-------|-----------|-----------:|---------|
| 13 | `EC50: lsl #18` | `0x1000` (12) | 12 | **A** |
| 12 | `EC5A: lsl #19` | `0x2000` (13) | 13 | **B** |
| 4  | `EC68: lsl #27` | `0x4000` (14) | 14 | **X** |
| 5  | `EC76: lsl #26` | `0xFFFF8000` → 15 | 15 | **Y** |
| 10 | `EC82: lsl #21` | `0x100` (8) | 8 | **LB** |
| 11 | `EC90: lsl #20` | `0x200` (9) | 9 | **RB** |
| 1  | `EC9E: lsl #30` | `0x40` (6) | 6 | **LS click (L3)** |
| 2  | `ECAA: lsl #29` | `0x80` (7) | 7 | **RS click (R3)** |
| 9  | `ECB6: lsl #22` | `1` (0) | 0 | **dpad up** |
| 8  | `ECC2: lsl #23` | `2` (1) | 1 | **dpad down** |
| 7  | `ECCE: lsl #24` | `4` (2) | 2 | **dpad left** |
| 6  | `ECDA: lsl #25` | `8` (3) | 3 | **dpad right** |
| 17 | `ECE6: lsl #14` | `0x400` (10) | 10 | **Guide / Home** |
| 3  | `ECF4: lsl #28` | `0x20` (5) | 5 | **Back** |
| 0  | `ED00: lsl #31` | `0x10` (4) | 4 | **Start** |

Physical assignment follows the documented XInput layout (bit0 up, 1 down, 2 left, 3 right,
4 start, 5 back, 6 L3, 7 R3, 8 LB, 9 RB, 10 guide, 12 A, 13 B, 14 X, 15 Y). Bit 11 of the XInput
mask is unused by this packer (it is the only undefined bit in XInput anyway).

### 5.2 Cross-check from `0xEDE8` (DInput mask, 15 buttons)

Same word, different order (body runs on `bit(31−N)`, ORing into `hw[F+66]`):

| DInput bit | btn bit | instr (`0xEDE8`) | mask ORed |
|-----------:|--------:|------------------|-----------|
| 0  | 13 | `EE40: f499 lsl #18` | `1` |
| 1  | 12 | `EE48: f4d9 lsl #19` | `2` |
| 2  | 26 | `EEE4: f159 lsl #5` | `4` |
| 3  | 4  | `EE54: f6d9 lsl #27` | `8` |
| 4  | 5  | `EE60: f699 lsl #26` | `16` |
| 5  | 25 | `EED8: f199 lsl #6` | `32` |
| 6  | 10 | `EE6C: f559 lsl #21` | `64` |
| 7  | 11 | `EE78: f519 lsl #20` | `128` |
| 8  | 14 | `EEBC: f459 lsl #17` | `0x100` |
| 9  | 15 | `EECA: f419 lsl #16` | `0x200` |
| 10 | 3  | `EEA0: f719 lsl #28` | `0x400` |
| 11 | 0  | `EEAE: f7d9 lsl #31` | `0x800` |
| 12 | 17 | `EEF0: f399 lsl #14` | `0x1000` |
| 13 | 1  | `EE84: f799 lsl #30` | `0x2000` |
| 14 | 2  | `EE92: f759 lsl #29` | `0x4000` |

Both packers draw from the same word, and the two sets are exact complements of each other:

* XInput mask: btn bits **0..13, 17** (15 bits; the dpad occupies 4 of them).
* DInput mask: btn bits **0,1,2,3,4,5,10,11,12,13,14,15,17,25,26** (15 bits; the dpad is encoded in
  the hat byte instead, freeing 4 slots).

So btn bits **14, 15, 25 and 26 are only ever visible in DInput mode** — XInput silently drops them.
That is four buttons beyond the ten face/shoulder/stick/start/back/home set. On a 2C the natural
reading is digital LT/RT (the XInput mask has no digital trigger bits, only the analog bytes) plus
the extra/profile button(s) — **unproven**, because no code names them.

**Unproven:** the *identity* of btn bits 14, 15, 25, 26 (and of 16, 18..31, which map to nothing).
The word is genuinely 32 bits wide — e.g. `0xD078/0xD07A: tloadr r3,[r6,#0] ; tshftls r3,r3,#3 ;
tshftrs r3,r3,#31` polls bit 28. Settling it: hook the button scan (`0xB8xx`/`0xD0xx`) or diff
`0x84342C` across a sweep in a controller tester on real hardware.

HID descriptor cross-check (`0x17B74`, report ID 1): 15 buttons + 1 pad bit, 4-bit hat with
`LogicalMax 7` / `Physical 0x14` (`0x39`), then 4 pad bits, 4 × 8-bit axes `0x30/0x31/0x32/0x35`,
2 × 8-bit `0xC4/0xC5` (Accelerator/Brake), plus output items `0x43..0x46`, a 63-byte constant input
report ID 2 and a 63-byte output report ID 0x81 at `0x17C04`. The 15-button DInput packing of
`0xEDE8` matches exactly — that is the independent confirmation that the btn-bit list above is
complete for the DInput path.

Descriptor variants selected by `0xE80C`–`0xE846`: transport 1 → len `0x91` @ `0x17B74`;
param 2 → len `0x21` @ `0x17C28` (vendor page 0xFF7A, report ID 2 / 0x81);
param 1 → len `0xA6` @ `0x17C60` (keyboard ID 1, consumer ID 2, gamepad ID 3 with 3 buttons,
16-bit X/Y ±0x7FFF and 8-bit Z/z-axis-twist `0x38`).

---

## 6. Analog axes and triggers

**The packers never read raw ADC.** They read 16-bit halfwords that are the output of the axis
pipeline at `0xB14C` (called from the main loop at `0xA458: 9000 9e78 tjl 0xb14c`).

Pipeline, as far as proven:

1. Gate on `hw[0x8433FE] != 0` (`B15C: ab00 tcmp r3,#0` / `B160: c106 tjne`).
2. Six channel reads `tjl 0x850C(channel_cfg, scale)` with channel config words taken from
   `0x843404, 0x8433DC, 0x843408, 0x8433E0, 0x8433F0, 0x8433F8`.
3. Every result is **halved**: `tshftrs r0,r0,#1` at `B182, B190, B1A0, B1B0, B1C0, B1D0`
   → raw 12-bit `0..0xFFF` becomes `0..0x7FF` at this stage (`0xFFF` is the full-scale constant,
   literal `0xB354 = 0x00000FFF`).
4. Three channels are inverted against full scale (`B1D2`–`B1DC`):
   ```
   B1D2: 0960  tloadr r1,[pc,#384]  ; 0xFFF
   B1D6: eb0a  tsubs  r2,r1,r4      ; 0xFFF - ch2
   B1DA: eb08  tsubs  r0,r1,r4      ; 0xFFF - ch5
   B1DC: eac9  tsubs  r1,r1,r3      ; 0xFFF - (ch6>>1)
   ```
5. Hysteresis / deadband of **20 counts** compares each new value with the previous one stored in
   `hw[0x8433F8, 0x8433FC, 0x843400, 0x843404]`:
   ```
   B1EA: eae1  tsubs r1,r4,r3
   B1EC: e7cc  tasrs r4,r1,#31        ; |x| via sign trick
   B1EE: e909  tadds r1,r1,r4
   B1F0: 0061  txors r1,r4
   B1F2: a914  tcmp  r1,#20
   B1F4: cc00  tjgt  0xB1F8           ; only update when |delta| > 20
   B1F6: 3308  tstorer r3,[sp,#32]    ; keep the old value
   ```
6. Calibration/mapping is delegated to `tjl 0x6B04` (`B2B0: 97f5 9c28`), which stores the six
   inputs into `hw[0x843860..0x84386A]` and calls `0x6A0C` with six output pointers.
   `0x6A0C` writes `0x7FFF` (literal `0x6B00`) to all outputs when its `0x6238` helper reports
   "axis not configured" (`6A2C: a800 tcmp r0,#0 ; 6A2E: c117 tjne 0x6A60`).
7. Results are stored (`B2D0`–`B334`):

   | store | target | meaning |
   |-------|--------|---------|
   | `B2D8` | `hw 0x8433EC` | axis 1 (LX) |
   | `B2E0` | `hw 0x8433F6` | axis 2 (LY) |
   | `B2E8` | `hw 0x8433E4` | axis 3 (RX) |
   | `B2EE` | `hw 0x8433E6` | axis 4 (RY) |
   | `B2F4` | `hw 0x843410` | aux |
   | `B2FA` | `hw 0x84340C` | aux |
   | `B302/B30E/B316/B320` | `0x8433F8/0x8433FC/0x843400/0x843404` | hysteresis slots |
   | `B32A/B334` | `hw 0x8433EA`, `hw 0x8433D8` | raw-ish channel 5/6 copies |

8. The packers then: XInput path inverts (`0x7FFF − v`, sign-extended 16-bit) and ships 16-bit;
   DInput path takes `(v<<16)>>24` = the high byte only, substitutes `127` when the byte is exactly
   `127`, and sign-extends (`EF4C..EF56`, `EF5E..EF68`).

Triggers: XInput byte 3 = `low(hw[0x84340E])` (LT), byte 4 = `low(hw[0x8433E8])` (RT) — both are
already 8-bit scaled values written elsewhere (not by `0xB14C`); DInput maps Accel = `low(0x8433E8)`,
Brake = `low(0x84340E)`.

**Centre / range — partially unproven.** What is proven: the packer's XInput-side value is
`0x7FFF − v` over a sign-extended 16-bit `v`, and the pipeline's "no axis" value is `v = 0x7FFF`,
which the packer turns into `0x0000` — i.e. the packer's neutral output is 0, the XInput stick
centre. The exact per-axis min/max used by `0x6A0C` (and therefore the real electrical centre, and
whether `v` is signed) is **not proven**: it depends on the per-axis calibration data that `0x6A0C`
reads through `0x6890`/`0x6964`/`0x10D24`. Settling it: single-step `0x6A0C` on hardware with a
stick centred vs. at each rail and read the six output halfwords.

---

## 7. Hook sites

All `tjl` are 4 bytes (2 halfwords): `97ff`/`9004`/`9003` followed by a 16-bit operand.
To divert a packer to your own handler, overwrite the 4 bytes at the call site with the same
`tjl` form targeting your code. The callee's contract is `int f(void)`; returning 1 means
"packet queued", which stops the A→B→C chain (`0xF39A`, `0xF438`, `0xF440`).

| site | bytes | targets | when it runs |
|------|-------|---------|--------------|
| `0xF396` | `97ff 9e8b` | packer A `0xF0B0` | every input change in wireless mode — **best universal hook** |
| `0xF434` | `97ff 9e90` | packer B `0xF158` | when A declined (wired) |
| `0xF43C` | `97ff 9efc` | packer C `0xF238` | when A and B declined (vendor mode) |
| `0xF446` | `97ff 9f45` | status report `0xF2D4` (0x83) | if `byte[0x8434EE] != 0` |
| `0xA45C` | `9004 9f8c` | report task `0xF378` | every main-loop pass (~5 ms) |
| `0xA440` | `9004 9e32` | wrapper `0xF0A8` → `tjl 0xA90C` (USB OUT dispatcher) | every pass |
| packer→sender | `0xF12C: 97ff 9d66` → `0xEBFC`; `0xF1CC: 97ff 9e0c` → `0xEDE8`; `0xF2A8: 97ff 9fa4` → `0xF1F4`; `0xF228: 97ff 9c6c` → `0xEB04` | | |

Note `0xF0A8` is only `tpush {lr}; tjl 0xA90C; tpop {pc}` — hooking `0xA45C` gives you a clean
per-tick entry point *after* `0xB388`/`0xB14C` have refreshed the button word and the axes, which
is the right place to synthesise your own report.

`sender` return values matter: `0xEBFC`/`0xEDE8`/`0xEB04` all return 0 when the USB FIFO is busy
(`0x800124` bit 0, or `0x800123` in the non-wired branch of `0xEB04`), so a hook that always
returns 1 can starve the real path.

---

## 8. Why a plain HID host sees no input reports (Q5)

`0xF378` is the only caller of the packers and it bails out immediately unless
`byte[0x8434D8] == 2`:

```
F37A: 0b34        tloadr r3,[pc,#208]   ; 0x8434D8
F37C: 481b        tloadrb r3,[r3,#0]
F37E: ab02        tcmp  r3,#2
F380: c000        tjeq  0xF384
F382: 6d70        tpop  {r4,r5,r6,pc}   ; not ready → no packet at all
```

`0x8434D8` is only ever set to 2 in two places:

1. **Wired** (`byte[0x842E38] == 1`): inside the control-request handler `0xE954`
   ```
   E98A: 0b07        tloadr r3,[pc,#28]   ; 0x842E38
   E98E: ab01        tcmp  r3,#1
   E990: c102        tjne  0xE998
   E992: a202        tmovs  r2,#2
   E994: 0b05        tloadr r3,[pc,#20]   ; 0x8434D8
   E996: 401a        tstorerb r2,[r3,#0]
   ```
   i.e. only after the host has issued the setup request that reaches `0xE954` (it answers with a
   22-byte / `0x16` packet built from the 5 ASCII bytes at `0x84399C`). A generic HID host never
   sends it, so the byte stays 0.

2. **Wireless / dongle** (`byte[0x842E38] == 0`): the vendor OUT parser `0xEABC`
   ```
   EABE: 0b0f        tloadr r3,[pc,#60]   ; 0x842E38
   EAC2: ab00        tcmp  r3,#0
   EAC4: c107        tjne  0xEAD6          ; only in wireless mode
   EAC6: 4803        tloadrb r3,[r0,#0]    ; pkt[0]
   EAC8: ab01        tcmp  r3,#1
   EACA: c005        tjeq  0xEAD8
   EACC: ab00        tcmp  r3,#0
   EACE: c102        tjne  0xEAD6
   EAD0: 4843        tloadrb r3,[r0,#1]    ; pkt[1]
   EAD2: ab08        tcmp  r3,#8
   EAD4: c00a        tjeq  0xEAEC          ; 0x00 0x08 xx yy = rumble
   EAD8: 4843        tloadrb r3,[r0,#1]
   EADA: ab03        tcmp  r3,#3
   EADC: c1fb        tjne  0xEAD6
   EADE: a903        tcmp  r1,#3           ; total length must be 3
   EAE0: c1f9        tjne  0xEAD6
   EAE2: a202        tmovs  r2,#2
   EAE4: 0b06        tloadr r3,[pc,#24]   ; 0x8434D8
   EAE6: 401a        tstorerb r2,[r3,#0]   ; host-ready
   ```
   So the wireless "init handshake" is a **3-byte OUT packet `01 03 xx`**; the rumble command is
   `00 08 xx yy` (dispatched to `0xFA68`, from `0xEAEC..0xEAF4`).

Cleared again by `0xE9B0` (`a200 tmovs r2,#0` → `byte[0x8434D8]=0`), which the USB dispatcher calls
from `0xA968` when bit `0x20000` of `word[0x800648]` is set (bus reset/deconfigure), and by
`0xE9D4`/`0xE9DC` on command `0x0B`.

Practical consequence: any host that only speaks plain HID (or any script that does not send
`01 03 03`) gets **zero** input reports, because the packet builder is never entered — not because
the endpoint is off. Two independent gates stack up:

* `0xF378` (all packers) needs `byte[0x8434D8] == 2`;
* the senders re-check it too (`0xEC14..0xEC1C` in `0xEBFC`), and packer C additionally needs
  `byte[0x8434D9] == 2` (vendor interface selected, set by `0xE80C`–`0xE846` when the host asks for
  that descriptor set) plus `byte[0x843468] != 0`, while packer A needs `byte[0x843468] == 0`.

---

## 9. Mode bytes, at a glance

| address | width | role | evidence |
|---------|-------|------|----------|
| `0x842E38` | 8 | transport: 0 = wireless/dongle, 1 = wired | gates in `0xF0B4`, `0xF15C`, `0xEBFC`(`0xEC0A`), `0xEABC`(`0xEABE`), `0xE954`(`0xE98A`) |
| `0x8434D8` | 8 | host-ready, must be 2 or nothing is sent | `0xF37E`, `0xEC1A`, sets at `0xE996`, `0xEAE6` |
| `0x8434D9` | 8 | vendor interface active, must be 2 for C / `0xEB04` | `0xF24A`, `0xEB04` pool `0xEB6C` |
| `0x843468` | 8 | mode select: 0 → A, non-0 → C | `0xF0BE..0xF0C4`, `0xF23C..0xF242` |
| `0x8434EE` | 8 | enable periodic 0x83 status report | `0xF3D2..0xF3D8`, `0xF446` |
| `0x84342C` | 32 | button word | §5 |
| `0x8434E0` | 32 | previous button word (change detect) | `0xF384..0xF38E` |
| `0x8434DB` | 8 | re-send counter, set to 100 on change (`0xF392: a364`) | `0xF390..0xF3E4` |
| `0x8434E4/6/8/A` | 16 | shadow copies of the four axes | `0xF3A6..0xF3C0` |
| `0x8434DA/DC` | 8 | shadow copies of the two trigger bytes | `0xF3C6..0xF3D0` |
| `0x843415` | 8 | battery raw → `0xB64C` maps to 0..10 | `0xB690`/`0xB64C` |
| `0x800114`, `0x80011C` | — | FIFO block (report-ID block), `0x800118` data port | `0xEDA8..`, `0xEB04` |
| `0x800123/24` | — | packet-ready / busy flags | `0xED9C`, `0xEB04` |

---

## 10. Open items ("unproven")

1. **Identity of btn bits 14, 15, 25, 26** (DInput-only) and of 16, 18..31 (unmapped) — see §5.2.
2. **Meaning of `0x843468`** — it flips between "packer A active" and "packer C active" but nothing
   in the traced path writes it (the writers are outside the traced set; refs at `0x6D60`, `0xB0C8`,
   `0xB754`, `0xC254`, `0xCA38`, `0xD094`, `0xD8D0`, `0xDDF8`, `0xF718`, `0xF7A4`).
3. **Axis centre / signedness** — see §6; needs `0x6A0C` single-stepping or a hardware sweep.
4. **`1B 30 6D` and `battery|0x10`** in the 20-byte frame — constants with no in-firmware consumer.
5. **Which control request reaches `0xE954`** — the handler builds a 0x16-byte reply from
   `0x84399C` but the request code check is in the dispatcher above it (`0xA9xx`), not decoded here.
