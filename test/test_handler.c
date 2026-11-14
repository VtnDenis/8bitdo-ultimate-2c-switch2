/* Host-side unit test for the subcommand handler.
   gcc -I src -o /tmp/th test/test_handler.c src/packer.c && /tmp/th
*/
#include <stdio.h>
#include <string.h>

static unsigned char last[64];
static int last_len;

void send_in(const unsigned char *buf, int len)
{
	if (len > 64) len = 64;
	memcpy(last, buf, (size_t)len);
	last_len = len;
}

#include "../src/packer.c"
#include "../src/handler.c"

static int fails;

static void case_(const char *what, const unsigned char *in, int n,
                  const unsigned char *want, int wn)
{
	memset(last, 0xEE, sizeof last);
	last_len = -1;
	int handled = usb_out_handler(in);
	if (!handled) {
		printf("FAIL %s: not handled\n", what);
		fails++;
		return;
	}
	if (last_len < wn || memcmp(last, want, (size_t)wn)) {
		printf("FAIL %s\n  got  ", what);
		for (int i = 0; i < last_len; i++) printf("%02x ", last[i]);
		printf("\n  want ");
		for (int i = 0; i < wn; i++) printf("%02x ", want[i]);
		printf("\n");
		fails++;
	} else {
		printf("ok   %s (%d B)\n", what, wn);
	}
}

int main(void)
{
	/* identify carries the MAC reversed relative to the device-info reply:
	   GP2040-CE does report[4+i] = mac[5-i] and the console compares them. */
	const unsigned char id[] = {0x81, 0x01, 0x00, 0x03, 0x01, 0x00, 0x00, 0x8A, 0xBB, 0x7C};
	unsigned char in[64];

	/* 80 01 identify -> 81 01 00 03 <mac> */
	memset(in, 0, sizeof in); in[0] = 0x80; in[1] = 0x01;
	case_("80 01 identify", in, 2, id, sizeof id);

	/* 80 02/03 echo as 81 xx; the 80 04 gate and everything after it answer with
	   a 0x30-shaped frame (byte 1 = the subtype), exactly like GP2040. */
	{
		const unsigned char want2[] = {0x81, 0x02}, want3[] = {0x81, 0x03};
		memset(in, 0, sizeof in); in[0] = 0x80; in[1] = 0x02;
		case_("80 02 handshake", in, 2, want2, 2);
		memset(in, 0, sizeof in); in[0] = 0x80; in[1] = 0x03;
		case_("80 03 baud", in, 2, want3, 2);
	}
	for (unsigned char s = 4; s <= 6; s++) {
		char name[16];
		const unsigned char want[] = {0x30, s};
		sprintf(name, "80 0%u gate", s);
		memset(in, 0, sizeof in); in[0] = 0x80; in[1] = s;
		case_((const char *)name, in, 2, want, 2);
	}

	/* 01 sub 00 -> 21 ... ack 0x80 */
	memset(in, 0, sizeof in); in[0] = 0x01; in[10] = 0x00;
	last_len = -1;
	if (!usb_out_handler(in)) { printf("FAIL 01 00 not handled\n"); fails++; }
	else if (last_len < 16 || last[0] != 0x21 || last[13] != 0x80 || last[14] != 0x00) {
		printf("FAIL 01 00 ack: len=%d ack=%02x\n", last_len, last[13]); fails++;
	} else printf("ok   01 00 ack\n");

	/* 01 sub 02 -> device info at [15..26] */
	memset(in, 0, sizeof in); in[0] = 0x01; in[10] = 0x02;
	last_len = -1;
	if (!usb_out_handler(in)) { printf("FAIL 01 02 not handled\n"); fails++; }
	else if (last_len < 27 || last[15] != 0x04 || last[16] != 0x91 || last[17] != 0x03) {
		printf("FAIL 01 02 info: len=%d %02x %02x %02x\n", last_len, last[15], last[16], last[17]); fails++;
	} else printf("ok   01 02 device info\n");

	/* 01 sub 10 SPI read of 0x6020, 24 bytes: echo addr/len then data */
	memset(in, 0, sizeof in); in[0] = 0x01; in[10] = 0x10;
	in[11] = 0x20; in[12] = 0x60; in[13] = 0x00; in[14] = 0x00; in[15] = 24;
	last_len = -1;
	if (!usb_out_handler(in)) { printf("FAIL 01 10 not handled\n"); fails++; }
	else if (last[15] != 0x20 || last[16] != 0x60 || last[20] != 0xE3 || last[21] != 0xFF)
		{ printf("FAIL 01 10 spi: %02x %02x %02x %02x\n", last[15], last[16], last[20], last[21]); fails++; }
	else printf("ok   01 10 spi read (%d B)\n", last_len);

	/* junk packets must not crash or reply */
	memset(in, 0, sizeof in);
	last_len = -1;
	if (usb_out_handler(in)) { printf("FAIL zero packet handled\n"); fails++; }
	else printf("ok   zero packet ignored\n");

	printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
	send_in; /* silence unused warnings */
	return fails != 0;
}
