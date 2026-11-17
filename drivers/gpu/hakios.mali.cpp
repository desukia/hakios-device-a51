/*
 * HakiosMali: the Galaxy A51's Mali-G72 for programs to draw with.
 *
 * devices/a51/drivers/gpu/hakios.a51gpu.c wakes the GPU and, with boot-args a51gpu=8 a51gpu_keep=1, keeps it on and
 * starts this service. A program that opens it speaks the interface of Linux's panfrost DRM driver (version 1.1), the
 * one Mesa's Panfrost knows: every request of that interface is a call of the user client with the request's number
 * (DRM_COMMAND_BASE + DRM_PANFROST_*, the low byte of the ioctl code) as the selector and the request's structure passed
 * in and back (userland: tools/mesa/hakios-drm stands in for libdrm inside Mesa). What Linux does with mmap() on the
 * device at a buffer's offset is the client memory of the type that is the buffer's handle here.
 *
 * What there is:
 *   GET_PARAM        the GPU's feature registers (read once by hakios.a51gpu.c)
 *   CREATE_BO        wired kernel memory, write-combined (Linux maps panfrost's buffers so on a GPU that is not
 *                    coherent with the processor's caches, and this one is not), entered into the GPU's address space;
 *                    PANFROST_BO_HEAP: addresses only, the memory comes 2 MB at a time as the GPU faults on it
 *   MMAP_BO          answers an offset that names the buffer (the handle is what is mapped)
 *   GET_BO_OFFSET    the buffer's GPU address
 *   SUBMIT           one job chain into the queue; the buffers it names remember it. Not Linux's: the chain's fence
 *                    number comes back in `out_sync` (hakios-drm makes the syncobj of it); `in_syncs` need nothing,
 *                    since chains run one at a time in the order given
 *   WAIT_BO          until the last chain that named the buffer is over (timeout_ns: CLOCK_MONOTONIC's, absolute)
 *   MADVISE          nothing is ever taken away: retained = 1
 *   GEM_CLOSE        the buffer given back (after the GPU is through with it)
 *   0x7b             not Linux's: wait for a fence number, for hakios-drm's syncobjs
 *
 * Not here yet: more than one address space (every client's buffers are in the one), performance counters, buffers
 * shared with other processes (IOSurfaces) and the window server's use of it.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

#include "MiniIOSurface.h"

extern "C" void flush_dcache64(addr64_t addr, unsigned count, int phys);

extern "C" {
/* devices/a51/drivers/gpu/hakios.a51gpu.c ("the GPU kept for programs") */
bool hakios_mali_ready(void);
bool hakios_mali_param(uint32_t which, uint64_t * value);
uint64_t hakios_mali_address(uint64_t size, uint64_t align, bool exec);
void hakios_mali_address_free(uint64_t start, uint64_t size);
bool hakios_mali_map(uint64_t va, uint64_t phys, uint64_t size, bool noexec);
void hakios_mali_mapped(uint64_t va, uint64_t size);
void hakios_mali_unmap(uint64_t va, uint64_t size);
uint32_t hakios_mali_submit(uint64_t jc, uint32_t requirements);
uint32_t hakios_mali_completed(void);
uint32_t hakios_mali_submitted(void);
bool hakios_mali_wait(uint32_t fence, uint64_t deadline);

void HakiosMaliStart(void);
bool HakiosMaliHeapGrow(uint64_t address);
const char * HakiosMaliSelfTest(void);
}

static char gSelfTest[200] = "not run yet";

const char *
HakiosMaliSelfTest(void)
{
	return gSelfTest;
}

/* Linux's include/uapi/drm/panfrost_drm.h and drm.h */
enum {
	kDrmGemClose            = 0x09,
	kPanfrostSubmit         = 0x40,
	kPanfrostWaitBO         = 0x41,
	kPanfrostCreateBO       = 0x42,
	kPanfrostMmapBO         = 0x43,
	kPanfrostGetParam       = 0x44,
	kPanfrostGetBOOffset    = 0x45,
	kPanfrostPerfcntEnable  = 0x46,
	kPanfrostPerfcntDump    = 0x47,
	kPanfrostMadvise        = 0x48,
	kHakiosWaitFence        = 0x7b,         /* ours */
	kHakiosUserMemory       = 0x7d,         /* ours, as HakiosGPU's: pages of the caller's own as a buffer (Linux's userptr) */
	kHakiosSurface          = 0x7e,         /* ours, as HakiosGPU's: an IOSurface's memory as a buffer */
};
#define kHakiosDescribe         0xffffffffu     /* a surface request that only asks what the buffer would be like */
#define PANFROST_BO_NOEXEC      1
#define PANFROST_BO_HEAP        2

struct drm_panfrost_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;                      /* here: the fence comes back in it */
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};
struct drm_panfrost_wait_bo {
	uint32_t handle, pad;
	int64_t timeout_ns;
};
struct drm_panfrost_create_bo {
	uint32_t size, flags, handle, pad;
	uint64_t offset;
};
struct drm_panfrost_mmap_bo {
	uint32_t handle, flags;
	uint64_t offset;
};
struct drm_panfrost_get_param {
	uint32_t param, pad;
	uint64_t value;
};
struct drm_panfrost_get_bo_offset {
	uint32_t handle, pad;
	uint64_t offset;
};
struct drm_panfrost_madvise {
	uint32_t handle, madv, retained;
};
struct drm_gem_close {
	uint32_t handle, pad;
};
struct hakios_user_memory {
	uint64_t address, size;                 /* in the caller, in whole pages */
	uint32_t handle;                        /* back: the buffer */
	uint32_t flags;
};
struct hakios_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* kHakiosDescribe: no buffer is wanted; else, back: the buffer, */
	uint64_t size;                          /* how big it is, */
	uint32_t offset, rowBytes;              /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixelFormat, planes;    /* and what they are */
};
struct hakios_wait_fence {
	uint32_t fence;
	uint32_t completed;                     /* out: the last fence that is over */
	int64_t timeout_ns;                     /* CLOCK_MONOTONIC's, absolute; 0: only look */
};

#define MALI_BUFFERS            8192
#define MALI_BUFFER_MAX         (512ull << 20)
#define MALI_HEAP_CHUNK         (2ull << 20)
#define MALI_HEAPS              32
#define MALI_HEAP_CHUNKS        256             /* 512 MB of heap at the most */
#define MALI_GIVE_BACK_MS       2000            /* how long a buffer given back waits for the GPU to be through with it */
#define MALI_PUSH_AGAIN_MS      4               /* an unchanged surface is pushed out of the cache again this long after the last */

struct mali_buffer {
	IOMemoryDescriptor *            memory;         /* NULL for a heap */
	uint8_t *                       kernel;         /* where an IOSurface's pixels are in the kernel (for the cache) */
	MiniIOSurface *                 surface;        /* whose memory it is, when not its own (held) */
	bool                            client;         /* the client's own pages (wired; out of the cache by physical address) */
	uint32_t                        seed;           /* the surface's when it was last pushed out of the cache, */
	uint64_t                        pushed;         /* ... and when */
	uint64_t                        address;        /* the GPU's; 0: this handle is free */
	uint64_t                        size;
	uint32_t                        flags;
	uint32_t                        fence;          /* the last chain that names it (0: none has) */
	int                             heap;           /* its place in gHeaps, or -1 */
};

/* the heaps of all clients, for the worker's faults (HakiosMaliHeapGrow()) */
static struct {
	uint64_t                        address, size;  /* 0: free */
	IOBufferMemoryDescriptor *      chunk[MALI_HEAP_CHUNKS];
} gHeaps[MALI_HEAPS];
static IOLock * gHeapLock;
static uint32_t gLog;                           /* boot-arg a51gpu_log=2: a line for every request of a client */
static IOLock * gMaliLock;                      /* the clients' buffers */

/* ---- memory ---- */

/* Enter wired memory into the GPU's address space at `address`, segment by segment. */
static bool
enter(IOMemoryDescriptor * memory, uint64_t address, uint64_t size, bool noexec)
{
	for (uint64_t offset = 0; offset < size;) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(offset, &length, kIOMemoryMapperNone);

		if (phys == 0 || length == 0) {
			return false;
		}
		if (length > size - offset) {
			length = size - offset;
		}
		if (!hakios_mali_map(address + offset, phys, length, noexec)) {
			return false;
		}
		offset += length;
	}
	hakios_mali_mapped(address, size);
	return true;
}

static IOBufferMemoryDescriptor *
gpuMemory(uint64_t size)
{
	/* zeroed; and made write-combined, which also pushes the zeroes out of the cache to where the GPU reads */
	return IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	           kIOMapWriteCombineCache, (vm_size_t) size, PAGE_SIZE);
}

/* The worker found the GPU stalled on a translation fault at `address`: memory for the heap there, if it is one. */
bool
HakiosMaliHeapGrow(uint64_t address)
{
	bool grown = false;

	if (gHeapLock == NULL) {
		return false;
	}
	IOLockLock(gHeapLock);
	for (int h = 0; h < MALI_HEAPS; h++) {
		if (gHeaps[h].address == 0 || address < gHeaps[h].address || address >= gHeaps[h].address + gHeaps[h].size) {
			continue;
		}
		uint32_t c = (uint32_t)((address - gHeaps[h].address) / MALI_HEAP_CHUNK);
		uint64_t at = gHeaps[h].address + c * MALI_HEAP_CHUNK;

		if (gHeaps[h].chunk[c] != NULL) {
			hakios_mali_mapped(at, MALI_HEAP_CHUNK);        /* there already: the GPU asked before it knew */
			grown = true;
			break;
		}
		IOBufferMemoryDescriptor * memory = gpuMemory(MALI_HEAP_CHUNK);
		if (memory == NULL || !enter(memory, at, MALI_HEAP_CHUNK, true)) {
			IOLog("HakiosMali: the heap at %llx cannot grow by 2 MB at %llx\n", gHeaps[h].address, at);
			OSSafeReleaseNULL(memory);
			break;
		}
		gHeaps[h].chunk[c] = memory;
		grown = true;
		break;
	}
	IOLockUnlock(gHeapLock);
	return grown;
}

/* nanoseconds of CLOCK_MONOTONIC (mach_continuous_time) to a deadline of mach_absolute_time */
static uint64_t
deadlineOf(int64_t timeout_ns)
{
	uint64_t now_ns, left;

	if (timeout_ns <= 0) {
		return 0;
	}
	if (timeout_ns == INT64_MAX) {
		return UINT64_MAX;
	}
	absolutetime_to_nanoseconds(mach_continuous_time(), &now_ns);
	if ((uint64_t) timeout_ns <= now_ns) {
		return 0;
	}
	nanoseconds_to_absolutetime((uint64_t) timeout_ns - now_ns, &left);
	return mach_absolute_time() + left;
}

/*
 * The display's driver asks before it shows a surface whether the GPU is through drawing into it (MiniIOMobileFramebuffer:
 * the last chain that named it, kept in the surface by submit()).
 */
bool
MiniIOSurfaceDrawn(MiniIOSurface * surface, uint64_t milliseconds)
{
	uint64_t deadline;

	if (surface->drawn == 0 || !hakios_mali_ready()) {
		return true;
	}
	clock_interval_to_deadline((uint32_t) milliseconds, NSEC_PER_MSEC, &deadline);
	return hakios_mali_wait(surface->drawn, deadline);
}

/* ---- the service ---- */

class HakiosMali : public IOService
{
	OSDeclareDefaultStructors(HakiosMali);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosMali, IOService);

bool
HakiosMali::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "HakiosMaliUserClient");
	setName("HakiosMali");
	registerService();
	return true;
}

/*
 * A test of the whole path before any program uses it: a write-value job through the queue into a buffer, and one
 * into a heap, which the GPU has to fault on and the worker to grow. Says in the log what it found.
 */
static void
selfTest(void)
{
	IOBufferMemoryDescriptor * memory = gpuMemory(PAGE_SIZE);
	uint64_t address = hakios_mali_address(PAGE_SIZE, 64 << 10, false);
	uint64_t heap = hakios_mali_address(4 * MALI_HEAP_CHUNK, MALI_HEAP_CHUNK, false);
	int h = -1;

	if (memory == NULL || address == 0 || heap == 0 || !enter(memory, address, PAGE_SIZE, true)) {
		IOLog("HakiosMali: self-test: no memory\n");
		strlcpy(gSelfTest, "no memory", sizeof(gSelfTest));
		OSSafeReleaseNULL(memory);
		return;
	}
	IOLockLock(gHeapLock);
	for (int i = 0; i < MALI_HEAPS; i++) {
		if (gHeaps[i].address == 0) {
			gHeaps[i].address = heap;
			gHeaps[i].size = 4 * MALI_HEAP_CHUNK;
			h = i;
			break;
		}
	}
	IOLockUnlock(gHeapLock);

	volatile uint32_t * job = (volatile uint32_t *) memory->getBytesNoCopy();
	uint64_t targets[2] = { address + 0x800, heap + MALI_HEAP_CHUNK + 0x40 };
	for (int t = 0; t < 2; t++) {
		volatile uint32_t * j = job + t * 32;                   /* 128 bytes apart */

		j[4] = 1u | (2u << 1) | (1u << 16);                     /* 64-bit, write value, index 1 */
		j[8] = (uint32_t) targets[t];
		j[9] = (uint32_t)(targets[t] >> 32);
		j[10] = 6;                                              /* immediate 32 */
		j[12] = 0xC0DE0000u | (uint32_t) t;
	}
	__asm__ volatile ("dsb sy");

	uint64_t began = mach_absolute_time(), ns, deadline;
	uint32_t f1 = hakios_mali_submit(address, 0), f2 = hakios_mali_submit(address + 128, 0);
	clock_interval_to_deadline(500, NSEC_PER_MSEC, &deadline);
	bool done = hakios_mali_wait(f2, deadline);
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);

	uint32_t heapValue = 0;
	IOLockLock(gHeapLock);
	if (h >= 0 && gHeaps[h].chunk[1] != NULL) {
		heapValue = *(volatile uint32_t *)((uint8_t *) gHeaps[h].chunk[1]->getBytesNoCopy() + 0x40);
	}
	IOLockUnlock(gHeapLock);
	snprintf(gSelfTest, sizeof(gSelfTest), "%s: jobs %u, %u %s in %llu us; buffer %x (wanted c0de0000), heap %x (wanted "
	    "c0de0001); headers %x %x", done && job[512] == 0xC0DE0000u && heapValue == 0xC0DE0001u ? "PASSED" : "FAILED",
	    f1, f2, done ? "over" : "NOT over", ns / 1000, job[512], heapValue, job[0], job[32]);
	IOLog("HakiosMali: self-test %s\n", gSelfTest);

	/* everything given back */
	hakios_mali_unmap(address, PAGE_SIZE);
	hakios_mali_address_free(address, PAGE_SIZE);
	memory->release();
	IOLockLock(gHeapLock);
	if (h >= 0) {
		hakios_mali_unmap(heap, gHeaps[h].size);
		for (int c = 0; c < MALI_HEAP_CHUNKS; c++) {
			OSSafeReleaseNULL(gHeaps[h].chunk[c]);
		}
		gHeaps[h].address = 0;
	}
	IOLockUnlock(gHeapLock);
	hakios_mali_address_free(heap, 4 * MALI_HEAP_CHUNK);
}

static void
startService(__unused void * parameter, __unused wait_result_t result)
{
	IOService * provider = NULL;
	HakiosMali * mali;

	/* the platform expert is IOKit's start: wait for it (this is started from the GPU's worker, early) */
	for (int i = 0; i < 1200 && (provider = IOService::getPlatform()) == NULL; i++) {
		IOSleep(100);
	}
	selfTest();
	mali = OSTypeAlloc(HakiosMali);
	if (mali == NULL || provider == NULL || !mali->init() || !mali->attach(provider)) {
		OSSafeReleaseNULL(mali);
		IOLog("HakiosMali: cannot create the service\n");
		return;
	}
	if (!mali->start(provider)) {
		mali->detach(provider);
		IOLog("HakiosMali: the service did not start\n");
	} else {
		IOLog("HakiosMali: the service is there (Panfrost's interface, version 1.1)\n");
		strlcat(gSelfTest, "; service registered", sizeof(gSelfTest));
	}
	mali->release();
}

void
HakiosMaliStart(void)
{
	thread_t thread;

	gHeapLock = IOLockAlloc();
	gMaliLock = IOLockAlloc();
	PE_parse_boot_argn("a51gpu_log", &gLog, sizeof(gLog));
	if (kernel_thread_start(startService, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* ---- the client ---- */

class HakiosMaliUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(HakiosMaliUserClient);

	task_t                  fTask;
	struct mali_buffer *    fBuffers;               /* handle n is fBuffers[n - 1] */
	uint32_t                fNextBuffer;
	uint32_t                fBufferCount;
	uint64_t                fBufferBytes;
	uint32_t                fStrangers;

	struct mali_buffer * buffer(uint32_t handle);
	void freeBuffer(struct mali_buffer * b);
	IOReturn createBO(struct drm_panfrost_create_bo * request);
	IOReturn submit(struct drm_panfrost_submit * request);
	IOReturn surfaceBuffer(struct hakios_surface * request);
	IOReturn userBuffer(struct hakios_user_memory * request);
	struct mali_buffer * newHandle(uint32_t * handle);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	IOReturn dispatchMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference);
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosMaliUserClient, IOUserClient);

bool
HakiosMaliUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
HakiosMaliUserClient::start(IOService * provider)
{
	if (!hakios_mali_ready() || !IOUserClient::start(provider)) {
		return false;
	}
	fBuffers = IONewZero(struct mali_buffer, MALI_BUFFERS);
	return fBuffers != NULL;
}

/* With gMaliLock held. */
struct mali_buffer *
HakiosMaliUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > MALI_BUFFERS || fBuffers[handle - 1].address == 0) {
		if (handle != 0 && fStrangers++ < 16) {
			IOLog("HakiosMali: buffer %u is asked for and is not there\n", handle);
		}
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/* With gMaliLock held: the GPU through with it first (or given up on), then out of its address space, then gone. */
void
HakiosMaliUserClient::freeBuffer(struct mali_buffer * b)
{
	uint64_t deadline;

	clock_interval_to_deadline(MALI_GIVE_BACK_MS, NSEC_PER_MSEC, &deadline);
	if (b->fence != 0 && !hakios_mali_wait(b->fence, deadline)) {
		IOLog("HakiosMali: a buffer at %llx is given back while the GPU still has it: kept for good\n", b->address);
		bzero(b, sizeof(*b));                           /* (its memory and addresses are the GPU's now) */
		return;
	}
	hakios_mali_unmap(b->address, b->size);
	if (b->heap >= 0) {
		IOLockLock(gHeapLock);
		for (int c = 0; c < MALI_HEAP_CHUNKS; c++) {
			OSSafeReleaseNULL(gHeaps[b->heap].chunk[c]);
		}
		gHeaps[b->heap].address = 0;
		IOLockUnlock(gHeapLock);
	}
	hakios_mali_address_free(b->address, b->size);
	if (b->client && b->memory != NULL) {
		b->memory->complete();                  /* (the client's pages unwired) */
	}
	OSSafeReleaseNULL(b->memory);
	if (b->surface != NULL) {
		MiniIOSurfaceDrop(b->surface);
	}
	fBufferCount--;
	fBufferBytes -= b->size;
	bzero(b, sizeof(*b));
}

/* A free handle, its slot cleared. With gMaliLock held; NULL: none left. */
struct mali_buffer *
HakiosMaliUserClient::newHandle(uint32_t * handle)
{
	for (uint32_t n = 0; n < MALI_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % MALI_BUFFERS;

		if (fBuffers[i].address == 0) {
			bzero(&fBuffers[i], sizeof(fBuffers[i]));
			fBuffers[i].heap = -1;
			*handle = i + 1;
			fNextBuffer = i + 1;
			return &fBuffers[i];
		}
	}
	return NULL;
}

/*
 * The memory of an IOSurface as a buffer: its pixels, from the page after the surface's shared state on (MiniIOSurface.h),
 * held for as long as the buffer is there. A surface that is a buffer of this client's already is that buffer again, as
 * with Linux's handles. Asked only to describe (kHakiosDescribe), no buffer is made. As HakiosGPU's surfaceBuffer().
 */
IOReturn
HakiosMaliUserClient::surfaceBuffer(struct hakios_surface * request)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	struct mali_buffer * b = NULL;
	IOMemoryDescriptor * memory;
	uint64_t size;

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (s->client != NULL) {
		MiniIOSurfaceDrop(s);                   /* (memory of a program's own with a surface made around it: not whole pages) */
		return kIOReturnUnsupported;
	}
	size = round_page_64(s->info.allocSize);
	memory = IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, size, kIODirectionInOut);
	if (memory == NULL || size == 0 || size > MALI_BUFFER_MAX) {
		OSSafeReleaseNULL(memory);
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kHakiosDescribe) {
		request->handle = 0;
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}
	IOLockLock(gMaliLock);
	for (uint32_t i = 0; i < MALI_BUFFERS; i++) {
		if (fBuffers[i].address != 0 && fBuffers[i].surface == s) {
			request->handle = i + 1;
			IOLockUnlock(gMaliLock);
			memory->release();
			MiniIOSurfaceDrop(s);
			return kIOReturnSuccess;
		}
	}
	b = newHandle(&request->handle);
	if (b != NULL && (b->address = hakios_mali_address(size, 64 << 10, false)) != 0 && enter(memory, b->address, size, true)) {
		b->memory = memory;
		b->kernel = s->kbase + PAGE_SIZE;
		b->surface = s;
		b->size = size;
		fBufferCount++;
		fBufferBytes += size;
		IOLockUnlock(gMaliLock);
		return kIOReturnSuccess;
	}
	if (b != NULL && b->address != 0) {
		hakios_mali_unmap(b->address, size);
		hakios_mali_address_free(b->address, size);
	}
	if (b != NULL) {
		bzero(b, sizeof(*b));
	}
	IOLockUnlock(gMaliLock);
	memory->release();
	MiniIOSurfaceDrop(s);
	return kIOReturnNoMemory;
}

/*
 * Pages of the client's own as a buffer, as Linux's userptr: wired for as long as the buffer is there and pushed out of
 * this processor's cache at every submission that names them (they are ordinary memory). As HakiosGPU's userBuffer().
 */
IOReturn
HakiosMaliUserClient::userBuffer(struct hakios_user_memory * request)
{
	IOMemoryDescriptor * memory = NULL;
	IOReturn prepared = kIOReturnNoMemory;
	struct mali_buffer * b;

	if ((request->address & PAGE_MASK) || (request->size & PAGE_MASK) || request->size == 0 ||
	    request->size > MALI_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	for (int pass = 0; pass < 2 && prepared != kIOReturnSuccess; pass++) {
		/* (pages the client may only read: the GPU too reads them only -- the second try) */
		memory = IOMemoryDescriptor::withAddressRange(request->address, request->size,
		    pass == 0 ? kIODirectionInOut : kIODirectionOut, fTask);
		if (memory == NULL) {
			return kIOReturnNoMemory;
		}
		prepared = memory->prepare();
		if (prepared != kIOReturnSuccess) {
			OSSafeReleaseNULL(memory);
		}
	}
	if (memory == NULL) {
		return kIOReturnVMError;
	}
	IOLockLock(gMaliLock);
	b = newHandle(&request->handle);
	if (b != NULL && (b->address = hakios_mali_address(request->size, 64 << 10, false)) != 0 &&
	    enter(memory, b->address, request->size, true)) {
		b->memory = memory;
		b->client = true;
		b->size = request->size;
		fBufferCount++;
		fBufferBytes += request->size;
		IOLockUnlock(gMaliLock);
		return kIOReturnSuccess;
	}
	if (b != NULL && b->address != 0) {
		hakios_mali_unmap(b->address, request->size);
		hakios_mali_address_free(b->address, request->size);
	}
	if (b != NULL) {
		bzero(b, sizeof(*b));
	}
	IOLockUnlock(gMaliLock);
	memory->complete();
	memory->release();
	return kIOReturnNoMemory;
}

IOReturn
HakiosMaliUserClient::createBO(struct drm_panfrost_create_bo * request)
{
	bool heap = request->flags & PANFROST_BO_HEAP, noexec = request->flags & PANFROST_BO_NOEXEC;
	uint64_t size = heap ? (request->size + MALI_HEAP_CHUNK - 1) & ~(MALI_HEAP_CHUNK - 1) : round_page_64(request->size);
	struct mali_buffer * b = NULL;
	uint32_t handle = 0;

	if (size == 0 || size > MALI_BUFFER_MAX || (heap && (!noexec || size > MALI_HEAP_CHUNKS * MALI_HEAP_CHUNK)) ||
	    (request->flags & ~(PANFROST_BO_NOEXEC | PANFROST_BO_HEAP))) {
		return kIOReturnBadArgument;            /* (Linux too: a heap must be NOEXEC) */
	}
	IOLockLock(gMaliLock);
	b = newHandle(&handle);
	if (b == NULL) {
		IOLockUnlock(gMaliLock);
		return kIOReturnNoMemory;
	}
	b->address = hakios_mali_address(size, heap || size >= MALI_HEAP_CHUNK ? MALI_HEAP_CHUNK : 4096, !noexec);
	if (b->address == 0) {
		IOLog("HakiosMali: no GPU addresses left for %llu bytes\n", size);
		IOLockUnlock(gMaliLock);
		return kIOReturnNoMemory;
	}
	if (heap) {
		IOLockLock(gHeapLock);
		for (int i = 0; i < MALI_HEAPS; i++) {
			if (gHeaps[i].address == 0) {
				gHeaps[i].address = b->address;
				gHeaps[i].size = size;
				b->heap = i;
				break;
			}
		}
		IOLockUnlock(gHeapLock);
	} else {
		b->memory = gpuMemory(size);
	}
	if ((heap && b->heap < 0) || (!heap && (b->memory == NULL || !enter(b->memory, b->address, size, noexec)))) {
		IOLog("HakiosMali: no memory for a buffer of %llu bytes (%u buffers, %llu MB with this client)\n", size,
		    fBufferCount, fBufferBytes >> 20);
		if (b->memory != NULL) {
			hakios_mali_unmap(b->address, size);
		}
		hakios_mali_address_free(b->address, size);
		OSSafeReleaseNULL(b->memory);
		bzero(b, sizeof(*b));
		IOLockUnlock(gMaliLock);
		return kIOReturnNoMemory;
	}
	b->size = size;
	b->flags = request->flags;
	b->fence = 0;
	fBufferCount++;
	fBufferBytes += size;
	request->handle = handle;
	request->offset = b->address;
	IOLockUnlock(gMaliLock);
	return kIOReturnSuccess;
}

/*
 * Where a submission's time goes (said with the requests every 30 s): waiting for a surface the display shows, pushing
 * surfaces and client pages out of the cache (how many bytes), waiting for room in the queue, the rest. Submissions were
 * 1200-1500 a second at 50-150 us each, 23% of the window server's drawing thread (glprof, 17 November).
 */
static uint64_t gSubmitShown, gSubmitFlush, gSubmitRoom, gSubmitAll, gSubmitSurfaceBytes, gSubmitClientBytes;
static uint32_t gSubmitSurfaces, gSubmitClients, gSubmits, gSubmitShownClean;

IOReturn
HakiosMaliUserClient::submit(struct drm_panfrost_submit * request)
{
	uint64_t t_began = mach_absolute_time(), t_flush = 0, t_room = 0, t_shown = 0;
	uint32_t handles[256], fence;
	uint32_t count = request->bo_handle_count;

	if (request->jc == 0 || count > 65536) {
		return kIOReturnBadArgument;
	}
	/*
	 * Not into a surface the display has yet to copy out, or is copying (MiniIOSurfaceWaitShown): CoreAnimation draws its
	 * next frame into the one being shown otherwise, and the panel gets it half drawn. 50 ms at the most.
	 */
	{
		MiniIOSurface * shown[8];
		unsigned found = 0;

		IOLockLock(gMaliLock);
		for (uint32_t done = 0; done < count && found < 8;) {
			uint32_t now = count - done > 256 ? 256 : count - done;

			if (copyin((user_addr_t)(request->bo_handles + done * sizeof(uint32_t)), handles, now * sizeof(uint32_t)) != 0) {
				break;
			}
			for (uint32_t i = 0; i < now && found < 8; i++) {
				struct mali_buffer * b = buffer(handles[i]);

				if (b != NULL && b->surface != NULL && b->surface->displaying != 0) {
					b->surface->retain();
					shown[found++] = b->surface;
				}
			}
			done += now;
		}
		IOLockUnlock(gMaliLock);
		for (unsigned i = 0; i < found; i++) {
			MiniIOSurfaceWaitShown(shown[i], 50);
			shown[i]->release();
		}
	}
	t_shown = mach_absolute_time();
	IOLockLock(gMaliLock);
	/*
	 * The GPU reads and writes memory, not this processor's cache. What the cache holds of an IOSurface goes out of it
	 * now: when the surface is said to have changed (its seed), and in any case once in a few milliseconds (not everybody
	 * who draws into a surface says so); the client's own pages, every time. Buffers made here are write-combined and
	 * need nothing. As HakiosGPU's submit().
	 */
	uint64_t now_abs = mach_absolute_time(), again;
	/*
	 * (Tests: boot-arg mali_flush=0, no IOSurface pushed out of the cache at all -- pictures may come out stale; mali_push_ms=N,
	 * an unchanged surface pushed again after N ms instead of 4. The display's frames took 37 ms a third while these pushes
	 * ran, 16 November: every surface a submission names, 11 MB each, line by line.)
	 */
	static uint32_t flush_surfaces = 1, push_ms = MALI_PUSH_AGAIN_MS;
	static bool flush_args;
	if (!flush_args) {
		PE_parse_boot_argn("mali_flush", &flush_surfaces, sizeof(flush_surfaces));
		PE_parse_boot_argn("mali_push_ms", &push_ms, sizeof(push_ms));
		flush_args = true;
	}
	nanoseconds_to_absolutetime((uint64_t) push_ms * NSEC_PER_MSEC, &again);
	for (uint32_t done = 0; done < count;) {
		uint32_t now = count - done > 256 ? 256 : count - done;

		if (copyin((user_addr_t)(request->bo_handles + done * sizeof(uint32_t)), handles, now * sizeof(uint32_t)) != 0) {
			break;
		}
		for (uint32_t i = 0; i < now; i++) {
			struct mali_buffer * b = buffer(handles[i]);

			if (b == NULL) {
				continue;
			}
			if (b->surface != NULL && flush_surfaces) {
				uint32_t seed = b->surface->shared()->seed;

				if (b->surface->shownClean) {
					/*
					 * the display's own picture, which only the GPU draws, out of the cache since the display read it
					 * (MiniIOMobileFramebuffer). Its seed is no sign of a processor's writing: the GL layer moves it at
					 * every glFlush, two a frame, and the push "again after 4 ms" came once a frame -- for 2 ms of the
					 * window server's drawing thread each time, 10 MB (surfaces 3 and 5 in turn, 17 November)
					 */
					b->seed = seed;
					if (now_abs - b->pushed >= again) {
						b->pushed = now_abs;
						gSubmitShownClean++;
					}
				} else if (seed != b->seed || now_abs - b->pushed >= again) {
					b->seed = seed;
					b->pushed = now_abs;
					flush_dcache64((addr64_t)(uintptr_t) b->kernel, (unsigned) b->size, FALSE);
					gSubmitSurfaces++;
					gSubmitSurfaceBytes += b->size;
				}
			} else if (b->client) {
				gSubmitClients++;
				gSubmitClientBytes += b->size;
				for (uint64_t offset = 0; offset < b->size;) {
					IOByteCount length = 0;
					addr64_t phys = b->memory->getPhysicalSegment(offset, &length, kIOMemoryMapperNone);

					if (phys == 0 || length == 0) {
						break;
					}
					length = MIN(length, b->size - offset);
					flush_dcache64(phys, (unsigned) length, TRUE);
					offset += length;
				}
			}
		}
		done += now;
	}
	__asm__ volatile ("dsb sy");
	t_flush = mach_absolute_time();
	/*
	 * A full queue is waited out here: Mesa does not try a refused submission again (panfrost_batch_submit failed: 16,
	 * and that batch's drawing is lost). Room comes as chains end; five seconds at the most.
	 */
	for (int tries = 0; (fence = hakios_mali_submit(request->jc, request->requirements)) == 0 && tries < 50; tries++) {
		uint64_t deadline;

		IOLockUnlock(gMaliLock);
		clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
		hakios_mali_wait(hakios_mali_completed() + 1, deadline);
		IOLockLock(gMaliLock);
	}
	t_room = mach_absolute_time();
	if (fence == 0) {
		IOLockUnlock(gMaliLock);
		IOLog("HakiosMali: the queue stays full: a submission refused\n");
		return kIOReturnBusy;
	}
	/* every buffer it names is the GPU's until this chain is over; a surface remembers it for the display (MiniIOSurfaceDrawn) */
	for (uint32_t done = 0; done < count;) {
		uint32_t now = count - done > 256 ? 256 : count - done;

		if (copyin((user_addr_t)(request->bo_handles + done * sizeof(uint32_t)), handles, now * sizeof(uint32_t)) != 0) {
			break;
		}
		for (uint32_t i = 0; i < now; i++) {
			struct mali_buffer * b = buffer(handles[i]);

			if (b != NULL) {
				b->fence = fence;
				if (b->surface != NULL) {
					b->surface->drawn = fence;
				}
			}
		}
		done += now;
	}
	gSubmitShown += t_shown - t_began;
	gSubmitFlush += t_flush - t_shown;
	gSubmitRoom += t_room - t_flush;
	gSubmitAll += mach_absolute_time() - t_began;
	gSubmits++;
	IOLockUnlock(gMaliLock);
	request->out_sync = fence;
	return kIOReturnSuccess;
}

IOReturn
HakiosMaliUserClient::clientClose(void)
{
	if (fBuffers != NULL) {
		IOLockLock(gMaliLock);
		for (uint32_t i = 0; i < MALI_BUFFERS; i++) {
			if (fBuffers[i].address != 0) {
				freeBuffer(&fBuffers[i]);
			}
		}
		IOLockUnlock(gMaliLock);
	}
	terminate();
	return kIOReturnSuccess;
}

void
HakiosMaliUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct mali_buffer, MALI_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

IOReturn
HakiosMaliUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct mali_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gMaliLock);
	b = buffer(type);
	if (b != NULL && b->memory != NULL) {           /* (a heap is the GPU's alone: Linux does not map it either) */
		b->memory->retain();
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gMaliLock);
	if (gLog >= 2) {
		IOLog("HakiosMali: map buffer %u -> %#x\n", (unsigned) type, ret);
	}
	return ret;
}

IOReturn
HakiosMaliUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	/*
	 * How many requests of each kind, and how long each took here, said every 30 s: a frame of the window server's took
	 * 30 ms of its thread, most of it in requests to this driver (glprof, 16 November).
	 */
	static uint64_t calls[128], spent[128], since;
	uint64_t began = mach_absolute_time();
	IOReturn ret = dispatchMethod(selector, args, dispatch, target, reference);
	uint64_t now = mach_absolute_time(), period;

	if (selector < 128) {
		OSIncrementAtomic64((SInt64 *) &calls[selector]);
		OSAddAtomic64((SInt64) (now - began), (SInt64 *) &spent[selector]);
	}
	nanoseconds_to_absolutetime(30 * NSEC_PER_SEC, &period);
	if (since == 0) {
		since = now;
	} else if (now - since >= period && OSCompareAndSwap64(since, now, &since)) {
		char line[600] = "";
		size_t at = 0;
		for (unsigned s = 0; s < 128 && at + 40 < sizeof(line); s++) {
			uint64_t n = calls[s], ns;
			if (n == 0) {
				continue;
			}
			absolutetime_to_nanoseconds(spent[s], &ns);
			at += (size_t) snprintf(line + at, sizeof(line) - at, " %#x: %llu (%llu us each)", s, n, ns / n / 1000);
			calls[s] = spent[s] = 0;
		}
		printf("HakiosMali: requests in 30 s, by selector:%s\n", line);
		if (gSubmits != 0) {
			uint64_t shown, flush, room, all;
			absolutetime_to_nanoseconds(gSubmitShown, &shown);
			absolutetime_to_nanoseconds(gSubmitFlush, &flush);
			absolutetime_to_nanoseconds(gSubmitRoom, &room);
			absolutetime_to_nanoseconds(gSubmitAll, &all);
			printf("HakiosMali: submissions in 30 s: %u, us each: %llu in all, %llu waiting for a shown surface, %llu pushing "
			    "out of the cache, %llu waiting for room; surfaces pushed %u (%llu MB), client buffers %u (%llu MB); not pushed, the "
			    "display had %u\n", gSubmits,
			    all / gSubmits / 1000, shown / gSubmits / 1000, flush / gSubmits / 1000, room / gSubmits / 1000, gSubmitSurfaces,
			    gSubmitSurfaceBytes >> 20, gSubmitClients, gSubmitClientBytes >> 20, gSubmitShownClean);
			gSubmitShown = gSubmitFlush = gSubmitRoom = gSubmitAll = gSubmitSurfaceBytes = gSubmitClientBytes = 0;
			gSubmitSurfaces = gSubmitClients = gSubmits = gSubmitShownClean = 0;
		}
	}
	return ret;
}

IOReturn
HakiosMaliUserClient::dispatchMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct drm_panfrost_submit submit;
		struct drm_panfrost_wait_bo waitBO;
		struct drm_panfrost_create_bo createBO;
		struct drm_panfrost_mmap_bo mmapBO;
		struct drm_panfrost_get_param param;
		struct drm_panfrost_get_bo_offset offset;
		struct drm_panfrost_madvise madvise;
		struct drm_gem_close close;
		struct hakios_wait_fence waitFence;
		struct hakios_surface surface;
		struct hakios_user_memory user;
	} r;
	size_t size = args->structureInputSize;
	struct mali_buffer * b;
	IOReturn ret = kIOReturnSuccess;
	uint32_t fence;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!hakios_mali_ready()) {
		return kIOReturnNotReady;
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	switch (selector) {
	case kPanfrostGetParam:
		ret = hakios_mali_param(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case kPanfrostCreateBO:
		ret = createBO(&r.createBO);
		break;
	case kPanfrostMmapBO:
	case kPanfrostGetBOOffset:
		IOLockLock(gMaliLock);
		b = buffer(r.offset.handle);
		if (b == NULL || (selector == kPanfrostMmapBO && b->memory == NULL)) {
			ret = kIOReturnBadArgument;
		} else {
			/* MMAP_BO: an offset that says which buffer (hakios-drm maps by handle); GET_BO_OFFSET: its GPU address */
			r.offset.offset = selector == kPanfrostMmapBO ? (uint64_t) r.offset.handle << 32 : b->address;
		}
		IOLockUnlock(gMaliLock);
		break;
	case kPanfrostSubmit:
		ret = submit(&r.submit);
		break;
	case kPanfrostWaitBO:
		IOLockLock(gMaliLock);
		b = buffer(r.waitBO.handle);
		fence = b != NULL ? b->fence : 0;
		IOLockUnlock(gMaliLock);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else if (!hakios_mali_wait(fence, deadlineOf(r.waitBO.timeout_ns))) {
			ret = kIOReturnTimeout;         /* (hakios-drm: ETIMEDOUT, which Mesa takes for "busy") */
		}
		break;
	case kPanfrostMadvise:
		IOLockLock(gMaliLock);
		ret = buffer(r.madvise.handle) != NULL ? kIOReturnSuccess : kIOReturnBadArgument;
		IOLockUnlock(gMaliLock);
		r.madvise.retained = 1;
		break;
	case kDrmGemClose:
		IOLockLock(gMaliLock);
		b = buffer(r.close.handle);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else {
			freeBuffer(b);
		}
		IOLockUnlock(gMaliLock);
		break;
	case kHakiosWaitFence:
		if ((int32_t)(r.waitFence.fence - hakios_mali_submitted()) > 0) {
			ret = kIOReturnBadArgument;     /* never issued: never over */
		} else if (!hakios_mali_wait(r.waitFence.fence, deadlineOf(r.waitFence.timeout_ns))) {
			ret = kIOReturnTimeout;
		}
		r.waitFence.completed = hakios_mali_completed();
		if (ret == kIOReturnTimeout) {
			ret = kIOReturnSuccess;         /* (the caller compares `completed`; a timeout is not an error here) */
		}
		break;
	case kHakiosSurface:
		ret = surfaceBuffer(&r.surface);
		break;
	case kHakiosUserMemory:
		ret = userBuffer(&r.user);
		break;
	case kPanfrostPerfcntEnable:
	case kPanfrostPerfcntDump:
	default:
		IOLog("HakiosMali: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	if (gLog >= 2) {
		switch (selector) {
		case kPanfrostCreateBO:
			IOLog("HakiosMali: create_bo %u bytes flags %x -> %#x: handle %u at %llx\n", r.createBO.size, r.createBO.flags,
			    ret, r.createBO.handle, r.createBO.offset);
			break;
		case kPanfrostSubmit:
			IOLog("HakiosMali: submit chain %llx req %x, %u buffers -> %#x: fence %u\n", r.submit.jc, r.submit.requirements,
			    r.submit.bo_handle_count, ret, r.submit.out_sync);
			break;
		case kPanfrostGetParam:
			IOLog("HakiosMali: get_param %u -> %#x: %llx\n", r.param.param, ret, r.param.value);
			break;
		default:
			IOLog("HakiosMali: request %#x (handle/first word %u) -> %#x\n", selector, r.close.handle, ret);
			break;
		}
	}
	if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
		if (args->structureOutputSize > size) {
			args->structureOutputSize = (uint32_t) size;
		}
		memcpy(args->structureOutput, &r, args->structureOutputSize);
	}
	return ret;
}
