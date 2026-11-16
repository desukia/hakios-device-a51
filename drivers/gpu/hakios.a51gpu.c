/*
 * The Galaxy A51's GPU, an Arm Mali-G72 MP3 (Bifrost) in the Exynos 9611, woken step by step up to reading what it
 * says it is. Nothing here is touched unless boot-arg a51gpu=N asks; steps 1..N are made, each said in the log before
 * it is made, reads before writes, and a step that does not find what it needs ends the sequence.
 *
 * Where the numbers come from: the phone's device tree (mali@11500000, its 0x5000 bytes; pd-g3d@11864060 with
 * need_smc = <0x11410204>; gpu_pmu_status_reg_offset 0x4064, local power mask 0xf) and Samsung's kernel for it
 * (Exynos9611Development/android_kernel_samsung_universal9611): cal-if/exynos9610/pmucal_local_exynos9610.h
 * (g3d_on: PMU 0x11860000 + 0x4060 G3D_CONFIGURATION := 0xf, then wait for G3D_STATUS (+0x4064) & 0xf == 0xf),
 * cal-if/pmucal_local.c (after the "on" sequence, exynos_pd_tz_restore(need_smc)), exynos-el3_mon.c and
 * include/linux/smc.h (that is SMC 0x82000410, SMC_CMD_PREAPRE_PD_ONOFF, with EXYNOS_WAKEUP_PD_DOWN = 1, the address,
 * EXYNOS_PD_RUNTIME_PM = 0: the secure monitor restores the domain's TZPC settings, without which the normal world
 * may not reach its registers). The Mali registers are Arm's (Linux's panfrost_regs.h): GPU_ID at 0x000 ... .
 *
 *   1  reads   the power domain's status, and the GPU block's clock controller (CMU_G3D, 0x11430000) as found
 *   2  writes  the power domain on (G3D_CONFIGURATION), and waits for its status         -- skipped if it is on
 *   3  asks    the secure monitor to restore the domain's protection settings (SMC 0x82000410)
 *   4  reads   the GPU's own registers: GPU_ID, its features, which cores, tilers and L2 are present
 *   5  resets  the GPU (GPU_CMD soft reset), interrupts masked: all of this is polled
 *   6  powers  on the L2 cache, then the shader cores and the tiler
 *   7  builds  a Mali LPAE page table for one 16 KB test page and loads it into address space 0
 *   8  runs    a write-value job on job slot 1 and checks what the GPU wrote
 *
 * What this does not do (the next pieces): the GPU's PLL and voltage (Samsung's DVFS through ACPM), interrupts, and the
 * interface Mesa's Panfrost driver would draw through.
 *
 * Status: steps 1..8 run on the phone (2026-11-13): reset, all 3 cores on, and the write-value job wrote 0xC0FFEE11
 * through AS0 with no MMU fault. The kept GPU (a51gpu_keep=1) runs too: interrupts arrive, the
 * self-test's jobs (one into a heap that had to grow) pass, HakiosMali registers.
 */
#include <kern/clock.h>
#include <stdarg.h>
#include <kern/startup.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <mach/vm_param.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);

#define PMU_PHYS                0x11860000UL
#define G3D_CONFIGURATION       0x4060
#define G3D_STATUS              0x4064
#define G3D_LOCAL_PWR_MASK      0xfu

#define CMU_G3D_PHYS            0x11430000UL
#define PLL_CON0_MUX_G3D_SWITCH_USER 0x0100
#define PLL_CON0_PLL_G3D        0x0120
#define CLK_CON_MUX_G3D_BUSD    0x1004
#define CLK_CON_DIV_G3D_BUSP    0x1804

#define SMC_CMD_PREPARE_PD_ONOFF 0x82000410UL
#define EXYNOS_WAKEUP_PD_DOWN   1
#define EXYNOS_PD_RUNTIME_PM    0
#define G3D_NEED_SMC            0x11410204UL

#define MALI_PHYS               0x11500000UL
#define MALI_SIZE               0x5000
#define GPU_ID                  0x000
#define L2_FEATURES             0x004
#define TILER_FEATURES          0x00C
#define MEM_FEATURES            0x010
#define MMU_FEATURES            0x014
#define AS_PRESENT              0x018
#define JS_PRESENT              0x01C
#define GPU_IRQ_RAWSTAT         0x020
#define GPU_STATUS              0x034
#define SHADER_PRESENT_LO       0x100
#define TILER_PRESENT_LO        0x110
#define L2_PRESENT_LO           0x120

#define GPU_IRQ_CLEAR           0x024
#define GPU_IRQ_MASK            0x028
#define GPU_IRQ_RESET_COMPLETED (1u << 8)
#define GPU_CMD                 0x030
#define GPU_CMD_SOFT_RESET      0x01
#define GPU_FAULT_STATUS        0x03C
#define SHADER_READY_LO         0x140
#define TILER_READY_LO          0x150
#define L2_READY_LO             0x160
#define SHADER_PWRON_LO         0x180
#define TILER_PWRON_LO          0x190
#define L2_PWRON_LO             0x1A0
#define SHADER_PWRTRANS_LO      0x200
#define L2_PWRTRANS_LO          0x220

#define JOB_INT_RAWSTAT         0x1000
#define JOB_INT_CLEAR           0x1004
#define JOB_INT_MASK            0x1008
#define JS_SLOT(n, r)           (0x1800 + (n) * 0x80 + (r))
#define JS_HEAD_LO(n)           JS_SLOT(n, 0x00)
#define JS_STATUS(n)            JS_SLOT(n, 0x24)
#define JS_HEAD_NEXT_LO(n)      JS_SLOT(n, 0x40)
#define JS_HEAD_NEXT_HI(n)      JS_SLOT(n, 0x44)
#define JS_AFFINITY_NEXT_LO(n)  JS_SLOT(n, 0x50)
#define JS_AFFINITY_NEXT_HI(n)  JS_SLOT(n, 0x54)
#define JS_CONFIG_NEXT(n)       JS_SLOT(n, 0x58)
#define JS_COMMAND_NEXT(n)      JS_SLOT(n, 0x60)
#define JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE (3u << 8)
#define JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE   (3u << 12)
#define JS_CONFIG_THREAD_PRI(n) ((n) << 16)
#define JS_COMMAND_START        0x01

#define MMU_INT_RAWSTAT         0x2000
#define MMU_INT_CLEAR           0x2004
#define MMU_INT_MASK            0x2008
#define MMU_AS(as, r)           (0x2400 + ((as) << 6) + (r))
#define AS_TRANSTAB_LO(as)      MMU_AS(as, 0x00)
#define AS_TRANSTAB_HI(as)      MMU_AS(as, 0x04)
#define AS_MEMATTR_LO(as)       MMU_AS(as, 0x08)
#define AS_MEMATTR_HI(as)       MMU_AS(as, 0x0C)
#define AS_COMMAND(as)          MMU_AS(as, 0x18)
#define AS_FAULTSTATUS(as)      MMU_AS(as, 0x1C)
#define AS_FAULTADDRESS_LO(as)  MMU_AS(as, 0x20)
#define AS_STATUS(as)           MMU_AS(as, 0x28)
#define AS_COMMAND_UPDATE       0x01
#define AS_STATUS_AS_ACTIVE     0x01
#define AS_TRANSTAB_ADRMODE_TABLE 0x3
#define AS_TRANSTAB_READ_INNER  (1u << 2)

#define MALI_PTE_TABLE          3ull
#define MALI_PTE_LEAF           (1ull | 3ull << 6 | 2ull << 8)
#define MALI_MEMATTR            (0x88ull | 0x8Dull << 8 | 0x88ull << 16)
#define GPU_TEST_VA             0x1000000ull
#define TEST_JS                 1               /* Panfrost on Bifrost: slot 0 fragment jobs, slot 1 the rest */
#define TEST_VALUE              0xC0FFEE11u

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
	return (int64_t)(int32_t)x0;
}

/* Push what this processor wrote to memory out of its cache and forget it: the GPU does not look into this cache. */
static void
to_memory(vm_offset_t start, vm_size_t size)
{
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
}

/* Wait up to `us` microseconds for (register & mask) == want; the last value read is returned through *last. */
static bool
wait_for(vm_offset_t reg, uint32_t mask, uint32_t want, uint32_t us, uint32_t *last)
{
	uint32_t v = 0;

	for (uint32_t i = 0; i <= us; i++) {
		v = rd(reg);
		if ((v & mask) == want) {
			break;
		}
		delay(1);
	}
	if (last) {
		*last = v;
	}
	return (v & mask) == want;
}

/* One XNU page (16 KB here, 4 KB elsewhere) of zeroed kernel memory, and its physical address if it is in one piece. */
static vm_offset_t
gpu_page(vm_size_t size, uint64_t *phys)
{
	vm_offset_t va = 0;

	if (kmem_alloc(kernel_map, &va, round_page(size), KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		return 0;
	}
	*phys = kvtophys(va);
	for (vm_size_t off = 0; off < size; off += 4096) {
		if (kvtophys(va + off) != *phys + off) {
			printf("a51gpu: the memory at %lx is not in one piece\n", va);
			return 0;
		}
	}
	return va;
}

/*
 * Steps 5..8: reset, cores on, one address space, one job. The register names and values are Linux's panfrost
 * (panfrost_regs.h, panfrost_gpu.c, panfrost_mmu.c, panfrost_job.c), the page table format io-pgtable-arm.c's
 * ARM_MALI_LPAE, the job descriptor Mesa's panfrost genxml (v6.xml: Job Header, Write Value Job).
 */
static bool
a51gpu_jobs(vm_offset_t mali, uint32_t steps)
{
	uint32_t v;
	uint32_t shaders = rd(mali + SHADER_PRESENT_LO), tilers = rd(mali + TILER_PRESENT_LO), l2 = rd(mali + L2_PRESENT_LO);

	printf("a51gpu: step 5: soft reset (GPU_CMD := SOFT_RESET, wait for RESET_COMPLETED)\n");
	wr(mali + GPU_IRQ_MASK, 0);                     /* no interrupts: everything here is polled */
	wr(mali + JOB_INT_MASK, 0);
	wr(mali + MMU_INT_MASK, 0);
	wr(mali + GPU_IRQ_CLEAR, 0xffffffff);
	wr(mali + GPU_CMD, GPU_CMD_SOFT_RESET);
	if (!wait_for(mali + GPU_IRQ_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, 100000, &v)) {
		printf("a51gpu: no reset after 100 ms (GPU_IRQ_RAWSTAT %x): stopping\n", v);
		return false;
	}
	wr(mali + GPU_IRQ_CLEAR, 0xffffffff);
	wr(mali + JOB_INT_CLEAR, 0xffffffff);
	wr(mali + MMU_INT_CLEAR, 0xffffffff);
	printf("a51gpu: reset done (GPU_IRQ_RAWSTAT was %x), GPU_STATUS %x, GPU_ID %08x\n", v, rd(mali + GPU_STATUS),
	    rd(mali + GPU_ID));
	if (steps < 6) {
		return false;
	}

	printf("a51gpu: step 6: power on L2 %x, then shader cores %x and tiler %x\n", l2, shaders, tilers);
	wr(mali + L2_PWRON_LO, l2);
	if (!wait_for(mali + L2_READY_LO, l2, l2, 20000, &v)) {
		printf("a51gpu: L2 not ready after 20 ms (L2_READY %x, L2_PWRTRANS %x): stopping\n", v,
		    rd(mali + L2_PWRTRANS_LO));
		return false;
	}
	wr(mali + SHADER_PWRON_LO, shaders);
	wr(mali + TILER_PWRON_LO, tilers);
	bool sh = wait_for(mali + SHADER_READY_LO, shaders, shaders, 20000, NULL);
	bool ti = wait_for(mali + TILER_READY_LO, tilers, tilers, 20000, NULL);
	printf("a51gpu: ready: L2 %x, shader cores %x, tiler %x (GPU_IRQ_RAWSTAT %x)\n", rd(mali + L2_READY_LO),
	    rd(mali + SHADER_READY_LO), rd(mali + TILER_READY_LO), rd(mali + GPU_IRQ_RAWSTAT));
	if (!sh || !ti) {
		printf("a51gpu: not every core came on (SHADER_PWRTRANS %x): stopping\n", rd(mali + SHADER_PWRTRANS_LO));
		return false;
	}
	if (steps < 7) {
		return false;
	}

	/*
	 * One 4-level table walk (Mali always starts at level 0, 4 KB granule) down to four 4 KB pages at GPU address
	 * GPU_TEST_VA, backed by one 16 KB page of ours. Tables: PA | 3. Leaves: PA | 1 (a "block" even at level 3,
	 * Mali's way), read and write (HAP 3 << 6), outer shareable (2 << 8), attribute index 0 (MEMATTR 0x88, what
	 * Panfrost uses for memory the GPU and processor do not keep coherent).
	 */
	printf("a51gpu: step 7: page tables and address space 0\n");
	uint64_t tables_pa, data_pa;
	vm_offset_t tables = gpu_page(0x4000, &tables_pa);
	vm_offset_t data = gpu_page(0x4000, &data_pa);
	if (!tables || !data) {
		printf("a51gpu: no memory for the tables or the job: stopping\n");
		return false;
	}
	uint64_t *l0 = (uint64_t *)tables, *l1 = (uint64_t *)(tables + 0x1000);
	uint64_t *l2t = (uint64_t *)(tables + 0x2000), *l3 = (uint64_t *)(tables + 0x3000);
	l0[(GPU_TEST_VA >> 39) & 511] = (tables_pa + 0x1000) | MALI_PTE_TABLE;
	l1[(GPU_TEST_VA >> 30) & 511] = (tables_pa + 0x2000) | MALI_PTE_TABLE;
	l2t[(GPU_TEST_VA >> 21) & 511] = (tables_pa + 0x3000) | MALI_PTE_TABLE;
	for (uint32_t i = 0; i < 4; i++) {
		l3[((GPU_TEST_VA >> 12) & 511) + i] = (data_pa + i * 0x1000) | MALI_PTE_LEAF;
	}
	to_memory(tables, 0x4000);
	uint64_t transtab = tables_pa | AS_TRANSTAB_READ_INNER | AS_TRANSTAB_ADRMODE_TABLE;
	printf("a51gpu: tables at %llx, test page at %llx -> GPU %llx; TRANSTAB %llx, MEMATTR %llx\n", tables_pa, data_pa,
	    (uint64_t)GPU_TEST_VA, transtab, (uint64_t)MALI_MEMATTR);
	wr(mali + AS_TRANSTAB_LO(0), (uint32_t)transtab);
	wr(mali + AS_TRANSTAB_HI(0), (uint32_t)(transtab >> 32));
	wr(mali + AS_MEMATTR_LO(0), (uint32_t)MALI_MEMATTR);
	wr(mali + AS_MEMATTR_HI(0), (uint32_t)(MALI_MEMATTR >> 32));
	wr(mali + AS_COMMAND(0), AS_COMMAND_UPDATE);
	if (!wait_for(mali + AS_STATUS(0), AS_STATUS_AS_ACTIVE, 0, 10000, &v)) {
		printf("a51gpu: address space 0 still busy after 10 ms (AS_STATUS %x): stopping\n", v);
		return false;
	}
	printf("a51gpu: address space 0 set (AS_STATUS %x, MMU_INT_RAWSTAT %x)\n", v, rd(mali + MMU_INT_RAWSTAT));
	if (steps < 8) {
		return false;
	}

	/*
	 * A write-value job: the GPU writes 0xC0FFEE11 at GPU_TEST_VA + 0x100 and records in the job's header that it
	 * finished. Job header (32 bytes): word 4 = 64-bit descriptor (bit 0) | type 2 "write value" (bits 1..7) |
	 * index 1 (bits 16..31). Payload at +32: address (64 bits), type 6 "immediate 32", the value at +48.
	 */
	printf("a51gpu: step 8: a write-value job on job slot %u\n", TEST_JS);
	volatile uint32_t *job = (volatile uint32_t *)data;
	volatile uint32_t *result = (volatile uint32_t *)(data + 0x100);
	*result = 0x5a5a5a5a;
	job[4] = 1u | (2u << 1) | (1u << 16);
	job[8] = (uint32_t)(GPU_TEST_VA + 0x100);
	job[9] = (uint32_t)((GPU_TEST_VA + 0x100) >> 32);
	job[10] = 6;
	job[12] = TEST_VALUE;
	job[13] = 0;
	to_memory(data, 0x200);

	wr(mali + JOB_INT_CLEAR, 0xffffffff);
	wr(mali + JS_HEAD_NEXT_LO(TEST_JS), (uint32_t)GPU_TEST_VA);
	wr(mali + JS_HEAD_NEXT_HI(TEST_JS), (uint32_t)(GPU_TEST_VA >> 32));
	wr(mali + JS_AFFINITY_NEXT_LO(TEST_JS), shaders);
	wr(mali + JS_AFFINITY_NEXT_HI(TEST_JS), 0);
	wr(mali + JS_CONFIG_NEXT(TEST_JS), 0 /* address space */ | JS_CONFIG_THREAD_PRI(8) |
	    JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE | JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE);
	wr(mali + JS_COMMAND_NEXT(TEST_JS), JS_COMMAND_START);

	uint32_t mask = (1u << TEST_JS) | (1u << (TEST_JS + 16));
	bool finished = false;                          /* done or failed, whichever comes */
	for (uint32_t i = 0; i <= 100000 && !finished; i++) {
		v = rd(mali + JOB_INT_RAWSTAT);
		finished = (v & mask) != 0;
		if (!finished) {
			delay(1);
		}
	}
	to_memory(data, 0x200);                         /* forget what the cache held: read what the GPU wrote */
	printf("a51gpu: JOB_INT_RAWSTAT %x (done bit %u, failed bit %u), JS_STATUS %x, JS_HEAD %x\n", v, TEST_JS,
	    TEST_JS + 16, rd(mali + JS_STATUS(TEST_JS)), rd(mali + JS_HEAD_LO(TEST_JS)));
	printf("a51gpu: job header: exception status %x, first incomplete task %x; result %x (wanted %x)\n", job[0],
	    job[1], *result, TEST_VALUE);
	printf("a51gpu: MMU_INT_RAWSTAT %x, AS0 fault status %x at %x, GPU_FAULT_STATUS %x, GPU_IRQ_RAWSTAT %x\n",
	    rd(mali + MMU_INT_RAWSTAT), rd(mali + AS_FAULTSTATUS(0)), rd(mali + AS_FAULTADDRESS_LO(0)),
	    rd(mali + GPU_FAULT_STATUS), rd(mali + GPU_IRQ_RAWSTAT));
	bool ran = finished && (v & (1u << TEST_JS)) && *result == TEST_VALUE;
	if (ran) {
		printf("a51gpu: the GPU ran a job: it wrote %x through its own MMU\n", *result);
	} else {
		printf("a51gpu: the job did not finish as hoped\n");
	}
	wr(mali + JOB_INT_CLEAR, 0xffffffff);
	return ran;
}

/* ---- the GPU kept for programs (a51gpu=8 a51gpu_keep=1) ----------------------------------------------------------- */

/*
 * After step 8 has shown that the GPU runs jobs, boot-arg a51gpu_keep=1 keeps it: powered, with one address space of
 * its own (AS0) that all programs' buffers are in, a queue of job chains, and the service HakiosMali
 * (iokit/Kernel/hakios.mali.cpp) that hands it to programs through the interface of Linux's panfrost driver, which
 * Mesa's Panfrost speaks. What is the GPU's is here; buffers and clients are there.
 *
 * One thread does everything that touches the GPU after this ("the worker"): it starts the next job chain when one is
 * done, maps the growable heap's memory when the GPU faults on it, and resets the GPU after a job that failed. The
 * GPU's three interrupts (dt: JOB 98, MMU 99, GPU 97; edge, as Linux has them) only mask their line and wake it. It
 * also looks every few milliseconds while a job runs, so it works without the interrupts too, only slower; the log
 * says once whether each interrupt arrives.
 *
 * Job chains run one at a time, in the order they were given, on slot 0 (fragment, PANFROST_JD_REQ_FS) or slot 1 (all
 * else). That keeps every dependency Mesa can express (a fragment chain after the vertex and tiler chain of the same
 * frame) without looking at any. A submission's number is its fence: done when `mali_completed` has reached it.
 */
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <kern/sched_prim.h>
#include <mach/mach_time.h>

#define IRQ_GPU                 97
#define IRQ_JOB                 98
#define IRQ_MMU                 99
#define GPU_IRQ_FAULT           (1u << 0)
#define GPU_IRQ_MULTIPLE_FAULT  (1u << 7)
#define GPU_FAULT_ADDRESS_LO    0x040
#define GPU_FAULT_ADDRESS_HI    0x044
#define CORE_FEATURES           0x008
#define AFBC_FEATURES           0x04C
#define THREAD_MAX_THREADS      0x0A0
#define THREAD_MAX_WORKGROUP    0x0A4
#define THREAD_MAX_BARRIER      0x0A8
#define THREAD_FEATURES         0x0AC
#define TEXTURE_FEATURES(n)     (0x0B0 + (n) * 4)
#define JS_FEATURES(n)          (0x0C0 + (n) * 4)
#define COHERENCY_FEATURES      0x300
#define THREAD_TLS_ALLOC        0x310
#define STACK_PRESENT_LO        0xE00
#define JS_HEAD_HI(n)           JS_SLOT(n, 0x04)
#define JS_COMMAND(n)           JS_SLOT(n, 0x20)
#define JS_COMMAND_HARD_STOP    0x03
#define AS_LOCKADDR_LO(as)      MMU_AS(as, 0x10)
#define AS_LOCKADDR_HI(as)      MMU_AS(as, 0x14)
#define AS_FAULTADDRESS_HI(as)  MMU_AS(as, 0x24)
#define AS_COMMAND_LOCK         0x02
#define AS_COMMAND_FLUSH_PT     0x04
#define MALI_PTE_XN             (3ull << 53)
#define MALI_VA_START           (32ull << 20)   /* Mesa: "lower 32MB are reserved, and the address space is 32-bit" */
#define MALI_VA_END             (4ull << 30)
#define MALI_QUEUE              256             /* a window server's frame is a few chains; it may run ahead by a few frames */
#define MALI_JOB_TIMEOUT_MS     2000
#define PANFROST_JD_REQ_FS      1u

extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
extern void gic_v3_spi_mask(uint32_t intid);
extern void gic_v3_spi_unmask(uint32_t intid);
extern uint64_t gic_v3_spi_count(uint32_t intid);

/* iokit/Kernel/hakios.mali.cpp */
extern void HakiosMaliStart(void);
extern bool HakiosMaliHeapGrow(uint64_t address);
extern const char *HakiosMaliSelfTest(void);

static LCK_GRP_DECLARE(mali_lock_group, "hakios-mali");
static LCK_MTX_DECLARE(mali_lock, &mali_lock_group);
static vm_offset_t kmali;
static bool mali_kept;
static uint32_t mali_log;                       /* boot-arg a51gpu_log=1: a line per submission */
static uint32_t mali_param_value[64];           /* the answers to GET_PARAM, read once */
static uint64_t mali_shaders, mali_tilers, mali_l2;

/* the address space: Mali LPAE, 4 levels, 4 KB; addresses below 4 GB, so one level-0 and one level-1 table */
static uint64_t mali_transtab;
static uint64_t *mali_l0, *mali_l1;
static uint64_t *mali_l2t[4];
static uint64_t *mali_l3t[4][512];
static vm_offset_t mali_table_page;             /* where the next 4 KB table comes from */
static uint64_t mali_table_page_pa;
static uint32_t mali_table_used = 4;
static bool mali_as_faulty;                     /* AS0 switched off after a fault nobody could mend: on again before the next job */

/* addresses of the GPU's not in use, in order */
static struct {
	uint64_t start, size;
} mali_free[1024];
static uint32_t mali_free_count;

/* the queue */
static struct {
	uint64_t jc;
	uint32_t requirements, fence;
} mali_queue[MALI_QUEUE];
static uint32_t mali_queue_head, mali_queue_count;
static uint32_t mali_submitted, mali_completed, mali_failed;
static uint32_t mali_running;                   /* the fence of the chain on the GPU; 0: none */
static uint32_t mali_running_slot;
static uint64_t mali_running_since;
/* Where the GPU's time goes, said with each status line: chains on the GPU, and the gaps between one done and the next
 * started while others waited (the interrupt, the worker waking, the start). */
static uint64_t mali_busy_abs, mali_gap_abs, mali_done_at;
static uint32_t mali_busy_jobs, mali_gap_jobs;
static bool mali_done_waiting;
static uint64_t mali_running_jc;
static bool mali_reset_wanted;
static uint32_t mali_irq_pending;               /* bits 0 GPU, 1 JOB, 2 MMU: lines that fired and wait for the worker */
static bool mali_irq_seen[3];
static uint32_t mali_resets, mali_heap_faults;
/* the last troubles, said again in the status line (the kernel log of a whole system's start does not keep them) */
static char mali_trouble[4][200];
static char mali_clock_state[240];             /* what the clock steps found (below) */
static uint32_t mali_trouble_count;

static void
trouble(const char *format, ...) __printflike(1, 2);

static void
trouble(const char *format, ...)
{
	va_list ap;
	char *at = mali_trouble[mali_trouble_count++ % 4];

	va_start(ap, format);
	vsnprintf(at, sizeof(mali_trouble[0]), format, ap);
	va_end(ap);
	printf("a51gpu: %s\n", at);
}

static inline uint32_t
mrd(uint32_t r)
{
	return rd(kmali + r);
}

static inline void
mwr(uint32_t r, uint32_t v)
{
	wr(kmali + r, v);
}

static bool
fence_reached(uint32_t done, uint32_t fence)
{
	return (int32_t)(done - fence) >= 0;
}

/* ---- the address space ---- */

static uint64_t *
mali_table(uint64_t *pa)
{
	if (mali_table_used == 4) {
		uint64_t page_pa;
		vm_offset_t page = gpu_page(0x4000, &page_pa);

		if (!page) {
			return NULL;
		}
		mali_table_page = page;
		mali_table_page_pa = page_pa;
		mali_table_used = 0;
	}
	*pa = mali_table_page_pa + mali_table_used * 0x1000;
	return (uint64_t *)(mali_table_page + 0x1000 * mali_table_used++);
}

static void
pte_set(uint64_t *pte, uint64_t value)
{
	*pte = value;
	__asm__ volatile ("dc civac, %0" : : "r" (pte));
}

/* The level-3 entry for `va`, making the tables on the way. With the lock held. */
static uint64_t *
mali_pte(uint64_t va, bool make)
{
	uint32_t i1 = (uint32_t)(va >> 30) & 3, i2 = (uint32_t)(va >> 21) & 511;
	uint64_t pa;

	if (mali_l2t[i1] == NULL) {
		if (!make || (mali_l2t[i1] = mali_table(&pa)) == NULL) {
			return NULL;
		}
		to_memory((vm_offset_t)mali_l2t[i1], 0x1000);
		pte_set(&mali_l1[i1], pa | MALI_PTE_TABLE);
	}
	if (mali_l3t[i1][i2] == NULL) {
		if (!make || (mali_l3t[i1][i2] = mali_table(&pa)) == NULL) {
			return NULL;
		}
		to_memory((vm_offset_t)mali_l3t[i1][i2], 0x1000);
		pte_set(&mali_l2t[i1][i2], pa | MALI_PTE_TABLE);
	}
	return &mali_l3t[i1][i2][(va >> 12) & 511];
}

static bool
as_wait(void)
{
	uint32_t v;

	if (!wait_for(kmali + AS_STATUS(0), AS_STATUS_AS_ACTIVE, 0, 10000, &v)) {
		printf("a51gpu: address space 0 stays busy (AS_STATUS %x)\n", v);
		return false;
	}
	return true;
}

static void
as_command(uint32_t command)
{
	as_wait();
	mwr(AS_COMMAND(0), command);
}

/* Panfrost's lock_region() and FLUSH_PT: the GPU forgets what it knew of these entries (and a stalled access retries). */
static void
as_flush(uint64_t va, uint64_t size)
{
	uint64_t end = va + size, region;
	uint32_t width = 64 - __builtin_clzll(va ^ (end - 1));

	if (size == 0 || mali_as_faulty) {
		return;
	}
	if (width < 15) {
		width = 15;                     /* AS_LOCK_REGION_MIN_SIZE: 32 KB */
	}
	width -= 1;
	region = (va & ~((1ull << width) - 1)) | width;
	mwr(AS_LOCKADDR_LO(0), (uint32_t)region);
	mwr(AS_LOCKADDR_HI(0), (uint32_t)(region >> 32));
	as_command(AS_COMMAND_LOCK);
	as_command(AS_COMMAND_FLUSH_PT);
	as_wait();
}

static void
as_enable(void)
{
	mwr(AS_TRANSTAB_LO(0), (uint32_t)mali_transtab);
	mwr(AS_TRANSTAB_HI(0), (uint32_t)(mali_transtab >> 32));
	mwr(AS_MEMATTR_LO(0), (uint32_t)MALI_MEMATTR);
	mwr(AS_MEMATTR_HI(0), (uint32_t)(MALI_MEMATTR >> 32));
	as_command(AS_COMMAND_UPDATE);
	as_wait();
	mali_as_faulty = false;
}

/* What Panfrost does with a fault nobody can mend: the address space off, so the job ends in a fault and not a hang. */
static void
as_disable(void)
{
	mwr(AS_TRANSTAB_LO(0), 0);
	mwr(AS_TRANSTAB_HI(0), 0);
	mwr(AS_MEMATTR_LO(0), 0);
	mwr(AS_MEMATTR_HI(0), 0);
	as_command(AS_COMMAND_UPDATE);
	as_wait();
	mali_as_faulty = true;
}

/*
 * `size` bytes of physical memory from `phys` on at GPU address `va` (both 4 KB aligned), read and write, executable
 * unless `noexec`. Not seen by the GPU until hakios_mali_mapped() for the range.
 */
bool
hakios_mali_map(uint64_t va, uint64_t phys, uint64_t size, bool noexec)
{
	bool ok = true;

	lck_mtx_lock(&mali_lock);
	for (uint64_t off = 0; off < size; off += 4096) {
		uint64_t *pte = mali_pte(va + off, true);

		if (pte == NULL) {
			ok = false;
			break;
		}
		pte_set(pte, (phys + off) | MALI_PTE_LEAF | (noexec ? MALI_PTE_XN : 0));
	}
	__asm__ volatile ("dsb sy");
	lck_mtx_unlock(&mali_lock);
	return ok;
}

void
hakios_mali_mapped(uint64_t va, uint64_t size)
{
	lck_mtx_lock(&mali_lock);
	as_flush(va, size);
	lck_mtx_unlock(&mali_lock);
}

void
hakios_mali_unmap(uint64_t va, uint64_t size)
{
	lck_mtx_lock(&mali_lock);
	for (uint64_t off = 0; off < size; off += 4096) {
		uint64_t *pte = mali_pte(va + off, false);

		if (pte != NULL && *pte != 0) {
			pte_set(pte, 0);
		}
	}
	__asm__ volatile ("dsb sy");
	as_flush(va, size);
	lck_mtx_unlock(&mali_lock);
}

/*
 * A stretch of the GPU's addresses: `align` a power of two (at least 4 KB); an executable one does not cross a 16 MB
 * line (Panfrost's rule: the shader program counter has 24 bits). 0: none left.
 */
uint64_t
hakios_mali_address(uint64_t size, uint64_t align, bool exec)
{
	uint64_t found = 0;

	size = (size + 4095) & ~4095ull;
	if (align < 4096) {
		align = 4096;
	}
	if (size == 0 || (exec && size > (16ull << 20))) {
		return 0;
	}
	lck_mtx_lock(&mali_lock);
	for (uint32_t i = 0; i < mali_free_count && !found; i++) {
		uint64_t start = (mali_free[i].start + align - 1) & ~(align - 1), end = mali_free[i].start + mali_free[i].size;

		if (exec && (start >> 24) != ((start + size - 1) >> 24)) {
			start = ((start >> 24) + 1) << 24;
		}
		if (start + size > end) {
			continue;
		}
		found = start;
		/* the stretch in the middle of free range i: what is before and after it stays free */
		uint64_t before = start - mali_free[i].start, after = end - (start + size);
		if (before && after) {
			if (mali_free_count == sizeof(mali_free) / sizeof(mali_free[0])) {
				found = 0;
				break;
			}
			memmove(&mali_free[i + 2], &mali_free[i + 1], (mali_free_count - i - 1) * sizeof(mali_free[0]));
			mali_free_count++;
			mali_free[i].size = before;
			mali_free[i + 1].start = start + size;
			mali_free[i + 1].size = after;
		} else if (before) {
			mali_free[i].size = before;
		} else if (after) {
			mali_free[i].start = start + size;
			mali_free[i].size = after;
		} else {
			memmove(&mali_free[i], &mali_free[i + 1], (mali_free_count - i - 1) * sizeof(mali_free[0]));
			mali_free_count--;
		}
	}
	lck_mtx_unlock(&mali_lock);
	return found;
}

void
hakios_mali_address_free(uint64_t start, uint64_t size)
{
	uint32_t i;

	size = (size + 4095) & ~4095ull;
	lck_mtx_lock(&mali_lock);
	for (i = 0; i < mali_free_count && mali_free[i].start < start; i++) {
	}
	if (i > 0 && mali_free[i - 1].start + mali_free[i - 1].size == start) {
		mali_free[i - 1].size += size;
		if (i < mali_free_count && start + size == mali_free[i].start) {
			mali_free[i - 1].size += mali_free[i].size;
			memmove(&mali_free[i], &mali_free[i + 1], (mali_free_count - i - 1) * sizeof(mali_free[0]));
			mali_free_count--;
		}
	} else if (i < mali_free_count && start + size == mali_free[i].start) {
		mali_free[i].start = start;
		mali_free[i].size += size;
	} else if (mali_free_count < sizeof(mali_free) / sizeof(mali_free[0])) {
		memmove(&mali_free[i + 1], &mali_free[i], (mali_free_count - i) * sizeof(mali_free[0]));
		mali_free[i].start = start;
		mali_free[i].size = size;
		mali_free_count++;
	}                                       /* (else the stretch is lost: 1024 holes are not expected) */
	lck_mtx_unlock(&mali_lock);
}

/* ---- jobs ---- */

/* With the lock held: the next chain onto its slot, if the GPU is free. */
static void
mali_start_next(void)
{
	if (mali_running != 0 || mali_queue_count == 0 || mali_reset_wanted) {
		return;
	}
	uint64_t jc = mali_queue[mali_queue_head].jc;
	uint32_t slot = (mali_queue[mali_queue_head].requirements & PANFROST_JD_REQ_FS) ? 0 : 1;

	mali_running = mali_queue[mali_queue_head].fence;
	mali_queue_head = (mali_queue_head + 1) % MALI_QUEUE;
	mali_queue_count--;
	if (mali_as_faulty) {
		as_enable();
	}
	mali_running_slot = slot;
	mali_running_jc = jc;
	mali_running_since = mach_absolute_time();
	if (mali_done_waiting) {
		mali_gap_abs += mali_running_since - mali_done_at;
		mali_gap_jobs++;
		mali_done_waiting = false;
	}
	mwr(JS_HEAD_NEXT_LO(slot), (uint32_t)jc);
	mwr(JS_HEAD_NEXT_HI(slot), (uint32_t)(jc >> 32));
	mwr(JS_AFFINITY_NEXT_LO(slot), (uint32_t)mali_shaders);
	mwr(JS_AFFINITY_NEXT_HI(slot), (uint32_t)(mali_shaders >> 32));
	mwr(JS_CONFIG_NEXT(slot), 0 /* AS0 */ | JS_CONFIG_THREAD_PRI(8) | JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE |
	    JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE);
	mwr(JS_COMMAND_NEXT(slot), JS_COMMAND_START);
	if (mali_log) {
		printf("a51gpu: job %u started on slot %u (chain at %llx)\n", mali_running, slot, jc);
	}
}

/* With the lock held: the chain on the GPU is over, as it went. */
static void
mali_retire(bool ok)
{
	if (!ok) {
		mali_failed++;
	}
	mali_done_at = mach_absolute_time();
	mali_busy_abs += mali_done_at - mali_running_since;
	mali_busy_jobs++;
	mali_done_waiting = mali_queue_count != 0;
	mali_completed = mali_running;
	mali_running = 0;
	thread_wakeup((event_t)&mali_completed);
}

/* A chain to run after those given before it. Its fence, or 0 when the queue is full or the GPU is not kept. */
uint32_t
hakios_mali_submit(uint64_t jc, uint32_t requirements)
{
	uint32_t fence = 0;

	lck_mtx_lock(&mali_lock);
	if (mali_kept && mali_queue_count < MALI_QUEUE && jc != 0) {
		uint32_t at = (mali_queue_head + mali_queue_count) % MALI_QUEUE;

		fence = ++mali_submitted;
		if (fence == 0) {
			fence = mali_submitted = 1;     /* (0 means none) */
		}
		mali_queue[at].jc = jc;
		mali_queue[at].requirements = requirements;
		mali_queue[at].fence = fence;
		mali_queue_count++;
		mali_start_next();
	}
	lck_mtx_unlock(&mali_lock);
	return fence;
}

uint32_t
hakios_mali_completed(void)
{
	return os_atomic_load(&mali_completed, relaxed);
}

uint32_t
hakios_mali_submitted(void)
{
	return os_atomic_load(&mali_submitted, relaxed);
}

/* Wait until chain `fence` is over; `deadline` in mach_absolute_time() (0: do not wait). false: not by then. */
bool
hakios_mali_wait(uint32_t fence, uint64_t deadline)
{
	bool done;

	lck_mtx_lock(&mali_lock);
	while (!(done = fence == 0 || fence_reached(mali_completed, fence)) && deadline > mach_absolute_time()) {
		lck_mtx_sleep_deadline(&mali_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_completed, THREAD_UNINT, deadline);
	}
	lck_mtx_unlock(&mali_lock);
	return done;
}

bool
hakios_mali_ready(void)
{
	return mali_kept;
}

/* The answers to GET_PARAM (Linux: panfrost_ioctl_get_param(), from pfdev->features). */
bool
hakios_mali_param(uint32_t which, uint64_t *value)
{
	if (which >= sizeof(mali_param_value) / sizeof(mali_param_value[0]) || which == 41 || which == 42) {
		return false;                   /* (41, 42: the system timestamp, interface 1.3; this is 1.1) */
	}
	*value = mali_param_value[which];
	return true;
}

/* ---- the worker ---- */

static void
mali_power_on(void)
{
	mwr(L2_PWRON_LO, (uint32_t)mali_l2);
	wait_for(kmali + L2_READY_LO, (uint32_t)mali_l2, (uint32_t)mali_l2, 20000, NULL);
	mwr(SHADER_PWRON_LO, (uint32_t)mali_shaders);
	mwr(TILER_PWRON_LO, (uint32_t)mali_tilers);
	wait_for(kmali + SHADER_READY_LO, (uint32_t)mali_shaders, (uint32_t)mali_shaders, 20000, NULL);
	wait_for(kmali + TILER_READY_LO, (uint32_t)mali_tilers, (uint32_t)mali_tilers, 20000, NULL);
}

static void
mali_irq_masks(void)
{
	mwr(GPU_IRQ_CLEAR, 0xffffffff);
	mwr(JOB_INT_CLEAR, 0xffffffff);
	mwr(MMU_INT_CLEAR, 0xffffffff);
	mwr(GPU_IRQ_MASK, GPU_IRQ_FAULT | GPU_IRQ_MULTIPLE_FAULT);
	mwr(JOB_INT_MASK, 0x00030003);          /* slots 0 and 1: done and failed */
	mwr(MMU_INT_MASK, 0x00010001);          /* AS0: page fault and bus error */
}

/* With the lock held: the GPU reset and brought back as it was, the chain on it (if any) failed. */
static void
mali_reset(const char *why)
{
	uint32_t v;

	mali_resets++;
	printf("a51gpu: resetting the GPU (%s; reset %u)\n", why, mali_resets);
	mwr(GPU_IRQ_MASK, 0);
	mwr(JOB_INT_MASK, 0);
	mwr(MMU_INT_MASK, 0);
	mwr(GPU_IRQ_CLEAR, 0xffffffff);
	mwr(GPU_CMD, GPU_CMD_SOFT_RESET);
	if (!wait_for(kmali + GPU_IRQ_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, 100000, &v)) {
		printf("a51gpu: no reset after 100 ms (GPU_IRQ_RAWSTAT %x)\n", v);
	}
	mali_power_on();
	as_enable();
	mali_irq_masks();
	if (mali_running) {
		mali_retire(false);
	}
	mali_reset_wanted = false;
}

static const char *
exception_name(uint32_t code)
{
	switch (code) {
	case 0x00: return "not started";
	case 0x01: return "done";
	case 0x03: return "stopped";
	case 0x40: return "job config fault";
	case 0x41: return "job power fault";
	case 0x42: return "job read fault";
	case 0x43: return "job write fault";
	case 0x44: return "job affinity fault";
	case 0x48: return "job bus fault";
	case 0x50: return "instruction invalid";
	case 0x51: return "instruction type mismatch";
	case 0x52: return "operand invalid";
	case 0x53: return "TLS fault";
	case 0x54: return "barrier fault";
	case 0x55: return "alignment fault";
	case 0x58: return "data invalid fault";
	case 0x59: return "tile range fault";
	case 0x5a: return "address range fault";
	case 0x60: return "out of memory";
	case 0x80: return "delayed bus fault";
	case 0x88: return "shareability fault";
	default:
		if ((code & 0xf8) == 0xc0) {
			return "translation fault";
		}
		if ((code & 0xf8) == 0xc8) {
			return "permission fault";
		}
		if ((code & 0xf8) == 0xd8) {
			return "access flag fault";
		}
		return "?";
	}
}

/* With the lock held. */
static void
mali_jobs_check(void)
{
	uint32_t raw = mrd(JOB_INT_RAWSTAT) & 0x00030003;

	if (raw == 0) {
		return;
	}
	mwr(JOB_INT_CLEAR, raw);
	if (mali_running == 0) {
		return;
	}
	uint32_t slot = mali_running_slot;
	if (raw & (1u << (slot + 16))) {
		uint32_t status = mrd(JS_STATUS(slot));

		trouble("job %u (chain %llx) FAILED on slot %u: %s (JS_STATUS %x), at %x%08x", mali_running,
		    mali_running_jc, slot, exception_name(status & 0xff), status, mrd(JS_HEAD_HI(slot)), mrd(JS_HEAD_LO(slot)));
		mali_reset_wanted = true;
		mali_reset("a job failed");
	} else if (raw & (1u << slot)) {
		if (mali_log) {
			printf("a51gpu: job %u done\n", mali_running);
		}
		mali_retire(true);
	}
}

/* With the lock held, which is let go of while the heap grows. */
static void
mali_mmu_check(void)
{
	uint32_t raw = mrd(MMU_INT_RAWSTAT) & 0x00010001;

	if (raw == 0) {
		return;
	}
	uint32_t status = mrd(AS_FAULTSTATUS(0));
	uint64_t address = mrd(AS_FAULTADDRESS_LO(0)) | (uint64_t)mrd(AS_FAULTADDRESS_HI(0)) << 32;
	bool mended = false;

	mwr(MMU_INT_CLEAR, raw);
	if (raw == 1 && (status & 0xf8) == 0xc0) {         /* a translation fault: maybe in a heap still to grow */
		lck_mtx_unlock(&mali_lock);
		mended = HakiosMaliHeapGrow(address);
		lck_mtx_lock(&mali_lock);
		if (mended) {
			mali_heap_faults++;
		}
	}
	if (!mended) {
		static const char *access[] = { "atomic", "execute", "read", "write" };

		trouble("MMU fault at %llx (job %u): %s (%x), %s access, from %x%s", address, mali_running,
		    exception_name(status & 0xff), status, access[(status >> 8) & 3], status >> 16,
		    (raw & 0x10000) ? ", bus error" : "");
		as_disable();
	}
}

static void
mali_gpu_check(void)
{
	uint32_t raw = mrd(GPU_IRQ_RAWSTAT) & (GPU_IRQ_FAULT | GPU_IRQ_MULTIPLE_FAULT);

	if (raw) {
		trouble("GPU fault %x at %x%08x (GPU_IRQ_RAWSTAT %x, job %u)", mrd(GPU_FAULT_STATUS),
		    mrd(GPU_FAULT_ADDRESS_HI), mrd(GPU_FAULT_ADDRESS_LO), raw, mali_running);
		mwr(GPU_IRQ_CLEAR, raw);
	}
}

/*
 * The CPU clusters' clocks, said with the status line (the bootloader sets them; ACPM would change them, and nothing
 * does yet). CMU_CPUCL0 (the A53s) 0x10900000: PLL_CON0 0x140; CMU_CPUCL1 (the A73s) 0x10800000: PLL_CON0 0x120; both:
 * MUX_CLK_CPUCLx_PLL 0x1000 (bit 0 clear: the PLL, set: the switch clock), DIV_CLK_CPUCLx_CPU 0x1810 (low bits: ratio - 1)
 * (Samsung cmucal-sfr.c, exynos9610). A PLL's rate: 26 MHz x MDIV [25:16] / (PDIV [13:8] x 2^SDIV [2:0]).
 */
static vm_offset_t cmu_cpucl0, cmu_cpucl1;

static uint32_t
pll_mhz(uint32_t con0)
{
	uint32_t m = (con0 >> 16) & 0x3ff, p = (con0 >> 8) & 0x3f, sdiv = con0 & 7;

	return p ? (uint32_t)(26ull * m / ((uint64_t)p << sdiv)) : 0;
}

static void
cpu_clocks_say(void)
{
	if (!cmu_cpucl0) {
		cmu_cpucl0 = ml_io_map(0x10900000UL, 0x2000);
		cmu_cpucl1 = ml_io_map(0x10800000UL, 0x2000);
	}
	if (!cmu_cpucl0 || !cmu_cpucl1) {
		return;
	}
	uint32_t p0 = rd(cmu_cpucl0 + 0x140), m0 = rd(cmu_cpucl0 + 0x1000), d0 = rd(cmu_cpucl0 + 0x1810);
	uint32_t p1 = rd(cmu_cpucl1 + 0x120), m1 = rd(cmu_cpucl1 + 0x1000), d1 = rd(cmu_cpucl1 + 0x1810);
	printf("a51gpu: CPU clocks: A53s PLL %u MHz / %u%s (con0 %x mux %x div %x), A73s PLL %u MHz / %u%s (con0 %x mux %x div %x); "
	    "max 1850 and 2400\n", pll_mhz(p0), (d0 & 0xf) + 1, (m0 & 1) ? " (NOT on the PLL)" : "", p0, m0, d0, pll_mhz(p1),
	    (d1 & 0xf) + 1, (m1 & 1) ? " (NOT on the PLL)" : "", p1, m1, d1);
}

/*
 * The SoC's temperatures, from its thermal unit (TMU, dt: BIG/LITTLE/G3D/ISP@10070000, one block): sensor 0 the GPU,
 * 1 the A53s (and the ISP), 2 the A73s. Samsung: drivers/thermal/samsung/exynos_tmu.c (exynos9610_tmu_initialize,
 * all_temp_show): CURRENT_TEMP1_0 0x40 holds sensors 0 and 1 (9 bits each), CURRENT_TEMP4_2 0x44 sensor 2 in its low 9;
 * TRIMINFO(n) at n * 4: the sensor's codes at 25 and 85 degrees (bits 0-8 and 9-17), and in TRIMINFO(0) bit 23 whether
 * both are trimmed (two-point) or only the first; a code of 0 means the dt's efuse value 0x37. CONTROL 0x20 bit 0: on.
 * Degrees: two-point (code - e25) * 60 / (e85 - e25) + 25, one-point code - e25 + 25. Only read here.
 */
static vm_offset_t tmu_base;

/* The three sensors' temperatures in degrees (-1000: no reading); false when the unit cannot be read. */
static bool
tmu_temps(int t[3])
{
	if (!tmu_base) {
		tmu_base = ml_io_map(0x10070000UL, 0x1000);
	}
	if (!tmu_base) {
		return false;
	}
	bool two_point = (rd(tmu_base + 0x00) >> 23) & 1;
	uint32_t now10 = rd(tmu_base + 0x40), now42 = rd(tmu_base + 0x44);
	uint32_t code[3] = { now10 & 0x1ff, (now10 >> 9) & 0x1ff, now42 & 0x1ff };

	for (uint32_t i = 0; i < 3; i++) {
		uint32_t trim = rd(tmu_base + i * 4), e25 = trim & 0x1ff, e85 = (trim >> 9) & 0x1ff;

		if (e25 == 0) {
			e25 = 0x37;
		}
		if (two_point && e85 != 0 && e85 != e25) {
			t[i] = ((int)code[i] - (int)e25) * 60 / ((int)e85 - (int)e25) + 25;
		} else {
			t[i] = (int)code[i] - (int)e25 + 25;
		}
		if (code[i] == 0) {
			t[i] = -1000;
		}
	}
	return true;
}

static void
tmu_say(void)
{
	int t[3];

	if (!tmu_temps(t)) {
		return;
	}
	printf("a51gpu: temperatures: GPU %d, A53s %d, A73s %d degrees (TMU control %x, codes %x %x, trim %x %x %x)\n", t[0],
	    t[1], t[2], rd(tmu_base + 0x20), rd(tmu_base + 0x40), rd(tmu_base + 0x44), rd(tmu_base + 0), rd(tmu_base + 4),
	    rd(tmu_base + 8));
}

/* In interrupt context: the line masked until the worker has looked, and the worker woken. */
static void
mali_irq(__unused void *context, uint32_t intid)
{
	gic_v3_spi_mask(intid);
	os_atomic_or(&mali_irq_pending, 1u << (intid - IRQ_GPU), relaxed);
	thread_wakeup((event_t)&mali_irq_pending);
}

static void
mali_worker(__unused void *parameter, __unused wait_result_t result)
{
	static const char *names[] = { "GPU", "JOB", "MMU" };
	bool attached[3] = { false, false, false };
	uint64_t pa;

	/* a fresh table of our own (the one of step 7 is left alone), with nothing in it */
	lck_mtx_lock(&mali_lock);
	mali_l0 = mali_table(&pa);
	mali_transtab = pa | AS_TRANSTAB_READ_INNER | AS_TRANSTAB_ADRMODE_TABLE;
	mali_l1 = mali_table(&pa);
	if (mali_l0 == NULL || mali_l1 == NULL) {
		lck_mtx_unlock(&mali_lock);
		printf("a51gpu: no memory for the GPU's tables: not kept\n");
		return;
	}
	to_memory((vm_offset_t)mali_l1, 0x1000);
	pte_set(&mali_l0[0], pa | MALI_PTE_TABLE);
	__asm__ volatile ("dsb sy");
	mali_free[0].start = MALI_VA_START;
	mali_free[0].size = MALI_VA_END - MALI_VA_START;
	mali_free_count = 1;
	as_enable();
	mali_irq_masks();
	mali_kept = true;
	lck_mtx_unlock(&mali_lock);

	for (uint32_t i = 0; i < 3; i++) {
		kern_return_t kr = gic_v3_spi_attach(IRQ_GPU + i, true, mali_irq, NULL);

		attached[i] = kr == KERN_SUCCESS;
		if (!attached[i]) {
			printf("a51gpu: interrupt %u (%s) cannot be had (%d): it is looked for instead\n", IRQ_GPU + i, names[i], kr);
		}
	}
	printf("a51gpu: THE GPU STAYS ON for programs: address space 0 at %llx, interrupts %u/%u/%u; the service HakiosMali\n",
	    mali_transtab, IRQ_GPU, IRQ_JOB, IRQ_MMU);
	HakiosMaliStart();

	/*
	 * Boot-arg a51gpu_report=S (default 30, 0: never): every S seconds one line of how it goes. A whole system's log
	 * pushes the lines of the GPU's start out of what the loader keeps; this one comes again.
	 */
	uint32_t report = 30;
	uint64_t next_report, report_interval;
	PE_parse_boot_argn("a51gpu_report", &report, sizeof(report));
	nanoseconds_to_absolutetime((uint64_t)(report ? report : 1) * NSEC_PER_SEC, &report_interval);
	next_report = mach_absolute_time() + report_interval;

	for (;;) {
		uint32_t pending = os_atomic_xchg(&mali_irq_pending, 0, acquire);
		uint64_t deadline;

		if (report && mach_absolute_time() >= next_report) {
			next_report = mach_absolute_time() + report_interval;
			printf("a51gpu: kept GPU: %u jobs given, %u over, %u failed, %u queued; interrupts GPU %llu JOB %llu MMU %llu "
			    "(attached %d%d%d); heap faults %u, resets %u; self-test: %s\n", mali_submitted, mali_completed,
			    mali_failed, mali_queue_count, gic_v3_spi_count(IRQ_GPU), gic_v3_spi_count(IRQ_JOB),
			    gic_v3_spi_count(IRQ_MMU), attached[0], attached[1], attached[2], mali_heap_faults, mali_resets,
			    HakiosMaliSelfTest());
			if (mali_clock_state[0]) {
				printf("a51gpu: clock: %s\n", mali_clock_state);
			}
			{
				uint64_t busy_ns, gap_ns;
				absolutetime_to_nanoseconds(mali_busy_abs, &busy_ns);
				absolutetime_to_nanoseconds(mali_gap_abs, &gap_ns);
				printf("a51gpu: lately: %u chains, %llu us each on the GPU on the average; %u gaps between one done and the next "
				    "started while it waited, %llu us each\n", mali_busy_jobs, mali_busy_jobs ? busy_ns / mali_busy_jobs / 1000 : 0,
				    mali_gap_jobs, mali_gap_jobs ? gap_ns / mali_gap_jobs / 1000 : 0);
				mali_busy_abs = mali_gap_abs = 0;
				mali_busy_jobs = mali_gap_jobs = 0;
			}
			cpu_clocks_say();
			tmu_say();
			for (uint32_t t = mali_trouble_count > 4 ? mali_trouble_count - 4 : 0; t < mali_trouble_count; t++) {
				printf("a51gpu: trouble %u of %u: %s\n", t + 1, mali_trouble_count, mali_trouble[t % 4]);
			}
		}

		for (uint32_t i = 0; i < 3; i++) {
			if ((pending & (1u << i)) && !mali_irq_seen[i]) {
				mali_irq_seen[i] = true;
				printf("a51gpu: interrupt %u (%s) arrives\n", IRQ_GPU + i, names[i]);
			}
		}
		lck_mtx_lock(&mali_lock);
		mali_gpu_check();
		mali_mmu_check();
		mali_jobs_check();
		if (mali_running != 0) {
			uint64_t limit;

			nanoseconds_to_absolutetime((uint64_t)MALI_JOB_TIMEOUT_MS * NSEC_PER_MSEC, &limit);
			if (mach_absolute_time() - mali_running_since > limit) {
				trouble("job %u (chain %llx) ran for %u ms: stopped", mali_running, mali_running_jc, MALI_JOB_TIMEOUT_MS);
				mwr(JS_COMMAND(mali_running_slot), JS_COMMAND_HARD_STOP);
				mali_reset("a job that does not end");
			}
		}
		mali_start_next();
		for (uint32_t i = 0; i < 3; i++) {
			if ((pending & (1u << i)) && attached[i]) {
				gic_v3_spi_unmask(IRQ_GPU + i);
			}
		}
		/* while a chain runs, look again in 2 ms whether an interrupt came or not; otherwise sleep until one does */
		clock_interval_to_deadline(mali_running ? 2 : 1000, NSEC_PER_MSEC, &deadline);
		assert_wait_deadline((event_t)&mali_irq_pending, THREAD_UNINT, deadline);
		lck_mtx_unlock(&mali_lock);
		if (os_atomic_load(&mali_irq_pending, relaxed) != 0) {
			clear_wait(current_thread(), THREAD_AWAKENED);
		} else {
			thread_block(THREAD_CONTINUE_NULL);
		}
	}
}

static thread_call_t mali_call;

static void
mali_start(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	thread_t thread;

	if (kernel_thread_start(mali_worker, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* After step 8: what GET_PARAM answers, read now, and the worker started one second from now (no threads this early). */
static void
mali_keep(vm_offset_t mali)
{
	uint64_t deadline;

	kmali = mali;
	PE_parse_boot_argn("a51gpu_log", &mali_log, sizeof(mali_log));
	mali_shaders = mrd(SHADER_PRESENT_LO);
	mali_tilers = mrd(TILER_PRESENT_LO);
	mali_l2 = mrd(L2_PRESENT_LO);
	/* the numbering is panfrost_drm.h's enum drm_panfrost_param */
	mali_param_value[0] = mrd(GPU_ID) >> 16;
	mali_param_value[1] = mrd(GPU_ID) & 0xffff;
	mali_param_value[2] = (uint32_t)mali_shaders;
	mali_param_value[3] = (uint32_t)mali_tilers;
	mali_param_value[4] = (uint32_t)mali_l2;
	mali_param_value[5] = mrd(STACK_PRESENT_LO);
	mali_param_value[6] = mrd(AS_PRESENT);
	mali_param_value[7] = mrd(JS_PRESENT);
	mali_param_value[8] = mrd(L2_FEATURES);
	mali_param_value[9] = mrd(CORE_FEATURES);
	mali_param_value[10] = mrd(TILER_FEATURES);
	mali_param_value[11] = mrd(MEM_FEATURES);
	mali_param_value[12] = mrd(MMU_FEATURES);
	mali_param_value[13] = mrd(THREAD_FEATURES);
	mali_param_value[14] = mrd(THREAD_MAX_THREADS);
	mali_param_value[15] = mrd(THREAD_MAX_WORKGROUP);
	mali_param_value[16] = mrd(THREAD_MAX_BARRIER);
	mali_param_value[17] = mrd(COHERENCY_FEATURES);
	for (uint32_t i = 0; i < 4; i++) {
		mali_param_value[18 + i] = mrd(TEXTURE_FEATURES(i));
	}
	for (uint32_t i = 0; i < 16; i++) {
		mali_param_value[22 + i] = mrd(JS_FEATURES(i));
	}
	mali_param_value[38] = __builtin_popcountll(mali_l2);  /* NR_CORE_GROUPS */
	mali_param_value[39] = mrd(THREAD_TLS_ALLOC);
	mali_param_value[40] = mrd(AFBC_FEATURES);              /* (41, 42: the timestamp ones, not answered) */
	printf("a51gpu: kept: core features %x, thread features %x (max threads %u, workgroup %u), texture features %x %x %x %x\n",
	    mali_param_value[9], mali_param_value[13], mali_param_value[14], mali_param_value[15], mali_param_value[18],
	    mali_param_value[19], mali_param_value[20], mali_param_value[21]);
	mali_call = thread_call_allocate(mali_start, NULL);
	clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(mali_call, deadline);
}

/* ---- the GPU's clock, through Samsung's ACPM (a51gpu_clock=N) ---------------------------------------------------- */

/*
 * The Mali's speed is not set here but by ACPM, the power management firmware in the SoC's APM core (a Cortex-M),
 * which sets the PLL and the GPU's supply voltage together. Its DVFS code is a plugin that the kernel uploads into the
 * APM's SRAM and attaches at every start (Android's log: "Loading exynos9610_acpm_fvp.fw ... plugin(id = 2) attach
 * done!"); without it nobody answers on the DVFS channel. Samsung: drivers/soc/samsung/acpm/acpm.c (plugins_init,
 * handle_dynamic_plugin), acpm_ipc.c, cal-if/acpm_dvfs.c; copies in ~/Desktop/ios-a51/ref/acpm.
 *   dt: acpm_ipc reg 0x11900000 (mailbox) and 0x2039000 (SRAM), initdata-base 0x6f00; acpm channel 4 (the framework:
 *       plugin attach); DVFS requests on channel 0, the clock controller's (cal-if gives acpm_dvfs that node: channel 5,
 *       the acpm_dvfs node's, is a one-slot TYPE_BUFFER channel the APM posts MIF notices into, never a request channel);
 *       mali g3d_cmu_cal_id 0xb040004 (ACPM domain 4), levels 260..1053 MHz; the CPU clusters are domains 2 and 3
 *   SRAM + initdata: struct acpm_framework: plugins at word 0, num_plugins word 1, ipc_channels word 2, ipc_ap_max word 6
 *   struct plugin (48 bytes): id, base_addr, acpm_ops, plugin_ops, secure/extern masks, timer (2 words),
 *       is_attached (byte, at 32), size (36), stay_attached (byte, at 40), fw_name (an SRAM offset, at 44)
 *   a plugin to attach (stay_attached, not is_attached): its file copied to SRAM + (base_addr & ~1), then on channel 4
 *       { 1 << 25 (attach) | id << 26 } -- the file from fw/ of the small image (userland/build.sh, from ../fw-acpm)
 *   a DVFS request on channel 5: { domain, rate in kHz, FREQ_REQ 0 / FREQ_GET 1, 0 }
 * Steps (a51gpu_clock=N), in a thread of their own that waits for the small image; what they find is said again with the
 * kept GPU's status line (a whole system's start pushes it out of the kernel log):
 *   1  reads   the plugin table and the DVFS channel
 *   2  uploads and attaches the plugins Android attaches, then asks the GPU's present speed (FREQ_GET)
 *   3  asks    for the top levels (or a51cpu0_khz, a51cpu1_khz, a51gpu_mhz), reads the PLLs, then watches the temperature
 */
#define ACPM_MBOX_PHYS          0x11900000UL
#define ACPM_SRAM_PHYS          0x02039000UL
#define ACPM_SRAM_SIZE          0x15000
#define ACPM_INITDATA           0x6f00
#define ACPM_FRAMEWORK_CHANNEL  4
#define ACPM_DVFS_CHANNEL       0               /* dt: clock-controller@0x12100000 (cal-if's node, which it hands acpm_dvfs) */
#define ACPM_CPUCL0_DOMAIN      2               /* Android: dvfs_cpucl0 - id : 2 (the A53s) */
#define ACPM_CPUCL1_DOMAIN      3               /* dvfs_cpucl1 - id : 3 (the A73s) */
#define ACPM_G3D_DOMAIN         4
#define ACPM_INTGR0             0x0008
#define ACPM_INTSR1             0x0028
#define ACPM_INTCR1             0x0020
#define ACPM_FREQ_REQ           0
#define ACPM_FREQ_GET           1
#define ACPM_ATTACH             (1u << 25)
#define ACPM_PLUGIN_SIZE        48

extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern void IOSleep(unsigned int milliseconds);

static const uint32_t g3d_levels_khz[] = { 1053000, 950000, 840000, 764000, 683000, 572000, 546000, 455000, 384000,
	                                   338000, 260000 };    /* dt: gpu_dvfs_table, first column */

struct acpm_channel {
	uint32_t id, field, owner, type;
	uint32_t rx_rear, rx_front, rx_base, rx_indr_buf, rx_indr_buf_base, rx_indr_buf_size;
	uint32_t tx_rear, tx_front, tx_base, q_len, q_elem_size, credit, mbox_addr, ap_poll;
};

static vm_offset_t acpm_sram, acpm_mbox, acpm_cmu;
static uint32_t acpm_seq, acpm_steps, acpm_mhz = 1053;
static char mali_clock_state[240];             /* said with the status line (declared above too) */

static void
clock_said(const char *format, ...) __printflike(1, 2);

static void
clock_said(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vsnprintf(mali_clock_state, sizeof(mali_clock_state), format, ap);
	va_end(ap);
	printf("a51gpu: clock: %s\n", mali_clock_state);
}

static bool
acpm_channel_read(uint32_t index, struct acpm_channel *ch)
{
	uint32_t channels = rd(acpm_sram + ACPM_INITDATA + 2 * 4), ap_max = rd(acpm_sram + ACPM_INITDATA + 6 * 4);

	if (index >= ap_max || channels == 0 || channels + (index + 1) * sizeof(*ch) > ACPM_SRAM_SIZE) {
		return false;
	}
	for (uint32_t w = 0; w < sizeof(*ch) / 4; w++) {
		((uint32_t *)ch)[w] = rd(acpm_sram + channels + index * sizeof(*ch) + w * 4);
	}
	return ch->q_len != 0 && ch->q_len <= 64 && ch->q_elem_size >= 16 && ch->q_elem_size <= 64 &&
	       ch->rx_base < ACPM_SRAM_SIZE && ch->tx_base < ACPM_SRAM_SIZE && ch->id <= 15;
}

/*
 * One request and its answer on a channel (cmd: 4 words in, the answer's 4 words back). The AP sends through the APM's
 * rx queue and reads answers from its tx queue; a queue of one element has indices that are always 0, and there the
 * doorbell alone says a request is there and INTSR1 alone that the answer is. false: no room, or no answer in 100 ms.
 */
static bool
acpm_call(const struct acpm_channel *ch, uint32_t cmd[4])
{
	bool single = ch->q_len == 1;
	uint32_t front = rd(acpm_sram + ch->rx_front), next = (front + 1) % ch->q_len, seq;

	if (!single && next == rd(acpm_sram + ch->rx_rear)) {
		printf("a51gpu: ACPM: channel %u's queue is full\n", ch->id);
		return false;
	}
	if (rd(acpm_mbox + ACPM_INTSR1) & (1u << ch->id) && single) {
		wr(acpm_mbox + ACPM_INTCR1, 1u << ch->id);      /* (an old answer nobody took) */
	}
	seq = acpm_seq = acpm_seq % 63 + 1;
	cmd[0] = (cmd[0] & ~(0x3fu << 16)) | seq << 16;
	for (uint32_t w = 0; w < 4; w++) {
		wr(acpm_sram + ch->rx_base + ch->q_elem_size * front + w * 4, cmd[w]);
	}
	__asm__ volatile ("dsb sy");
	wr(acpm_sram + ch->rx_front, next);
	wr(acpm_mbox + ACPM_INTGR0, (1u << ch->id) << 16);
	for (uint32_t i = 0; i < 100000; i++) {
		if (single) {
			if (!(rd(acpm_mbox + ACPM_INTSR1) & (1u << ch->id))) {
				delay(1);
				continue;
			}
			for (uint32_t w = 0; w < 4; w++) {
				cmd[w] = rd(acpm_sram + ch->tx_base + w * 4);
			}
			wr(acpm_mbox + ACPM_INTCR1, 1u << ch->id);
			return true;
		}
		uint32_t rear = rd(acpm_sram + ch->tx_rear), tfront = rd(acpm_sram + ch->tx_front);
		for (uint32_t at = rear; at != tfront; at = (at + 1) % ch->q_len) {
			vm_offset_t e = acpm_sram + ch->tx_base + ch->q_elem_size * at;

			if (((rd(e) >> 16) & 0x3f) != seq) {
				continue;
			}
			for (uint32_t w = 0; w < 4; w++) {
				cmd[w] = rd(e + w * 4);
			}
			if (at != rear) {               /* (Samsung's way: the answer's place filled with the oldest, then that one dropped) */
				for (uint32_t w = 0; w < ch->q_elem_size / 4; w++) {
					wr(e + w * 4, rd(acpm_sram + ch->tx_base + ch->q_elem_size * rear + w * 4));
				}
			}
			rear = (rear + 1) % ch->q_len;
			wr(acpm_sram + ch->tx_rear, rear);
			if (rear == rd(acpm_sram + ch->tx_front)) {
				wr(acpm_mbox + ACPM_INTCR1, 1u << ch->id);
			}
			return true;
		}
		delay(1);
	}
	printf("a51gpu: ACPM: no answer on channel %u in 100 ms (INTSR1 %x, tx rear %u front %u)\n", ch->id,
	    rd(acpm_mbox + ACPM_INTSR1), rd(acpm_sram + ch->tx_rear), rd(acpm_sram + ch->tx_front));
	return false;
}

/* A string of the APM's SRAM (a plugin's file name), copied out; "" when there is none. */
static void
sram_string(uint32_t offset, char *out, size_t size)
{
	size_t i = 0;

	for (; offset != 0 && offset < ACPM_SRAM_SIZE && i + 1 < size; i++, offset++) {
		char c = (char)((rd(acpm_sram + (offset & ~3u)) >> (8 * (offset & 3))) & 0xff);

		if (c == 0) {
			break;
		}
		out[i] = c;
	}
	out[i] = 0;
}

static void
clock_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct acpm_channel framework, dvfs;
	uint32_t plugins = rd(acpm_sram + ACPM_INITDATA), count = rd(acpm_sram + ACPM_INITDATA + 4), cmd[4];
	uint32_t attached = 0, wanted = 0;
	char name[48];

	printf("a51gpu: clock step 1: ACPM's plugins (%u at SRAM %x) and channels\n", count, plugins);
	if (count == 0 || count > 16 || plugins == 0 || plugins + count * ACPM_PLUGIN_SIZE > ACPM_SRAM_SIZE) {
		clock_said("the plugin table is not what was expected (%u at %x)", count, plugins);
		return;
	}
	for (uint32_t i = 0; i < count; i++) {
		vm_offset_t p = acpm_sram + plugins + i * ACPM_PLUGIN_SIZE;
		uint32_t is = rd(p + 32) & 0xff, stay = rd(p + 40) & 0xff;

		sram_string(rd(p + 44), name, sizeof(name));
		printf("a51gpu: ACPM plugin %u: id %u base %x size %x attached %u stay %u file \"%s\" (raw %x %x %x %x)\n", i,
		    rd(p), rd(p + 4), rd(p + 36), is, stay, name, rd(p + 32), rd(p + 36), rd(p + 40), rd(p + 44));
		if (is && stay) {
			attached |= 1u << rd(p);
		}
		if (!is && stay) {
			wanted |= 1u << rd(p);
		}
	}
	if (!acpm_channel_read(ACPM_FRAMEWORK_CHANNEL, &framework) || !acpm_channel_read(ACPM_DVFS_CHANNEL, &dvfs)) {
		clock_said("the ACPM channels are not what was expected");
		return;
	}
	clock_said("step 1: plugins attached %x, to attach %x; GPU PLL con0 %x", attached, wanted, rd(acpm_cmu + PLL_CON0_PLL_G3D));
	if (acpm_steps < 2) {
		return;
	}

	/* step 2: the plugins Android attaches, from fw/ of the small image (waited for: it comes with the system's start) */
	for (uint32_t i = 0; i < count; i++) {
		vm_offset_t p = acpm_sram + plugins + i * ACPM_PLUGIN_SIZE;
		uint32_t id = rd(p), base = rd(p + 4) & ~1u;
		const uint8_t *file = NULL;
		uint64_t size = 0;

		if ((rd(p + 32) & 0xff) || !(rd(p + 40) & 0xff)) {
			continue;
		}
		sram_string(rd(p + 44), name, sizeof(name));
		for (int t = 0; t < 240 && name[0] && (file = hakios_image_file("fw", name, &size, NULL)) == NULL; t++) {
			IOSleep(500);
		}
		if (file == NULL || size == 0 || base == 0 || base + size > ACPM_SRAM_SIZE) {
			clock_said("step 2: plugin %u's file fw/%s not found or does not fit (%llu bytes at %x)", id, name, size, base);
			return;
		}
		for (uint64_t off = 0; off + 4 <= size; off += 4) {
			wr(acpm_sram + base + off, (uint32_t)file[off] | (uint32_t)file[off + 1] << 8 |
			    (uint32_t)file[off + 2] << 16 | (uint32_t)file[off + 3] << 24);
		}
		__asm__ volatile ("dsb sy");
		cmd[0] = ACPM_ATTACH | id << 26;
		cmd[1] = cmd[2] = cmd[3] = 0;
		bool answered = acpm_call(&framework, cmd);
		printf("a51gpu: ACPM: plugin %u (%s, %llu bytes at %x) %s (answer %x %x %x %x); attached now %u\n", id, name,
		    size, base, answered ? "attached" : "NOT ANSWERED", cmd[0], cmd[1], cmd[2], cmd[3], rd(p + 32) & 0xff);
		if (!answered) {
			clock_said("step 2: plugin %u (%s) not attached: no answer", id, name);
			return;
		}
	}
	/* the DVFS plugin owns its channel: what it looks like now that the plugin runs (it was a placeholder before) */
	struct acpm_channel before = dvfs;
	if (!acpm_channel_read(ACPM_DVFS_CHANNEL, &dvfs)) {
		clock_said("step 2: plugins attached; the DVFS channel is not what was expected now");
		return;
	}
	char layout[120];
	snprintf(layout, sizeof(layout), "DVFS channel: queue %u x %u (was %u x %u), type %u poll %u, rx %x/%x/%x tx %x/%x/%x",
	    dvfs.q_len, dvfs.q_elem_size, before.q_len, before.q_elem_size, dvfs.type, dvfs.ap_poll, dvfs.rx_rear,
	    dvfs.rx_front, dvfs.rx_base, dvfs.tx_rear, dvfs.tx_front, dvfs.tx_base);
	printf("a51gpu: ACPM: %s\n", layout);
	uint32_t khz_now[3];
	static const uint32_t domains[3] = { ACPM_CPUCL0_DOMAIN, ACPM_CPUCL1_DOMAIN, ACPM_G3D_DOMAIN };
	for (uint32_t d = 0; d < 3; d++) {
		cmd[0] = domains[d];
		cmd[1] = 0;
		cmd[2] = ACPM_FREQ_GET;
		cmd[3] = 0;
		if (!acpm_call(&dvfs, cmd)) {
			clock_said("step 2: plugins attached, but no answer to FREQ_GET (domain %u); %s", domains[d], layout);
			return;
		}
		khz_now[d] = cmd[1];
	}
	uint32_t was = khz_now[2];
	clock_said("step 2: ACPM answers: A53s %u kHz, A73s %u kHz, GPU %u kHz; PLL con0 GPU %x", khz_now[0], khz_now[1],
	    khz_now[2], rd(acpm_cmu + PLL_CON0_PLL_G3D));
	if (acpm_steps < 3) {
		return;
	}

	/*
	 * Step 3: the levels asked for, each one of the DT's (cpufreq domain@0 and @1 mif-perf tables, mali gpu_dvfs_table).
	 * By default the highest -- 1742, 2314 and 1053 MHz -- since the temperature watch below keeps them in Samsung's
	 * limits (on the phone: the A73s held near 83 degrees under load, down to 1976 MHz and back). a51cpu0_khz,
	 * a51cpu1_khz, a51gpu_mhz ask for less.
	 */
	static const uint32_t cl0_khz[] = { 1742000, 1638000, 1534000, 1456000, 1326000, 1222000, 1118000, 1053000, 910000,
		                            806000, 702000, 598000, 403000 };
	static const uint32_t cl1_khz[] = { 2314000, 2210000, 2184000, 2080000, 1976000, 1898000, 1768000, 1664000, 1508000,
		                            1456000, 1352000, 1248000, 1144000, 1040000, 936000, 832000, 728000 };
	uint32_t want[3] = { 1742000, 2314000, 0 };
	PE_parse_boot_argn("a51cpu0_khz", &want[0], sizeof(want[0]));
	PE_parse_boot_argn("a51cpu1_khz", &want[1], sizeof(want[1]));
	for (uint32_t i = 0; i < sizeof(g3d_levels_khz) / sizeof(g3d_levels_khz[0]); i++) {
		if ((g3d_levels_khz[i] + 500) / 1000 == acpm_mhz || g3d_levels_khz[i] / 1000 == acpm_mhz) {
			want[2] = g3d_levels_khz[i];
		}
	}
	bool ok0 = false, ok1 = false;
	for (uint32_t i = 0; i < sizeof(cl0_khz) / sizeof(cl0_khz[0]); i++) {
		ok0 |= cl0_khz[i] == want[0];
	}
	for (uint32_t i = 0; i < sizeof(cl1_khz) / sizeof(cl1_khz[0]); i++) {
		ok1 |= cl1_khz[i] == want[1];
	}
	if (!ok0 || !ok1 || want[2] == 0) {
		clock_said("step 3: not one of the DT's levels: A53s %u kHz %s, A73s %u kHz %s, GPU %u MHz %s", want[0],
		    ok0 ? "ok" : "NO", want[1], ok1 ? "ok" : "NO", acpm_mhz, want[2] ? "ok" : "NO");
		return;
	}
	bool answered[3];
	for (uint32_t d = 0; d < 3; d++) {
		cmd[0] = domains[d];
		cmd[1] = want[d];
		cmd[2] = ACPM_FREQ_REQ;
		cmd[3] = 0;
		answered[d] = acpm_call(&dvfs, cmd);
	}
	/*
	 * The memory interface (MIF, domain 0) and the internal bus (INT, domain 1), at their highest (dt devfreq_mif and
	 * devfreq_int freq_info: max 2093 and 667 MHz; Android holds MIF there only for 40 s after boot, then from 676 MHz up
	 * as the load asks, and nobody asked here). Tried as the cause of the display's slow frames (16 November) and it was
	 * not: those were three processors writing the write-combined framebuffer at once (MiniIOMobileFramebuffer,
	 * CONFIG_FB_THREADS=1). So asked only when given: a51mif_khz=2093000, a51int_khz=667000.
	 */
	uint32_t bus_khz[2] = { 0, 0 };
	bool bus_answered[2] = { false, false };
	PE_parse_boot_argn("a51mif_khz", &bus_khz[0], sizeof(bus_khz[0]));
	PE_parse_boot_argn("a51int_khz", &bus_khz[1], sizeof(bus_khz[1]));
	for (uint32_t d = 0; d < 2; d++) {
		if (bus_khz[d] != 0) {
			cmd[0] = d;                     /* dm_mif 0, dm_int 1 (dt acpm_dvfs: cal_id 0xb040000, 0xb040001) */
			cmd[1] = bus_khz[d];
			cmd[2] = ACPM_FREQ_REQ;
			cmd[3] = 0;
			bus_answered[d] = acpm_call(&dvfs, cmd);
		}
	}
	clock_said("step 3: asked the memory interface %u kHz (%s), the internal bus %u kHz (%s)", bus_khz[0],
	    bus_answered[0] ? "answered" : "no answer", bus_khz[1], bus_answered[1] ? "answered" : "no answer");
	IOSleep(10);
	uint32_t pll0 = cmu_cpucl0 ? rd(cmu_cpucl0 + 0x140) : 0, pll1 = cmu_cpucl1 ? rd(cmu_cpucl1 + 0x120) : 0;
	if (!cmu_cpucl0) {
		vm_offset_t c0 = ml_io_map(0x10900000UL, 0x2000), c1 = ml_io_map(0x10800000UL, 0x2000);
		pll0 = c0 ? rd(c0 + 0x140) : 0;
		pll1 = c1 ? rd(c1 + 0x120) : 0;
	}
	uint32_t pllg = rd(acpm_cmu + PLL_CON0_PLL_G3D);
	clock_said("step 3: asked A53s %u, A73s %u, GPU %u kHz (%s%s%s); PLLs now A53s %u MHz, A73s %u MHz, GPU %u MHz (were "
	    "%u, %u, %u)", want[0], want[1], want[2], answered[0] ? "a" : "-", answered[1] ? "a" : "-", answered[2] ? "a" : "-",
	    pll_mhz(pll0), pll_mhz(pll1), pll_mhz(pllg), khz_now[0] / 1000, khz_now[1] / 1000, was / 1000);

	/*
	 * The temperature watch, from now on, once a second: Samsung's own limits (dt thermal-zones: the big cores throttle
	 * from 83 degrees, passive; the little ones from 81; 115 is "hot"). A domain whose sensor reads above 83 drops one level
	 * a second; below 75 it climbs back one level every five seconds, up to what was asked; above 100 it falls at once to
	 * the speed the boot loader left. Never below that speed. Sensor 0 the GPU, 1 the A53s, 2 the A73s.
	 */
	const uint32_t *levels[3] = { cl0_khz, cl1_khz, g3d_levels_khz };
	const uint32_t counts[3] = { sizeof(cl0_khz) / sizeof(cl0_khz[0]), sizeof(cl1_khz) / sizeof(cl1_khz[0]),
		                     sizeof(g3d_levels_khz) / sizeof(g3d_levels_khz[0]) };
	const uint32_t boot_khz[3] = { 1053000, 1456000, 260000 };
	const uint32_t sensor_of[3] = { 1, 2, 0 };
	uint32_t asked[3], at[3], floor_at[3], drops = 0, rises = 0, seconds = 0;
	int hottest[3] = { -1000, -1000, -1000 };

	for (uint32_t d = 0; d < 3; d++) {
		asked[d] = at[d] = floor_at[d] = 0;
		for (uint32_t i = 0; i < counts[d]; i++) {
			if (levels[d][i] == want[d]) {
				asked[d] = at[d] = i;
			}
			if (levels[d][i] == boot_khz[d]) {
				floor_at[d] = i;
			}
		}
	}
	for (;;) {
		int t[3];

		IOSleep(1000);
		seconds++;
		if (!tmu_temps(t)) {
			continue;
		}
		for (uint32_t d = 0; d < 3; d++) {
			int temp = t[sensor_of[d]];
			uint32_t to = at[d];

			if (temp == -1000) {
				continue;
			}
			if (temp > hottest[d]) {
				hottest[d] = temp;
			}
			if (temp > 100) {
				to = floor_at[d];
			} else if (temp > 83 && at[d] < floor_at[d]) {
				to = at[d] + 1;
			} else if (temp < 75 && at[d] > asked[d] && seconds % 5 == 0) {
				to = at[d] - 1;
			}
			if (to == at[d]) {
				continue;
			}
			cmd[0] = domains[d];
			cmd[1] = levels[d][to];
			cmd[2] = ACPM_FREQ_REQ;
			cmd[3] = 0;
			if (acpm_call(&dvfs, cmd)) {
				if (to > at[d]) {
					drops++;
				} else {
					rises++;
				}
				printf("a51gpu: temperature watch: %s %d degrees: %u -> %u MHz\n", d == 0 ? "A53s" : d == 1 ? "A73s" : "GPU",
				    temp, levels[d][at[d]] / 1000, levels[d][to] / 1000);
				at[d] = to;
			}
		}
		if (seconds % 30 == 0) {
			clock_said("watching: now A53s %u, A73s %u, GPU %u MHz (asked %u, %u, %u); temperatures %d, %d, %d (hottest %d, %d, "
			    "%d); %u steps down, %u up", levels[0][at[0]] / 1000, levels[1][at[1]] / 1000, levels[2][at[2]] / 1000,
			    want[0] / 1000, want[1] / 1000, want[2] / 1000, t[1], t[2], t[0], hottest[0], hottest[1], hottest[2], drops,
			    rises);
		}
	}
}

static thread_call_t clock_call;

static void
clock_start(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	thread_t thread;

	if (kernel_thread_start(clock_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* From step 4 on: the mappings now, the steps in a thread one second from now (no threads this early). */
static void
a51gpu_clock(vm_offset_t cmu)
{
	uint64_t deadline;

	if (!PE_parse_boot_argn("a51gpu_clock", &acpm_steps, sizeof(acpm_steps)) || acpm_steps == 0) {
		return;
	}
	PE_parse_boot_argn("a51gpu_mhz", &acpm_mhz, sizeof(acpm_mhz));
	acpm_cmu = cmu;
	acpm_sram = ml_io_map(ACPM_SRAM_PHYS, ACPM_SRAM_SIZE);
	acpm_mbox = ml_io_map(ACPM_MBOX_PHYS, 0x1000);
	if (!acpm_sram || !acpm_mbox) {
		printf("a51gpu: cannot map ACPM's SRAM or mailbox\n");
		return;
	}
	clock_call = thread_call_allocate(clock_start, NULL);
	clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(clock_call, deadline);
}

static void
a51gpu_init(void)
{
	uint32_t steps = 0, status;
	vm_offset_t pmu, cmu, mali;

	if (!PE_parse_boot_argn("a51gpu", &steps, sizeof(steps)) || steps == 0) {
		return;                                 /* nothing is touched unless a boot-argument asks */
	}

	printf("a51gpu: step 1: the power domain pd-g3d and the GPU's clock controller, read\n");
	pmu = ml_io_map(PMU_PHYS, 0x5000);
	cmu = ml_io_map(CMU_G3D_PHYS, 0x4000);
	if (!pmu || !cmu) {
		printf("a51gpu: cannot map the PMU or CMU_G3D\n");
		return;
	}
	status = rd(pmu + G3D_STATUS);
	printf("a51gpu: G3D_CONFIGURATION %x, G3D_STATUS %x (on: low 4 bits all set)\n", rd(pmu + G3D_CONFIGURATION), status);
	if ((status & G3D_LOCAL_PWR_MASK) == G3D_LOCAL_PWR_MASK) {
		/* the clock controller sits inside the domain: read it only while the domain is on */
		printf("a51gpu: CMU_G3D: SWITCH_USER mux %x, PLL_G3D con0 %x, BUSD mux %x, BUSP div %x\n",
		    rd(cmu + PLL_CON0_MUX_G3D_SWITCH_USER), rd(cmu + PLL_CON0_PLL_G3D), rd(cmu + CLK_CON_MUX_G3D_BUSD),
		    rd(cmu + CLK_CON_DIV_G3D_BUSP));
	}
	if (steps < 2) {
		return;
	}

	if ((status & G3D_LOCAL_PWR_MASK) != G3D_LOCAL_PWR_MASK) {
		printf("a51gpu: step 2: the power domain on (G3D_CONFIGURATION := 0xf)\n");
		wr(pmu + G3D_CONFIGURATION, (rd(pmu + G3D_CONFIGURATION) & ~G3D_LOCAL_PWR_MASK) | G3D_LOCAL_PWR_MASK);
		for (int i = 0; i < 10000; i++) {       /* 10 ms */
			status = rd(pmu + G3D_STATUS);
			if ((status & G3D_LOCAL_PWR_MASK) == G3D_LOCAL_PWR_MASK) {
				break;
			}
			delay(1);
		}
		printf("a51gpu: G3D_STATUS %x\n", status);
		if ((status & G3D_LOCAL_PWR_MASK) != G3D_LOCAL_PWR_MASK) {
			printf("a51gpu: the domain does not come on (its supply off?): stopping\n");
			return;
		}
		printf("a51gpu: CMU_G3D: SWITCH_USER mux %x, PLL_G3D con0 %x, BUSD mux %x, BUSP div %x\n",
		    rd(cmu + PLL_CON0_MUX_G3D_SWITCH_USER), rd(cmu + PLL_CON0_PLL_G3D), rd(cmu + CLK_CON_MUX_G3D_BUSD),
		    rd(cmu + CLK_CON_DIV_G3D_BUSP));
	} else {
		printf("a51gpu: step 2: the power domain is on already\n");
	}
	if (steps < 3) {
		return;
	}

	printf("a51gpu: step 3: SMC 0x%lx (wake, 0x%lx, runtime pm): the domain's protection settings restored\n",
	    SMC_CMD_PREPARE_PD_ONOFF, G3D_NEED_SMC);
	int64_t ret = smc(SMC_CMD_PREPARE_PD_ONOFF, EXYNOS_WAKEUP_PD_DOWN, G3D_NEED_SMC, EXYNOS_PD_RUNTIME_PM);
	printf("a51gpu: the secure monitor answers %lld\n", ret);
	if (ret != 0) {
		printf("a51gpu: stopping\n");
		return;
	}
	if (steps < 4) {
		return;
	}

	printf("a51gpu: step 4: the GPU's registers (0x%lx)\n", MALI_PHYS);
	mali = ml_io_map(MALI_PHYS, MALI_SIZE);
	if (!mali) {
		printf("a51gpu: cannot map the GPU\n");
		return;
	}
	uint32_t id = rd(mali + GPU_ID);
	printf("a51gpu: GPU_ID %08x: product %04x (0x6221 is a Mali-G72), version r%up%u\n", id, id >> 16,
	    (id >> 12) & 0xf, (id >> 4) & 0xf);
	printf("a51gpu: L2_FEATURES %x, TILER_FEATURES %x, MEM_FEATURES %x, MMU_FEATURES %x\n", rd(mali + L2_FEATURES),
	    rd(mali + TILER_FEATURES), rd(mali + MEM_FEATURES), rd(mali + MMU_FEATURES));
	printf("a51gpu: AS_PRESENT %x, JS_PRESENT %x, GPU_STATUS %x, GPU_IRQ_RAWSTAT %x\n", rd(mali + AS_PRESENT),
	    rd(mali + JS_PRESENT), rd(mali + GPU_STATUS), rd(mali + GPU_IRQ_RAWSTAT));
	printf("a51gpu: present: shader cores %x, tilers %x, L2 %x\n", rd(mali + SHADER_PRESENT_LO),
	    rd(mali + TILER_PRESENT_LO), rd(mali + L2_PRESENT_LO));
	a51gpu_clock(cmu);
	if (steps < 5) {
		return;
	}
	if (a51gpu_jobs(mali, steps) && steps >= 8) {
		uint32_t keep = 0;

		if (PE_parse_boot_argn("a51gpu_keep", &keep, sizeof(keep)) && keep) {
			mali_keep(mali);
		}
	}
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, a51gpu_init);
