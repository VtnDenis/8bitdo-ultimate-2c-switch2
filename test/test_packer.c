/* Host-side unit test for the report packer (no hardware needed).
   gcc -I src -o /tmp/tp test/test_packer.c src/packer.c && /tmp/tp
*/
#include <stdio.h>
#include <string.h>

#include "../src/packer.c"

static int fails;

static void expect_frame(const char *what, unsigned long word, unsigned lx, unsigned ly,
                         unsigned rx, unsigned ry, const unsigned char *want, int n)
{
	unsigned char out[64];
	g_input.btn = word;
	s_tick = 0;
	g_input.lx = (u16)lx; g_input.ly = (u16)ly;
	g_input.rx = (u16)rx; g_input.ry = (u16)ry;
	memset(out, 0xEE, sizeof out);
	pack_report(out);
	if (memcmp(out, want, (size_t)n)) {
		printf("FAIL %s\n  got  ", what);
		for (int i = 0; i < n; i++) printf("%02x ", out[i]);
		printf("\n  want ");
		for (int i = 0; i < n; i++) printf("%02x ", want[i]);
		printf("\n");
		fails++;
	} else {
		printf("ok   %s\n", what);
	}
}

int main(void)
{
	/* neutral: no buttons, both sticks centred (2048 = 0x800) */
	const unsigned char idle[] = {0x30, 0x00, 0x80, 0x00, 0x80, 0x00,
	                              0x00, 0x08, 0x80, 0x00, 0x08, 0x80, 0x09};
	expect_frame("idle", 0, 2048, 2048, 2048, 2048, idle, sizeof idle);

	/* face buttons: bit12 A, bit13 B, bit5 X, bit4 Y */
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x08, 0x80, 0x00}; expect_frame("A", 1u << 12, 2048, 2048, 2048, 2048, w, 6); }
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x04, 0x80, 0x00}; expect_frame("B", 1u << 13, 2048, 2048, 2048, 2048, w, 6); }
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x02, 0x80, 0x00}; expect_frame("X", 1u << 5, 2048, 2048, 2048, 2048, w, 6); }
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x01, 0x80, 0x00}; expect_frame("Y", 1u << 4, 2048, 2048, 2048, 2048, w, 6); }

	/* shoulders and triggers */
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x40, 0x80, 0x00}; expect_frame("R", 1u << 11, 2048, 2048, 2048, 2048, w, 6); }
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x80, 0x80, 0x00}; expect_frame("ZR", 1u << 15, 2048, 2048, 2048, 2048, w, 6); }
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x00, 0x80, 0x40}; expect_frame("L", 1u << 10, 2048, 2048, 2048, 2048, w, 6); }
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x00, 0x80, 0x80}; expect_frame("ZL", 1u << 14, 2048, 2048, 2048, 2048, w, 6); }

	/* system buttons */
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x00, 0x82, 0x00}; expect_frame("Plus", 1u << 0, 2048, 2048, 2048, 2048, w, 6); }
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x00, 0x81, 0x00}; expect_frame("Minus", 1u << 3, 2048, 2048, 2048, 2048, w, 6); }
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x00, 0x90, 0x00}; expect_frame("Home", 1u << 17, 2048, 2048, 2048, 2048, w, 6); }
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x00, 0x88, 0x00}; expect_frame("LStick", 1u << 1, 2048, 2048, 2048, 2048, w, 6); }
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x00, 0x84, 0x00}; expect_frame("RStick", 1u << 2, 2048, 2048, 2048, 2048, w, 6); }

	/* d-pad: Down=01 Up=02 Right=04 Left=08 in byte 5 */
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x00, 0x80, 0x01}; expect_frame("dpad down", 1u << 8, 2048, 2048, 2048, 2048, w, 6); }
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x00, 0x80, 0x02}; expect_frame("dpad up", 1u << 9, 2048, 2048, 2048, 2048, w, 6); }
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x00, 0x80, 0x04}; expect_frame("dpad right", 1u << 6, 2048, 2048, 2048, 2048, w, 6); }
	{ const unsigned char w[] = {0x30, 0, 0x80, 0x00, 0x80, 0x08}; expect_frame("dpad left", 1u << 7, 2048, 2048, 2048, 2048, w, 6); }

	/* sticks: min / max / asymmetric, 12-bit little-endian packed */
	{ const unsigned char w[] = {0x30, 0, 0x80, 0, 0x80, 0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	  expect_frame("sticks min", 0, 0, 0, 0, 0, w, 12); }
	{ const unsigned char w[] = {0x30, 0, 0x80, 0, 0x80, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	  expect_frame("sticks max", 0, 4095, 4095, 4095, 4095, w, 12); }
	/* lx=0x100 -> 00 00 01 ; ly=0x234 -> 34 02 ; rx=0x567 -> 67 56 ; ry=0x89A -> 9A 08 */
	{ const unsigned char w[] = {0x30, 0, 0x80, 0, 0x80, 0, 0x00, 0x41, 0x23, 0x67, 0xA5, 0x89};
	  expect_frame("stick packing", 0, 0x100, 0x234, 0x567, 0x89A, w, 12); }

	printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
	return fails != 0;
}
