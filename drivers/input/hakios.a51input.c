/*
 * The Samsung Galaxy A51's (SM-A515F, Exynos 9611) buttons and touch screen, as a source of records for
 * /dev/hakios-touch (sys/hakios.input.h). Buttons with boot-arg buttons=1, the touch screen with touchscreen=1.
 *
 * Where the numbers come from: the phone's own device tree (a51-dump/a51.dts), and Samsung's kernel for it
 * (Exynos9611Development/android_kernel_samsung_universal9611): drivers/input/touchscreen/melfas/mss100,
 * drivers/i2c/busses/i2c-s3c2410.c, drivers/pinctrl/samsung/pinctrl-exynos-arm64.c, cal-if/exynos9610.
 *
 * Buttons: gpio-keys, gpa1 pins 5 (volume up), 6 (volume down), 7 (power), active low, in the ALIVE pin controller
 * (0x11850000, gpa1 at 0x40: DAT at 0x44). Read only, 60 times a second; a key changes when two looks in a row say so,
 * and a key seen held before it was ever seen released is not believed. Power is the side button
 * (HAKIOS_BUTTON_LOCK): the iPhone X has a side button and two volume keys, as this phone does.
 *
 * Touch: a Melfas MSS100 (melfas,mms_ts, firmware mss100_a51) at 0x48 on the I2C controller at 0x13870000
 * (samsung,s3c2440-i2c: the classic s3c2440 register set). The chip runs its own firmware; it needs its supply, a bus,
 * and to be asked for its events:
 *   supply      tsp_ldo_en: a fixed regulator on gpg3 pin 1, active high ("regulator-boot-on")
 *   bus pins    gpp1 0 (SDA), 1 (SCL), function 2, no pull (TOP pin controller 0x139B0000, gpp1 at 0x20)
 *   bus clock   CMU_PERI 0x10030000 + 0x2054, GOUT_BLK_PERI_UID_I2C_2_IPCLKPORT_PCLK (forced on: CG_VAL | MANUAL)
 *   interrupt   gpa0 pin 0, low while the chip has events: read as an input, not taken as an interrupt
 *   events      MIP4 protocol: read 0x02 0x10 (packet info: bit 7 the category, 0 = touches; the low 7 bits the
 *               number of events, size type 1), then 0x02 0x11 for that many events of 16 bytes (event format 10):
 *               byte 0 bits 7..6 the state (1 press, 2 move, 3 release), 5..2 the finger + 1, 1..0 the type
 *               (1 = screen); x = byte1 << 4 | byte3 >> 4, y = byte2 << 4 | byte3 & 15, over the chip's resolution
 *               (0x01 0x10 / 0x12, little endian; the tree says 4095 x 4095 for 1080 x 2400)
 * The bus is driven by polling, as i2c-s3c2410.c does for its "polling" variant: the same states, its interrupt line
 * never taken. SCL is PCLK / 512, under the 400 kHz the tree allows whatever the PERI bus clock is.
 *
 * Steps, each said in the log before it is made (boot-arg a51_touch_steps=N stops after the first N):
 *   1  the supply on (gpg3-1 high), and the chip's 150 ms to start
 *   2  the bus: its clock, its pins, the controller set up
 *   3  the chip: its name and resolution read
 *   4  touches (the default)
 *
 * Status: written for the phone, built; what has been seen working is in devices/a51/PORTING.md.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <kern/startup.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

/* ---- registers ---- */

#define ALIVE_GPIO_PHYS         0x11850000UL
#define TOP_GPIO_PHYS           0x139B0000UL
#define CMU_PERI_PHYS           0x10030000UL
#define I2C_PHYS                0x13870000UL

#define BANK_CON(b)             ((b) + 0x0)
#define BANK_DAT(b)             ((b) + 0x4)
#define BANK_PUD(b)             ((b) + 0x8)
#define GPA0                    0x020           /* ALIVE */
#define GPA1                    0x040           /* ALIVE */
#define GPP1                    0x020           /* TOP */
#define GPG3                    0x120           /* TOP */

#define PERI_I2C_2_PCLK_GATE    0x2054
#define CMU_GATE_FORCE_ON       ((1u << 21) | (1u << 20))       /* CG_VAL | MANUAL */

#define IICCON                  0x00
#define IICSTAT                 0x04
#define IICADD                  0x08
#define IICDS                   0x0C
#define IICLC                   0x10
#define IICCON_ACKEN            (1u << 7)
#define IICCON_TXDIV_512        (1u << 6)
#define IICCON_IRQEN            (1u << 5)
#define IICCON_IRQPEND          (1u << 4)
#define IICSTAT_MASTER_RX       (2u << 6)
#define IICSTAT_MASTER_TX       (3u << 6)
#define IICSTAT_START           (1u << 5)       /* written: START; read: busy */
#define IICSTAT_TXRXEN          (1u << 4)
#define IICSTAT_LASTBIT         (1u << 0)       /* 1: the last byte was not acknowledged */
#define IICLC_FILTER_ON         (1u << 2)
#define IICLC_SDA_DELAY15       (3u << 0)

#define MELFAS_ADDR             0x48
#define MIP_EVENT_SIZE          16
#define MIP_MAX_FINGERS         10

#define POLL_MS                 16              /* buttons */
#define TOUCH_POLL_MS           8               /* the touch screen's line */

static vm_offset_t alive, top, peri, i2c;

static inline uint32_t
rd(vm_offset_t a)
{
	return *(volatile uint32_t *)a;
}

static inline void
wr(vm_offset_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
}

/* one pin of a bank: function (4 bits each) and pull (4 bits each, 0 = none) */
static void
pin_setup(vm_offset_t bank, unsigned pin, uint32_t function, uint32_t pull)
{
	wr(BANK_CON(bank), (rd(BANK_CON(bank)) & ~(0xfu << (pin * 4))) | (function << (pin * 4)));
	wr(BANK_PUD(bank), (rd(BANK_PUD(bank)) & ~(0xfu << (pin * 4))) | (pull << (pin * 4)));
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- buttons ---- */

static struct a51_button {
	const char *name;
	unsigned    pin;                /* of gpa1 */
	uint32_t    record;             /* HAKIOS_BUTTON_* */
	int         pressed, candidate, seen_released;
} buttons[] = {
	{ "volume up", 5, HAKIOS_BUTTON_VOLUME_UP, 0, 0, 0 },
	{ "volume down", 6, HAKIOS_BUTTON_VOLUME_DOWN, 0, 0, 0 },
	{ "power", 7, HAKIOS_BUTTON_LOCK, 0, 0, 0 },
};
static int buttons_on;

static void
buttons_look(void)
{
	uint32_t dat = rd(BANK_DAT(alive + GPA1));

	for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		struct a51_button *b = &buttons[i];
		int now = !(dat & (1u << b->pin));      /* active low */

		if (!now) {
			b->seen_released = 1;
		}
		if (now == b->pressed) {
			b->candidate = 0;
			continue;
		}
		if (!b->candidate) {
			b->candidate = 1;               /* once: wait for the next look to say the same */
			continue;
		}
		b->candidate = 0;
		if (now && !b->seen_released) {
			continue;                       /* held since before we looked: not believed */
		}
		b->pressed = now;
		printf("a51input: %s %s\n", b->name, now ? "pressed" : "released");
		hakios_touch_post(now ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, b->record, 0, 0);
	}
}

/* ---- the I2C master, polled ---- */

static int
i2c_wait_pending(void)
{
	for (int i = 0; i < 10000; i++) {       /* 10 ms */
		if (rd(i2c + IICCON) & IICCON_IRQPEND) {
			return 0;
		}
		delay(1);
	}
	return -1;
}

static void
i2c_clear_pending(void)
{
	wr(i2c + IICCON, rd(i2c + IICCON) & ~IICCON_IRQPEND);
}

static void
i2c_stop(uint32_t mode)
{
	wr(i2c + IICSTAT, mode | IICSTAT_TXRXEN);       /* START cleared: STOP */
	i2c_clear_pending();
	for (int i = 0; i < 1000 && (rd(i2c + IICSTAT) & IICSTAT_START); i++) {
		delay(1);
	}
}

/* the address byte with a START; mode IICSTAT_MASTER_TX or _RX; 0 when acknowledged */
static int
i2c_start(uint32_t mode, uint8_t addr_byte, int repeated)
{
	wr(i2c + IICSTAT, mode | IICSTAT_TXRXEN);
	*(volatile uint8_t *)(i2c + IICDS) = addr_byte;
	delay(1);
	wr(i2c + IICCON, rd(i2c + IICCON) | IICCON_ACKEN);
	wr(i2c + IICSTAT, mode | IICSTAT_TXRXEN | IICSTAT_START);
	if (repeated) {
		i2c_clear_pending();
	}
	if (i2c_wait_pending() != 0) {
		return -1;
	}
	return (rd(i2c + IICSTAT) & IICSTAT_LASTBIT) ? -2 : 0;
}

/* write wlen bytes, then (repeated START) read rlen; 0 or a negative step number */
static int
i2c_xfer(uint8_t addr, const uint8_t *wbuf, unsigned wlen, uint8_t *rbuf, unsigned rlen)
{
	int r;

	for (int i = 0; i < 1000 && (rd(i2c + IICSTAT) & IICSTAT_START); i++) {
		delay(1);                       /* the bus busy from before */
	}
	wr(i2c + IICCON, IICCON_ACKEN | IICCON_TXDIV_512 | IICCON_IRQEN);       /* scale 0, nothing pending */

	if ((r = i2c_start(IICSTAT_MASTER_TX, (uint8_t)(addr << 1), 0)) != 0) {
		i2c_stop(IICSTAT_MASTER_TX);
		return r == -1 ? -1 : -2;
	}
	for (unsigned i = 0; i < wlen; i++) {
		*(volatile uint8_t *)(i2c + IICDS) = wbuf[i];
		delay(1);
		i2c_clear_pending();
		if (i2c_wait_pending() != 0) {
			i2c_stop(IICSTAT_MASTER_TX);
			return -3;
		}
		if (rd(i2c + IICSTAT) & IICSTAT_LASTBIT) {
			i2c_stop(IICSTAT_MASTER_TX);
			return -4;
		}
	}
	if (rlen == 0) {
		i2c_stop(IICSTAT_MASTER_TX);
		return 0;
	}
	if ((r = i2c_start(IICSTAT_MASTER_RX, (uint8_t)(addr << 1 | 1), 1)) != 0) {
		i2c_stop(IICSTAT_MASTER_RX);
		return r == -1 ? -5 : -6;
	}
	for (unsigned i = 0; i < rlen; i++) {
		if (i == rlen - 1) {
			wr(i2c + IICCON, rd(i2c + IICCON) & ~IICCON_ACKEN);     /* no ACK after the last byte */
		}
		i2c_clear_pending();
		if (i2c_wait_pending() != 0) {
			i2c_stop(IICSTAT_MASTER_RX);
			return -7;
		}
		rbuf[i] = *(volatile uint8_t *)(i2c + IICDS);
	}
	i2c_stop(IICSTAT_MASTER_RX);
	wr(i2c + IICCON, rd(i2c + IICCON) | IICCON_ACKEN);
	return 0;
}

static int
mip_read(uint8_t r0, uint8_t r1, uint8_t *buf, unsigned len)
{
	uint8_t reg[2] = { r0, r1 };

	return i2c_xfer(MELFAS_ADDR, reg, 2, buf, len);
}

/* ---- the touch screen ---- */

static uint32_t res_x = 4096, res_y = 4096;
static int finger_down[MIP_MAX_FINGERS];
static int touch_on;
static uint64_t touch_errors;

static uint32_t
scale(uint32_t v, uint32_t range)
{
	if (range == 0) {
		return 0;
	}
	if (v >= range) {
		v = range - 1;
	}
	return (uint32_t)((uint64_t)v * 65535 / (range - 1));
}

/*
 * How often a finger that moves is reported (the screen's content follows it no more often): the time from one move
 * of the first finger to its next, and how long the chip's two reads took. Said every 10 s that had moves.
 */
static uint32_t touch_gaps[7];                  /* under 10, 15, 20, 30, 50, 100 ms, longer */
static uint32_t touch_moves, touch_reads;
static uint64_t touch_last_move, touch_read_ns, touch_read_worst;

static void
touch_gap(void)
{
	uint64_t now = mach_absolute_time(), ns;
	static const uint32_t limits[] = { 10, 15, 20, 30, 50, 100 };
	unsigned b;

	if (touch_last_move != 0) {
		absolutetime_to_nanoseconds(now - touch_last_move, &ns);
		for (b = 0; b < 6 && ns >= (uint64_t)limits[b] * NSEC_PER_MSEC; b++) {
		}
		touch_gaps[b]++;
	}
	touch_last_move = now;
	touch_moves++;
}

static void
touch_report(void)
{
	if (touch_moves == 0) {
		return;
	}
	printf("a51input: touch, 10 s: %u moves of the first finger, from one to the next: under 10 ms %u, 10-15 %u, 15-20 %u, "
	    "20-30 %u, 30-50 %u, 50-100 %u, longer %u; %u reads of the chip, %llu us each, the longest %llu us\n", touch_moves,
	    touch_gaps[0], touch_gaps[1], touch_gaps[2], touch_gaps[3], touch_gaps[4], touch_gaps[5], touch_gaps[6], touch_reads,
	    touch_reads != 0 ? touch_read_ns / touch_reads / 1000 : 0, touch_read_worst / 1000);
	bzero(touch_gaps, sizeof(touch_gaps));
	touch_moves = touch_reads = 0;
	touch_read_ns = touch_read_worst = 0;
}

static void
touch_events(void)
{
	uint8_t info = 0, buf[MIP_EVENT_SIZE * MIP_MAX_FINGERS];
	unsigned size;
	int r;
	uint64_t began = mach_absolute_time(), ns;

	if ((r = mip_read(0x02, 0x10, &info, 1)) != 0) {
		if (touch_errors++ < 10) {
			printf("a51input: touch: packet info: i2c step %d failed\n", -r);
		}
		return;
	}
	size = (info & 0x7f) * ((info & 0x80) ? 1 : MIP_EVENT_SIZE);
	if (size == 0 || size > sizeof(buf)) {
		size = sizeof(buf);
		(void)mip_read(0x02, 0x11, buf, size);  /* drain, as the vendor's driver does */
		return;
	}
	if ((r = mip_read(0x02, 0x11, buf, size)) != 0) {
		if (touch_errors++ < 10) {
			printf("a51input: touch: packet data: i2c step %d failed\n", -r);
		}
		return;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
	touch_reads++;
	touch_read_ns += ns;
	touch_read_worst = ns > touch_read_worst ? ns : touch_read_worst;
	if (info & 0x80) {
		return;                         /* an alert (ESD, gestures...): nothing to post */
	}
	for (unsigned i = 0; i + MIP_EVENT_SIZE <= size; i += MIP_EVENT_SIZE) {
		const uint8_t *p = &buf[i];
		unsigned state = p[0] >> 6, type = p[0] & 3;
		int id = (int)((p[0] >> 2) & 0xf) - 1;
		uint32_t x = (uint32_t)p[1] << 4 | p[3] >> 4, y = (uint32_t)p[2] << 4 | (p[3] & 0xf);

		if (type != 1 || id < 0 || id >= MIP_MAX_FINGERS) {
			continue;
		}
		x = scale(x, res_x);
		y = scale(y, res_y);
		if (state == 2 && id == 0 && finger_down[id]) {
			touch_gap();
		} else if (state == 3 && id == 0) {
			touch_last_move = 0;
		}
		if (state == 1 || state == 2) {
			hakios_touch_post(finger_down[id] ? HAKIOS_TOUCH_MOVE : HAKIOS_TOUCH_DOWN, (uint32_t)id, x, y);
			finger_down[id] = 1;
		} else if (state == 3 && finger_down[id]) {
			hakios_touch_post(HAKIOS_TOUCH_UP, (uint32_t)id, x, y);
			finger_down[id] = 0;
		}
	}
}

static int
touch_bringup(uint32_t steps)
{
	uint8_t name[16] = { 0 }, res[4];
	int r;

	printf("a51input: touch step 1: the supply on (tsp_ldo_en, gpg3-1)\n");
	pin_setup(top + GPG3, 1, 1 /* output */, 0);
	wr(BANK_DAT(top + GPG3), rd(BANK_DAT(top + GPG3)) | (1u << 1));
	pin_setup(alive + GPA0, 0, 0 /* input */, 0);  /* the chip's line, read, not taken as an interrupt */
	sleep_ms(150);
	if (steps < 2) {
		return -1;
	}

	printf("a51input: touch step 2: the bus (I2C at 0x%lx, its clock, gpp1-0/1)\n", I2C_PHYS);
	wr(peri + PERI_I2C_2_PCLK_GATE, rd(peri + PERI_I2C_2_PCLK_GATE) | CMU_GATE_FORCE_ON);
	pin_setup(top + GPP1, 0, 2, 0);
	pin_setup(top + GPP1, 1, 2, 0);
	wr(i2c + IICLC, IICLC_FILTER_ON | IICLC_SDA_DELAY15);
	wr(i2c + IICCON, IICCON_ACKEN | IICCON_TXDIV_512 | IICCON_IRQEN);
	printf("a51input: touch: I2C CON %x STAT %x LC %x\n", rd(i2c + IICCON), rd(i2c + IICSTAT), rd(i2c + IICLC));
	if (steps < 3) {
		return -1;
	}

	printf("a51input: touch step 3: the chip (Melfas at 0x%x)\n", MELFAS_ADDR);
	if ((r = mip_read(0x01, 0x00, name, 15)) != 0) {
		printf("a51input: touch: the chip does not answer (i2c step %d failed): no touch screen\n", -r);
		return -1;
	}
	if (mip_read(0x01, 0x10, res, 4) == 0) {
		uint32_t x = res[0] | (uint32_t)res[1] << 8, y = res[2] | (uint32_t)res[3] << 8;
		if (x >= 256 && x <= 16384 && y >= 256 && y <= 16384) {
			res_x = x;
			res_y = y;
		}
	}
	for (int i = 0; i < 15; i++) {
		if (name[i] < 0x20 || name[i] > 0x7e) {
			name[i] = name[i] ? '.' : 0;
		}
	}
	printf("a51input: touch: \"%s\", %u x %u\n", (const char *)name, res_x, res_y);
	if (steps < 4) {
		return -1;
	}
	printf("a51input: touch step 4: touches, the chip's line looked at every %d ms\n", TOUCH_POLL_MS);
	return 0;
}

/* ---- the thread ---- */

static uint32_t touch_steps = 4;

static void
a51input_thread(__unused void *arg, __unused wait_result_t wr_)
{
	unsigned tick = 0;

	if (touch_on && touch_bringup(touch_steps) != 0) {
		touch_on = 0;
	}
	printf("a51input: watching%s%s\n", buttons_on ? " the buttons" : "", touch_on ? " the touch screen" : "");
	for (;;) {
		if (touch_on && !(rd(BANK_DAT(alive + GPA0)) & 1)) {
			touch_events();                 /* the chip holds its line low while it has events */
		}
		if (buttons_on && (tick % (POLL_MS / TOUCH_POLL_MS)) == 0) {
			buttons_look();
		}
		tick++;
		if (touch_on && tick % (10000 / TOUCH_POLL_MS) == 0) {
			touch_report();
		}
		sleep_ms(TOUCH_POLL_MS);
	}
}

static thread_call_t a51input_call;

static void
a51input_start(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	thread_t thread;

	if (kernel_thread_start(a51input_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

static void
a51input_init(void)
{
	uint32_t v = 0;
	uint64_t deadline;

	touch_on = PE_parse_boot_argn("touchscreen", &v, sizeof(v)) && v != 0;
	v = 0;
	if (PE_parse_boot_argn("buttons", &v, sizeof(v))) {
		buttons_on = v != 0;
	} else {
		buttons_on = touch_on;          /* as on redwood: on with the touch screen unless buttons=0 */
	}
	PE_parse_boot_argn("a51_touch_steps", &touch_steps, sizeof(touch_steps));
	if (!touch_on && !buttons_on) {
		return;                         /* nothing is touched unless a boot-argument asks */
	}
	alive = ml_io_map(ALIVE_GPIO_PHYS, 0x1000);
	top = ml_io_map(TOP_GPIO_PHYS, 0x1000);
	peri = ml_io_map(CMU_PERI_PHYS, 0x3000);
	i2c = ml_io_map(I2C_PHYS, 0x1000);
	if (!alive || !top || !peri || !i2c) {
		printf("a51input: cannot map the registers\n");
		return;
	}
	/* a thread cannot be made this early: one second later, from a thread call */
	a51input_call = thread_call_allocate(a51input_start, NULL);
	clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(a51input_call, deadline);
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, a51input_init);
