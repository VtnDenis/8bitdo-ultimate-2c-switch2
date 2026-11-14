/*
 * handler.c - Nintendo Switch Pro Controller output reports and replies.
 *
 * Two command channels arrive on interrupt OUT EP 0x01, byte 0 is the report
 * ID (the console uses interrupt transfers, not SET_REPORT, so the ID is on
 * the wire):
 *
 *   0x80  session/USB config. byte 1 is the command 01..06 (and 91/92 raw
 *         UART, not implemented here). 0x01 and 0x02 echo back as 0x81 + byte 1;
 *         0x04 (the gate that starts input streaming) and everything after it
 *         answer with an input-report-shaped 0x30 frame whose byte 1 is the
 *         subtype, which is what GP2040-CE does and what the console waits for.
 *   0x01  rumble (bytes 2..9) + subcommand at byte 10. Reply is report ID 0x21.
 *
 * Reference: GP2040-CE SwitchProDriver::handleConfigReport() and
 * handleFeatureReport(). Byte maps: docs/switchpro_payload.md.
 *
 * Freestanding: no libc, no malloc, no stdint.h, no floats.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

#define REPORT_LEN 64

/* Wired to the firmware's existing IN-endpoint sender. len is always 64. */
extern void send_in(const unsigned char *buf, int len);

/* packer.c */
extern void pack_report(u8 *out);
extern void pack_input_block(u8 *dst);
extern u8   pack_tick(void);

/* Fixed MAC. 7C:BB:8A is 8BitDo's own prefix, borrowed from GP2040-CE; any
 * locally-administered value works (the console does not validate it).
 * reply_81() sends it reversed, the way sendIdentify() does. */
static const u8 k_mac[6] = { 0x7C, 0xBB, 0x8A, 0x00, 0x00, 0x01 };

/* Player-light bitfield last set by subcommand 0x30. */
static u8 s_player;

/* Single reply buffer. Zero-initialised .bss, no malloc. */
static u8 s_reply[REPORT_LEN];

/* Let the glue read back the last framed reply (diagnostics only). */
const u8 *handler_last_reply(void)
{
	return s_reply;
}

/* ------------------------------------------------------------------ */
/* SPI flash image                                                     */
/*                                                                     */
/* The console only reads these two banks; everything else is 0xFF.     */
/* Copied from GP2040's factoryConfigData[] (bank 0x6000) and           */
/* userCalibrationData[] (bank 0x8000), which are the constants that a  */
/* working device-mode implementation serves.                           */
/*                                                                     */
/* 0x6000 bank, offsets relative to 0x6000:                             */
/*   0000 16  serial number, all FF (= none)                            */
/*   0012 1   device type 03 = Pro Controller                           */
/*   0013 1   A0                                                        */
/*   001B 1   color info 02                                             */
/*   0020 24  6-axis factory calibration                                */
/*   003D 9   left stick calibration                                    */
/*   0046 9   right stick calibration                                   */
/*   0050 3   body RGB, 0053 buttons, 0056 left grip, 0059 right grip   */
/*   005C 1   controller region 01                                      */
/*   0080 6   stick device parameters                                   */
/*   0086 18  stick device parameters 1                                 */
/*   0098 18  stick device parameters 2                                 */
/*   rest FF                                                            */
/* ------------------------------------------------------------------ */
static const u8 k_spi6000[0xB0] = {
	/* 0x6000 */
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/* 0x6010 */
	0xFF, 0xFF, 0x03, 0xA0, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0x02, 0xFF, 0xFF, 0xFF, 0xFF,
	/* 0x6020 motion calibration */
	0xE3, 0xFF, 0x39, 0xFF, 0xED, 0x01, 0x00, 0x40,
	0x00, 0x40, 0x00, 0x40, 0x09, 0x00, 0xEA, 0xFF,
	/* 0x6030 */
	0xA1, 0xFF, 0x3B, 0x34, 0x3B, 0x34, 0x3B, 0x34,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xA4, 0x46, 0x6A,
	/* 0x6040 left cal tail, right cal, 0x4F */
	0x00, 0x08, 0x80, 0xA4, 0x46, 0x6A, 0x00, 0x08,
	0x80, 0xA4, 0x46, 0x6A, 0xA4, 0x46, 0x6A, 0xFF,
	/* 0x6050 colours */
	0x1B, 0x1B, 0x1D, 0xFF, 0xFF, 0xFF, 0xEC, 0x00,
	0x8C, 0xEC, 0x00, 0x8C, 0x01, 0xFF, 0xFF, 0xFF,
	/* 0x6060 */
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/* 0x6070 */
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/* 0x6080 stick device parameters + params 1 */
	0x50, 0xFD, 0x00, 0x00, 0xC6, 0x0F, 0x0F, 0x30,
	0x61, 0xAE, 0x90, 0xD9, 0xD4, 0x14, 0x54, 0x41,
	/* 0x6090 */
	0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63,
	0x0F, 0x30, 0x61, 0xAE, 0x90, 0xD9, 0xD4, 0x14,
	/* 0x60A0 params 2 tail, then FF */
	0x54, 0x41, 0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33,
	0x36, 0x63, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

/*
 * 0x8000 bank, user stick calibration. Offsets relative to 0x8000:
 *   0000 16  FF
 *   0010 2   magic B2 A1, then 9-byte left calibration
 *   001B 2   magic B2 A1, then 9-byte right calibration
 *   0026 16  FF
 *   0036 9   zero - GP2040's array is 0x3F long and its initialiser stops
 *            here, so these read back 0x00 rather than 0xFF. Kept as-is.
 */
static const u8 k_spi8000[0x3F] = {
	/* 0x8000 */
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/* 0x8010 left */
	0xB2, 0xA1, 0xA4, 0x46, 0x6A, 0x00, 0x08, 0x80,
	0xA4, 0x46, 0x6A, 0xB2, 0xA1, 0x00, 0x08, 0x80,
	/* 0x8020 right */
	0xA4, 0x46, 0x6A, 0xA4, 0x46, 0x6A, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	/* 0x8030 */
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* ------------------------------------------------------------------ */

static void zero64(void)
{
	u8 i;

	for (i = 0; i < REPORT_LEN; i++)
		s_reply[i] = 0x00;
}

/*
 * Serves the two known banks; anything else reads 0xFF, exactly like GP2040
 * for an address it has no data for.
 */
static void spi_read(u8 *dst, u32 addr, u8 len)
{
	const u8 *src = 0;
	u16 size = 0;
	u16 off = (u16)(addr & 0xFFu);
	u8 i;

	if ((addr & 0xFFFFFF00UL) == 0x6000UL) {
		src = k_spi6000;
		size = sizeof(k_spi6000);
	} else if ((addr & 0xFFFFFF00UL) == 0x8000UL) {
		src = k_spi8000;
		size = sizeof(k_spi8000);
	}

	for (i = 0; i < len; i++) {
		u16 o = (u16)(off + i);

		dst[i] = (src != 0 && o < size) ? src[o] : 0xFF;
	}
}

/* Reply on the 0x80 channel: 0x81, command echoed, identify carries type+MAC. */
static void reply_81(u8 cmd)
{
	u8 i;

	zero64();
	s_reply[0] = 0x81;
	s_reply[1] = cmd;
	if (cmd == 0x01) {
		s_reply[2] = 0x00;
		s_reply[3] = 0x03; /* Pro Controller */
		for (i = 0; i < 6; i++)
			s_reply[4 + i] = k_mac[5 - i]; /* identify MAC is reversed vs device info */
	}
	send_in(s_reply, REPORT_LEN);
}

/*
 * Gate replies (0x04 disable-timeout, 0x05, 0x06, unknown) are input-report
 * shaped in the reference implementation: report 0x30 with the command in
 * byte 1, not a 0x81 ack.
 */
static void reply_30(u8 cmd)
{
	pack_report(s_reply);
	s_reply[1] = cmd;
	send_in(s_reply, REPORT_LEN);
}

/*
 * Frame the 0x21 reply header: current controller state at [2..11], vibrator
 * at [12], ACK at [13], subcommand ID at [14]. Payload goes to s_reply[15..].
 */
static void reply_21(u8 sub, u8 ack)
{
	zero64();
	s_reply[0] = 0x21;
	s_reply[1] = pack_tick();
	pack_input_block(s_reply + 2);
	s_reply[12] = 0x09;
	s_reply[13] = ack;
	s_reply[14] = sub;
}

int usb_out_handler(const unsigned char *in)
{
	u8 sub;
	u8 i;
	u32 addr;

	/* ---- channel A: session / USB config ----------------------------- */
	if (in[0] == 0x80) {
		switch (in[1]) {
		case 0x01: /* IDENTIFY          -> 81 01 00 03 <mac> */
		case 0x02: /* HANDSHAKE, once per session */
		case 0x03: /* BAUD RATE 3 Mbit  */
			reply_81(in[1]);
			return 1;
		case 0x04: /* DISABLE USB TIMEOUT - gate to 0x30 streaming */
		case 0x05: /* ENABLE USB TIMEOUT  */
		case 0x06: /* RESET               */
			reply_30(in[1]);
			return 1;
		default:
			/* 0x91/0x92 raw UART passthrough is not implemented. */
			return 0;
		}
	}

	/* ---- channel B: rumble + subcommand ------------------------------ */
	if (in[0] != 0x01)
		return 0;

	sub = in[10];

	/* GP2040 does not answer an oversized SPI read (64 - 20 = 44). */
	if (sub == 0x10 && in[15] > 44)
		return 1;

	reply_21(sub, 0x80); /* ack 0x80 is the default for every subcommand */

	switch (sub) {
	case 0x00: /* GET_CONTROLLER_STATE */
		s_reply[15] = 0x03;
		break;

	case 0x01: /* BLUETOOTH_PAIR_REQUEST: ack 0x81, like the reference */
		s_reply[13] = 0x81;
		s_reply[15] = 0x03;
		break;

	case 0x02: /* REQUEST_DEVICE_INFO: fw 04 91, type 03, 02, MAC, 01, 02 */
		s_reply[13] = 0x82;
		s_reply[15] = 0x04;
		s_reply[16] = 0x91;
		s_reply[17] = 0x03;
		s_reply[18] = 0x02;
		for (i = 0; i < 6; i++)
			s_reply[19 + i] = k_mac[i];
		s_reply[25] = 0x01;
		s_reply[26] = 0x02;
		break;

	case 0x03: /* SET_MODE: echo the requested mode (0x30 = full input) */
		s_reply[15] = in[11];
		break;

	case 0x08: /* SET_SHIPMENT: accepted, like GP2040 (no payload byte) */
		break;

	case 0x04: /* TRIGGER_BUTTONS: ack 0x83 and no payload, like the reference */
		s_reply[13] = 0x83;
		break;

	case 0x10: /* SPI_READ: echo address+length, then the data */
		addr = (u32)in[11] |
		       ((u32)in[12] << 8) |
		       ((u32)in[13] << 16) |
		       ((u32)in[14] << 24);
		s_reply[13] = 0x90;
		s_reply[15] = in[11];
		s_reply[16] = in[12];
		s_reply[17] = in[13];
		s_reply[18] = in[14];
		s_reply[19] = in[15];
		spi_read(s_reply + 20, addr, in[15]);
		break;

	case 0x30: /* SET_PLAYER_LIGHTS: low nibble solid, high nibble flashing */
		s_player = in[11];
		break;

	case 0x31: /* GET_PLAYER_LIGHTS */
		s_reply[13] = 0xB0;
		s_reply[15] = s_player;
		break;

	case 0x38: /* SET_HOME_LIGHT: accepted, not animated */
		s_reply[15] = 0x00;
		break;

	case 0x40: /* TOGGLE_IMU: no IMU in the report */
		s_reply[15] = 0x00;
		break;

	case 0x41: /* IMU sensitivity */
		break;

	case 0x43: /* READ_IMU raw registers: echo address and size */
		s_reply[13] = 0xC0;
		s_reply[15] = in[11];
		s_reply[16] = in[12];
		break;

	case 0x48: /* ENABLE_VIBRATION */
		s_reply[15] = 0x00;
		break;

	case 0x50: /* GET_VOLTAGE: 0x0683 */
		s_reply[13] = 0xD0;
		s_reply[15] = 0x83;
		s_reply[16] = 0x06;
		break;

	default: /* unknown subcommand: ack plus 0x03, like GP2040 */
		s_reply[15] = 0x03;
		break;
	}

	send_in(s_reply, REPORT_LEN);
	return 1;
}
