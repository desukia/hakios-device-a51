/**
  Memory map of the Samsung Galaxy A51 (SM-A515F, Exynos 9611, 4 GB).

  Where the numbers come from (the phone itself, bootloader A515FXXU8HWI1):
    - RAM: the /memory@* nodes S-Boot writes into the device tree (secure holes already cut out):
        0x080000000 - 0x0BAB00000, 0x0C0000000 - 0x0E0000000, 0x0E1900000 - 0x100000000,
        0x880000000 - 0x900000000
    - carve-outs: /reserved-memory of the same tree, and S-Boot's command line
      (sec_debug logs at 0x91202000 - 0x91B88000, s3cfb.bootloaderfb=0xca000000)
    - registers: /proc/iomem and the device tree

  The UEFI lives at 0xA0000000, away from S-Boot's debug logs (0x912xxxxx) and the Linux
  load addresses (kernel 0x80080000, initrd 0x89000000) the boot shim is started from.

  Regions must not overlap and are listed in ascending order within each bank.
**/
#include <Library/MemoryMapLib.h>

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  //
  // DRAM bank 0: 0x80000000 - 0xBAB00000
  //
  {"Sec Debug Magic",    0x80000000, 0x00002000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},  // sec_debug_magic, kaslr
  {"RAM Partition",      0x80002000, 0x06FFE000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"EL2 Code",           0x87000000, 0x00200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},  // el2_code (Samsung uH)
  {"RAM Partition",      0x87200000, 0x0A000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"Sec Debug Logs",     0x91200000, 0x00A00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},  // sec_avc/tsp/disp/debug_next
  {"RAM Partition",      0x91C00000, 0x0E400000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"UEFI FD",            0xA0000000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI Stack",         0xA0200000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"DXE Heap",           0xA0240000, 0x05400000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"RAM Partition",      0xA5640000, 0x154C0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},

  //
  // DRAM bank 1: 0xC0000000 - 0xE0000000
  //
  {"Sec Log",            0xC0000000, 0x00080000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},  // seclog_mem
  {"RAM Partition",      0xC0080000, 0x00F80000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"XNU Payload",        0xC1000000, 0x04000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},  // HakiOS: the boot shim copies the XNUP container here
  {"RAM Partition",      0xC5000000, 0x05000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  // three framebuffers of 1080x2400x4 = 0x9E3400 each: the boot loader's at 0xCA000000, and at 0xCAA00000 and
  // 0xCB400000 two more that HakiOS's display driver switches DECON between (fb_decon_double, fb_decon_second,
  // fb_decon_third: a frame is written into one DECON neither reads nor is about to, without waiting for it)
  {"Display Reserved",   0xCA000000, 0x01E00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},
  {"RAM Partition",      0xCBE00000, 0x14200000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},

  //
  // DRAM bank 2: 0xE1900000 - 0x100000000
  //
  {"Secure Camera",      0xE1900000, 0x02000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"RAM Partition",      0xE3900000, 0x05B00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"ABOX",               0xE9400000, 0x02800000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"RAM Partition",      0xEBC00000, 0x01400000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"Modem",              0xED000000, 0x08A00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},  // modem_if
  {"CP RAM Logging",     0xF5A00000, 0x01E00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"RAM Partition",      0xF7800000, 0x01800000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"Debug Snapshot",     0xF9000000, 0x01800000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},  // ess_setup=0xf9000000
  {"RAM Partition",      0xFA800000, 0x00800000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"GNSS",               0xFB000000, 0x00800000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"RAM Partition",      0xFB800000, 0x01800000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"CHUB",               0xFD000000, 0x00080000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"RAM Partition",      0xFD080000, 0x02F80000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},

  //
  // DRAM bank 3: 0x880000000 - 0x900000000
  //
  {"RAM Partition",     0x880000000, 0x7E600000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"EL2 Early Mem",     0x8FE600000, 0x01A00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},

  //
  // Registers
  //
  // Not here: the GPIO controllers (PcdGpioCtrlAddr) and the Speedy bus (PcdSpeedyBusAddr). GpioDxe and SpeedyDxe map
  // them themselves with MapMemoryRegion(), whose gDS->AddMemorySpace() fails on a range already in this map; GpioDxe
  // then leaves the controller out and every key read returns EFI_NOT_READY (seen on the phone: Vol Up did nothing).
  //
  {"Chip Info",          0x10000000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PMU",                0x11860000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Clock Controller",   0x12100000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GICD",               0x12301000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GICC",               0x12302000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GICH",               0x12304000, 0x00002000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GICV",               0x12306000, 0x00002000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"CMU USB",            0x13030000, 0x00008000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"USB DRD PHY",        0x131D0000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},  // USBCON v3.0.0
  {"USB DRD Link",       0x13200000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},  // DWC3
  {"CMU FSYS",           0x13400000, 0x00008000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SYSREG FSYS",        0x13410000, 0x00002000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UFS UniPro",         0x13510000, 0x00008000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UFS HCI",            0x13520000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},  // HCI, vendor HCI, PHY
  {"UFSP",               0x13530000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UART",               0x13820000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Decon",              0x148B0000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
