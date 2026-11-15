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
 *   4  after MM_START_IND: the WLAN service's HIP4 region (drivers/net/wireless/scsc/hip4.h: 8 KB of control --
 *              struct hip4_hip_control: init, config versions 5 and 4, scoreboard, six queues -- then 32 KB of MIB,
 *              the two .hcf files one after the other, 1 MB + 64 KB of transmit and 1 MB of receive space) laid out
 *              after step 3's, SM_MSG_START_REQ for service 1 (WLAN) sent on management channel 1 with the region's
 *              offset, and the firmware's SM_MSG_START_CFM waited for; then what it filled into the config read
 *
 *   5  after SM_MSG_START_CFM: a first MLME request over HIP4, MLME-ADD-VIF.req (interface 1, a station, a locally
 *              administered address), in an mbulk of the control pool put on the from-host control queue (scoreboard
 *              byte 0, from-host bit 5); every to-host control signal of the next 3 s logged and handed back (the to-host
 *              RFB queue), up to MLME-ADD-VIF.cfm
 *
 * What the steps found is said again every 30 s for five minutes: a whole system's start pushes the first lines out of
 * the kernel log.
 *
 * Status: steps 1 to 5 run on the phone (2026-11-15): the firmware starts (MM_START_IND) and so does the WLAN service
 * (SM_MSG_START_CFM, SAP MLME 14.7 / MA 14.1, as on Android); HIP4 signalling works both ways (MLME-ADD-VIF.req answered
 * by MLME-ADD-VIF.cfm, SUCCESS). Not yet: scan, connect, packets.
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

/*
 * Step 4's region, hip4.h on the A51 (no PCIe, no RX NAPI, SMAPPER built in): control 0x0000 (8 KB), MIB 0x2000 (32 KB),
 * transmit 0xa000 (1 MB of data blocks, 64 KB of control blocks), receive 0x11a000 (1 MB): 0x21a000 in all. Inside the
 * control area (struct hip4_hip_control): init at 0 (magic 0xcaaa0400, the version the firmware takes at 4, then the
 * locations of config A = version 4 and B = version 5), config v5 at 32, config v4 at 160 (227 bytes, packed), the
 * scoreboard at 448 (256 words), the queues at 1472 + n * 1088 (256 words, then read / write / total bytes, 64-aligned).
 * The interrupt bits are the next Samsung's allocator would give after the seven streams of step 3: to-host 7 (one
 * for every queue, without RX NAPI), from-host 5.
 */
#define HIP_CONTROL             0x000000
#define HIP_MIB                 0x002000
#define HIP_MIB_SIZE            0x008000
#define HIP_TX                  0x00a000
#define HIP_TX_SIZE             0x110000
#define HIP_RX                  0x11a000
#define HIP_RX_SIZE             0x100000
#define HIP_TOTAL               0x21a000
#define HIP_V5                  32
#define HIP_V4                  160
#define HIP_SCOREBOARD          448
#define HIP_QUEUE(n)            (1472 + (n) * 1088)
#define HIP_TOHOST_BIT          7
#define HIP_FROMHOST_BIT        5

static inline void
w8(vm_offset_t a, uint8_t v)
{
	*(volatile uint8_t *)a = v;
}

static inline void
w16(vm_offset_t a, uint16_t v)
{
	*(volatile uint16_t *)a = v;
}

static inline void
w32(vm_offset_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
}

static void cache_out(vm_offset_t start, vm_size_t size);
static uint32_t runtime_panic_offset;          /* the firmware's R4 panic record, from its header (step 3) */
static char wifi_dump[7000];                    /* hex of the panic record and the log ring, said again with the status */

/* Words as hex lines into wifi_dump (16 a line, each line tagged), for the log that keeps only its end. */
static void
dump_words(const char *tag, const uint32_t *w, uint32_t count)
{
	for (uint32_t i = 0; i < count; i += 16) {
		size_t n = strlen(wifi_dump);

		if (n + 200 > sizeof(wifi_dump)) {
			return;
		}
		n += snprintf(wifi_dump + n, sizeof(wifi_dump) - n, "a51wifi: dump %s %03x:", tag, i * 4);
		for (uint32_t j = i; j < i + 16 && j < count; j++) {
			n += snprintf(wifi_dump + n, sizeof(wifi_dump) - n, " %08x", w[j]);
		}
		snprintf(wifi_dump + n, sizeof(wifi_dump) - n, "\n");
	}
}

/* The host's half of a config (the same for both versions up to log_config_sz, at 64 bytes in). */
static void
hip_config_common(vm_offset_t c, uint32_t hip_ref, uint16_t version, uint16_t length, uint32_t mib_size)
{
	w32(c + 0, 0xcaba0401);
	w16(c + 4, version);
	w16(c + 6, length);
	w8(c + 31, 64);                         /* host_cache_line */
	w32(c + 32, hip_ref + HIP_TX);          /* host_buf_loc */
	w32(c + 36, HIP_TX_SIZE);
	w32(c + 40, hip_ref + HIP_RX);          /* fw_buf_loc */
	w32(c + 44, HIP_RX_SIZE);
	w32(c + 48, mib_size ? hip_ref + HIP_MIB : 0);
	w32(c + 52, mib_size);
	w32(c + 56, 0);                         /* log_config_loc */
	w32(c + 60, 0);
}

/*
 * What of an .hcf file the firmware is given, as Samsung's slsi_mib_open_file and slsi_mib_get_platform cut it: the
 * 8-byte header off (its last byte 1: the first file format), then, when the payload starts with key 0xfffe (version
 * 1) and key 0xffff (the platform name, a vldata string: a 0xa0-type header octet whose low bits give the size of a
 * length field after it), those keys off as well, the string padded to 16 bits. On this phone: 60 bytes off
 * mx140_wlan.hcf (4326 -> 4266), 80 off mx140_wlan_sw.hcf (284 -> 204), as Android's log says it loads them.
 */
static const uint8_t *
hcf_payload(const uint8_t *data, uint64_t length, uint32_t *out_length)
{
	if (data == NULL || length < 8 || data[7] != 1) {
		*out_length = 0;
		return NULL;
	}
	data += 8;
	length -= 8;
	if (length >= 12 && data[0] == 0xfe && data[1] == 0xff && data[2] == 0x01 && data[3] == 0x00 && data[4] == 0x01 &&
	    data[5] == 0x00 && data[6] == 0xff && data[7] == 0xff) {
		uint32_t pos = 8, name = data[pos] | (uint32_t)data[pos + 1] << 16;

		pos += 2;
		if (name >= 2 && pos + name <= length && (data[pos] & 0xa0) && (data[pos] & 0x17) <= 2) {
			uint32_t field = data[pos] & 0x17;

			pos += 1;
			name -= 1;
			pos += field;
			name -= field;
			name += name & 1;
			pos += name;
			if (pos <= length) {
				data += pos;
				length -= pos;
			}
		}
	}
	*out_length = (uint32_t)length;
	return data;
}

/*
 * Step 5's signalling, as Samsung's hip4.c and mbulk.c do it. An mbulk is a 20-byte descriptor (packed; its class is a
 * packed enum, one byte): next offset 0, flag 4 (1: a signal follows), class 5, pool id 6, refcount 7, data size 8,
 * signal size 10, length 12, head 14, chain offset 16; the signal buffer right after it, the FAPI signal 4 bytes into
 * it. (A 23-byte layout, the class taken as 4 bytes, made the firmware panic in mif_q.c:378, HOSTIO_GENERIC.) The scoreboard keeps each queue's indices as single bytes: the host's at
 * 0..5 (FH_CTRL write, FH_DAT write, FH_RFB read, TH_CTRL read, TH_DAT read, TH_RFB write), the firmware's at 64..69.
 */
#define MBULK_DESC              20
#define MBULK_POOL_CTRL         1
#define MBULK_CLASS_FROM_HOST_CTL 5
#define HIP_TX_CTL              0x10a000        /* the control pool: 32 blocks of 2 KB */
#define Q_FH_CTRL               0
#define Q_TH_CTRL               3
#define Q_TH_RFB                5
#define MLME_ADD_VIF_REQ        0x2007
#define MLME_ADD_VIF_CFM        0x2107
#define FAPI_SENDER             0xc001          /* SLSI_TX_PROCESS_ID_MIN */

static uint32_t hip_base;                       /* step 4's region, as an offset in the 16 MB */

/* Put a FAPI signal (no payload) into control block `block` and onto the from-host control queue. */
static void
hip_send_signal(vm_offset_t mbox, uint32_t block, const uint8_t *signal, uint32_t length)
{
	vm_offset_t h = wlbt_memory + hip_base, m = h + HIP_TX_CTL + block * 2048;
	uint32_t sig_bufsz = ((MBULK_DESC + length + 4 + 3) & ~3u) - MBULK_DESC;

	bzero((void *)m, 2048);
	w8(m + 4, 1 | 4);                               /* MBULK_F_SIG | MBULK_F_OBOUND (the firmware checks it: mif_q.c) */
	w8(m + 5, MBULK_CLASS_FROM_HOST_CTL);
	w8(m + 6, MBULK_POOL_CTRL);
	w16(m + 10, (uint16_t)sig_bufsz);
	w16(m + 14, (uint16_t)sig_bufsz);               /* head: no data after the signal */
	memcpy((void *)(m + MBULK_DESC + 4), signal, length);
	cache_out(m, 2048);

	cache_out(h + HIP_SCOREBOARD, 128);
	uint8_t w = *(volatile uint8_t *)(h + HIP_SCOREBOARD + 0);
	w32(h + HIP_QUEUE(Q_FH_CTRL) + 4 * w, hip_base + HIP_TX_CTL + block * 2048);
	cache_out(h + HIP_QUEUE(Q_FH_CTRL) + 4 * w, 4);
	w8(h + HIP_SCOREBOARD + 0, (uint8_t)(w + 1));
	cache_out(h + HIP_SCOREBOARD, 4);
	wr(mbox + MBOX_INTGR1, 1u << HIP_FROMHOST_BIT);
}

/* Step 5: MLME-ADD-VIF.req, and what comes back. */
static void
wifi_step5(vm_offset_t mbox)
{
	vm_offset_t h = wlbt_memory + hip_base;
	uint8_t req[38];
	static const uint8_t address[6] = { 0x02, 0xa5, 0x1c, 0x00, 0x00, 0x01 };       /* locally administered */
	char seen[200] = "";
	bool cfm = false;
	uint16_t result = 0xffff, vif = 0;

	vm_offset_t mx = wlbt_memory + mx_heap;

	/* the firmware's log stream (read index 0x9000, write 0x9004, ring at 0x5000): never read so far; caught up here */
	cache_out(mx + 0x9000, 8);
	uint32_t log_r = rd(mx + 0x9000), log_w = rd(mx + 0x9004);
	w32(mx + 0x9000, log_w);
	cache_out(mx + 0x9000, 8);
	printf("a51wifi: step 5: MLME-ADD-VIF.req over HIP4 (log stream was %u/%u, caught up)\n", log_r, log_w);
	bzero(req, sizeof(req));
	req[0] = MLME_ADD_VIF_REQ & 0xff;
	req[1] = MLME_ADD_VIF_REQ >> 8;
	req[4] = FAPI_SENDER & 0xff;                     /* receiver PID 0 at 2, sender at 4, firmware reference 0 at 6 */
	req[5] = FAPI_SENDER >> 8;
	req[10] = 1;                                     /* vif 1 */
	memcpy(req + 12, address, 6);                    /* interface address */
	req[18] = 2;                                     /* FAPI_VIFTYPE_STATION */
	memcpy(req + 20, address, 6);                    /* device address */
	hip_send_signal(mbox, 0, req, sizeof(req));

	/* the to-host control queue, for 3 s: every signal logged and its block handed back */
	for (int ms = 0; ms <= 3000 && !cfm; ms += 10) {
		cache_out(h + HIP_SCOREBOARD, 128);
		uint8_t r = *(volatile uint8_t *)(h + HIP_SCOREBOARD + Q_TH_CTRL);
		uint8_t fw_w = *(volatile uint8_t *)(h + HIP_SCOREBOARD + 64 + Q_TH_CTRL);

		while (r != fw_w) {
			cache_out(h + HIP_QUEUE(Q_TH_CTRL) + 4 * r, 4);
			uint32_t ref = rd(h + HIP_QUEUE(Q_TH_CTRL) + 4 * r);
			if (ref >= WLBT_MEMORY - 256) {
				break;
			}
			vm_offset_t m = wlbt_memory + ref;
			cache_out(m, 256);
			const uint8_t *sig = (const uint8_t *)(m + MBULK_DESC + 4);
			uint16_t id = sig[0] | sig[1] << 8;
			size_t n = strlen(seen);
			snprintf(seen + n, sizeof(seen) - n, "%04x ", id);
			printf("a51wifi: to host: signal %04x (receiver %04x, sender %04x), %02x %02x %02x %02x %02x %02x %02x %02x\n", id,
			    sig[2] | sig[3] << 8, sig[4] | sig[5] << 8, sig[10], sig[11], sig[12], sig[13], sig[14], sig[15], sig[16],
			    sig[17]);
			if (id == MLME_ADD_VIF_CFM) {
				vif = sig[10] | sig[11] << 8;
				result = sig[12] | sig[13] << 8;
				cfm = true;
			}
			/* handed back: onto the to-host RFB queue */
			uint8_t rw = *(volatile uint8_t *)(h + HIP_SCOREBOARD + Q_TH_RFB);
			w32(h + HIP_QUEUE(Q_TH_RFB) + 4 * rw, ref);
			cache_out(h + HIP_QUEUE(Q_TH_RFB) + 4 * rw, 4);
			w8(h + HIP_SCOREBOARD + Q_TH_RFB, (uint8_t)(rw + 1));
			r++;
			w8(h + HIP_SCOREBOARD + Q_TH_CTRL, r);
			cache_out(h + HIP_SCOREBOARD, 8);
			wr(mbox + MBOX_INTGR1, 1u << HIP_FROMHOST_BIT);
		}
		if (!cfm) {
			IOSleep(10);
		}
	}
	cache_out(h + HIP_SCOREBOARD, 128);
	if (cfm) {
		wifi_said("step 5: MLME-ADD-VIF.cfm: vif %u, result %#x (%s); signals seen: %s", vif, result,
		    result == 0 ? "SUCCESS" : "an error", seen);
	} else {
		/* what the firmware did: its panic record, the to-AP ring, the newest 512 words of its log */
		char slots[160] = "";
		wifi_dump[0] = 0;
		if (runtime_panic_offset != 0 && runtime_panic_offset + 256 < WLBT_MEMORY) {
			cache_out(wlbt_memory + runtime_panic_offset, 256);
			dump_words("panic", (const uint32_t *)(wlbt_memory + runtime_panic_offset), 32);
		}
		cache_out(mx + MX_TOAP_RIDX, 8);
		cache_out(mx + MX_TOAP_RING, MX_PACKETS * MX_PACKET_SIZE);
		for (int i = 0; i < MX_PACKETS; i++) {
			const uint8_t *sl = (const uint8_t *)(mx + MX_TOAP_RING + i * MX_PACKET_SIZE);
			size_t n = strlen(slots);
			snprintf(slots + n, sizeof(slots) - n, "[%02x %02x %02x %02x %02x %02x] ", sl[0], sl[1], sl[2], sl[3], sl[4], sl[5]);
		}
		cache_out(mx + 0x9000, 8);
		cache_out(mx + 0x5000, 0x4000);
		uint32_t now_w = rd(mx + 0x9004), from = now_w > 512 ? now_w - 512 : 0;
		if (now_w <= 0x1000) {
			dump_words("log", (const uint32_t *)(mx + 0x5000 + 4 * from), now_w - from);
		}
		printf("%s", wifi_dump);
		printf("a51wifi: step 5 detail: to-AP ring %u/%u slots %s; log stream now %u/%u (dump from word %u)\n",
		    rd(mx + MX_TOAP_RIDX), rd(mx + MX_TOAP_WIDX), slots, rd(mx + 0x9000), now_w, from);
		wifi_said("step 5: no MLME-ADD-VIF.cfm in 3 s; signals seen: %s; scoreboard host %02x %02x %02x %02x %02x %02x, firmware "
		    "%02x %02x %02x %02x %02x %02x; mailbox ISSR0 %x INTSR0 %x", seen[0] ? seen : "none",
		    *(volatile uint8_t *)(h + HIP_SCOREBOARD + 0), *(volatile uint8_t *)(h + HIP_SCOREBOARD + 1),
		    *(volatile uint8_t *)(h + HIP_SCOREBOARD + 2), *(volatile uint8_t *)(h + HIP_SCOREBOARD + 3),
		    *(volatile uint8_t *)(h + HIP_SCOREBOARD + 4), *(volatile uint8_t *)(h + HIP_SCOREBOARD + 5),
		    *(volatile uint8_t *)(h + HIP_SCOREBOARD + 64), *(volatile uint8_t *)(h + HIP_SCOREBOARD + 65),
		    *(volatile uint8_t *)(h + HIP_SCOREBOARD + 66), *(volatile uint8_t *)(h + HIP_SCOREBOARD + 67),
		    *(volatile uint8_t *)(h + HIP_SCOREBOARD + 68), *(volatile uint8_t *)(h + HIP_SCOREBOARD + 69),
		    rd(mbox + MBOX_ISSR(0)), rd(mbox + MBOX_INTSR0));
	}
}

/* Step 4: the region built, the WLAN service asked to start. false: no answer. */
static bool
wifi_step4(vm_offset_t mbox, uint32_t heap_end)
{
	uint32_t hip_ref = (heap_end + 0xfff) & ~0xfffu;

	hip_base = hip_ref;
	vm_offset_t h = wlbt_memory + hip_ref, mx = wlbt_memory + mx_heap;
	const uint8_t *mib[2];
	uint64_t mib_len[2] = { 0, 0 };
	uint32_t mib_size = 0;

	printf("a51wifi: step 4: the WLAN service's HIP4 region at %x (%u bytes), SM_MSG_START_REQ\n", hip_ref, HIP_TOTAL);
	if (hip_ref + HIP_TOTAL > (8u << 20)) {
		wifi_said("step 4: the HIP4 region would end past the first 8 MB (%x): stopping", hip_ref + HIP_TOTAL);
		return false;
	}
	mib[0] = hakios_image_file("fw", "mx140_wlan.hcf", &mib_len[0], NULL);
	mib[1] = hakios_image_file("fw", "mx140_wlan_sw.hcf", &mib_len[1], NULL);
	bzero((void *)h, HIP_MIB + HIP_MIB_SIZE);
	uint32_t part[2] = { 0, 0 };
	for (int i = 0; i < 2; i++) {
		const uint8_t *payload = hcf_payload(mib[i], mib_len[i], &part[i]);

		if (payload != NULL && mib_size + part[i] <= HIP_MIB_SIZE) {
			memcpy((void *)(h + HIP_MIB + mib_size), payload, part[i]);
			mib_size += part[i];
		}
	}
	printf("a51wifi: MIB: %u + %u bytes of the two .hcf files (of %llu and %llu)\n", part[0], part[1], mib_len[0], mib_len[1]);

	/* config v5 (B) and v4 (A), then init pointing at both */
	vm_offset_t v5 = h + HIP_V5, v4 = h + HIP_V4;
	hip_config_common(v5, hip_ref, 5, 120, mib_size);
	w8(v5 + 64, HIP_FROMHOST_BIT);
	w8(v5 + 65, HIP_TOHOST_BIT);
	w32(v5 + 68, hip_ref + HIP_SCOREBOARD);
	w16(v5 + 72, 6);
	w16(v5 + 74, 256);
	w16(v5 + 76, 1);
	hip_config_common(v4, hip_ref, 4, 227, mib_size);
	w8(v4 + 64, HIP_FROMHOST_BIT);
	for (int q = 0; q < 6; q++) {
		w8(v4 + 68 + q, HIP_TOHOST_BIT);
	}
	w32(v4 + 76, hip_ref + HIP_SCOREBOARD);
	w16(v4 + 80, 6);
	w16(v4 + 82, 256);
	w16(v4 + 84, 1);
	for (int q = 0; q < 6; q++) {
		w32(v5 + 80 + 4 * q, hip_ref + HIP_QUEUE(q));
		w32(v4 + 88 + 4 * q, hip_ref + HIP_QUEUE(q));
	}
	w32(h + 0, 0xcaaa0400);
	w32(h + 8, hip_ref + HIP_V4);           /* version_a_ref */
	w32(h + 12, hip_ref + HIP_V5);          /* version_b_ref */
	cache_out(h, HIP_MIB + HIP_MIB_SIZE);

	/* SM_MSG_START_REQ: channel 1 (service management), service 1 (WLAN), message 0, the region's offset */
	cache_out(mx + MX_FROMAP_RIDX, 8);
	uint32_t fw_slot = rd(mx + MX_FROMAP_WIDX) % MX_PACKETS;
	vm_offset_t packet = mx + MX_FROMAP_RING + fw_slot * MX_PACKET_SIZE;
	bzero((void *)packet, MX_PACKET_SIZE);
	w8(packet + 0, 1);
	w8(packet + 1, 1);
	w8(packet + 2, 0);
	w8(packet + 3, (uint8_t)hip_ref);
	w8(packet + 4, (uint8_t)(hip_ref >> 8));
	w8(packet + 5, (uint8_t)(hip_ref >> 16));
	w8(packet + 6, (uint8_t)(hip_ref >> 24));
	cache_out(packet, MX_PACKET_SIZE);
	w32(mx + MX_FROMAP_WIDX, (fw_slot + 1) % MX_PACKETS);
	cache_out(mx + MX_FROMAP_WIDX, 4);
	wr(mbox + MBOX_INTGR1, 1u << 2);        /* the from-AP management stream's write bit */

	/* the answer: SM_MSG_START_CFM in the to-AP ring */
	uint32_t ridx = 0, widx = 0;
	for (int ms = 0; ms <= 10000; ms += 10) {     /* Samsung waits 3 s; longer here, to see whether it comes late */
		cache_out(mx + MX_TOAP_RIDX, 8);
		ridx = rd(mx + MX_TOAP_RIDX);
		widx = rd(mx + MX_TOAP_WIDX);
		if (widx != ridx) {
			break;
		}
		IOSleep(10);
	}
	cache_out(h, HIP_V4 + 227);
	uint32_t taken = rd(h + 4);
	if (widx == ridx) {
		/* what there is to see: the panic record, every to-AP slot, what the firmware filled in, its log stream */
		char panic[120] = "", slots[160] = "";
		uint32_t panic_offset = runtime_panic_offset;
		if (panic_offset != 0 && panic_offset + 32 < WLBT_MEMORY) {
			cache_out(wlbt_memory + panic_offset, 32);
			uint32_t *pr = (uint32_t *)(wlbt_memory + panic_offset);
			snprintf(panic, sizeof(panic), "%x %x %x %x %x %x %x %x", pr[0], pr[1], pr[2], pr[3], pr[4], pr[5], pr[6], pr[7]);
		}
		cache_out(mx + MX_TOAP_RING, MX_PACKETS * MX_PACKET_SIZE);
		for (int i = 0; i < MX_PACKETS; i++) {
			const uint8_t *sl = (const uint8_t *)(mx + MX_TOAP_RING + i * MX_PACKET_SIZE);
			size_t n = strlen(slots);
			snprintf(slots + n, sizeof(slots) - n, "[%02x %02x %02x %02x %02x %02x] ", sl[0], sl[1], sl[2], sl[3], sl[4], sl[5]);
		}
		cache_out(mx + 0x5000, 64);
		cache_out(mx + 0x9000, 8);
		/* the whole panic record (its second word: its length in bytes) and the log ring, kept for saying again */
		wifi_dump[0] = 0;
		if (panic_offset != 0 && panic_offset + 256 < WLBT_MEMORY) {
			uint32_t *pr = (uint32_t *)(wlbt_memory + panic_offset);
			cache_out(wlbt_memory + panic_offset, 256);
			uint32_t words = pr[1] / 4;
			dump_words("panic", pr, words > 0 && words <= 64 ? words : 16);
		}
		cache_out(mx + 0x5000, 0x4000);
		uint32_t log_words = rd(mx + 0x9004);
		dump_words("log", (const uint32_t *)(mx + 0x5000), log_words <= 256 ? log_words : 256);
		printf("%s", wifi_dump);
		printf("a51wifi: step 4 detail: panic record %s; to-AP slots %s; v4 filled by the firmware: compat %x SAP %x %x %x %x "
		    "build %u patch %u; log stream indices %u/%u, its first words %x %x %x %x\n", panic, slots, rd(v4 + 8),
		    *(volatile uint16_t *)(v4 + 12), *(volatile uint16_t *)(v4 + 14), *(volatile uint16_t *)(v4 + 16),
		    *(volatile uint16_t *)(v4 + 18), rd(v4 + 20), rd(v4 + 24), rd(mx + 0x9000), rd(mx + 0x9004), rd(mx + 0x5000),
		    rd(mx + 0x5004), rd(mx + 0x5008), rd(mx + 0x500c));
		wifi_said("step 4: no answer to SM_MSG_START_REQ in 10 s (to-AP ring %u/%u, from-AP %u/%u, mailbox ISSR0 %x ISSR1 %x "
		    "INTSR0 %x); init version %x; v4 SAP MLME %x build %u; panic record %s; log %u/%u", ridx, widx,
		    rd(mx + MX_FROMAP_RIDX), rd(mx + MX_FROMAP_WIDX), rd(mbox + MBOX_ISSR(0)), rd(mbox + MBOX_ISSR(1)),
		    rd(mbox + MBOX_INTSR0), taken, *(volatile uint16_t *)(v4 + 12), rd(v4 + 20), panic, rd(mx + 0x9000), rd(mx + 0x9004));
		return false;
	}
	cache_out(mx + MX_TOAP_RING, MX_PACKETS * MX_PACKET_SIZE);
	const uint8_t *answer = (const uint8_t *)(mx + MX_TOAP_RING + (ridx % MX_PACKETS) * MX_PACKET_SIZE);
	w32(mx + MX_TOAP_RIDX, (ridx + 1) % MX_PACKETS);
	cache_out(mx + MX_TOAP_RIDX, 4);
	wr(mbox + 0x00c /* INTCR0 */, 0xffff0000);
	bool cfm = answer[0] == 1 && answer[1] == 1 && answer[2] == 1;
	vm_offset_t taken_config = taken == 5 ? v5 : v4;
	wifi_said("step 4: %s (channel %u, service %u, message %u); the firmware took config version %u: SAP MLME %x, MA %x, "
	    "debug %x, test %x, build %u, patch %u, headroom %u, tailroom %u; MIB %u bytes", cfm ? "SM_MSG_START_CFM, WLAN started" :
	    "an answer", answer[0], answer[1], answer[2], taken, *(volatile uint16_t *)(taken_config + 12),
	    *(volatile uint16_t *)(taken_config + 14), *(volatile uint16_t *)(taken_config + 16),
	    *(volatile uint16_t *)(taken_config + 18), rd(taken_config + 20), rd(taken_config + 24),
	    *(volatile uint8_t *)(taken_config + 28), *(volatile uint8_t *)(taken_config + 29), mib_size);
	return cfm;
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
	runtime_panic_offset = le32(fw + 176);
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
	if (start_ind && wifi_steps >= 4) {
		IOSleep(100);
		if (wifi_step4(mbox, mx_heap + MX_HEAP_SIZE) && wifi_steps >= 5) {
			char started[256];

			strlcpy(started, wifi_state, sizeof(started));
			IOSleep(100);
			wifi_step5(mbox);
			printf("a51wifi: (step 4 was: %s)\n", started);
		}
	}
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
		if (wifi_dump[0]) {
			printf("%s", wifi_dump);
		}
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
