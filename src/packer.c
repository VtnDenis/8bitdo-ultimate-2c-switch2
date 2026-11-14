/*
 * packer.c - Nintendo Switch Pro Controller input report 0x30, 64 bytes.
 *
 * Reference implementation: GP2040-CE SwitchProDriver (SwitchProDescriptors.h
 * SwitchProReport / SwitchInputReport, and process() in SwitchProDriver.cpp).
 * Byte map documentation: docs/switchpro_payload.md.
 *
 * Freestanding: no libc, no stdint.h, no floats, no division.
 * Build: -Os -ffreestanding -fno-builtin -ffunction-sections -fdata-sections
 *
 * tc32 is ILP32: unsigned long is 32 bits, which is what u32 relies on.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

#define REPORT_LEN 64

/*
 * Bit index of each button inside the 32-bit state word at 0x84342C, proven
 * from the stock XInput report builder 0xEBFC (every test is
 * `tshftls rN, word, #31-bit`) and confirmed on hardware. Buttons the 2C does
 * not have (SR/SL) have no bit and always report 0.
 */
#define BIT_PLUS     0    /* Start                       */
#define BIT_L3       1    /* left stick click            */
#define BIT_R3       2    /* right stick click           */
#define BIT_MINUS    3    /* Back / View                 */
#define BIT_Y        4    /* Xbox X  -> Nintendo Y (left)  */
#define BIT_X        5    /* Xbox Y  -> Nintendo X (top)   */
#define BIT_RIGHT    6    /* d-pad                       */
#define BIT_LEFT     7
#define BIT_DOWN     8
#define BIT_UP       9
#define BIT_L        10   /* LB                          */
#define BIT_R        11   /* RB                          */
#define BIT_A        12   /* Xbox B  -> Nintendo A (right) */
#define BIT_B        13   /* Xbox A  -> Nintendo B (bottom) */
#define BIT_ZL       14   /* digital trigger bits, dropped by XInput */
#define BIT_ZR       15
#define BIT_HOME     17   /* Guide                       */
#define BIT_CAPTURE  31   /* not on this pad; kept so the map is total */

/* 1 if the word is active-low (bit clear = pressed). Flip if inverted. */
#define BTN_ACTIVE_LOW 0

/*
 * GP2040 sets the ChargingGrip bit (byte 4, 0x80) unconditionally for a Pro
 * Controller and the console accepts that. Set to 0 to leave it clear.
 */
#define CHARGING_GRIP 1

#define BTN(w, n) (((w) >> (n)) & 1u)

/* ------------------------------------------------------------------ */
/* Stick state, filled by glue.c read_state() from the stock globals   */
/* (0x8433EC lx, 0x8433F6 ly, 0x8433E4 rx, 0x8433E6 ry).               */
/*                                                                     */
/* Values are already in REPORT orientation, 0..4095, 2048 = centre:    */
/* 0 = up/left, 4095 = down/right. The glue layer applies the inversion */
/* the firmware's ADC needs, so this file does no math.                 */
/* ------------------------------------------------------------------ */
struct input_state {
	u32 btn;      /* button word, bits as in parse_buttons' BIT_* map */
	u16 lx;
	u16 ly;
	u16 rx;
	u16 ry;
	u8  zl;       /* analog triggers 0..255, folded into ZL/ZR above */
	u8  zr;
};

/* Centre on all four axes until the glue writes real values. */
struct input_state g_input;

/* Rolling report counter, wraps at 256. Lives in .bss. */
static u8 s_tick;

/* ------------------------------------------------------------------ */

/*
 * Fills dst[0..2] with report bytes 3, 4 and 5.
 * Bit positions on the wire are fixed by the report descriptor's button
 * usages and by GP2040's SwitchInputReport bitfields; only the source bits
 * are configurable.
 */
void parse_buttons(u32 word, u8 *dst)
{
	u32 b = word;
#if BTN_ACTIVE_LOW
	b = ~b;
#endif
	/* byte 3: Y X B A SR SL R ZR */
	dst[0] = (u8)((BTN(b, BIT_Y)   << 0) |
	              (BTN(b, BIT_X)   << 1) |
	              (BTN(b, BIT_B)   << 2) |
	              (BTN(b, BIT_A)   << 3) |
	              /* SR/SL: no such buttons, bits 4/5 stay 0 */
	              (BTN(b, BIT_R)   << 6) |
	              (BTN(b, BIT_ZR)  << 7));
	/* byte 4: Minus Plus RStick LStick Home Capture - ChargingGrip */
	dst[1] = (u8)((BTN(b, BIT_MINUS)   << 0) |
	              (BTN(b, BIT_PLUS)    << 1) |
	              (BTN(b, BIT_R3)      << 2) |
	              (BTN(b, BIT_L3)      << 3) |
	              (BTN(b, BIT_HOME)    << 4) |
	              (BTN(b, BIT_CAPTURE) << 5) |
	              ((u32)CHARGING_GRIP  << 7));
	/* byte 5: Down Up Right Left SR SL L ZL */
	dst[2] = (u8)((BTN(b, BIT_DOWN)  << 0) |
	              (BTN(b, BIT_UP)    << 1) |
	              (BTN(b, BIT_RIGHT) << 2) |
	              (BTN(b, BIT_LEFT)  << 3) |
	              /* SR/SL: bits 4/5 stay 0 */
	              (BTN(b, BIT_L)     << 6) |
	              (BTN(b, BIT_ZL)    << 7));
}

/* Clamp instead of masking: a value of 0x1000 masked would slam the stick. */
static u16 clamp12(u16 v)
{
	return v > 0x0FFFu ? 0x0FFFu : v;
}

/* 12-bit little-endian pair, low nibble of byte 1 = top bits of X. */
static void pack_stick(u8 *d, u16 x, u16 y)
{
	x = clamp12(x);
	y = clamp12(y);
	d[0] = (u8)(x & 0xFF);
	d[1] = (u8)(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
	d[2] = (u8)((y >> 4) & 0xFF);
}

/*
 * Fills dst[0..9] = report bytes 2..11: battery/connection, the three
 * button bytes, then both sticks. Shared with handler.c so the 0x21 reply
 * carries byte-identical controller state.
 */
void pack_input_block(u8 *dst)
{
	u8 btn[3];

	parse_buttons(g_input.btn, btn);

	/* low nibble = connection info (0 = wired Pro), high = battery (8 = full) */
	dst[0] = 0x80;
	dst[1] = btn[0];
	dst[2] = btn[1];
	dst[3] = btn[2];
	pack_stick(dst + 4, g_input.lx, g_input.ly);
	pack_stick(dst + 7, g_input.rx, g_input.ry);
}

/* Value byte 1 of the next 0x30 will carry. Used by the 0x21 replies. */
u8 pack_tick(void)
{
	return s_tick;
}

/*
 * Builds the full 64-byte input report.
 *
 * 0       0x30
 * 1       rolling counter
 * 2       0x80  battery 8 (full) | connection 0 (wired)
 * 3..5    buttons
 * 6..11   sticks, 12-bit packed, centre = 00 08 80
 * 12      0x09  vibrator ack, GP2040 hardcodes this
 * 13..48  IMU, zero (no IMU is reported; GP2040 sends zeroes too)
 * 49..63  zero padding
 */
void pack_report(u8 *out)
{
	u8 i;

	for (i = 13; i < REPORT_LEN; i++)
		out[i] = 0x00;

	out[0] = 0x30;
	out[1] = s_tick++;
	pack_input_block(out + 2);
	out[12] = 0x09;
}
