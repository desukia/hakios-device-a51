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
 *   6  after MLME-ADD-VIF.cfm: a passive scan, MLME-ADD-SCAN.req (vif 0, scan id 0x100, full scan, real-time reports)
 *              with Samsung's scan-timing and channel-list elements (2.4 GHz 1-13, 5 GHz 36-64, 100-140, 149-165);
 *              MLME-SCAN.ind's beacons and probe responses (both to-host queues) collected until MLME-SCAN-DONE.ind
 *              or 20 s, the networks listed
 *   7  after the scan: WPA2-Personal to the network fw/wifi.conf names (ssid=..., psk=... or pmk=<64 hex>; never
 *              printed): MLME-CONNECT.req (SSID and our RSN element: CCMP, PSK, the AP's group cipher), MLME-CONNECT.ind,
 *              the 4-way handshake (hakios.a51wpa.c) over MA-UNITDATA.ind / MLME-SEND-FRAME.req, MLME-SETKEYS
 *              (pairwise, group) once message 4 is out, MLME-CONNECT-RES; then the data frames that come in, counted
 *   8  the radio as Darwin's Wi-Fi: en0 (link down) at once; after step 5 the station side for wifid and Settings,
 *              the queues looked at every 2 ms, data frames to and from en0; fw/wifi.conf joined as a standing join
 *
 * What the steps found is said again every 30 s for five minutes: a whole system's start pushes the first lines out of
 * the kernel log.
 *
 * Status: steps 1 to 5 run on the phone (2026-11-15): the firmware starts (MM_START_IND) and so does the WLAN service
 * (SM_MSG_START_CFM, SAP MLME 14.7 / MA 14.1, as on Android); HIP4 signalling works both ways (MLME-ADD-VIF.req answered
 * by MLME-ADD-VIF.cfm, SUCCESS); step 6's passive scan lists networks (MLME-SCAN.ind, chained blocks); step 7 joins a
 * WPA2-Personal network (handshake, both keys set) and decrypted data frames come in; step 8 (a51wifi=8) makes the radio
 * Darwin's Wi-Fi: en0, wifid's station side, DHCP and traffic both ways (iOS online, the status bar's Wi-Fi icon).
 * Not yet: an active scan, the 2.4 GHz side of the home router (NOT_ALLOWED), power saving.
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
#include <kern/locks.h>
#include <sys/errno.h>
#include <sys/hakios.net.h>
#include <sys/hakios.sta.h>
#include <sys/kpi_mbuf.h>
#include <sys/random.h>
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
#define MBOX_INTCR0             0x00c
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

/*
 * Put a FAPI signal into control block `block` and onto the from-host control queue. A payload goes after the signal
 * buffer, past the headroom the firmware asked for (its config's unidat_req_headroom/tailroom), as hip4_skb_to_mbulk.
 */
static uint8_t hip_headroom, hip_tailroom;

/*
 * A signal (and its payload) in the block at `offset` in the region, onto from-host queue `q`: the control pool's blocks
 * go on FH_CTRL (pool 1, class 5), the data pool's on FH_DAT (pool 0, class 4), as hip4_skb_to_mbulk.
 */
static bool
hip_put(vm_offset_t mbox, uint32_t offset, int q, uint8_t pool, uint8_t clas, const uint8_t *signal, uint32_t length,
    const uint8_t *payload, uint32_t payload_length)
{
	vm_offset_t h = wlbt_memory + hip_base, m = h + offset;
	uint32_t sig_bufsz = ((MBULK_DESC + length + 4 + 3) & ~3u) - MBULK_DESC;

	/* a queue holds 255 (its indices are bytes): full when the write index is one behind the firmware's read index
	 * (hip4_q_add_signal: -ENOSPC). 512 data blocks and no such check let the write index lap the read index */
	cache_out(h + HIP_SCOREBOARD, 128);
	if ((uint8_t)(*(volatile uint8_t *)(h + HIP_SCOREBOARD + q) + 1) == *(volatile uint8_t *)(h + HIP_SCOREBOARD + 64 + q)) {
		return false;
	}

	bzero((void *)m, 2048);
	w8(m + 4, 1 | 4);                               /* MBULK_F_SIG | MBULK_F_OBOUND (the firmware checks it: mif_q.c) */
	w8(m + 5, clas);
	w8(m + 6, pool);
	w16(m + 10, (uint16_t)sig_bufsz);
	memcpy((void *)(m + MBULK_DESC + 4), signal, length);
	if (payload_length != 0) {
		w8(m + 7, 1);                           /* refcount 1 with a data buffer */
		w16(m + 8, (uint16_t)(payload_length + hip_headroom + hip_tailroom));
		w16(m + 12, (uint16_t)payload_length);
		w16(m + 14, (uint16_t)(sig_bufsz + hip_headroom));
		memcpy((void *)(m + MBULK_DESC + sig_bufsz + hip_headroom), payload, payload_length);
	} else {
		w16(m + 14, (uint16_t)sig_bufsz);       /* head: no data after the signal */
	}
	cache_out(m, 2048);

	cache_out(h + HIP_SCOREBOARD, 128);
	uint8_t w = *(volatile uint8_t *)(h + HIP_SCOREBOARD + q);
	w32(h + HIP_QUEUE(q) + 4 * w, hip_base + offset);
	cache_out(h + HIP_QUEUE(q) + 4 * w, 4);
	w8(h + HIP_SCOREBOARD + q, (uint8_t)(w + 1));
	cache_out(h + HIP_SCOREBOARD, 8);
	wr(mbox + MBOX_INTGR1, 1u << HIP_FROMHOST_BIT);
	return true;
}

static void
hip_send(vm_offset_t mbox, uint32_t block, const uint8_t *signal, uint32_t length, const uint8_t *payload,
    uint32_t payload_length)
{
	(void)hip_put(mbox, HIP_TX_CTL + block * 2048, Q_FH_CTRL, MBULK_POOL_CTRL, MBULK_CLASS_FROM_HOST_CTL, signal, length,
	    payload, payload_length);
}

static void
hip_send_signal(vm_offset_t mbox, uint32_t block, const uint8_t *signal, uint32_t length)
{
	hip_send(mbox, block, signal, length, NULL, 0);
}

/*
 * Take what the firmware put on to-host queue `q` (TH_CTRL or TH_DAT): each block to `take` (its signal, the data after
 * its head and the data's length), then handed back on TH_RFB. Returns how many were taken.
 */
static uint8_t hip_last_mbulk[64];           /* the block being taken: its descriptor and what follows */
static uint32_t hip_last_ref;

typedef void (*hip_taker_t)(const uint8_t *signal, uint32_t signal_length, const uint8_t *data, uint32_t data_length);

/* Hand a to-host block back to the firmware: onto TH_RFB. */
static void
hip_give_back(uint32_t ref)
{
	vm_offset_t h = wlbt_memory + hip_base;
	uint8_t rw = *(volatile uint8_t *)(h + HIP_SCOREBOARD + Q_TH_RFB);

	w32(h + HIP_QUEUE(Q_TH_RFB) + 4 * rw, ref);
	cache_out(h + HIP_QUEUE(Q_TH_RFB) + 4 * rw, 4);
	w8(h + HIP_SCOREBOARD + Q_TH_RFB, (uint8_t)(rw + 1));
	cache_out(h + HIP_SCOREBOARD, 8);
}

#define MBULK_F_CHAIN_HEAD      0x40
#define MBULK_F_CHAIN           0x80
#define MBULK_MAX_CHAIN         16
static uint8_t hip_rx_data[8192];               /* a chained block's data, put together */

static uint32_t
hip_take(vm_offset_t mbox, int q, hip_taker_t take)
{
	vm_offset_t h = wlbt_memory + hip_base;
	uint32_t taken = 0;

	cache_out(h + HIP_SCOREBOARD, 128);
	uint8_t r = *(volatile uint8_t *)(h + HIP_SCOREBOARD + q);
	uint8_t fw_w = *(volatile uint8_t *)(h + HIP_SCOREBOARD + 64 + q);

	while (r != fw_w) {
		cache_out(h + HIP_QUEUE(q) + 4 * r, 4);
		uint32_t ref = rd(h + HIP_QUEUE(q) + 4 * r);
		if (ref >= WLBT_MEMORY - 4096) {
			break;
		}
		vm_offset_t m = wlbt_memory + ref;
		cache_out(m, 64);
		uint16_t sig_bufsz = *(volatile uint16_t *)(m + 10), len = *(volatile uint16_t *)(m + 12);
		uint16_t head = *(volatile uint16_t *)(m + 14);
		memcpy(hip_last_mbulk, (const void *)m, sizeof(hip_last_mbulk));
		hip_last_ref = ref;
		uint8_t flag = *(volatile uint8_t *)(m + 4);
		if (MBULK_DESC + head + len <= 4096 && sig_bufsz >= 4) {
			cache_out(m, MBULK_DESC + head + len);
			/* the data: this block's, then each chained block's (as hip4_mbulk_to_skb), all handed back */
			uint32_t have = len < sizeof(hip_rx_data) ? len : sizeof(hip_rx_data);
			memcpy(hip_rx_data, (const void *)(m + MBULK_DESC + head), have);
			if ((flag & MBULK_F_CHAIN_HEAD) && (flag & MBULK_F_CHAIN)) {
				uint32_t next = *(volatile uint32_t *)(m + 16);
				for (int c = 0; c < MBULK_MAX_CHAIN && next != 0 && next < WLBT_MEMORY - 4096; c++) {
					vm_offset_t n = wlbt_memory + next;
					cache_out(n, 64);
					uint16_t n_len = *(volatile uint16_t *)(n + 12), n_head = *(volatile uint16_t *)(n + 14);
					uint8_t n_flag = *(volatile uint8_t *)(n + 4);
					if (MBULK_DESC + n_head + n_len <= 4096) {
						cache_out(n, MBULK_DESC + n_head + n_len);
						uint32_t more = n_len;
						if (have + more > sizeof(hip_rx_data)) {
							more = sizeof(hip_rx_data) - have;
						}
						memcpy(hip_rx_data + have, (const void *)(n + MBULK_DESC + n_head), more);
						have += more;
					}
					uint32_t following = *(volatile uint32_t *)(n + 16);
					hip_give_back(next);
					if (!(n_flag & MBULK_F_CHAIN)) {
						break;
					}
					next = following;
				}
			}
			take((const uint8_t *)(m + MBULK_DESC + 4), sig_bufsz - 4u, hip_rx_data, have);
		}
		hip_give_back(ref);
		r++;
		w8(h + HIP_SCOREBOARD + q, r);
		cache_out(h + HIP_SCOREBOARD, 8);
		wr(mbox + MBOX_INTGR1, 1u << HIP_FROMHOST_BIT);
		taken++;
	}
	return taken;
}

/* Step 6's scan: what came back. */
#define MLME_ADD_SCAN_REQ       0x2005
#define MLME_ADD_SCAN_CFM       0x2105
#define MLME_SCAN_IND           0x2300
#define MLME_SCAN_DONE_IND      0x2301
#define Q_TH_DAT                4
#define SCAN_MAX                40

static struct {
	uint8_t bssid[6];
	char ssid[33];
	uint16_t mhz;
	int16_t rssi;
	uint8_t secured;
	uint8_t rsn[48];                /* the AP's RSN element, whole (id, length, body) */
	uint8_t rsn_length;
	uint16_t interval, capability;
	uint64_t heard_ms;
	uint16_t elements_length;
	uint8_t elements[HAKIOS_STA_ELEMENTS_MOST];     /* the beacon's elements as received (for Darwin's Wi-Fi side) */
} scan_seen[SCAN_MAX];

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000;
}
static uint32_t scan_count, scan_frames, scan_other;
static int scan_cfm = -1, scan_done;
static char scan_ids[120];
static char scan_debug[1300];

static void
scan_take(const uint8_t *sig, uint32_t sig_length, const uint8_t *data, uint32_t data_length)
{
	uint16_t id = sig_length >= 2 ? (uint16_t)(sig[0] | sig[1] << 8) : 0;

	if (id == MLME_ADD_SCAN_CFM && sig_length >= 14) {
		scan_cfm = sig[12] | sig[13] << 8;
		return;
	}
	if (id == MLME_SCAN_DONE_IND) {
		scan_done = 1;
		return;
	}
	if (id != MLME_SCAN_IND || sig_length < 16) {
		size_t n = strlen(scan_ids);
		scan_other++;
		if (n + 6 < sizeof(scan_ids)) {
			snprintf(scan_ids + n, sizeof(scan_ids) - n, "%04x ", id);
		}
		return;
	}
	scan_frames++;
	if (scan_frames <= 3) {                        /* the layout, for checking: block, signal, data */
		char line[400];
		int k = snprintf(line, sizeof(line), "a51wifi: scan report %u: block %x:", scan_frames, hip_last_ref);
		for (int b = 0; b < 64 && k < (int)sizeof(line) - 4; b++) {
			k += snprintf(line + k, sizeof(line) - k, "%s%02x", b % 4 ? "" : " ", hip_last_mbulk[b]);
		}
		k += snprintf(line + k, sizeof(line) - k, "; signal %u, data %u:", sig_length, data_length);
		for (uint32_t b = 0; b < 48 && b < data_length && k < (int)sizeof(line) - 4; b++) {
			k += snprintf(line + k, sizeof(line) - k, "%s%02x", b % 4 ? "" : " ", data[b]);
		}
		size_t n = strlen(scan_debug);
		snprintf(scan_debug + n, sizeof(scan_debug) - n, "%s\n", line);
	}
	/* the data: the 802.11 frame (beacon 0x80 or probe response 0x50): BSSID at 16, fixed fields to 36, elements */
	if (data_length < 36 || (data[0] != 0x80 && data[0] != 0x50)) {
		return;
	}
	const uint8_t *bssid = data + 16;
	uint32_t i;
	for (i = 0; i < scan_count; i++) {
		if (memcmp(scan_seen[i].bssid, bssid, 6) == 0) {
			break;
		}
	}
	if (i == scan_count) {
		if (scan_count == SCAN_MAX) {
			return;
		}
		scan_count++;
		bzero(&scan_seen[i], sizeof(scan_seen[i]));
		memcpy(scan_seen[i].bssid, bssid, 6);
	}
	scan_seen[i].mhz = (uint16_t)((sig[12] | sig[13] << 8) / 2);
	scan_seen[i].rssi = (int16_t)(sig[14] | sig[15] << 8);
	scan_seen[i].secured = (data[34] & 0x10) != 0;          /* capability: privacy */
	scan_seen[i].interval = (uint16_t)(data[32] | data[33] << 8);
	scan_seen[i].capability = (uint16_t)(data[34] | data[35] << 8);
	scan_seen[i].heard_ms = now_ms();
	scan_seen[i].elements_length = (uint16_t)(data_length - 36 < HAKIOS_STA_ELEMENTS_MOST ? data_length - 36 :
	    HAKIOS_STA_ELEMENTS_MOST);
	memcpy(scan_seen[i].elements, data + 36, scan_seen[i].elements_length);
	for (uint32_t e = 36; e + 2 <= data_length && e + 2 + data[e + 1] <= data_length; e += 2 + data[e + 1]) {
		if (data[e] == 0x30 && 2u + data[e + 1] <= sizeof(scan_seen[i].rsn)) {
			scan_seen[i].rsn_length = (uint8_t)(2 + data[e + 1]);
			memcpy(scan_seen[i].rsn, data + e, scan_seen[i].rsn_length);
		}
		if (data[e] == 0 && scan_seen[i].ssid[0] == 0) {
			uint32_t n = data[e + 1] < 32 ? data[e + 1] : 32;
			for (uint32_t k = 0; k < n; k++) {
				uint8_t c = data[e + 2 + k];
				scan_seen[i].ssid[k] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
			}
			scan_seen[i].ssid[n] = 0;
		}
	}
}

static void
step6_take(const uint8_t *sig, uint32_t sig_length, const uint8_t *data, uint32_t data_length)
{
	scan_take(sig, sig_length, data, data_length);
}

/* Step 6: a passive scan over the usual channels. */
static void
wifi_step6(vm_offset_t mbox)
{
	static const uint16_t channels_5g[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132,
		                                136, 140, 149, 153, 157, 161, 165 };
	uint8_t req[36], data[200];
	uint32_t n = 0;

	vm_offset_t h = wlbt_memory + hip_base;
	cache_out(h, 512);
	vm_offset_t config = rd(h + 4) == 5 ? h + HIP_V5 : h + HIP_V4;
	hip_headroom = *(volatile uint8_t *)(config + 28);
	hip_tailroom = *(volatile uint8_t *)(config + 29);

	bzero(req, sizeof(req));
	req[0] = MLME_ADD_SCAN_REQ & 0xff;
	req[1] = MLME_ADD_SCAN_REQ >> 8;
	req[4] = FAPI_SENDER & 0xff;
	req[5] = FAPI_SENDER >> 8;
	/* vif 0 at 10 (a scan belongs to no interface), scan id at 12: interface 1 << 8 | SLSI_SCAN_HW_ID 0 */
	req[13] = 1;
	req[14] = 2;                                    /* FAPI_SCANTYPE_FULL_SCAN */
	static const uint8_t address[6] = { 0x02, 0xa5, 0x1c, 0x00, 0x00, 0x01 };
	memcpy(req + 16, address, 6);
	req[22] = 0x04;                                 /* FAPI_REPORTMODE_REAL_TIME */

	/* no SSIDs: a passive scan. Scan timing: Samsung's defaults (all 0) */
	static const uint8_t timing[] = { 0xdd, 0x11, 0x00, 0x16, 0x32, 0x01, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	memcpy(data + n, timing, sizeof(timing));
	n += sizeof(timing);
	/* the channel list: frequency * 2 (le16), then the scan policy */
	uint32_t list = n;
	static const uint8_t list_header[] = { 0xdd, 0x05, 0x00, 0x16, 0x32, 0x01, 0x02 };
	memcpy(data + n, list_header, sizeof(list_header));
	n += sizeof(list_header);
	for (int c = 1; c <= 13; c++) {
		uint16_t f = (uint16_t)(2 * (2407 + 5 * c));
		data[n++] = f & 0xff;
		data[n++] = f >> 8;
		data[n++] = 0x01;                       /* FAPI_SCANPOLICY_PASSIVE */
	}
	for (size_t c = 0; c < sizeof(channels_5g) / sizeof(channels_5g[0]); c++) {
		uint16_t f = (uint16_t)(2 * (5000 + 5 * channels_5g[c]));
		data[n++] = f & 0xff;
		data[n++] = f >> 8;
		data[n++] = 0x01;
	}
	data[list + 1] = (uint8_t)(n - list - 2);

	printf("a51wifi: step 6: MLME-ADD-SCAN.req, passive, %u channels (headroom %u, tailroom %u)\n",
	    (n - list - 7) / 3, hip_headroom, hip_tailroom);
	hip_send(mbox, 1, req, sizeof(req), data, n);

	for (int ms = 0; ms <= 20000 && !scan_done; ms += 20) {
		uint32_t got = hip_take(mbox, Q_TH_CTRL, step6_take) + hip_take(mbox, Q_TH_DAT, step6_take);
		if (scan_cfm > 0 && scan_frames == 0 && ms > 2000) {
			break;                          /* refused */
		}
		if (got == 0) {
			IOSleep(20);
		}
	}

	/* the list, said again with the status */
	wifi_dump[0] = 0;
	for (uint32_t i = 0; i < scan_count; i++) {
		size_t k = strlen(wifi_dump);
		if (k + 120 > sizeof(wifi_dump)) {
			break;
		}
		snprintf(wifi_dump + k, sizeof(wifi_dump) - k, "a51wifi: network %2u: %02x:%02x:%02x:%02x:%02x:%02x %4u MHz %4d dBm %s \"%s\"\n",
		    i + 1, scan_seen[i].bssid[0], scan_seen[i].bssid[1], scan_seen[i].bssid[2], scan_seen[i].bssid[3],
		    scan_seen[i].bssid[4], scan_seen[i].bssid[5], scan_seen[i].mhz, scan_seen[i].rssi,
		    scan_seen[i].secured ? "secured" : "open   ", scan_seen[i].ssid[0] ? scan_seen[i].ssid : "(hidden)");
	}
	if (scan_count == 0) {                          /* nothing read: the raw reports, to see why */
		size_t k = strlen(wifi_dump);
		snprintf(wifi_dump + k, sizeof(wifi_dump) - k, "%s", scan_debug);
	}
	printf("%s", wifi_dump);
	wifi_said("step 6: scan %s: MLME-ADD-SCAN.cfm %s%x, %u scan reports, %u networks; other signals: %s",
	    scan_done ? "done (MLME-SCAN-DONE.ind)" : "not done in 20 s", scan_cfm < 0 ? "none " : "result ",
	    scan_cfm < 0 ? 0 : scan_cfm, scan_frames, scan_count, scan_ids[0] ? scan_ids : "none");
}

/*
 * Step 7: connect. hakios.a51wpa.c does the handshake's arithmetic (checked on the PC against Python's hashlib/hmac,
 * RFC 3394 and an access point simulated in Python); here the signals around it, as Samsung's mlme.c and rx.c.
 */
void a51wpa_pmk(const char *passphrase, size_t passphrase_length, const uint8_t *ssid, size_t ssid_length, uint8_t pmk[32]);
void a51wpa_begin(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t snonce[32],
    const uint8_t *rsn, size_t rsn_length);
int a51wpa_frame(const uint8_t *frame, size_t length, uint8_t *out, size_t *out_length);
size_t a51wpa_keys(uint8_t tk[16], uint8_t gtk[32], int *gtk_id, uint8_t rsc[8]);
const char *a51wpa_failure(void);
void a51wpa_forget(void);

#define MLME_CONNECT_REQ        0x200b
#define MLME_CONNECT_CFM        0x210b
#define MLME_CONNECT_IND        0x2303
#define MLME_CONNECT_RES        0x2200
#define MLME_SEND_FRAME_REQ     0x2010
#define MLME_SEND_FRAME_CFM     0x2110
#define MLME_FRAME_TRANSMISSION_IND 0x230c
#define MLME_SETKEYS_REQ        0x2018
#define MLME_SETKEYS_CFM        0x2118
#define MLME_DISCONNECT_IND     0x2308
#define MLME_DISCONNECTED_IND   0x2309
#define MA_UNITDATA_IND         0x1300
#define MLME_RECEIVED_FRAME_IND 0x230d          /* EAPOL comes this way (data unit descriptor 1: an 802.3 frame) */
#define DEBUG_WORD12_IND        0x8301
#define DEBUG_GENERIC_IND       0x8302

static const uint8_t station_address[6] = { 0x02, 0xa5, 0x1c, 0x00, 0x00, 0x01 };     /* locally administered */

static bool svc_on;                     /* a51wifi=8: the radio is Darwin's Wi-Fi (en0) */
static void svc_input(const uint8_t *frame, uint32_t length, uint16_t descriptor);
static bool svc_send(bool data, const uint8_t *sig, uint32_t length, const uint8_t *payload, uint32_t payload_length);

static struct {
	uint8_t bssid[6];
	uint32_t next_block;            /* control blocks 2..31 in turn */
	bool open;                      /* no RSN: no handshake, no keys */
	uint16_t host_tag;
	uint16_t m4_tag;
	int connect_cfm, connect_ind, keys_cfm, keys_wanted;
	bool m4_out, keys_set, connected;
	int wpa_result;
	uint32_t eapol, data_frames, data_bytes, setkeys_bad, disconnected;
	uint16_t disconnect_reason;
	uint16_t key_result[2];         /* MLME-SETKEYS.cfm's result: pairwise, group */
	uint8_t cipher_group[4];
	char ids[120];
	char last_data[80];
	vm_offset_t mbox;
} link;

static void
hdr(uint8_t *sig, uint16_t id)
{
	sig[0] = id & 0xff;
	sig[1] = id >> 8;
	sig[4] = FAPI_SENDER & 0xff;
	sig[5] = FAPI_SENDER >> 8;
	sig[10] = 1;                                    /* vif 1 */
}

static void
link_send(const uint8_t *sig, uint32_t length, const uint8_t *data, uint32_t data_length)
{
	if (svc_on) {
		(void)svc_send(false, sig, length, data, data_length);
		return;
	}
	hip_send(link.mbox, link.next_block, sig, length, data, data_length);
	link.next_block = link.next_block >= 31 ? 2 : link.next_block + 1;
}

/* An EAPOL frame to the AP: MLME-SEND-FRAME.req with an 802.3 frame (tx.c slsi_tx_eapol, mlme.c send_frame_data). */
static void
send_eapol(const uint8_t *eapol, size_t length, bool m4)
{
	uint8_t sig[40], frame[300];

	bzero(sig, sizeof(sig));
	hdr(sig, MLME_SEND_FRAME_REQ);
	link.host_tag = (uint16_t)(link.host_tag + 4);
	sig[12] = link.host_tag & 0xff;
	sig[13] = link.host_tag >> 8;
	sig[14] = 1;                                    /* FAPI_DATAUNITDESCRIPTOR_IEEE802_3_FRAME */
	sig[16] = m4 ? 3 : 2;                           /* FAPI_MESSAGETYPE_EAPOL_KEY_M4 / _M123 */
	memcpy(frame, link.bssid, 6);
	memcpy(frame + 6, station_address, 6);
	frame[12] = 0x88;
	frame[13] = 0x8e;
	memcpy(frame + 14, eapol, length);
	if (m4) {
		link.m4_tag = link.host_tag;
		link.m4_out = false;
	}
	link_send(sig, sizeof(sig), frame, (uint32_t)(14 + length));
}

static void
send_key(uint16_t key_id, uint16_t key_type, const uint8_t *address, const uint8_t *rsc, const uint8_t cipher[4],
    const uint8_t *key, uint32_t key_length)
{
	uint8_t sig[56];

	bzero(sig, sizeof(sig));
	hdr(sig, MLME_SETKEYS_REQ);
	sig[12] = (uint8_t)((key_length * 8) & 0xff);
	sig[13] = (uint8_t)((key_length * 8) >> 8);
	sig[14] = (uint8_t)key_id;
	sig[16] = (uint8_t)key_type;
	memcpy(sig + 18, address, 6);
	if (rsc != NULL) {
		memcpy(sig + 24, rsc, 8);               /* the RSC's bytes, as Samsung packs them (pairs, little endian) */
	}
	/* the cipher suite selector, as the u32 00-0f-ac-04 (WLAN_CIPHER_SUITE_CCMP) little endian */
	sig[40] = cipher[3];
	sig[41] = cipher[2];
	sig[42] = cipher[1];
	sig[43] = cipher[0];
	link_send(sig, sizeof(sig), key, key_length);
	link.keys_wanted++;
}

static void
link_note(uint16_t id)
{
	size_t n = strlen(link.ids);

	if (n + 6 < sizeof(link.ids)) {
		snprintf(link.ids + n, sizeof(link.ids) - n, "%04x ", id);
	}
}

static void
step7_take(const uint8_t *sig, uint32_t sig_length, const uint8_t *data, uint32_t data_length)
{
	uint16_t id = sig_length >= 2 ? (uint16_t)(sig[0] | sig[1] << 8) : 0;
	uint16_t a = sig_length >= 14 ? (uint16_t)(sig[12] | sig[13] << 8) : 0;
	uint16_t b = sig_length >= 16 ? (uint16_t)(sig[14] | sig[15] << 8) : 0;

	switch (id) {
	case MLME_CONNECT_CFM:
		link.connect_cfm = a;
		return;
	case MLME_CONNECT_IND:
		link.connect_ind = a;
		return;
	case MLME_SETKEYS_CFM:
		if (link.keys_cfm < 2) {
			link.key_result[link.keys_cfm] = a;
		}
		link.keys_cfm++;
		if (a != 0) {
			link.setkeys_bad = a;
		}
		return;
	case MLME_FRAME_TRANSMISSION_IND:
		if (a == link.m4_tag && link.m4_tag != 0) {
			link.m4_out = true;
		}
		return;
	case MLME_SEND_FRAME_CFM:
	case DEBUG_WORD12_IND:
	case DEBUG_GENERIC_IND:
		return;
	case MLME_DISCONNECT_IND:
	case MLME_DISCONNECTED_IND:
		link.disconnected++;
		link.disconnect_reason = sig_length >= 20 ? (uint16_t)(sig[18] | sig[19] << 8) : 0xffff;
		link_note(id);
		return;
	case MLME_RECEIVED_FRAME_IND:
	case MA_UNITDATA_IND: {
		/* an 802.3 frame: MA-UNITDATA.ind's after any proprietary information (its length at signal 20);
		 * MLME-RECEIVED-FRAME.ind's when its data unit descriptor (signal 12) is 1 (rx.c) */
		uint32_t skip = 0;
		if (id == MLME_RECEIVED_FRAME_IND && a != 1) {
			link_note(id);
			return;
		}
		if (id == MA_UNITDATA_IND && sig_length >= 22) {
			skip = (uint32_t)(sig[20] | sig[21] << 8);
		}
		if (skip + 14 > data_length) {
			return;
		}
		const uint8_t *f = data + skip;
		uint32_t n = data_length - skip;
		if (f[12] == 0x88 && f[13] == 0x8e) {
			uint8_t out[256];
			size_t out_length = 0;
			link.eapol++;
			int r = a51wpa_frame(f + 14, n - 14, out, &out_length);
			if (r == 2 || r == 4) {
				send_eapol(out, out_length, r == 4);
			}
			if (r != 0) {
				link.wpa_result = r;
			}
			return;
		}
		if (link.connected) {
			if (svc_on) {
				svc_input(f, n, id == MA_UNITDATA_IND && sig_length >= 14 ? a : 1);
			}
			link.data_frames++;
			link.data_bytes += n;
			snprintf(link.last_data, sizeof(link.last_data), "%02x:%02x:%02x:%02x:%02x:%02x type %02x%02x, %u bytes",
			    f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], n);
		}
		return;
	}
	default:
		link_note(id);
		return;
	}
}

/*
 * After each look at the queues: message 4 out (or 1 s gone) -> the keys (as wpa_supplicant), both confirmed ->
 * MLME-CONNECT-RES (rx.c, on M4's transmission status); an open network: MLME-CONNECT-RES once associated.
 */
static uint64_t link_m4_at;

static void
link_progress(void)
{
	if (link.connected) {
		return;
	}
	if (link.open) {
		if (link.connect_ind == 0) {
			uint8_t res[24];
			bzero(res, sizeof(res));
			hdr(res, MLME_CONNECT_RES);
			link_send(res, sizeof(res), NULL, 0);
			link.connected = true;
		}
		return;
	}
	if (link.wpa_result == 4 && link_m4_at == 0) {
		link_m4_at = now_ms();
	}
	if (!link.keys_set && link_m4_at != 0 && (link.m4_out || now_ms() - link_m4_at > 1000)) {
		uint8_t tk[16], gtk[32], rsc[8];
		int gtk_id;
		size_t gtk_length = a51wpa_keys(tk, gtk, &gtk_id, rsc);
		/* both against the AP's address: "Sta Group Key will use the peer address" (cfg80211_ops.c slsi_add_key) */
		send_key(0, 1, link.bssid, NULL, link.cipher_group, tk, 16);
		send_key((uint16_t)gtk_id, 0, link.bssid, rsc, link.cipher_group, gtk, (uint32_t)gtk_length);
		bzero(tk, sizeof(tk));
		bzero(gtk, sizeof(gtk));
		link.keys_set = true;
	}
	if (link.keys_set && link.keys_cfm >= link.keys_wanted) {
		uint8_t res[24];
		bzero(res, sizeof(res));
		hdr(res, MLME_CONNECT_RES);
		link_send(res, sizeof(res), NULL, 0);
		link.connected = true;
	}
}

/* fw/wifi.conf: ssid=NAME, psk=PASSPHRASE or pmk=64 hex digits. Its values are never printed. */
static bool
wifi_conf(char *ssid, size_t ssid_size, char *psk, size_t psk_size, uint8_t pmk[32], bool *have_pmk)
{
	uint64_t length = 0;
	const char *text = hakios_image_file("fw", "wifi.conf", &length, NULL);

	ssid[0] = psk[0] = 0;
	*have_pmk = false;
	if (text == NULL) {
		return false;
	}
	for (uint64_t i = 0; i < length;) {
		uint64_t end = i;
		while (end < length && text[end] != '\n' && text[end] != '\r') {
			end++;
		}
		const char *line = text + i;
		uint64_t n = end - i;
		if (n > 5 && memcmp(line, "ssid=", 5) == 0 && n - 5 < ssid_size) {
			memcpy(ssid, line + 5, n - 5);
			ssid[n - 5] = 0;
		} else if (n > 4 && memcmp(line, "psk=", 4) == 0 && n - 4 < psk_size) {
			memcpy(psk, line + 4, n - 4);
			psk[n - 4] = 0;
		} else if (n == 68 && memcmp(line, "pmk=", 4) == 0) {
			bool good = true;
			for (int k = 0; k < 64; k++) {
				char c = line[4 + k];
				int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
				if (v < 0) {
					good = false;
					break;
				}
				pmk[k / 2] = (uint8_t)((k & 1) ? (pmk[k / 2] | v) : v << 4);
			}
			*have_pmk = good;
		}
		i = end;
		while (i < length && (text[i] == '\n' || text[i] == '\r')) {
			i++;
		}
	}
	return ssid[0] != 0 && (psk[0] != 0 || *have_pmk);
}

/* Does an RSN element (whole) list this suite (00-0f-ac-type) in the list starting at `at`? */
static bool
rsn_has(const uint8_t *rsn, uint32_t length, uint32_t *at, uint8_t type)
{
	bool found = false;

	if (*at + 2 > length) {
		return false;
	}
	uint32_t count = rsn[*at] | rsn[*at + 1] << 8;
	*at += 2;
	for (uint32_t k = 0; k < count && *at + 4 <= length; k++, *at += 4) {
		if (rsn[*at] == 0x00 && rsn[*at + 1] == 0x0f && rsn[*at + 2] == 0xac && rsn[*at + 3] == type) {
			found = true;
		}
	}
	return found;
}

static void
wifi_step7(vm_offset_t mbox)
{
	char ssid[33], psk[64];
	uint8_t pmk[32], snonce[32];
	bool have_pmk;

	bzero(&link, sizeof(link));
	link.mbox = mbox;
	link.next_block = 2;
	if (!wifi_conf(ssid, sizeof(ssid), psk, sizeof(psk), pmk, &have_pmk)) {
		wifi_said("step 7: no fw/wifi.conf with ssid= and psk= (or pmk=): stopping");
		return;
	}
	/* the network: the strongest with that SSID */
	int best = -1;
	for (uint32_t i = 0; i < scan_count; i++) {
		if (strcmp(scan_seen[i].ssid, ssid) == 0 && (best < 0 || scan_seen[i].rssi > scan_seen[best].rssi)) {
			best = (int)i;
		}
	}
	if (best < 0) {
		wifi_said("step 7: the network of fw/wifi.conf was not in the scan (%u networks): stopping", scan_count);
		bzero(psk, sizeof(psk));
		return;
	}
	/* its RSN element: version, group cipher, pairwise ciphers, AKMs, capabilities */
	const uint8_t *ap = scan_seen[best].rsn;
	uint32_t ap_length = scan_seen[best].rsn_length, at = 8;
	if (ap_length < 8 + 2 + 4 + 2 + 4) {
		wifi_said("step 7: the network has no RSN element (WPA1 or open): not handled; stopping");
		bzero(psk, sizeof(psk));
		return;
	}
	memcpy(link.cipher_group, ap + 4, 4);
	bool ccmp = rsn_has(ap, ap_length, &at, 4), psk_akm = rsn_has(ap, ap_length, &at, 2);
	uint16_t caps = at + 2 <= ap_length ? (uint16_t)(ap[at] | ap[at + 1] << 8) : 0;
	bool group_ok = link.cipher_group[0] == 0x00 && link.cipher_group[1] == 0x0f && link.cipher_group[2] == 0xac &&
	    link.cipher_group[3] == 4;
	if (!ccmp || !psk_akm || !group_ok || (caps & 0x40)) {
		wifi_said("step 7: the network wants what is not handled yet (CCMP pairwise %d, PSK %d, CCMP group %d, MFP "
		    "required %d): stopping", ccmp, psk_akm, group_ok, (caps & 0x40) != 0);
		bzero(psk, sizeof(psk));
		return;
	}
	static const uint8_t rsn[22] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
		                         0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00 };

	if (!have_pmk) {
		a51wpa_pmk(psk, strlen(psk), (const uint8_t *)ssid, strlen(ssid), pmk);
	}
	bzero(psk, sizeof(psk));
	read_random(snonce, sizeof(snonce));
	memcpy(link.bssid, scan_seen[best].bssid, 6);
	a51wpa_begin(pmk, link.bssid, station_address, snonce, rsn, sizeof(rsn));
	bzero(pmk, sizeof(pmk));

	/* MLME-CONNECT.req: vif 1, the BSSID, open system, the channel (MHz * 2); data: the SSID and RSN elements */
	uint8_t sig[34], data[64];
	uint32_t n = 0, ssid_length = (uint32_t)strlen(ssid);
	bzero(sig, sizeof(sig));
	hdr(sig, MLME_CONNECT_REQ);
	memcpy(sig + 12, link.bssid, 6);
	sig[20] = (uint8_t)((scan_seen[best].mhz * 2) & 0xff);
	sig[21] = (uint8_t)((scan_seen[best].mhz * 2) >> 8);
	data[n++] = 0;
	data[n++] = (uint8_t)ssid_length;
	memcpy(data + n, ssid, ssid_length);
	n += ssid_length;
	memcpy(data + n, rsn, sizeof(rsn));
	n += sizeof(rsn);
	printf("a51wifi: step 7: MLME-CONNECT.req to %02x:%02x:%02x:%02x:%02x:%02x at %u MHz (%d dBm)\n", link.bssid[0],
	    link.bssid[1], link.bssid[2], link.bssid[3], link.bssid[4], link.bssid[5], scan_seen[best].mhz,
	    scan_seen[best].rssi);
	link.connect_cfm = link.connect_ind = -1;
	link_send(sig, sizeof(sig), data, n);

	/* the association, the handshake, the keys: 20 s; then what comes in, 20 s more */
	int done_at = -1, m4_at = -1;
	for (int ms = 0; ms <= 40000; ms += 10) {
		uint32_t got = hip_take(mbox, Q_TH_CTRL, step7_take) + hip_take(mbox, Q_TH_DAT, step7_take);
		if (link.wpa_result == 4 && m4_at < 0) {
			m4_at = ms;
		}
		/* message 4 out (or 1 s gone): the keys (as wpa_supplicant), then MLME-CONNECT-RES (rx.c, on M4's status) */
		if (!link.keys_set && m4_at >= 0 && (link.m4_out || ms - m4_at > 1000)) {
			uint8_t tk[16], gtk[32], rsc[8];
			int gtk_id;
			size_t gtk_length = a51wpa_keys(tk, gtk, &gtk_id, rsc);
			/* both against the AP's address: "Sta Group Key will use the peer address" (cfg80211_ops.c slsi_add_key) */
			send_key(0, 1, link.bssid, NULL, link.cipher_group, tk, 16);
			send_key((uint16_t)gtk_id, 0, link.bssid, rsc, link.cipher_group, gtk, (uint32_t)gtk_length);
			bzero(tk, sizeof(tk));
			bzero(gtk, sizeof(gtk));
			link.keys_set = true;
		}
		if (link.keys_set && !link.connected && link.keys_cfm >= link.keys_wanted) {
			uint8_t res[24];
			bzero(res, sizeof(res));
			hdr(res, MLME_CONNECT_RES);
			link_send(res, sizeof(res), NULL, 0);
			link.connected = true;
			done_at = ms;
			printf("a51wifi: step 7: keys set, MLME-CONNECT-RES sent\n");
		}
		if (link.wpa_result < 0 || link.disconnected || (link.connect_cfm > 0) || (link.connect_ind > 0)) {
			break;
		}
		if (done_at < 0 && ms > 20000) {
			break;
		}
		if (done_at >= 0 && ms - done_at > 20000) {
			break;
		}
		if (got == 0) {
			IOSleep(10);
		}
	}
	a51wpa_forget();
	if (link.connected && !link.disconnected) {
		wifi_said("step 7: CONNECTED to \"%s\" (WPA2; SETKEYS.cfm pairwise %#x, group %#x); in 20 s: %u data frames, %u bytes%s%s; "
		    "other signals: %s", ssid, link.key_result[0], link.key_result[1], link.data_frames,
		    link.data_bytes, link.data_frames ? ", the last from " : "", link.last_data, link.ids[0] ? link.ids : "none");
	} else {
		wifi_said("step 7: not connected: CONNECT.cfm %d, CONNECT.ind %d, EAPOL frames %u, handshake %d%s%s, keys %u/%u, "
		    "disconnected %u (reason %u); other signals: %s", link.connect_cfm, link.connect_ind, link.eapol,
		    link.wpa_result, link.wpa_result < 0 ? ": " : "", link.wpa_result < 0 ? a51wpa_failure() : "",
		    link.keys_cfm, link.keys_wanted, link.disconnected, link.disconnect_reason, link.ids[0] ? link.ids : "none");
	}
}

/*
 * a51wifi=8: the radio as Darwin's Wi-Fi. en0 (drivers/net/hakios.net.c) is made as soon as this driver's thread runs,
 * its link down; once the firmware's interface is added (step 5) the station side (sys/hakios.sta.h) is attached, so
 * Darwin's Wi-Fi side (hakios.apple80211.c: wifid, Settings) can scan and join, and the driver's thread becomes the
 * one that looks at the firmware's queues, every 2 ms: scan reports, the join and its handshake, the data frames
 * (to en0), the blocks the firmware hands back. fw/wifi.conf, if there is one, is joined at the start and again
 * whenever the radio is turned on (a standing join).
 */
static LCK_GRP_DECLARE(a51wifi_lck_grp, "a51wifi");
static LCK_MTX_DECLARE(svc_lock, &a51wifi_lck_grp);     /* the pools and the from-host queues: one sender at a time */

#define HIP_TX_DAT              0x00a000        /* the data pool: 512 blocks of 2 KB */
#define DAT_BLOCKS              512
#define CTL_BLOCKS              32
#define Q_FH_DAT                1
#define Q_FH_RFB                2
#define MBULK_POOL_DATA         0
#define MBULK_CLASS_FROM_HOST_DAT 4
#define MA_UNITDATA_REQ         0x1000
#define MA_UNITDATA_CFM         0x1100
#define MLME_DISCONNECT_REQ     0x200e
#define MLME_DISCONNECT_CFM     0x210e

static vm_offset_t svc_mbox;
static char svc_events[700];            /* what the service did, said again with each minute's line */

static void
svc_event(const char *format, ...) __printflike(1, 2);

static void
svc_event(const char *format, ...)
{
	char line[160];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("a51wifi: %s\n", line);
	size_t n = strlen(svc_events);
	if (n + strlen(line) + 4 < sizeof(svc_events)) {
		snprintf(svc_events + n, sizeof(svc_events) - n, "[%llus] %s; ", now_ms() / 1000, line);
	}
}
static uint8_t ctl_used[CTL_BLOCKS], dat_used[DAT_BLOCKS];
static uint32_t ctl_next, dat_next;
static uint16_t svc_data_tag;
static struct {
	enum hakios_sta_phase phase;
	int last_error;
	struct hakios_sta_bss bss;      /* the network joined or being joined */
	bool scanning;
	uint64_t sent, sent_bytes, dropped_out, received, received_bytes, dropped_in, amsdu;
	uint32_t no_block, queue_full, waited, dat_busy, dat_busy_most;
} svc;

/* A free block of a pool, taken; -1: none. Under svc_lock. */
static int
pool_take(uint8_t *used, uint32_t count, uint32_t *next)
{
	for (uint32_t k = 0; k < count; k++) {
		uint32_t i = (*next + k) % count;
		if (!used[i]) {
			used[i] = 1;
			*next = (i + 1) % count;
			return (int)i;
		}
	}
	return -1;
}

/* The blocks the firmware has handed back (FH_RFB: firmware writes byte 66, host reads byte 2), free again. */
static void
svc_reclaim(void)
{
	vm_offset_t h = wlbt_memory + hip_base;

	lck_mtx_lock(&svc_lock);
	cache_out(h + HIP_SCOREBOARD, 128);
	uint8_t r = *(volatile uint8_t *)(h + HIP_SCOREBOARD + Q_FH_RFB);
	uint8_t fw_w = *(volatile uint8_t *)(h + HIP_SCOREBOARD + 64 + Q_FH_RFB);
	while (r != fw_w) {
		cache_out(h + HIP_QUEUE(Q_FH_RFB) + 4 * r, 4);
		uint32_t ref = rd(h + HIP_QUEUE(Q_FH_RFB) + 4 * r) - hip_base;
		if (ref >= HIP_TX_DAT && ref < HIP_TX_DAT + DAT_BLOCKS * 2048) {
			if (dat_used[(ref - HIP_TX_DAT) / 2048] && svc.dat_busy > 0) {
				svc.dat_busy--;
			}
			dat_used[(ref - HIP_TX_DAT) / 2048] = 0;
		} else if (ref >= HIP_TX_CTL && ref < HIP_TX_CTL + CTL_BLOCKS * 2048) {
			ctl_used[(ref - HIP_TX_CTL) / 2048] = 0;
		}
		r++;
	}
	w8(h + HIP_SCOREBOARD + Q_FH_RFB, r);
	cache_out(h + HIP_SCOREBOARD, 8);
	lck_mtx_unlock(&svc_lock);
}

/* A signal to the firmware in a block of its pool: control (MLME) or data (MA). false: no block free. */
static bool
svc_send(bool data, const uint8_t *sig, uint32_t length, const uint8_t *payload, uint32_t payload_length)
{
	lck_mtx_lock(&svc_lock);
	int b = data ? pool_take(dat_used, DAT_BLOCKS, &dat_next) : pool_take(ctl_used, CTL_BLOCKS, &ctl_next);
	if (b < 0) {
		svc.no_block++;
		lck_mtx_unlock(&svc_lock);
		return false;
	}
	bool put = data ?
	    hip_put(svc_mbox, HIP_TX_DAT + (uint32_t)b * 2048, Q_FH_DAT, MBULK_POOL_DATA, MBULK_CLASS_FROM_HOST_DAT, sig, length,
	    payload, payload_length) :
	    hip_put(svc_mbox, HIP_TX_CTL + (uint32_t)b * 2048, Q_FH_CTRL, MBULK_POOL_CTRL, MBULK_CLASS_FROM_HOST_CTL, sig, length,
	    payload, payload_length);
	if (!put) {
		(data ? dat_used : ctl_used)[b] = 0;    /* the queue is full: the block stays free */
		svc.queue_full++;
	} else if (data && ++svc.dat_busy > svc.dat_busy_most) {
		svc.dat_busy_most = svc.dat_busy;
	}
	lck_mtx_unlock(&svc_lock);
	return put;
}

/* en0's frame to send: MA-UNITDATA.req (tx.c): vif 1, a host tag, priority 0 (best effort), peer 1 (the AP), 802.3. */
static bool
svc_transmit(mbuf_t packet, size_t length)
{
	static uint8_t frame[HAKIOS_NET_FRAME_MAX];     /* (one sender at a time: hakios.net.c's tx lock) */
	uint8_t sig[34];

	if (svc.phase != HAKIOS_STA_JOINED || !link.connected || length < 14 || length > sizeof(frame) ||
	    mbuf_copydata(packet, 0, length, frame) != 0) {
		svc.dropped_out++;
		return false;
	}
	bzero(sig, sizeof(sig));
	sig[0] = MA_UNITDATA_REQ & 0xff;
	sig[1] = MA_UNITDATA_REQ >> 8;
	sig[4] = FAPI_SENDER & 0xff;
	sig[5] = FAPI_SENDER >> 8;
	sig[10] = 1;                                    /* vif */
	svc_data_tag = (uint16_t)((svc_data_tag + 4) & 0x7ffc);
	sig[12] = svc_data_tag & 0xff;
	sig[13] = svc_data_tag >> 8;
	sig[16] = 1;                                    /* peer index: the AP (queue set 0 + 1) */
	sig[18] = 1;                                    /* FAPI_DATAUNITDESCRIPTOR_IEEE802_3_FRAME */
	/* no block or no room in the queue: wait for the firmware to hand some back, up to 10 ms (Linux stops its queue
	 * instead: the frames wait rather than go) */
	bool sent = svc_send(true, sig, sizeof(sig), frame, (uint32_t)length);
	if (!sent) {
		svc.waited++;
	}
	for (int tries = 0; !sent && tries < 10; tries++) {
		IOSleep(1);
		svc_reclaim();
		sent = svc_send(true, sig, sizeof(sig), frame, (uint32_t)length);
	}
	if (!sent) {
		svc.dropped_out++;
		return false;
	}
	svc.sent++;
	svc.sent_bytes += length;
	return true;
}

/* A frame that has come in, to en0: an 802.3 frame (descriptor 1), or an A-MSDU (3) taken apart into Ethernet frames. */
static void
svc_input(const uint8_t *f, uint32_t n, uint16_t descriptor)
{
	static uint8_t frame[HAKIOS_NET_FRAME_MAX];

	if (descriptor == 1 || descriptor == 0) {
		if (n >= 14 && n <= HAKIOS_NET_FRAME_MAX) {
			hakios_net_input(f, n);
			svc.received++;
			svc.received_bytes += n;
		} else {
			svc.dropped_in++;
		}
		return;
	}
	if (descriptor != 3 && descriptor != 2) {
		svc.dropped_in++;
		return;
	}
	/* subframes: destination, source, length (big endian), LLC/SNAP and the payload, each padded to 4 bytes */
	for (uint32_t at = 0; at + 14 <= n;) {
		uint32_t length = (uint32_t)(f[at + 12] << 8 | f[at + 13]);
		const uint8_t *p = f + at + 14;
		if (at + 14 + length > n) {
			break;
		}
		if (length >= 8 && p[0] == 0xaa && p[1] == 0xaa && p[2] == 0x03 && length - 8 + 14 <= sizeof(frame)) {
			memcpy(frame, f + at, 12);
			frame[12] = p[6];
			frame[13] = p[7];
			memcpy(frame + 14, p + 8, length - 8);
			hakios_net_input(frame, length - 8 + 14);
			svc.received++;
			svc.received_bytes += length + 6;
			svc.amsdu++;
		} else {
			svc.dropped_in++;
		}
		at = (at + 14 + length + 3) & ~3u;
	}
}

/*
 * The station's interface in the firmware, as Samsung's driver keeps it: added right before MLME-CONNECT.req
 * (cfg80211_ops.c slsi_connect) and deleted when a join fails or the network is left or lost (slsi_handle_disconnect).
 * Step 5 left one added.
 */
#define MLME_DEL_VIF_REQ        0x2008
#define MLME_DEL_VIF_CFM        0x2108
static bool svc_vif_up, svc_vif_lost;
static volatile int svc_vif_cfm;                /* the last ADD-VIF/DEL-VIF.cfm's result; -1: none yet */

static int
svc_vif(bool add)
{
	uint8_t req[38];

	bzero(req, sizeof(req));
	hdr(req, add ? MLME_ADD_VIF_REQ : MLME_DEL_VIF_REQ);
	if (add) {
		memcpy(req + 12, station_address, 6);
		req[18] = 2;                            /* FAPI_VIFTYPE_STATION */
		memcpy(req + 20, station_address, 6);
	}
	svc_vif_cfm = -1;
	if (!svc_send(false, req, add ? 38 : 24, NULL, 0)) {
		return ENOBUFS;
	}
	for (int ms = 0; ms < 2000 && svc_vif_cfm < 0; ms += 10) {
		IOSleep(10);
	}
	svc_vif_up = add && svc_vif_cfm == 0;
	if (svc_vif_cfm != 0) {
		svc_event("MLME-%s-VIF.cfm: %d", add ? "ADD" : "DEL", svc_vif_cfm);
	}
	return svc_vif_cfm == 0 ? 0 : EIO;
}

/* ---- the station side: called on hakios.sta.c's thread, one at a time ---- */

static const uint16_t svc_channels_2g[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
static const uint16_t svc_channels_5g[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132,
	                                    136, 140, 149, 153, 157, 161, 165 };

static uint16_t
channel_mhz(uint16_t channel)
{
	return channel <= 14 ? (uint16_t)(2407 + 5 * channel) : (uint16_t)(5000 + 5 * channel);
}

static bool
channel_radar(uint16_t channel)
{
	return channel >= 52 && channel <= 140;
}

static unsigned
svc_op_channels(struct hakios_sta_channel *channels, unsigned most)
{
	unsigned n = 0;

	for (size_t i = 0; i < sizeof(svc_channels_2g) / 2 && n < most; i++, n++) {
		channels[n].frequency = channel_mhz(svc_channels_2g[i]);
		channels[n].flags = 0;
	}
	for (size_t i = 0; i < sizeof(svc_channels_5g) / 2 && n < most; i++, n++) {
		channels[n].frequency = channel_mhz(svc_channels_5g[i]);
		channels[n].flags = channel_radar(svc_channels_5g[i]) ? HAKIOS_STA_CHANNEL_PASSIVE | HAKIOS_STA_CHANNEL_RADAR : 0;
	}
	return n;
}

/* MLME-ADD-SCAN.req as step 6, over the channels asked for (all: none asked); with an SSID, an active scan for it. */
static int
svc_op_scan(const struct hakios_sta_scan *what)
{
	uint8_t req[36], data[260];
	uint32_t n = 0;
	/* always passive for now: an active scan wants the probe request's elements (rates, capabilities) that wpa_supplicant
	 * hands Samsung's driver, and the first one tried, without them, was followed by a silent firmware */
	bool active = false;

	bzero(req, sizeof(req));
	req[0] = MLME_ADD_SCAN_REQ & 0xff;
	req[1] = MLME_ADD_SCAN_REQ >> 8;
	req[4] = FAPI_SENDER & 0xff;
	req[5] = FAPI_SENDER >> 8;
	req[13] = 1;                                    /* scan id 0x100 */
	req[14] = 2;                                    /* full scan */
	memcpy(req + 16, station_address, 6);
	req[22] = 0x04;                                 /* real-time reports */
	if (active) {
		data[n++] = 0;
		data[n++] = (uint8_t)(what->ssid_length > 32 ? 32 : what->ssid_length);
		memcpy(data + n, what->ssid, data[n - 1]);
		n += data[n - 1];
	}
	static const uint8_t timing[] = { 0xdd, 0x11, 0x00, 0x16, 0x32, 0x01, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	memcpy(data + n, timing, sizeof(timing));
	n += sizeof(timing);
	uint32_t list = n;
	static const uint8_t list_header[] = { 0xdd, 0x05, 0x00, 0x16, 0x32, 0x01, 0x02 };
	memcpy(data + n, list_header, sizeof(list_header));
	n += sizeof(list_header);
	for (int band = 0; band < 2; band++) {
		const uint16_t *c = band ? svc_channels_5g : svc_channels_2g;
		size_t count = band ? sizeof(svc_channels_5g) / 2 : sizeof(svc_channels_2g) / 2;
		for (size_t i = 0; i < count; i++) {
			uint16_t mhz = channel_mhz(c[i]);
			bool wanted = what == NULL || what->frequency_count == 0;
			for (unsigned k = 0; what != NULL && k < what->frequency_count && k < 64; k++) {
				wanted |= what->frequencies[k] == mhz;
			}
			if (!wanted || n + 3 > sizeof(data)) {
				continue;
			}
			data[n++] = (uint8_t)((2 * mhz) & 0xff);
			data[n++] = (uint8_t)((2 * mhz) >> 8);
			data[n++] = active && !channel_radar(c[i]) ? 0x00 : 0x01;      /* FAPI_SCANPOLICY_PASSIVE */
		}
	}
	data[list + 1] = (uint8_t)(n - list - 2);
	if (n - list == sizeof(list_header)) {
		return EINVAL;                          /* none of the channels asked for */
	}

	scan_count = scan_frames = scan_other = 0;
	scan_cfm = -1;
	scan_done = 0;
	svc.scanning = true;
	enum hakios_sta_phase was = svc.phase;
	if (was == HAKIOS_STA_IDLE) {
		svc.phase = HAKIOS_STA_SCANNING;
	}
	if (!svc_send(false, req, sizeof(req), data, n)) {
		svc.scanning = false;
		svc.phase = was;
		return ENOBUFS;
	}
	for (int ms = 0; ms < 20000 && !scan_done; ms += 20) {
		if (scan_cfm > 0) {
			break;
		}
		IOSleep(20);
	}
	svc.scanning = false;
	if (svc.phase == HAKIOS_STA_SCANNING) {
		svc.phase = was;
	}
	svc_event("scan: %s, %u reports, %u networks", scan_cfm > 0 ? "refused" : scan_done ? "done" : "not done in 20 s",
	    scan_frames, scan_count);
	return scan_cfm > 0 ? EIO : 0;
}

static void
fill_bss(struct hakios_sta_bss *bss, uint32_t i)
{
	bzero(bss, sizeof(*bss));
	memcpy(bss->bssid, scan_seen[i].bssid, 6);
	bss->ssid_length = (uint8_t)strlen(scan_seen[i].ssid);
	memcpy(bss->ssid, scan_seen[i].ssid, bss->ssid_length);
	bss->frequency = scan_seen[i].mhz;
	bss->rssi = scan_seen[i].rssi;
	bss->beacon_interval = scan_seen[i].interval;
	bss->capability = scan_seen[i].capability;
	bss->age_ms = (uint32_t)(now_ms() - scan_seen[i].heard_ms);
	bss->elements_length = scan_seen[i].elements_length;
	memcpy(bss->elements, scan_seen[i].elements, bss->elements_length);
}

static void
svc_op_heard(void (*each)(const struct hakios_sta_bss *bss, void *context), void *context)
{
	static struct hakios_sta_bss bss;

	for (uint32_t i = 0; i < scan_count; i++) {
		fill_bss(&bss, i);
		each(&bss, context);
	}
}

static void
svc_disconnect(void)
{
	uint8_t req[32];

	bzero(req, sizeof(req));
	hdr(req, MLME_DISCONNECT_REQ);
	memcpy(req + 12, link.bssid, 6);
	req[18] = 3;                                    /* reason: the station is leaving */
	(void)svc_send(false, req, sizeof(req), NULL, 0);
	IOSleep(300);
}

static int
svc_op_leave(void)
{
	if (svc.phase == HAKIOS_STA_JOINED || svc.phase == HAKIOS_STA_JOINING) {
		svc.phase = HAKIOS_STA_IDLE;
		hakios_net_link_state(false);
		svc_disconnect();
	}
	link.connected = false;
	a51wpa_forget();
	if (svc_vif_up) {
		(void)svc_vif(false);
	}
	return 0;
}

static int svc_join_one(const struct hakios_sta_join *what, uint8_t *tried);

static int
svc_op_join(const struct hakios_sta_join *what)
{
	if (svc.phase == HAKIOS_STA_JOINED || svc.phase == HAKIOS_STA_JOINING) {
		(void)svc_op_leave();
	}
	/* the access point: of that name (and that BSSID, if one is given), heard best (5 GHz counted 15 dB better: step 7
	 * joined there, and the 2.4 GHz side answered NOT_ALLOWED); a scan first if none was heard; the next on a refusal */
	int error = ENOENT;
	static uint8_t tried[SCAN_MAX];
	bzero(tried, sizeof(tried));
	for (int attempt = 0; attempt < 3; attempt++) {
		error = svc_join_one(what, tried);
		if (error != ECONNREFUSED) {
			break;
		}
	}
	return error;
}

static int
svc_join_one(const struct hakios_sta_join *what, uint8_t *tried)
{
	static const uint8_t zero[6];
	uint8_t pmk[32], snonce[32];
	int best = -1;
	for (int pass = 0; pass < 2 && best < 0; pass++) {
		if (pass == 1) {
			struct hakios_sta_scan scan;
			bzero(&scan, sizeof(scan));
			memcpy(scan.ssid, what->ssid, what->ssid_length);
			scan.ssid_length = what->ssid_length;
			(void)svc_op_scan(&scan);
		}
		for (uint32_t i = 0; i < scan_count; i++) {
			int score = scan_seen[i].rssi + (scan_seen[i].mhz > 5000 ? 15 : 0);
			int best_score = best < 0 ? 0 : scan_seen[best].rssi + (scan_seen[best].mhz > 5000 ? 15 : 0);
			if (!tried[i] && strlen(scan_seen[i].ssid) == what->ssid_length &&
			    memcmp(scan_seen[i].ssid, what->ssid, what->ssid_length) == 0 &&
			    (memcmp(what->bssid, zero, 6) == 0 || memcmp(what->bssid, scan_seen[i].bssid, 6) == 0) &&
			    (best < 0 || score > best_score)) {
				best = (int)i;
			}
		}
	}
	if (best < 0) {
		svc_event("join: the network is not in the scan (%u heard, or every one tried)", scan_count);
		svc.last_error = ENOENT;
		return ENOENT;
	}
	tried[best] = 1;

	bzero(&link, sizeof(link));
	link.mbox = svc_mbox;
	link.connect_cfm = link.connect_ind = -1;
	link_m4_at = 0;
	memcpy(link.bssid, scan_seen[best].bssid, 6);
	link.open = what->security == HAKIOS_STA_OPEN;
	static const uint8_t rsn[22] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
		                         0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00 };
	if (!link.open) {
		const uint8_t *ap = scan_seen[best].rsn;
		uint32_t ap_length = scan_seen[best].rsn_length, at = 8;
		if (ap_length < 18) {
			svc.last_error = ENOTSUP;
			return ENOTSUP;
		}
		memcpy(link.cipher_group, ap + 4, 4);
		bool ccmp = rsn_has(ap, ap_length, &at, 4), psk_akm = rsn_has(ap, ap_length, &at, 2);
		uint16_t caps = at + 2 <= ap_length ? (uint16_t)(ap[at] | ap[at + 1] << 8) : 0;
		if (!ccmp || !psk_akm || link.cipher_group[3] != 4 || (caps & 0x40)) {
			printf("a51wifi: join: the network wants what is not handled (CCMP %d, PSK %d, group cipher %u, MFP required %d)\n",
			    ccmp, psk_akm, link.cipher_group[3], (caps & 0x40) != 0);
			svc.last_error = ENOTSUP;
			return ENOTSUP;
		}
		if (what->passphrase_length > 0) {
			a51wpa_pmk((const char *)what->passphrase, what->passphrase_length, what->ssid, what->ssid_length, pmk);
		} else {
			memcpy(pmk, what->pmk, 32);
		}
		read_random(snonce, sizeof(snonce));
		a51wpa_begin(pmk, link.bssid, station_address, snonce, rsn, sizeof(rsn));
		bzero(pmk, sizeof(pmk));
	}
	fill_bss(&svc.bss, (uint32_t)best);
	svc.last_error = 0;
	svc.phase = HAKIOS_STA_JOINING;
	if (svc_vif_lost && svc_vif_up) {
		(void)svc_vif(false);
	}
	svc_vif_lost = false;
	if (!svc_vif_up && svc_vif(true) != 0) {
		a51wpa_forget();
		svc.phase = HAKIOS_STA_IDLE;
		svc.last_error = EIO;
		return EIO;
	}

	uint8_t sig[34], data[64];
	uint32_t n = 0;
	bzero(sig, sizeof(sig));
	hdr(sig, MLME_CONNECT_REQ);
	memcpy(sig + 12, link.bssid, 6);
	sig[20] = (uint8_t)((scan_seen[best].mhz * 2) & 0xff);
	sig[21] = (uint8_t)((scan_seen[best].mhz * 2) >> 8);
	data[n++] = 0;
	data[n++] = what->ssid_length;
	memcpy(data + n, what->ssid, what->ssid_length);
	n += what->ssid_length;
	if (!link.open) {
		memcpy(data + n, rsn, sizeof(rsn));
		n += sizeof(rsn);
	}
	svc_event("join: MLME-CONNECT.req at %u MHz (%d dBm), %s", scan_seen[best].mhz, scan_seen[best].rssi,
	    link.open ? "open" : "WPA2");
	link_send(sig, sizeof(sig), data, n);

	int error = ETIMEDOUT;
	for (int ms = 0; ms < 20000; ms += 20) {
		if (link.connected) {
			error = 0;
			break;
		}
		if (link.wpa_result < 0) {
			error = EAUTH;                  /* message 3's MIC: the passphrase */
			break;
		}
		if (link.connect_cfm > 0 || link.connect_ind > 0) {
			error = ECONNREFUSED;
			break;
		}
		if (link.disconnected) {
			error = link.eapol ? EAUTH : ECONNREFUSED;     /* sent away during the handshake: most often the key */
			break;
		}
		IOSleep(20);
	}
	if (error != 0) {
		svc_event("join failed: %d (CONNECT.cfm %d, .ind %d, EAPOL %u, handshake %d%s%s, disconnected %u reason %u)",
		    error, link.connect_cfm, link.connect_ind, link.eapol, link.wpa_result, link.wpa_result < 0 ? ": " : "",
		    link.wpa_result < 0 ? a51wpa_failure() : "", link.disconnected, link.disconnect_reason);
		if (link.connect_ind == 0 && !link.disconnected) {
			svc_disconnect();
		}
		a51wpa_forget();
		(void)svc_vif(false);
		svc.phase = HAKIOS_STA_IDLE;
		svc.last_error = error;
		return error;
	}
	a51wpa_forget();
	svc.phase = HAKIOS_STA_JOINED;
	hakios_net_link_state(true);
	svc_event("joined (%s); en0's link is up", link.open ? "open" : "WPA2, keys set");
	return 0;
}

static int
svc_op_power(bool on)
{
	if (!on) {
		(void)svc_op_leave();
		svc.phase = HAKIOS_STA_OFF;
	} else if (svc.phase == HAKIOS_STA_OFF) {
		svc.phase = HAKIOS_STA_IDLE;
	}
	return 0;
}

static void
svc_op_state(struct hakios_sta_state *state)
{
	state->phase = svc.phase;
	state->bss = svc.bss;
	state->last_error = svc.last_error;
}

static struct hakios_sta_ops svc_ops = {
	.power = svc_op_power,
	.scan = svc_op_scan,
	.heard = svc_op_heard,
	.join = svc_op_join,
	.leave = svc_op_leave,
	.state = svc_op_state,
	.channels = svc_op_channels,
};

static struct hakios_net_link svc_link = {
	.name = "the Galaxy A51's radio (Samsung WLBT)",
	.wireless = true,
	.transmit = svc_transmit,
};

/* en0, its link down: as early as can be (Darwin's Wi-Fi side looks for the interface once, when it starts) */
static void
svc_attach_interface(void)
{
	memcpy(svc_link.address, station_address, 6);
	if (!hakios_net_attach(&svc_link)) {
		printf("a51wifi: EN0 HAS A LINK ALREADY: the radio is not Darwin's Wi-Fi\n");
	}
}

static void
svc_take(const uint8_t *sig, uint32_t sig_length, const uint8_t *data, uint32_t data_length)
{
	uint16_t id = sig_length >= 2 ? (uint16_t)(sig[0] | sig[1] << 8) : 0;

	if (id == MLME_ADD_SCAN_CFM || id == MLME_SCAN_IND || id == MLME_SCAN_DONE_IND) {
		scan_take(sig, sig_length, data, data_length);
		return;
	}
	if (id == MA_UNITDATA_CFM || id == MLME_DISCONNECT_CFM || id == 0x2316 /* event log */) {
		return;
	}
	if (id == MLME_ADD_VIF_CFM || id == MLME_DEL_VIF_CFM) {
		svc_vif_cfm = sig_length >= 14 ? (sig[12] | sig[13] << 8) : 0;
		return;
	}
	step7_take(sig, sig_length, data, data_length);
}

/* The driver's thread from here on. */
static void
svc_run(vm_offset_t mbox)
{
	char ssid[33], psk[64];
	uint8_t pmk[32];
	bool have_pmk;
	uint64_t said = now_ms();

	svc_mbox = mbox;
	bzero(ctl_used, sizeof(ctl_used));
	bzero(dat_used, sizeof(dat_used));
	ctl_used[0] = ctl_used[1] = 1;                  /* step 5's block (it comes back on FH_RFB like the others) */
	vm_offset_t h = wlbt_memory + hip_base;
	cache_out(h, 512);
	vm_offset_t config = rd(h + 4) == 5 ? h + HIP_V5 : h + HIP_V4;
	hip_headroom = *(volatile uint8_t *)(config + 28);
	hip_tailroom = *(volatile uint8_t *)(config + 29);
	bzero(&link, sizeof(link));
	link.mbox = mbox;
	svc.phase = HAKIOS_STA_IDLE;
	svc_vif_up = true;                              /* step 5's */
	svc_on = true;
	memcpy(svc_ops.address, station_address, 6);
	hakios_sta_attach(&svc_ops);
	wifi_said("service: the radio is Darwin's Wi-Fi (en0); headroom %u, tailroom %u", hip_headroom, hip_tailroom);

	if (wifi_conf(ssid, sizeof(ssid), psk, sizeof(psk), pmk, &have_pmk)) {
		struct hakios_sta_join *what = kalloc_data(sizeof(*what), Z_WAITOK | Z_ZERO);
		if (what != NULL) {
			what->ssid_length = (uint8_t)strlen(ssid);
			memcpy(what->ssid, ssid, what->ssid_length);
			what->security = HAKIOS_STA_WPA2_PSK;
			if (have_pmk) {
				memcpy(what->pmk, pmk, 32);
			} else {
				what->passphrase_length = (uint8_t)strlen(psk);
				memcpy(what->passphrase, psk, what->passphrase_length);
			}
			int error = hakios_sta_ask_join_standing(what);
			svc_event("the network of fw/wifi.conf asked for (a standing join): %d", error);
			bzero(what, sizeof(*what));
			kfree_data(what, sizeof(*what));
		}
	}
	bzero(psk, sizeof(psk));
	bzero(pmk, sizeof(pmk));

	uint32_t panic_seen = 0;
	uint64_t panic_checked = 0;
	if (runtime_panic_offset != 0 && runtime_panic_offset + 16 < WLBT_MEMORY) {
		cache_out(wlbt_memory + runtime_panic_offset, 16);
		panic_seen = *(volatile uint32_t *)(wlbt_memory + runtime_panic_offset + 8);
		svc_event("panic record at the start: %x", panic_seen);
	}
	for (;;) {
		if (now_ms() - panic_checked >= 1000 && runtime_panic_offset != 0) {
			panic_checked = now_ms();
			cache_out(wlbt_memory + runtime_panic_offset, 16);
			uint32_t p2 = *(volatile uint32_t *)(wlbt_memory + runtime_panic_offset + 8);
			uint32_t p3 = *(volatile uint32_t *)(wlbt_memory + runtime_panic_offset + 12);
			if (p2 != panic_seen) {
				panic_seen = p2;
				svc_event("FIRMWARE PANIC: log string %x, id %x (INTSR0 %x, INTMR0 %x)", p2, p3, rd(mbox + MBOX_INTSR0),
				    rd(mbox + MBOX_INTMR0));
			}
		}
		/* the to-host interrupt cleared first, as hip4's handler does (scsc_service_mifintrbit_bit_clear): the firmware
		 * re-raises one the host leaves set every 2 s and panics after 10 (mif_th.c:1638, HOST_NOT_RESPONSIVE) */
		wr(mbox + MBOX_INTCR0, 1u << (16 + HIP_TOHOST_BIT));     /* (unconditionally: the status reads masked) */
		uint32_t got = hip_take(mbox, Q_TH_CTRL, svc_take) + hip_take(mbox, Q_TH_DAT, svc_take);
		svc_reclaim();
		link_progress();
		if (svc.phase == HAKIOS_STA_JOINED && link.disconnected) {
			svc_event("the network was lost (reason %u)", link.disconnect_reason);
			svc.phase = HAKIOS_STA_IDLE;
			svc.last_error = ECONNRESET;
			link.connected = false;
			hakios_net_link_state(false);
			hakios_sta_lost(ECONNRESET);
			svc_vif_lost = true;            /* deleted and added again before the next join */
		}
		if (now_ms() - said >= 60000) {
			said = now_ms();
			/* the firmware's state: its panic record (word 2: its log string's offset, 3: the panic id), the queues */
			uint32_t pw[4] = { 0, 0, 0, 0 };
			if (runtime_panic_offset != 0 && runtime_panic_offset + 16 < WLBT_MEMORY) {
				cache_out(wlbt_memory + runtime_panic_offset, 16);
				memcpy(pw, (const void *)(wlbt_memory + runtime_panic_offset), 16);
			}
			vm_offset_t sb = wlbt_memory + hip_base + HIP_SCOREBOARD;
			cache_out(sb, 128);
			uint32_t busy_ctl = 0, busy_dat = 0;
			for (int i = 0; i < CTL_BLOCKS; i++) {
				busy_ctl += ctl_used[i];
			}
			for (int i = 0; i < DAT_BLOCKS; i++) {
				busy_dat += dat_used[i];
			}
			printf("a51wifi: service: panic record %x %x %x %x; queues host %02x %02x %02x %02x %02x %02x, firmware %02x %02x %02x "
			    "%02x %02x %02x; blocks busy: control %u, data %u; events: %s\n", pw[0], pw[1], pw[2], pw[3],
			    *(volatile uint8_t *)(sb + 0), *(volatile uint8_t *)(sb + 1), *(volatile uint8_t *)(sb + 2),
			    *(volatile uint8_t *)(sb + 3), *(volatile uint8_t *)(sb + 4), *(volatile uint8_t *)(sb + 5),
			    *(volatile uint8_t *)(sb + 64), *(volatile uint8_t *)(sb + 65), *(volatile uint8_t *)(sb + 66),
			    *(volatile uint8_t *)(sb + 67), *(volatile uint8_t *)(sb + 68), *(volatile uint8_t *)(sb + 69), busy_ctl, busy_dat,
			    svc_events[0] ? svc_events : "none");
			printf("a51wifi: service: phase %d; sent %llu frames (%llu bytes, %llu dropped; waited %u times: %u without a block, "
			    "%u a full queue; data blocks busy now %u, at most %u), received %llu (%llu bytes, %llu A-MSDU subframes, %llu "
			    "dropped); other signals: %s\n", svc.phase, svc.sent, svc.sent_bytes, svc.dropped_out, svc.waited, svc.no_block,
			    svc.queue_full, svc.dat_busy, svc.dat_busy_most, svc.received, svc.received_bytes, svc.amsdu, svc.dropped_in,
			    link.ids[0] ? link.ids : "none");
			link.ids[0] = 0;
		}
		if (got == 0) {
			IOSleep(2);
		}
	}
}

/* Step 5: MLME-ADD-VIF.req, and what comes back. */
static bool vif_added;

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
	vif_added = cfm && result == 0;
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
			if (wifi_steps >= 8 && vif_added) {
				svc_run(mbox);                  /* (does not return) */
			}
			if (wifi_steps >= 6 && vif_added) {
				char added[256];

				strlcpy(added, wifi_state, sizeof(added));
				IOSleep(100);
				wifi_step6(mbox);
				printf("a51wifi: (step 5 was: %s)\n", added);
				if (wifi_steps >= 7 && scan_count > 0) {
					char scanned[256];

					strlcpy(scanned, wifi_state, sizeof(scanned));
					IOSleep(100);
					wifi_step7(mbox);
					printf("a51wifi: (step 6 was: %s)\n", scanned);
				}
			}
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

	if (wifi_steps >= 8) {
		svc_attach_interface();
	}
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
