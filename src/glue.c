/* Glue between the injected blob and firmware 1.09 (Ultimate 2C, TC32).
   Installed over three stock call sites:
      0xAA68  tjl 0xe720  ->  tjl sw_out_hook   (EP5 OUT: the console's command)
      0xAA2E  tjl 0xe720  ->  tjl sw_out6_hook  (EP6 OUT: same, other endpoint)
      0xA45C  tjl 0xf378  ->  tjl sw_tick       (main loop: the 0x30 input stream)
   No libc, no .data with non-zero initialisers (the blob is flashed as-is). */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define R8(a)  (*(volatile u8 *)(a))
#define R16(a) (*(volatile u16 *)(a))

#define FW_SEND     ((int (*)(const u8 *, int))0xEB04)             /* r0=ptr r1=len            */
#define FW_SEND_ID  ((int (*)(const u8 *, int, int))0xEB7C)        /* r0=ptr r1=len r2=reportID */

static volatile u16 s_sends;   /* frames that actually went out        */
static volatile u16 s_drops;   /* OUT packets lost to a full queue     */
static volatile u8 s_busy;     /* a packet is being handled            */

struct input_state { u32 btn; u16 lx, ly, rx, ry; u8 zl, zr; };
extern struct input_state g_input;
extern void pack_report(u8 *out);                      /* packer.c   */
extern int  usb_out_handler(const unsigned char *in);  /* handler.c  */
extern void read_state(void);                          /* below      */
extern const u8 *handler_last_reply(void);             /* handler.c  */
static const u8 *s_lr;
static void trace_pkt(const unsigned char *buf);

/* Direct EP4 (0x84) burst, exactly like the stock XInput packer does it at
   0xEDAA: clear the length byte, push bytes through the data port, kick.
   Not gated on the generic sender's state, which is why we do it by hand. */
static int ep4_send(const u8 *f, int n)
{
	volatile u8 *len = (volatile u8 *)0x800114;
	volatile u8 *dat = (volatile u8 *)0x80011C;
	volatile u8 *ctl = (volatile u8 *)0x800124;
	int i;

	if (*ctl & 1)                              /* endpoint still busy (stock checks this) */
		return 0;
	*len = 0;
	for (i = 0; i < 64; i++)
		*dat = i < n ? f[i] : 0;
	*ctl = 1;
	s_sends++;
	return 1;
}

static int ep4_send64(const u8 *f)
{
	return ep4_send(f, 64);
}

static volatile u8 s_src;      /* which OUT site last fired             */
static volatile u8 s_len;      /* length the stock code claimed         */
static volatile u8 s_res;      /* handler result                        */
static volatile u8 s_sent;     /* a 0x30 frame has already gone out    */
static volatile u8 s_has;
static const u8 *s_rp;         /* pending reply: buffer handed to send_in */
static volatile u8 s_rl;
static volatile u8 s_pending;  /* a reply is waiting for a free EP4    */
static u8 s_last[2];           /* first bytes of the last reply sent   */
static volatile u8 s_ready;    /* console sent 80 04: stream inputs now */

/* The console sends its ladder back-to-back; a single slot dropped packets
   (identified by a missing 80 01 in the trace). Queue them instead.
   Depth 2 and 60-byte-free entries keep the whole scratch block inside the
   248-byte hole at 0x8432C0 (see blob.ld). */
#define QN 2
#define QS 40
static u8 s_q[QN][QS];
static volatile u8 s_qh, s_qn;

/* EP4 is busy most of the time (we stream 0x30 frames), so a reply built in
   response to an OUT packet usually arrives when the endpoint is taken. Keep it
   and retry on the next main-loop pass instead of dropping it. */
static int flush_reply(void)
{
	if (s_pending != 1) {                      /* never trust a stale byte */
		s_pending = 0;
		return 0;
	}
	if (!ep4_send(s_rp, s_rl))
		return 0;
	s_pending = 0;
	return 1;
}

void send_in(const unsigned char *buf, int len)
{
	s_rp = buf;
	s_rl = (u8)len;
	s_last[0] = buf[0];
	s_last[1] = len > 1 ? buf[1] : 0;
	s_pending = 1;
	flush_reply();
}

/* Two hook sites fire for the same OUT packet and hand over two different
   layouts: one passes the report itself, the other a {len, report...} buffer.
   Normalise both and queue the packet; the second site skips it because the
   report id never appears at both offsets in one buffer. */
static void cap(const unsigned char *buf, int len, u8 src)
{
	int i, off = 0;
	unsigned char id = buf[0];

	if (id != 0x80 && id != 0x81 && id != 0x01) {
		if (buf[1] != 0x80 && buf[1] != 0x81 && buf[1] != 0x01)
			return;                    /* not a console packet at all */
		off = 1;
	}
	if (s_qn >= QN) {
		s_drops++;
		return;
	}
	s_src = src;
	s_len = (u8)len;
	for (i = 0; i < QS; i++)
		s_q[(u8)((s_qh + s_qn) % QN)][i] = buf[i + off];
	s_qn++;
}

/* Handle one queued packet. Called from the OUT hook (interrupt context, so the
   reply goes out within microseconds, like the reference firmware) and from the
   main loop for anything the ISR left behind. s_busy keeps the two apart. */
static void process(void)
{
	u8 *p;

	if (s_busy || s_qn == 0)
		return;
	s_busy = 1;
	p = s_q[s_qh];
	s_qh = (u8)((s_qh + 1) % QN);
	s_qn--;
	s_res = (u8)usb_out_handler(p);
	s_lr = handler_last_reply();
	if (p[0] == 0x80 && p[1] == 0x04)
		s_ready = 1;                   /* the gate: only now stream inputs */
	trace_pkt(p);
	flush_reply();
	s_busy = 0;
}

/* 0xAA68: EP5 OUT, r0 = buffer, r1 = length already set up by the stock ISR. */
void sw_out_hook(const unsigned char *buf, int len)
{
	cap(buf, len, 0xA5);
	process();
	((void (*)(const unsigned char *, int))0xE720)(buf, len);
}

/* 0xAA2E: EP6 OUT branch, same registers. */
void sw_out6_hook(const unsigned char *buf, int len)
{
	cap(buf, len, 0xA6);
	process();
	((void (*)(const unsigned char *, int))0xE720)(buf, len);
}

/* Diagnostic: append the first 128 OUT packets to blank flash at 0x53000 (an
   area the 2C toolkit documents as unused) so the console's side of the
   conversation can be read back through the bootloader with tools/trace_read.py.
   Costs one 4 KB sector erase per power-up; nothing reads it at runtime.
   24-byte record: pkt[0..11], len, site, result, drops(2), sends(2), reply[0..1]. */
#define TRACE_BASE 0x53000u
#define TRACE_REC  32u

static unsigned short s_trace_n;

static void trace_pkt(const unsigned char *buf)
{
	unsigned char r[TRACE_REC];
	int i;

	if (s_trace_n >= 128)
		return;
	if (s_trace_n == 0) {
		((void (*)(void))0xBD00)();            /* flash_unlock */
		((void (*)(u32))0xFE98)(TRACE_BASE);   /* flash_erase, one 4K sector */
	}
	for (i = 0; i < 20; i++)
		r[i] = buf[i];
	r[20] = s_len;
	r[21] = s_src;
	r[22] = s_res;
	r[23] = s_ready;
	r[24] = (u8)(s_drops & 0xFF);
	r[25] = (u8)(s_drops >> 8);
	if (s_lr != 0) {
		for (i = 0; i < 6; i++)
			r[26 + i] = s_lr[13 + i];
	} else {
		for (i = 0; i < 6; i++)
			r[26 + i] = 0;
	}
	((void (*)(u32, u32, unsigned char *))*(volatile unsigned long *)0x842E3C)
		(TRACE_BASE + (u32)s_trace_n * TRACE_REC, TRACE_REC, r);
	s_trace_n++;
}


#ifndef DIAG_ALWAYS_STREAM
#define DIAG_ALWAYS_STREAM 0
#endif

/* 0xA45C: main loop, every pass. */
int sw_tick(void)
{
	u8 fr[64];
	while (s_qn)
		process();

	/* The stock DInput sender is change-driven (~20 Hz). A wired Pro
	   Controller streams every few ms, so drive the stream from the 5 ms
	   main loop instead. sw_pack() keeps g_input fresh from the stock
	   packer's registers; nothing goes out before the console's 80 04. */
	if (s_pending == 1) {
		/* A console command is waiting for the IN endpoint: pause the
		   input stream so the reply gets the next free transfer. */
		flush_reply();
	} else if (s_ready || DIAG_ALWAYS_STREAM) {
		read_state();
		if (g_input.zl > 32)
			g_input.btn |= 1u << 14;      /* BIT_ZL */
		else
			g_input.btn &= ~(1u << 14);
		if (g_input.zr > 32)
			g_input.btn |= 1u << 15;      /* BIT_ZR */
		else
			g_input.btn &= ~(1u << 15);
		pack_report(fr);
		ep4_send64(fr);
	}

	return ((int (*)(void))0xF378)();
}

/* The 0xF1CC hook is NOT installed any more: swallowing the stock DInput
   packet also stopped the pad from scanning its buttons (measured: with the
   hook in place the button word and all four axis words froze). With the
   stock pipeline undisturbed the live state is readable from SRAM:
   0x84342C u32 button bits, axis words centred on 0x7FFF spanning
   0x0000..0xFFFF, triggers at 0x84340E (L) and 0x8433E8 (R). */
static u32 rd32(u32 a)
{
	return (u32)((volatile u8 *)a)[0] | ((u32)((volatile u8 *)a)[1] << 8) |
	       ((u32)((volatile u8 *)a)[2] << 16) | ((u32)((volatile u8 *)a)[3] << 24);
}

static u16 rd16(u32 a)
{
	return (u16)((u16)((volatile u8 *)a)[0] | ((u16)((volatile u8 *)a)[1] << 8));
}

void read_state(void)
{
	g_input.btn = rd32(0x84342Cu) & 0x0003FFFFu;
	/* all four axes are stored inverted relative to the Switch wire format */
	g_input.lx = (u16)((0xFFFFu - rd16(0x8433ECu)) >> 4);
	g_input.ly = (u16)((0xFFFFu - rd16(0x8433F6u)) >> 4);
	g_input.rx = (u16)((0xFFFFu - rd16(0x8433E4u)) >> 4);
	g_input.ry = (u16)((0xFFFFu - rd16(0x8433E6u)) >> 4);
	g_input.zl = ((volatile u8 *)0x84340Eu)[0];
	g_input.zr = ((volatile u8 *)0x8433E8u)[0];
}

/* Hook bytes: let the assembler encode tjl, build_blob.sh extracts each section.
   patch.py writes these four bytes over the stock call sites. */
__asm__(
	".code 16\n"
	".section .hookaa68,\"ax\"\n"
	"tjl sw_out_hook\n"
	".section .hookaa2e,\"ax\"\n"
	"tjl sw_out6_hook\n"
	".section .hooka45c,\"ax\"\n"
	"tjl sw_tick\n"
);

