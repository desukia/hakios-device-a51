/*
 * The Galaxy A51's Wi-Fi: Samsung's WLBT core inside the Exynos 9611 (the "S620" Maxwell, dt scsc_wifibt@119c0000),
 * woken step by step like the GPU (hakios.a51gpu.c). Nothing here is touched unless boot-arg a51wifi=N asks; steps
 * 1..N are made, each said before it is made, reads before writes.
 *
 * The core runs firmware of its own (mx140.bin, from /vendor/etc/wifi of the phone's Android, in fw/ of the small
 * image: userland/build.sh from ../fw-wifi-a51) out of 16 MB of the application processor's memory that it reaches
 * through bus windows. Samsung's driver (drivers/misc/samsung/scsc: mxman.c, platform_mif_9610.c, mif_reg_S5E9610.h,
 * fwhdr.c, fwimage.c; copies in ~/Desktop/ios-a51/ref/scsc) starts it so -- see PORTING.md, "WiFi":
 *   the 16 MB, the firmware copied to its start; a configuration block (mxconf) in it; mailboxes 0..3 (entry point,
 *   mxconf, 0xbcdeedcb, flags); the PMU's power request, reset release and start; the core's CFG_REQ interrupt answered
 *   (TZPC opened by SMC, the bus windows set, boot_cfg acknowledged); its MM_START_IND on the management transport.
 *
 *   1  reads   the PMU's WLBT registers (0x11860000: WLBT_CTRL_NS 0x50, WLBT_CTRL_S 0x54, WLBT_STAT 0x58, WLBT_DEBUG
 *              0x5c, CENTRAL_SEQ_WLBT_CONFIGURATION 0x180 and _STATUS 0x184) and the AP-WLBT mailbox (119c0000: its
 *              mask, status and eight shared registers); then, once the small image is there, the firmware's header
 *              (its "smxf" magic, versions, lengths, entry point, build) and its header and constant-part CRC32s; and
 *              takes 16 MB of physically contiguous memory for the core
 *
 *   2  writes  the firmware into the 16 MB; mailboxes 0 (entry point), 2 (0xbcdeedcb), 1 and 3 left 0 (no mxconf yet:
 *              step 3); the PMU's power request (WLBT_CTRL_NS MASK_PWR_REQ), reset release (WLBT_RESET_SET clear) and
 *              start (WLBT_CTRL_S WLBT_START); then waits a second for the core's CFG_REQ (SPI 73, level) and answers
 *              it as platform_cfg_req_isr does: TZPC opened (SMC 0x82000710, WLBT << 32 | SET_CONN_TZPC), WLBT_REMAP
 *              0 := 0x80000, CHIP_VERSION_ID's EMA bits, the dbus window 0x80000000-0x81400000 onto the 16 MB, the
 *              six pbus windows (mailboxes CP and SHUB, USI_CMG00, SYSREG_CMGP2WLBT, GPIO_CMGP, SHUB), boot_cfg 0 := 1,
 *              the keep-alive patch (fw/wlbt_ka_patch.bin, Samsung's ka_patch[], at boot_cfg + 0x1000), boot_cfg 0 := 0,
 *              BOOT_CFG_ACK (boot_cfg 4 := 1), and MASK_PWR_REQ handed back; then watches two seconds
 *
 *   3  with step 2: before the start, a configuration block (mxconf.h: magic 0x79828486, version 0.5) and the
 *              management transport's two rings (mxmgmt_transport: 4 packets of 128 bytes each way, cpacket_buffer: the
 *              packets, then a read and a write index) in the 16 MB just after the firmware's runtime part, mailbox 1
 *              pointing at the block; after the CFG_REQ answer, the to-AP ring watched two seconds for the firmware's
 *              MM_START_IND (channel 0, message 0, its argument the WLBT hardware revision). Without it, the firmware's
 *              panic record (fwhdr r4_panic_record_offset) is read
 *
 * What the steps found is said again every 30 s for five minutes: a whole system's start pushes the first lines out of
 * the kernel log.
 *
 * Status: steps 1 to 3 run on the phone (2026-11-15): the firmware starts and sends MM_START_IND (arg 0). Not yet:
 * the WLAN service (service management, then the slsi driver's HIP4).
 */
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern void IOSleep(unsigned int milliseconds);

#define PMU_PHYS                0x11860000UL    /* dt system-controller@11860000 (samsung,syscon-phandle of scsc_wifibt) */
#define WLBT_CTRL_NS            0x0050
#define WLBT_CTRL_S             0x0054
#define WLBT_STAT               0x0058
#define WLBT_DEBUG              0x005c
#define CENTRAL_SEQ_WLBT_CONFIGURATION 0x0180
#define CENTRAL_SEQ_WLBT_STATUS 0x0184          /* bits 23:16: the sequencer's state (0x80: in reset, ready) */

#define MBOX_PHYS               0x119c0000UL    /* dt scsc_wifibt reg, 0x180 bytes: the AP-WLBT mailbox */
#define MBOX_INTMR0             0x010
#define MBOX_INTSR0             0x014
#define MBOX_ISSR(r)            (0x080 + 4 * (r))

#define WLBT_RESET_SET          (1u << 0)       /* in WLBT_CTRL_NS */
#define MASK_PWR_REQ            (1u << 18)      /* in WLBT_CTRL_NS: the AP holds the power request */
#define WLBT_START              (1u << 0)       /* in WLBT_CTRL_S */
#define MBOX_INTGR1             0x01c
#define MBOX_INTSR1             0x028

#define WLBT_REMAP_PHYS         0x14c50000UL    /* dt syscons: wlbt_remap, dbus_baaw, pbus_baaw, boot_cfg */
#define DBUS_BAAW_PHYS          0x14c20000UL
#define PBUS_BAAW_PHYS          0x14c30000UL
#define BOOT_CFG_PHYS           0x14c60000UL
#define BAAW_ACCESS             (1u << 31 | 1u << 1 | 1u << 0)  /* init done, write, read */
#define CHIP_VERSION_EMA_MASK   0x003fffffu
#define CHIP_VERSION_EMA_VALUE  (1u << 20 | 1u << 18 | 1u << 13 | 1u << 11 | 1u << 5 | 1u << 2)
#define SMC_CMD_CONN_IF         0x82000710UL
#define IRQ_CFG_REQ             (32 + 73)       /* dt: interrupts ... 0x49 4 ("CFG_REQ"), level */
#define MBOX2_MAGIC             0xbcdeedcbu

#define WLBT_MEMORY             (16u << 20)     /* dt reserved-memory wifibt_if: size 0x1000000 */
#define FW_NAME                 "mx140.bin"

static uint32_t wifi_steps;
static char wifi_state[256] = "";
static vm_offset_t wlbt_memory;
static uint64_t wlbt_memory_pa;

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

static int64_t
smc(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
	    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return (int64_t)x0;
}

extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
extern void gic_v3_spi_mask(uint32_t intid);
extern void gic_v3_spi_detach(uint32_t intid);

static volatile uint32_t cfg_req_seen;

/* In interrupt context: CFG_REQ is level and stays up until it is acknowledged; masked here, answered by the thread. */
static void
cfg_req_irq(__unused void *context, uint32_t intid)
{
	gic_v3_spi_mask(intid);
	cfg_req_seen = 1;
	thread_wakeup((event_t)&cfg_req_seen);
}

static void
wifi_said(const char *format, ...) __printflike(1, 2);

static void
wifi_said(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vsnprintf(wifi_state, sizeof(wifi_state), format, ap);
	va_end(ap);
	printf("a51wifi: %s\n", wifi_state);
}

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Linux's ether_crc(): CRC-32 most significant bit first, polynomial 0x04c11db7, starting from all ones, not inverted. */
static uint32_t
ether_crc(const uint8_t *p, uint32_t length)
{
	uint32_t crc = 0xffffffffu;

	while (length--) {
		uint8_t byte = *p++;

		for (int bit = 0; bit < 8; bit++, byte >>= 1) {
			uint32_t top = crc >> 31;

			crc <<= 1;
			if (top ^ (byte & 1)) {
				crc ^= 0x04c11db7u;
			}
		}
	}
	return crc;
}

/*
 * Step 3's layout, as offsets in the 16 MB (which is how the core is told addresses: a "mifram ref", the distance from
 * its start), just after the firmware's runtime part. mxconf (mxconf.h, natural alignment, 240 bytes): magic 0, version
 * 4 (minor, major as two halves), the management transport at 8 (to-AP stream, then from-AP), the gdb R4 / M4 transports
 * at 56 / 104, the log stream at 152, soc_revision 176, flags 180, fwconfig 184, gdb M4_1 192. A stream (24 bytes):
 * buffer, number of packets, packet size, read-index location, write-index location, then the read and write
 * interrupt bits. The bits as Samsung's allocation gives them for the first transport: the to-AP stream is written
 * by the firmware with to-host bit 0 and read by us with from-host bit 1 (0 is the R4's panic bit); the from-AP one
 * written by us with from-host bit 2, read by the firmware with to-host bit 1.
 */
#define MX_CONF                 0x000
#define MX_TOAP_RING            0x400
#define MX_TOAP_RIDX            0x600
#define MX_TOAP_WIDX            0x604
#define MX_FROMAP_RING          0x800
#define MX_FROMAP_RIDX          0xa00
#define MX_FROMAP_WIDX          0xa04
#define MX_PACKETS              4
#define MX_PACKET_SIZE          128
#define MX_HEAP_SIZE            0x10000         /* what the layout below takes */
#define CHIPID_PHYS             0x10000000UL    /* PRO_ID: its low byte the revision */

static uint32_t mx_heap;                        /* where the layout starts in the 16 MB */

static void
mx_stream_sized(vm_offset_t conf, uint32_t ring, uint32_t ridx, uint32_t widx, uint8_t read_bit, uint8_t write_bit,
    uint32_t packets, uint32_t packet_size)
{
	*(volatile uint32_t *)(conf + 0) = mx_heap + ring;
	*(volatile uint32_t *)(conf + 4) = packets;
	*(volatile uint32_t *)(conf + 8) = packet_size;
	*(volatile uint32_t *)(conf + 12) = mx_heap + ridx;
	*(volatile uint32_t *)(conf + 16) = mx_heap + widx;
	*(volatile uint8_t *)(conf + 20) = read_bit;
	*(volatile uint8_t *)(conf + 21) = write_bit;
}

static void
mx_stream(vm_offset_t conf, uint32_t ring, uint32_t ridx, uint32_t widx, uint8_t read_bit, uint8_t write_bit)
{
	mx_stream_sized(conf, ring, ridx, widx, read_bit, write_bit, MX_PACKETS, MX_PACKET_SIZE);
}

/* The block and the rings written (the memory is zero apart from the firmware); returns the mifram ref of the block. */
static uint32_t
mx_build(uint32_t runtime_length)
{
	vm_offset_t chipid = ml_io_map(CHIPID_PHYS, 0x1000);

	mx_heap = (runtime_length + 0xfff) & ~0xfffu;
	vm_offset_t base = wlbt_memory + mx_heap, conf = base + MX_CONF;

	bzero((void *)base, MX_HEAP_SIZE);
	*(volatile uint32_t *)(conf + 0) = 0x79828486;          /* MXCONF_MAGIC */
	*(volatile uint16_t *)(conf + 4) = 0;                   /* version major */
	*(volatile uint16_t *)(conf + 6) = 5;                   /* minor */
	mx_stream(conf + 8, MX_TOAP_RING, MX_TOAP_RIDX, MX_TOAP_WIDX, 1, 0);
	mx_stream(conf + 32, MX_FROMAP_RING, MX_FROMAP_RIDX, MX_FROMAP_WIDX, 1, 2);
	/*
	 * The other transports Samsung's transports_init makes, after the management one, with the interrupt bits its
	 * allocation then gives them (the A51 is built without SCSC_MX450_GDB_SUPPORT: the M4's from-host bits are a set of
	 * their own, bit 0 reserved there too; a gdb transport's from-AP stream uses the reserved panic bit): the R4's gdb
	 * channel (2 KB of 4-byte packets each way: to-AP write bit 2 / read 3, from-AP read 3 / write 0), the M4's (to-AP
	 * write 4 / read 1, from-AP read 5 / write 0), and the log stream (16 KB of 4-byte packets, to-AP: write 6 / read 4).
	 */
	mx_stream_sized(conf + 56, 0x1000, 0x1800, 0x1804, 3, 2, 512, 4);       /* gdb R4, to AP */
	mx_stream_sized(conf + 80, 0x2000, 0x2800, 0x2804, 3, 0, 512, 4);       /* gdb R4, from AP */
	mx_stream_sized(conf + 104, 0x3000, 0x3800, 0x3804, 1, 4, 512, 4);      /* gdb M4, to AP */
	mx_stream_sized(conf + 128, 0x4000, 0x4800, 0x4804, 5, 0, 512, 4);      /* gdb M4, from AP */
	mx_stream_sized(conf + 152, 0x5000, 0x9000, 0x9004, 4, 6, 4096, 4);     /* the log */
	memset((void *)(base + 0x1000), 0xff, 0x800);           /* the to-AP (IN) streams start 0xff, as mif_stream_init */
	memset((void *)(base + 0x3000), 0xff, 0x800);
	memset((void *)(base + 0x5000), 0xff, 0x4000);
	*(volatile uint32_t *)(conf + 176) = chipid ? (rd(chipid) & 0xff) : 0;
	*(volatile uint32_t *)(conf + 180) = 0;                 /* flags: FM not on */
	memset((void *)(base + MX_TOAP_RING), 0xff, MX_PACKETS * MX_PACKET_SIZE);   /* as mif_stream_init, an IN stream */
	return mx_heap + MX_CONF;
}

static void
cache_out(vm_offset_t start, vm_size_t size)
{
	for (vm_offset_t line = start & ~63ul; line < start + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
}

/* Step 2: the core started, its CFG_REQ answered. */
static void
wifi_step2(const uint8_t *fw, uint64_t size)
{
	vm_offset_t pmu = ml_io_map(PMU_PHYS, 0x1000), mbox = ml_io_map(MBOX_PHYS, 0x1000);
	uint32_t entry = le32(fw + 40);
	uint64_t patch_size = 0;
	const uint8_t *patch = hakios_image_file("fw", "wlbt_ka_patch.bin", &patch_size, NULL);

	printf("a51wifi: step 2: the firmware into the core's memory, the mailboxes, power, reset release, start\n");
	if (patch == NULL || patch_size == 0 || (patch_size & 3) || patch_size > 0x200) {
		wifi_said("step 2: fw/wlbt_ka_patch.bin missing or not whole words (%llu bytes): stopping", patch_size);
		return;
	}
	if ((rd(pmu + CENTRAL_SEQ_WLBT_STATUS) >> 16 & 0xff) != 0 || (rd(pmu + WLBT_CTRL_S) & WLBT_START)) {
		wifi_said("step 2: the core is not as step 1 found it (STATUS %x CTRL_S %x): stopping",
		    rd(pmu + CENTRAL_SEQ_WLBT_STATUS), rd(pmu + WLBT_CTRL_S));
		return;
	}
	/* the firmware at the start of the 16 MB, out of this processor's cache: the core reads memory */
	memcpy((void *)wlbt_memory, fw, (size_t)size);
	for (vm_offset_t line = wlbt_memory; line < wlbt_memory + WLBT_MEMORY; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");

	/* the boot ROM reads: mailbox 0 where to jump, 1 the configuration (step 3; none before), 2 the magic, 3 the flags */
	uint32_t conf_ref = 0;
	if (wifi_steps >= 3) {
		conf_ref = mx_build(le32(fw + 36));
		cache_out(wlbt_memory + mx_heap, MX_HEAP_SIZE);
		/* the to-host bits: masked and cleared as platform_mif_9610 starts (INTMR0/INTCR0 0xffff0000) */
		wr(mbox + MBOX_INTMR0, 0xffff0000);
		wr(mbox + 0x00c /* INTCR0 */, 0xffff0000);
	}
	wr(mbox + MBOX_ISSR(0), entry);
	wr(mbox + MBOX_ISSR(1), conf_ref);
	wr(mbox + MBOX_ISSR(2), MBOX2_MAGIC);
	wr(mbox + MBOX_ISSR(3), 0);
	__asm__ volatile ("dsb sy");

	cfg_req_seen = 0;
	kern_return_t kr = gic_v3_spi_attach(IRQ_CFG_REQ, false, cfg_req_irq, NULL);
	if (kr != KERN_SUCCESS) {
		wifi_said("step 2: the CFG_REQ interrupt cannot be had (%d): stopping", kr);
		return;
	}
	uint32_t ns_before = rd(pmu + WLBT_CTRL_NS);
	wr(pmu + WLBT_CTRL_NS, rd(pmu + WLBT_CTRL_NS) | MASK_PWR_REQ);         /* power */
	wr(pmu + WLBT_CTRL_NS, rd(pmu + WLBT_CTRL_NS) & ~WLBT_RESET_SET);      /* out of reset */
	wr(pmu + WLBT_CTRL_S, rd(pmu + WLBT_CTRL_S) | WLBT_START);             /* start */
	printf("a51wifi: started: WLBT_CTRL_NS %x -> %x, CTRL_S %x; waiting for CFG_REQ\n", ns_before, rd(pmu + WLBT_CTRL_NS),
	    rd(pmu + WLBT_CTRL_S));

	for (int ms = 0; ms < 1000 && !cfg_req_seen; ms += 10) {
		assert_wait_timeout((event_t)&cfg_req_seen, THREAD_UNINT, 10, NSEC_PER_MSEC);
		if (cfg_req_seen) {
			clear_wait(current_thread(), THREAD_AWAKENED);
		} else {
			thread_block(THREAD_CONTINUE_NULL);
		}
	}
	if (!cfg_req_seen) {
		gic_v3_spi_detach(IRQ_CFG_REQ);
		wifi_said("step 2: no CFG_REQ in a second: PMU STATUS %x DEBUG %x CTRL_NS %x CTRL_S %x", rd(pmu + CENTRAL_SEQ_WLBT_STATUS),
		    rd(pmu + WLBT_DEBUG), rd(pmu + WLBT_CTRL_NS), rd(pmu + WLBT_CTRL_S));
		return;
	}

	/* the answer, as platform_cfg_req_isr */
	delay(100);
	int64_t tzpc = smc(SMC_CMD_CONN_IF, 1ull << 32 | 0 /* EXYNOS_SET_CONN_TZPC */, 0, 0);
	vm_offset_t remap = ml_io_map(WLBT_REMAP_PHYS, 0x1000), dbus = ml_io_map(DBUS_BAAW_PHYS, 0x1000);
	vm_offset_t pbus = ml_io_map(PBUS_BAAW_PHYS, 0x1000), boot = ml_io_map(BOOT_CFG_PHYS, 0x2000);
	wr(remap + 0x0, 0x80000000u >> 12);
	uint32_t id = rd(remap + 0x10);
	wr(remap + 0x10, (id & ~CHIP_VERSION_EMA_MASK) | CHIP_VERSION_EMA_VALUE);
	wr(dbus + 0x0, 0x80000000u >> 12);
	wr(dbus + 0x4, 0x81400000u >> 12);
	wr(dbus + 0x8, (uint32_t)(wlbt_memory_pa >> 12));
	wr(dbus + 0xc, BAAW_ACCESS);
	static const uint32_t pbus_windows[6][3] = {   /* the core's address range, onto: */
		{ 0xa0000000, 0xa0010000, 0x11950000 },     /* the CP-to-WLBT mailbox */
		{ 0xa0010000, 0xa0060000, 0x119a0000 },     /* the SHUB-to-WLBT mailbox */
		{ 0xa0060000, 0xa0100000, 0x11d00000 },     /* USI_CMG00 */
		{ 0xa0100000, 0xa0110000, 0x11c80000 },     /* SYSREG_CMGP2WLBT */
		{ 0xa0110000, 0xa0120000, 0x11c20000 },     /* GPIO_CMGP */
		{ 0xa0120000, 0xa0160000, 0x11200000 },     /* SHUB */
	};
	for (uint32_t w = 0; w < 6; w++) {
		wr(pbus + w * 0x10 + 0x0, pbus_windows[w][0] >> 12);
		wr(pbus + w * 0x10 + 0x4, pbus_windows[w][1] >> 12);
		wr(pbus + w * 0x10 + 0x8, pbus_windows[w][2] >> 12);
		wr(pbus + w * 0x10 + 0xc, BAAW_ACCESS);
	}
	wr(boot + 0x0, 1);
	for (uint64_t off = 0; off < patch_size; off += 4) {
		wr(boot + 0x1000 + off, le32(patch + off));
	}
	wr(boot + 0x0, 0);
	wr(boot + 0x4, 1);                      /* BOOT_CFG_ACK: the firmware may run from here */
	delay(100);
	wr(pmu + WLBT_CTRL_NS, rd(pmu + WLBT_CTRL_NS) & ~MASK_PWR_REQ);
	gic_v3_spi_detach(IRQ_CFG_REQ);
	printf("a51wifi: CFG_REQ answered: TZPC SMC %lld, CHIP_VERSION_ID %x -> %x, dbus window onto %llx, %llu bytes of patch\n",
	    tzpc, id, rd(remap + 0x10), wlbt_memory_pa, patch_size);

	/* two seconds of watching: what the core does with no configuration to read */
	uint32_t st[3];
	for (int i = 0; i < 3; i++) {
		IOSleep(i ? 1000 : 10);
		st[i] = rd(pmu + CENTRAL_SEQ_WLBT_STATUS);
		printf("a51wifi: after %d ms: PMU STATUS %x CTRL_NS %x DEBUG %x; mailbox INTSR0 %x INTSR1 %x ISSR0..3 %x %x %x %x\n",
		    i ? i * 1000 : 10, st[i], rd(pmu + WLBT_CTRL_NS), rd(pmu + WLBT_DEBUG), rd(mbox + MBOX_INTSR0),
		    rd(mbox + MBOX_INTSR1), rd(mbox + MBOX_ISSR(0)), rd(mbox + MBOX_ISSR(1)), rd(mbox + MBOX_ISSR(2)),
		    rd(mbox + MBOX_ISSR(3)));
	}
	wifi_said("step 2: CFG_REQ came and was answered (TZPC SMC %lld); after 2 s: PMU STATUS %x, mailbox INTSR1 %x, ISSR0..3 "
	    "%x %x %x %x", tzpc, st[2], rd(mbox + MBOX_INTSR1), rd(mbox + MBOX_ISSR(0)), rd(mbox + MBOX_ISSR(1)),
	    rd(mbox + MBOX_ISSR(2)), rd(mbox + MBOX_ISSR(3)));
	if (wifi_steps < 3) {
		return;
	}

	/* step 3: the firmware's first message, in the to-AP ring (it may well have come during the two seconds) */
	vm_offset_t base = wlbt_memory + mx_heap;
	uint32_t widx = 0, ridx = 0;
	for (int ms = 0; ms <= 3000; ms += 10) {
		cache_out(base + MX_TOAP_RIDX, 8);
		ridx = rd(base + MX_TOAP_RIDX);
		widx = rd(base + MX_TOAP_WIDX);
		if (widx != ridx) {
			break;
		}
		IOSleep(10);
	}
	if (widx == ridx) {
		uint32_t panic_offset = le32(fw + 176);
		char words[200] = "";
		if (panic_offset != 0 && panic_offset + 64 < WLBT_MEMORY) {
			cache_out(wlbt_memory + panic_offset, 64);
			uint32_t *p = (uint32_t *)(wlbt_memory + panic_offset);
			snprintf(words, sizeof(words), "%x %x %x %x %x %x %x %x", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
		}
		wifi_said("step 3: no message from the firmware (to-AP ring indices %u/%u, mailbox INTSR0 %x ISSR0 %x); its panic "
		    "record at %x: %s", ridx, widx, rd(mbox + MBOX_INTSR0), rd(mbox + MBOX_ISSR(0)), panic_offset, words);
		return;
	}
	char got[160] = "";
	uint32_t at = ridx % MX_PACKETS;
	cache_out(base + MX_TOAP_RING, MX_PACKETS * MX_PACKET_SIZE);
	const uint8_t *packet = (const uint8_t *)(base + MX_TOAP_RING + at * MX_PACKET_SIZE);
	uint32_t arg = (uint32_t)packet[2] | (uint32_t)packet[3] << 8 | (uint32_t)packet[4] << 16 | (uint32_t)packet[5] << 24;
	snprintf(got, sizeof(got), "channel %u, message %u, arg %x (raw %02x %02x %02x %02x %02x %02x %02x %02x)", packet[0],
	    packet[1], arg, packet[0], packet[1], packet[2], packet[3], packet[4], packet[5], packet[6], packet[7]);
	/* taken: the read index moved on, the to-host bit cleared */
	*(volatile uint32_t *)(base + MX_TOAP_RIDX) = (ridx + 1) % MX_PACKETS;
	cache_out(base + MX_TOAP_RIDX, 4);
	wr(mbox + 0x00c /* INTCR0 */, 0xffff0000);
	bool start_ind = packet[0] == 0 && packet[1] == 0;
	wifi_said("step 3: %s from the firmware: %s; ring indices were %u/%u", start_ind ? "MM_START_IND" : "a message", got, ridx,
	    widx);
}

/* Step 1's second half, once the small image is there: the firmware read and checked, the core's memory taken. */
static void
wifi_thread(__unused void *parameter, __unused wait_result_t result)
{
	const uint8_t *fw = NULL;
	uint64_t size = 0;
	char build[96] = "";

	for (int t = 0; t < 240 && (fw = hakios_image_file("fw", FW_NAME, &size, NULL)) == NULL; t++) {
		IOSleep(500);
	}
	if (fw == NULL || size < 256) {
		wifi_said("step 1: fw/%s not found in the small image (userland/build.sh puts ../fw-wifi-a51 there)", FW_NAME);
	} else {
		/* fwhdr.c, version 2: magic at 8, version minor/major at 12/14, header length 16, API version 20/22, firmware
		 * CRC 24, constant length 28, constant CRC 32, runtime length 36, entry point 40, build id 48 */
		uint32_t hdr_length = le32(fw + 16), const_length = le32(fw + 28), runtime = le32(fw + 36);
		uint32_t entry = le32(fw + 40), header_crc = hdr_length >= 4 && hdr_length <= size ? le32(fw + hdr_length - 4) : 0;
		bool magic = fw[8] == 's' && fw[9] == 'm' && fw[10] == 'x' && fw[11] == 'f';

		for (uint32_t i = 0; i + 1 < sizeof(build) && 48 + i < hdr_length && fw[48 + i] >= ' ' && fw[48 + i] < 127; i++) {
			build[i] = (char)fw[48 + i];
			build[i + 1] = 0;
		}
		bool header_ok = header_crc != 0 && hdr_length <= size && ether_crc(fw, hdr_length - 4) == header_crc;
		/* the constant part may be empty (this firmware's: its length is the header's, its CRC that of nothing,
		 * 0xffffffff); the whole firmware after the header has a CRC of its own (fwimage_check_fw_crc) */
		bool const_ok = const_length >= hdr_length && const_length <= size &&
		    ether_crc(fw + hdr_length, const_length - hdr_length) == le32(fw + 32);
		bool fw_ok = hdr_length <= size && ether_crc(fw + hdr_length, (uint32_t)size - hdr_length) == le32(fw + 24);
		printf("a51wifi: firmware %s: %llu bytes, magic %s, header v%u.%u, API v%u.%u, header %u bytes, constant part %u, "
		    "runtime %u, entry point %x; build \"%s\"; header CRC %s, constant CRC %s, firmware CRC %s\n", FW_NAME, size,
		    magic ? "smxf" : "WRONG",
		    fw[14] | fw[15] << 8, fw[12] | fw[13] << 8, fw[22] | fw[23] << 8, fw[20] | fw[21] << 8, hdr_length, const_length,
		    runtime, entry, build, header_ok ? "ok" : "WRONG", const_ok ? "ok" : "WRONG", fw_ok ? "ok" : "WRONG");

		/* the core's 16 MB, in one piece (the bus window points at its start) */
		kern_return_t kr = kmem_alloc_contig(kernel_map, &wlbt_memory, WLBT_MEMORY, 0, 0, 0,
		    KMA_KOBJECT | KMA_ZERO, VM_KERN_MEMORY_IOKIT);
		if (kr == KERN_SUCCESS) {
			wlbt_memory_pa = kvtophys(wlbt_memory);
		}
		wifi_said("step 1: firmware %s, %llu bytes, entry %x, runtime %u, CRCs header %s, constant %s, firmware %s; 16 MB for "
		    "the core %s at %llx; PMU WLBT_CTRL_NS %x CTRL_S %x STATUS %x", magic ? "smxf" : "NOT smxf", size, entry, runtime,
		    header_ok ? "ok" : "WRONG", const_ok ? "ok" : "WRONG", fw_ok ? "ok" : "WRONG", kr == KERN_SUCCESS ? "taken" : "NOT TAKEN",
		    wlbt_memory_pa,
		    rd(ml_io_map(PMU_PHYS, 0x1000) + WLBT_CTRL_NS), rd(ml_io_map(PMU_PHYS, 0x1000) + WLBT_CTRL_S),
		    rd(ml_io_map(PMU_PHYS, 0x1000) + CENTRAL_SEQ_WLBT_STATUS));
	}
	if (wifi_steps >= 2 && fw != NULL && wlbt_memory != 0) {
		wifi_step2(fw, size);
	}
	for (int i = 0; i < 10; i++) {          /* said again, for the log that keeps only the end */
		IOSleep(30000);
		printf("a51wifi: %s\n", wifi_state);
	}
}

static thread_call_t wifi_call;

static void
wifi_start(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	thread_t thread;

	if (kernel_thread_start(wifi_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

static void
a51wifi_init(void)
{
	vm_offset_t pmu, mbox;
	uint64_t deadline;

	if (!PE_parse_boot_argn("a51wifi", &wifi_steps, sizeof(wifi_steps)) || wifi_steps == 0) {
		return;                         /* nothing is touched unless a boot-argument asks */
	}
	printf("a51wifi: step 1: the PMU's WLBT registers and the AP-WLBT mailbox, read\n");
	pmu = ml_io_map(PMU_PHYS, 0x1000);
	mbox = ml_io_map(MBOX_PHYS, 0x1000);
	if (!pmu || !mbox) {
		printf("a51wifi: cannot map the PMU or the mailbox\n");
		return;
	}
	uint32_t status = rd(pmu + CENTRAL_SEQ_WLBT_STATUS);
	printf("a51wifi: PMU: WLBT_CTRL_NS %x, WLBT_CTRL_S %x, WLBT_STAT %x, WLBT_DEBUG %x, CENTRAL_SEQ_WLBT_CONFIGURATION %x, "
	    "STATUS %x (sequencer state %x)\n", rd(pmu + WLBT_CTRL_NS), rd(pmu + WLBT_CTRL_S), rd(pmu + WLBT_STAT),
	    rd(pmu + WLBT_DEBUG), rd(pmu + CENTRAL_SEQ_WLBT_CONFIGURATION), status, (status >> 16) & 0xff);
	printf("a51wifi: mailbox: INTMR0 %x, INTSR0 %x, ISSR0..7 %x %x %x %x %x %x %x %x\n", rd(mbox + MBOX_INTMR0),
	    rd(mbox + MBOX_INTSR0), rd(mbox + MBOX_ISSR(0)), rd(mbox + MBOX_ISSR(1)), rd(mbox + MBOX_ISSR(2)),
	    rd(mbox + MBOX_ISSR(3)), rd(mbox + MBOX_ISSR(4)), rd(mbox + MBOX_ISSR(5)), rd(mbox + MBOX_ISSR(6)),
	    rd(mbox + MBOX_ISSR(7)));
	wifi_said("step 1: PMU WLBT_CTRL_NS %x CTRL_S %x STATUS %x; waiting for the small image for the firmware",
	    rd(pmu + WLBT_CTRL_NS), rd(pmu + WLBT_CTRL_S), status);

	/* the firmware is in the small image, which comes later: a thread, one second from now (no threads this early) */
	wifi_call = thread_call_allocate(wifi_start, NULL);
	clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(wifi_call, deadline);
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, a51wifi_init);
