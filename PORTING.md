# Galaxy A51 (SM-A515F, Exynos 9611): port status

Gathered on 2026-11-12 from the phone over adb with root. The raw dumps are in `../../../a51-dump/`: `a51.dtb`/`a51.dts`,
`iomem.txt`, `interrupts.txt`, `cpuinfo.txt`, `partitions.txt` and `dmesg.txt`.

**Status: the UEFI boots on the phone. The kernel side is not started.** HakiOS upstream supports only Qualcomm (redwood/SM7325), QEMU, and an
experimental MediaTek fork. No Exynos device has been ported, and its porting guide requires "an ARM64 Qualcomm SoC".

## The hardware

| Part | A51 | What HakiOS expects (redwood) | Reusable? |
|---|---|---|---|
| CPU | 4x A53 (MPIDR 0x000-3) + 4x A73 (0x100-3) | 8 cores numbered by one MPIDR field (`core_shift`) | needs loader change: two clusters |
| Interrupts | **GIC-400 / GICv2**, GICD 0x12301000, GICC 0x12302000 | GICv3 via `ICC_*` sysregs | **no**, new driver |
| Timer | armv8 timer 26 MHz, PPIs 13/14/11/10 | same | yes |
| PSCI | SMC | SMC | yes |
| Boot path | Samsung S-Boot, Odin download mode | Android fastboot `fastboot boot` | **no** |
| UEFI | none exists | Mu-Silicium platform | **no**, must write |
| UART | samsung,exynos-uart @ 0x13820000 | none / PL011 | new early-console driver (optional) |
| Storage | Samsung exynos-ufs @ 0x13520000 | Qualcomm UFS | needs a UEFI UFS driver |
| USB | DWC3 @ 0x13200000 | DWC3 | a UEFI USB-fn driver could be adapted |
| GPU | Mali-G72 MP3 | Adreno 642L + hakios-metal | **no**: software rendering only |
| Display | DECON-F, S-Boot fb at 0xca000000, 1080x2400 | UEFI GOP | yes, via a simple-framebuffer GOP |
| PMIC | S2MPU09 | Qualcomm PM7325 | no |
| Touch, audio, Wi-Fi, modem | Samsung/Exynos parts | Qualcomm parts | no |

The bootloader is unlocked: the FDT's real bootargs say `vbmeta.device_state=unlocked` and `verifiedbootstate=orange`.
`/proc/cmdline` is spoofed to "locked/green" by the root setup.

## Blockers, in order

1. **UEFI for Exynos 9611.** *Boots on the phone (2026-11-12): the logo, then "No Boot OS". The first image ignored Vol Up because GPIO/Speedy were also in the memory map (fixed, not yet re-tested).* It lives in `uefi/files/`:
   `Silicon/Samsung/S5E9611Pkg` (derived from Mu-Silicium's S5E7885Pkg, which has the same GIC-400 addresses and
   DPU 2.0 DECON), `Platforms/Samsung/a51Pkg` (derived from a10Pkg) and `Resources/Configs/a51.toml`. The facts are
   the phone's: the memory map is the S-Boot `/memory` banks with every `/reserved-memory` carve-out, the FD is at
   0xA0000000, the framebuffer is at 0xCA000000 (1080x2400), DECON-F is 0x148B0000 (`HW_SW_TRIG_CONTROL`=0x1281), the GPIO
   banks come from Samsung's exynos9610 pinctrl table, the keys are gpa1 5/6/7, and the boot image is header v2 with
   the stock DT table. The HakiOS payload window is 0xC1000000 (64 MB), because redwood's 0xF0000000 is the A51's
   modem carve-out. Build it with `python3 build_uefi.py -d a51` after `uefi/apply.sh a51`. Images are made with
   `mkxnuimg.py --device a51`.
   Not yet there: UFS (SamsungPkg/UfsDxe needs a 9611 PlatformUfsLib), USB fastboot (UsbfnDwc3Dxe needs a
   PlatformUsbfnLib) and the PMIC (S2MPU09).
   **First test:** flash `out/hakios-uefi-a51-RECOVERY.tar.md5` to recovery (Odin AP slot, or
   `heimdall flash --RECOVERY xnu-a51.img`), then boot recovery with Power + Vol Up while plugged into USB. Expected
   result: Silicium's boot logo, then its "No Boot OS" picture (no payload; with no UFS driver the log write to
   `logdump` finds no disk and writes nothing). A black screen means the DECON trigger or framebuffer address is wrong.
   A reboot straight back to S-Boot means the memory map or the shim is wrong. Undo it with `out/restore-stock-recovery-a51.tar.md5`.
2. **Getting it onto the phone without fastboot.** S-Boot only boots Android boot images from `boot`/`recovery`. The UEFI
   has to be packed as the kernel of a boot image (as Exynos edk2 ports and HakiOS's own U-Boot path do) and **flashed** with
   Odin or Heimdall, most safely into `recovery`. This writes to the phone, unlike upstream's RAM-only `fastboot boot`.
   Keep the stock `recovery.img` and a full firmware for Odin.
3. **The loader's transport.** `xl` and `xnuloader` talk fastboot-over-USB through UEFI's USB-fn protocol. The UEFI has to
   provide that over the A51's DWC3, or the loader needs to read the payload from a partition/RAM disk instead.
4. **A GICv2 driver** for the kernel (`xnu/files/osfmk/arm64/gic_v2_irq.c`), mirroring `gic_v3_irq.c`'s interface: the same
   `gic_*_spi_attach/mask/unmask`, SGIs through GICD_SGIR, the memory-mapped GICC (IAR/EOIR), and a `/arm-io/gic` node
   with GICD+GICC instead of GICD+GICR. The loader's `struct platform` needs a GIC version field.
5. **The loader's platform entry**: `platform_a51` in `xnuloader/loader.c`, plus cluster-aware core numbering
   (MPIDR aff1 = cluster). `small_cores` = cluster 0.
6. Then the HakiOS steps 2-4 of `docs/porting/index.md`: first boot to launchd with no drivers, then Exynos drivers written
   from Samsung's kernel source (Exynos9611Development/android_kernel_samsung_universal9611, GrassKernel). Mainline Linux
   has no 9611 support; postmarketOS's `device-samsung-a51` runs on the downstream kernel.

Steps 4 and 5 can be written and tested in QEMU (`-machine virt,gic-version=2`) before any phone hardware is involved.

## UFS and USB in the UEFI (2026-11-12: USB and UFS work on the phone)

Seen on the phone: the loader starts, says `usb: started`, and the PC sees it over USB. So the 9611 PlatformUsbfnLib,
UsbfnDwc3Dxe, UsbDeviceBridgeDxe and the loader's A51 detection (fb 0xCA000000, payload 0xC1000000) work. UFS: works, every partition is read (user, 2026-11-12).

- `S5E9611Pkg/Library/PlatformUfsLib`: Samsung's ufs-cal-9610 tables, ported onto S5E9810Pkg's UEFI CAL glue with the
  9610 differences: lane stride 0x140, board 0 (every entry applies), UNIPRO_DBG_APB on lane 1 only, 100 ms lock waits.
  BoardInit sets UFS_EMBD to 133.25 MHz, gpf0-0/1 to function 3 and FSYS IO coherency. The 1 lane, HS-G3 rate B
  target matches the Android kernel's log.
- `S5E9611Pkg/Library/PlatformUsbfnLib`: USBCON v3.0.0 PHY bring-up in phy-exynos-usb3p1.c order (minor-0 reset
  bits, link reset, Q-channel, HS tune from the phone's usb_hs_tune, PIPE3 override). PMU 0x704 mask 0x3 means not isolated.
  The USB clock gates are forced on.
- `hakios-uefi/files/.../UsbDeviceBridgeDxe`: Qualcomm's EFI_USB_DEVICE_PROTOCOL (what xnuloader's fastboot uses)
  on top of EFI_USBFN_IO_PROTOCOL from SamsungPkg's DWC3 driver. Endpoint 0 is answered from the loader's descriptors.
- xnuloader recognises the A51 by its framebuffer (0xCA000000): payload at 0xC1000000, platform "a51" (one core, GICv2
  addresses, no UART), a read-only UFS survey at start, and **no kernel start** until the kernel has a GICv2 driver.
  It writes nothing to storage: its log partition `logdump` does not exist on the A51, so logs come over USB.
- Test image: `out/hakios-a51-usbtest-RECOVERY.tar.md5` (UEFI + loader in USB mode). Expected: the loader's text on
  screen, including `ufs: unit N ...` lines and a GPT list, then `usb: started`. On the PC, `fastboot devices` shows
  `xnuloader`, and `fastboot oem log` / `fastboot get_staged log.txt` return the log. A hang at the logo means
  UfsDxe's init is stuck (hold Vol Down + Power to reboot).

## The kernel's GICv2 path (2026-11-12, works in QEMU, not yet run on the phone)

`xnu/files/osfmk/arm64/gic_v3_irq.c` drives a GICv2 (GIC-400) through the same entry points when the cores have no GIC
system registers (ID_AA64PFR0_EL1.GIC == 0). The loader reads the same bit: it writes `/arm-io/gic` as `arm,gic-400`
(distributor + CPU interface), and it saves and restores the ICC_* registers only on a GICv3. `qemu/run.sh` takes `QEMU_GIC=2`.

Seen in QEMU (logs/qemu-gicv3.log, qemu-gicv2.log, qemu-gicv2-smp4.log): GICv3 still reaches user space. GICv2 reaches user
space on 1 core and on 4 (each core learns its CPU-interface bit, 0x1/0x2/0x4/0x8, and the panic's stop signal brings all
cores back to the loader).

`CONFIG_UFS` is off for the A51: the kernel's `ufs=` path drives Qualcomm's UFS host controller.

Phone test: `out/hakios-a51-kernel-RECOVERY.tar.md5` holds the loader in USB mode, the kernel, `hello`, and
`-v cpus=1 loader_return_after=30`. Then run `fastboot oem run` and `fastboot get_staged log.txt`.

## ARMv8.0 (2026-11-12): the A51's cores have no PAN or UAO

On the phone the kernel showed nothing, even with `bootcpu=1` and even at checkpoint 1. QEMU with `QEMU_CPU=cortex-a53`
reproduced it. There were two ARMv8.1/8.2 assumptions, each an undefined instruction on a Cortex-A53/A73:
- the kernel set PAN unconditionally (`arm_init`, `copyio`, `pcb` checks). Now `arm64_pan_supported` (ID_AA64MMFR1_EL1.PAN)
  gates every access through `arm64_pan_set()/arm64_pan_get()` in `osfmk/arm64/proc_reg.h` (in xnu/hakios.patch). Cores
  with PAN keep it.
- the loader's return stubs cleared PAN and UAO with raw `msr` instructions: every way back to the loader (checkpoint,
  loader_return_after, panic) faulted. They are now guarded by ID_AA64MMFR1_EL1.PAN / ID_AA64MMFR2_EL1.UAO.
QEMU A53 + GICv2: user space and a clean return on core 0 and core 1. A76 + GICv3 unchanged.

## First XNU boot on the phone (2026-11-12)

On core 0 (`bootcpu=0`), with `-v cpus=1 loader_return_after=30`, the kernel ran to user space: "Hello from user space on
Darwin! I am process 1". Before that it brought up `gic: v2` and the screen console at 0xCA000000 (1080x2400) and ran all of bsd_init.
Then came the expected "initproc failed" panic when hello exits, and a clean return to the loader with the whole log
(logs/a51-boot0.txt). The kernel sat at pa 0x883004000.

Firmware quirks found on the way (the loader now copes with the first one):
- PSCI CPU_ON does not pass context_id in x0 (the new core gets 0xbfffe01c): other_cpu_entry finds other_cpu_start
  PC-relatively. Built, not yet flashed: out/hakios-a51-kernel-RECOVERY.tar.md5.
- PSCI AFFINITY_INFO returns -1 as a 32-bit value (4294967295), so the loader takes cores 1-3 as busy. It needs
  sign-extending before SMP (cpus=N).

Next: SMP across both clusters (MPIDR aff1), Samsung drivers (buttons, the S2MPU09 PMIC, touch), then the Darwin userland.

## Multi-core (2026-11-12: works across both clusters)

- Loader: the A51 has 8 cores in clusters of 4 (`cluster_size`): cores 0-3 are the A53s, MPIDR 0x0-0x3, marked
  small; cores 4-7 are the A73s, 0x100-0x103. PSCI results are sign-extended from 32 bits.
- Seen on the phone: the kernel boots to user space on its own on an A53 (core 1, 2) or an A73 (core 4, MPIDR 0x100,
  GIC interface bit 0x10).
- The kernel's PSCI start-up of a second core reset the whole phone (two A73s). The loader's CPU_ON (entry 0xA1xxxxxx)
  works; the kernel's entry is above 4 GB (0x8834xxxxx). The firmware treats PSCI values as 32-bit, so the loader now
  leaves a CPU_ON trampoline below 4 GB (`/chosen/psci-entry-trampoline`: "ldr x9, slot; br x9"). psci_smp.c writes the
  entry into its slot through an uncached mapping. Works in QEMU (`psci_trampoline=1`, 4 cores) and on the phone:
  - `bootcpu=4 cpus=2`: "2 of 2 processors are running" (A73s 0x100, 0x101; GIC interface bits 0x10, 0x20), user space, clean return.
  - `bootcpu=6 cpus=7`: "5 of 7 processors are running": A73s 0x102, 0x103 and A53s 1-3 (bits 0x40 0x80 0x8 0x4 0x2), user space,
    clean return. The two that did not join (0x100, 0x101) had been switched off with CPU_OFF earlier in the same boot.
- Small cores start 28 s late by default (`small_cores_after`, from redwood). Use `small_cores_after=0` for tests with hello.
- A core switched off with PSCI CPU_OFF (by the loader's stub or the kernel's psci_smp_stop_others) does not start again in
  the same boot: CPU_ON returns 0, but the core never arrives. Every core is available after a fresh boot. Not yet understood.
  Before a long session, boot the phone fresh. For the most cores, boot the kernel on an A73 (bootcpu=4..7) and set
  small_cores_after=0. bootcpu=0 would give all 8, but the loader then cannot watch.
- From a parallel session (same image, before the trampoline): `cpus=4 bootcpu=1` reset the phone at once. The cores
  were offered as 1 2 3 100, and 0x100 (an A73, not marked small) was started by the kernel straight away. `bootcpu=4 cpus=2` reset it
  too. Both are kernel-started cores with entries above 4 GB, consistent with the trampoline explanation. The alternative (the A73
  rail off under UEFI, ACPM) is unlikely: the loader started A73 core 4 and the kernel ran on it alone to user space.

## Darwin userland 16.5 (2026-11-13)

- IPSW: iPhone10,3/10,6 16.5 20F66 (6,165,698,198 bytes), the only build HakiOS's userland pipeline takes. iOS 16.7.x
  would need the kernel rebased to Darwin 22.6 as well.
- This host's umask is 002, and 7-Zip does not keep Apple's modes, so the extracted tree and the launchd plists came out
  group-writable. launchd refused SpringBoard's and backboardd's jobs ("bad ownership/permissions"). Fix: strip go-w
  from sysimage/{rootfs,cryptex,cryptex-app} (no sticky directories there) and build with `umask 022`.
- QEMU, Cortex-A53 + GICv2 (the A51's CPU and GIC), the A51 kernel build, RAM recipe: no illegal instruction anywhere
  (Apple's arm64 code runs on ARMv8.0). With 4 cores: 88 daemons, then SpringBoard's home screen at about 9 minutes under TCG
  (logs/a53-smp4-9min.png).
- Next, on the phone: CONFIG_UFS back on (ufs=2 is served by the loader through UEFI's 9611 UFS driver; only ufs_native=
  is Qualcomm's), a `darwin` partition carved off userdata, FLASHBOOT=1 images, and a root on flash. The kernel's 2 GB window
  (one RAM bank) may be the next limit.

## The `darwin` partition: plan (2026-11-13, nothing written yet)

Read in TWRP (inputs in a51-dump/carve/): userdata is sda32, plain f2fs (raw superblock magic, no metadata encryption),
106.21 GiB, 3 GB used, the last partition. Samsung leaves 251 blocks of slack before the backup GPT, and
tools/userdata-carve.py now takes up to 2 MB of slack into the new partition. `userdata-carve.py a51-dump/carve 20`: userdata
3392512..25992955 (86.21 GiB, the f2fs shrunk to 22600192 blocks), darwin 25993216..31236090 (20.000 GiB, sda33, APFS type);
both tables check out, and only entries 31/32 and the headers change.
The shrink needs F2FS_IOC_RESIZE_FS in the running kernel: TWRP's (Grass 4.14.113) answers ENOTTY. Next: probe under Android's
kernel (4.14.356): ENOTTY as well (GrassKernel has no F2FS_IOC_RESIZE_FS; same ioctl number as the 9611 source, which has it). Options: --wipe + TWRP Format Data, an offline resize.f2fs shrink in TWRP, or a kernel with the ioctl.

## The `darwin` partition: done (2026-11-13)

`userdata-carve.py --wipe a51-dump/carve 20` was written from TWRP (/data unmounted), in the tool's order; the read-back was
identical. After a reboot to recovery: sda32 userdata 86.21 GiB, sda33 `darwin` (PARTNAME darwin) 20.002 GiB at LBA
25992704..31236090. `twrp format data` made a new f2fs of 22600188 blocks (the partition: 22600192). Android boots on it
(a fresh setup). The original tables are in a51-dump/carve/gpt-{head,tail}.orig.bin. To undo: put them back from TWRP, then Format Data.

## iOS on the flash (2026-11-13)

- System region (darwin blocks 0..2042367): Apple's root volume (096-04088-077.dmg as raw APFS, 5628755968 bytes, sha256
  9f430a58...), then the OS cryptex 096-04666-078.dmg (2279bf44...) and the App cryptex 096-03536-083.dmg (6b9d1f09...),
  byte for byte; whole image sha256 9efb156f... Built in ~/Desktop/ios-a51/darwin-img. tools/apfs/dmg.py now falls back to
  xnu-deps/host/bin/lzfse when the python lzfse module is missing.
- tools/darwin-write.py: per-device constants (DARWIN_DEVICE=a51: sda33, the A51's GUID and LBAs, a 128-entry GPT,
  13520000.ufs), and `install-system IMAGE` (DARWIN_WRITE=yes DARWIN_SYSTEM=yes) for phones without fastboot. Written from
  rooted Android: dd at 85 MB/s, read back the same.
- Kernel: CONFIG_HAKIOS_UFS=y, CONFIG_UFS=2 (loader-served; ufs_native= stays off). Userland: `umask 022;
  CRYPTEXES=flash FLASHBOOT=1 REST=1 userland/build.sh` -> flashboot.apfs (4 MB, the payload's resident image).
- Boot: ~/Desktop/ios-a51/a51-ios.sh (root_apfs=ufs_darwin ufs=2 bootcpu=4 cpus=7, screenshots every 30 s).
- Not yet: the data volume (mkdatavol.py needs mkfs.hfsplus, i.e. hfsprogs, and has the POCO's darwin size); /private/var is a RAM filesystem.
- First flash-root boot (2026-11-13): the root volume and both cryptexes mounted from darwin, `memory 2016 MB, 7 cpu`, then
  pid 1 exited: "failed to query hardware target: 22". dyld reads the `hw.target` sysctl, which comes from the identity the
  loader writes into the device tree. The flashed image's loader was a `make loader-open` build (a placeholder identity).
  Rebuilt with the IPSW identity (`make loader`): out/hakios-a51-kernel-RECOVERY.tar.md5. Use `make loader` for every phone image.
- 2026-11-13: iOS 16.5 BOOTED on the A51 from darwin (identity loader, a51-ios.sh), seen on the phone's screen. Touch does not work (no driver yet).

## iOS 16.5 HOME SCREEN ON THE PHONE (2026-11-13)

With the identity loader: `a51-ios.sh 300` (root_apfs=ufs_darwin ufs=2 bootcpu=4 cpus=7 small_cores_after=0 -v). SpringBoard and
backboardd were spawned at about 25 s, 152 daemons in all; the home screen (wallpaper, Calendar widget, live clock) is in the loader's
screenshots from about 2 minutes on (logs/a51-ios-276s.png). No panic and no jetsam kill in 5 minutes on the 2 GB window, and
a clean return to the loader. The text over the screen is -v's console: leave -v out once the boot no longer needs watching.
Not yet: touch and buttons (no A51 input drivers: iOS shows, but cannot be used), the data volume (/private/var is in RAM),
a boot without the PC (the recovery image's payload in one-shot mode with the flash-root command line), the GPU (software
rendering only), sound, Wi-Fi, battery.

## Buttons and touch (2026-11-13: work)

devices/a51/drivers/input/hakios.a51input.c (CONFIG_HAKIOS_A51INPUT, boot-args touchscreen=1 [buttons=1]):
- buttons: gpa1 5/6/7 (volume up, volume down, power, active low), read 60 times a second. Seen: every key pressed and
  released, and iOS reacting to them.
- touch: Melfas MSS100 at 0x48 on the s3c2440-type I2C at 0x13870000 (polled master after i2c-s3c2410.c, SCL = PCLK/512),
  supply tsp_ldo_en = gpg3-1, events read while gpa0-0 is low (MIP4, event format 10, 16-byte events). Seen: iOS used by
  touch (swiping pages, opening apps).
Run: `a51-ios.sh 300 touchscreen=1`.

## GPU (2026-11-13): reached

devices/a51/drivers/gpu/hakios.a51gpu.c (CONFIG_HAKIOS_A51GPU, boot-arg a51gpu=N), run with `EXTRA=a51gpu=N a51-run.sh CORE 0 TAG 1`:
- pd-g3d is on when the kernel starts (G3D_CONFIGURATION f, G3D_STATUS 3000f), and PLL_G3D runs (con0 a0a00412: enable and lock).
- SMC 0x82000410 (EXYNOS_WAKEUP_PD_DOWN, 0x11410204, runtime PM), Samsung's exynos_pd_tz_restore: the monitor answers 0.
- Mali registers at 0x11500000: GPU_ID 62210010 (Mali-G72 r0p1), shader cores 0x7 (MP3), 1 tiler, 1 L2, AS_PRESENT ff,
  JS_PRESENT 7, MMU_FEATURES 2830 (48-bit VA, 40-bit PA), GPU_IRQ_RAWSTAT 100 (reset completed).
Next: (1) the kernel driver, after Linux's panfrost: soft reset, power the L2/tiler/shader cores, an MMU address space with
Mali LPAE page tables, a first job run to completion (job slot IRQs polled); (2) an IOKit service with panfrost's interface
(create/mmap BO, submit, wait, params) behind tools/mesa/hakios-drm; (3) Mesa built with panfrost (mk-mesa.sh DRIVERS=softpipe,panfrost);
(4) the window server on GL (userland GL=1, src/inject.c's GPU check taught Mali). Clock and voltage stay as the firmware left them
(DVFS through ACPM is later work). Metal stays Adreno-only.

### GPU driver, steps 5..8 (run: the GPU runs jobs)

`a51gpu=8` goes on past reading the IDs: soft reset (GPU_CMD 1, wait RESET_COMPLETED), L2 then shader cores 0x7 and
tiler on, a Mali LPAE table (4 levels, 4 KB granule, leaves PA|0x2C1, MEMATTR 0x888D88) mapping one 16 KB kernel page
at GPU address 0x1000000 into AS0, then a write-value job (type 2, immediate-32 0xC0FFEE11 to +0x100) on job slot 1.
Everything is polled, with interrupts masked. Test: `EXTRA="a51gpu=8" ./a51-run.sh 4 0 gpu8 1`. If it stops early, run
it again with a lower N to narrow down which step fails.

Result (logs/a51-gpu8.txt): reset complete; L2 1, shader cores 7, tiler 1 ready; AS0 active with no fault; the job
finished on JS1 (JOB_INT_RAWSTAT 2, header exception status 1 = done) and the page held 0xC0FFEE11. No MMU or GPU
fault. (The panic at the end of that log is the test ramdisk's pid 1 exiting, as in every a51-run.sh run.)
Next: interrupts on the GIC, then the IOKit service with Panfrost's interface for tools/mesa/hakios-drm.

### GPU kept for programs: interrupts and HakiosMali (run: works)

`a51gpu=8 a51gpu_keep=1`: after step 8 the GPU stays on. hakios.a51gpu.c ("the GPU kept for programs") owns AS0 (tables
made on demand, GPU addresses 32 MB..4 GB as Mesa expects), a queue that runs job chains one at a time (slot 0 for
PANFROST_JD_REQ_FS, slot 1 otherwise), and a worker thread. The interrupts are SPIs 97 GPU, 98 JOB and 99 MMU (from the
DT; Linux has them edge-triggered). Their handlers only mask the line and wake the worker. The worker also checks every
2 ms while a job runs, so it still works if the GICv2 SPIs don't arrive; the log says once per line whether they do.
Failed jobs reset the GPU. Faults on a heap buffer grow it by 2 MB; any other fault switches AS0 off, so the job fails
instead of hanging. `a51gpu_log=1` logs each job.

hakios.mali.cpp (iokit) is the service HakiosMali, speaking panfrost uAPI 1.1 (Mesa 25 requires at least 1.1): GET_PARAM,
CREATE_BO (incl. HEAP), MMAP_BO (offset = handle<<32; hakios-drm maps by handle), GET_BO_OFFSET, SUBMIT (fence back in
out_sync), WAIT_BO, MADVISE, GEM_CLOSE, plus 0x7b wait-for-fence. Before registering, it runs a self-test: two write-value
jobs through the queue, one into a buffer and one into a heap, which has to fault and grow.
Test with a full iOS boot (the a51-run.sh ramdisk panics too early): `./a51-ios.sh 120 a51gpu=8 a51gpu_keep=1`.
Still to do: hakios-drm for panfrost (open "panfrost", mmap by handle, syncobjs over 0x7b) and Mesa built with panfrost.

Result (2026-11-13, full iOS boot, `./a51-ios.sh 120 a51gpu=8 a51gpu_keep=1`): iOS at the home screen, no panic, and the
30-second status line (`a51gpu: kept GPU: ...`, kept late in the log on purpose) says: self-test PASSED. Both jobs
finished in 1.3 ms, the buffer read c0de0000, and the heap read c0de0001 after one heap fault was grown. Interrupts work
on the GICv2: JOB 2, MMU 1, GPU 0. No resets; HakiosMali registered.

### Mesa on the Mali (2026-11-13)

Mesa 25.0.7 built with `DRIVERS=softpipe,panfrost` (tools/mesa/zz-panfrost-darwin.patch, hakios-drm panfrost mode; meson,
mako and pyyaml are in ~/Desktop/ios-a51/venv, so put its bin on PATH for tools/mk-mesa.sh). On the phone, with
`GL=test RC_JOBS=rc.jobs.gltest CRYPTEXES=flash FLASHBOOT=1 REST=1 userland/build.sh` and
`./a51-ios.sh 600 touchscreen=1 a51gpu=8 a51gpu_keep=1`, gl_test reports
"Mesa; Mali-G72 (Panfrost); OpenGL ES 3.1 Mesa 25.0.7". Its first jobs reached the GPU (4 jobs over, 0 failed, the tiler
heap grew a second time), then it stopped at "MESA: error: export failed": Panfrost makes its GL fences by exporting
sync files, which hakios-drm only stubbed. Sync files are now implemented in-process (a /dev/null descriptor that
records which fence it stands for); this is rebuilt but not yet run.
userland/build.sh fix: with FLASHBOOT=1, files over 4 MB stay in root.apfs. A flash boot has no system image, so the
/usr/local/redwood symlink pointed nowhere; with Mesa inside, root.apfs is 6 MB.
Note: out/libOSMesa.8.dylib is now the panfrost build; rerun mk-mesa.sh with the default DRIVERS for redwood's Adreno.

#### Where it stopped (2026-11-13, evening)

The sync-file fix worked: gl_test no longer crashes (the segfault came from the NULL fence left by the failed export). It
reads back a full 1080x2400 frame, but every pixel is 0: 2 of Mesa's 4 jobs FAILED on the GPU, and the driver did 2
resets. The failure details were printed at about 60 s and fell out of the returned kernel log, which only starts at
about 98 s. Built, not yet run: the kernel keeps its last 4 troubles (failed job with chain address and exception name,
MMU fault, GPU fault, timeout) and repeats them with the 30 s status line.
Next run: `./a51-ios.sh 600 touchscreen=1 a51gpu=8 a51gpu_keep=1 a51gpu_log=2`, then read the
`a51gpu: trouble N of M:` lines. gl_test now has a SIGSEGV handler that prints pc, lr and a backtrace as lib+offset;
symbolize against the unstripped xnu-deps/build/mesa/build-softpipe-panfrost/src/gallium/targets/osmesa/libOSMesa.8.dylib.

#### The crash found (2026-11-14)

gl_test's crash handler caught it this time. The crash is in glReadPixels → st_ReadPixels → panfrost_ptr_map →
util_blitter_blit → blitter_draw → panfrost_draw_vbo, at `str q1, [x0]`: the viewport descriptor written into a freshly
allocated pool buffer whose CPU mapping isn't there. Cause: panfrost_bo_munmap() calls munmap() on a mapping IOKit made
(IOConnectMapMemory64). IOKit doesn't notice, so when the buffer comes back from Mesa's buffer cache and is mapped again,
IOKit returns the old, now-unmapped address. Fix in zz-panfrost-darwin.patch (pan_bo.c): on Darwin, unmap through
hakios_drm_unmap(). The same bug likely caused the 2 failed jobs of the earlier run (descriptors written to the wrong
memory, so the GPU read garbage). Rebuilt (Mesa and root.apfs), not yet run.

#### First triangle drawn by the Mali (2026-11-14)

With the unmap fix, gl_test on the phone reports: Mali-G72 (Panfrost), OpenGL ES 3.1. A 1080x2400 frame was drawn in
60 ms and read back in 57 ms, with GL error 0. The corner reads 26 26 51 255 (the clear colour 0.1, 0.1, 0.2) and the
triangle's corners read 235 12 8 / 12 235 8 / 10 10 235; 26% of the pixels are inside the triangle, exactly what its
geometry gives. GPU: 6 jobs, 0 failed, no resets, heap grown twice. So the earlier failed jobs were the same stale-mapping
bug. Not yet: putting a GL picture on the panel (gl_test needs a GPU service with the surface/show requests, which
HakiosMali lacks) and the window server drawing with GL (userland GL=1, src/inject.c, which only knows the Adreno).

### The window server on GL (written, built, not yet run; 2026-11-14)

- HakiosMali: IOSurface buffers (0x7e, with describe) and buffers of a client's own pages (0x7d), as HakiosGPU has them.
  At each SUBMIT it flushes the named surfaces from the CPU cache (when the seed changed, or after 4 ms) and the
  client's own pages (always), and records each surface's fence in surface->drawn. It also provides
  MiniIOSurfaceDrawn(), so the display waits for the GPU before showing a surface (the stub in
  drivers/core/hakios.stubs.cpp steps aside for HAKIOS_A51GPU too).
- Mesa: tools/mesa/zz-panfrost-surfaces.patch (OSMesa's surface hook for Panfrost too; Panfrost's import asks the
  size through hakios_drm_import_size() instead of lseek() on the surface ID).
- userland/src/inject.c: HakiosMali counts as a GPU, so CoreAnimation loads libhakios.gl and draws with OpenGL ES.
- Build: `GL=1 CRYPTEXES=flash FLASHBOOT=1 REST=1 userland/build.sh`; run: `./a51-ios.sh 600 touchscreen=1 a51gpu=8 a51gpu_keep=1`.

#### The window server draws on the Mali (2026-11-14)

First GL=1 run: "the server has an accelerated renderer". The home screen (wallpaper, widgets, icons) and Control Center
are drawn through Mesa Panfrost on the Mali: 38515 GPU jobs in 600 s, 0 failed, 0 resets. Frames average about 30 ms
(about 33 fps). Per frame that is about 4.2 ms waiting for the GPU, 2.6 ms making textures from IOSurfaces and 0.3 ms
handing commands on; the rest is CPU time in the window server. 1171 IOSurfaces were copied rather than shared (surfaces
around a program's own memory, which HakiosGPU doesn't share either).
Bug: 232 × "MESA: error: panfrost_batch_submit failed: 16". HakiosMali returned EBUSY when its 64-entry queue was
full, and Mesa drops a refused batch. Fixed (built, not yet run): SUBMIT waits for room (up to 5 s) and the queue holds
256 chains.
The GPU still runs at 260 MHz (PLL_G3D a0a00412; the DT allows up to 1053 MHz): DVFS through ACPM is next.

Queue fix run (2026-11-14): 0 × "panfrost_batch_submit failed" (232 before), 44645 jobs, 0 failed, and the window server
now averages about 22 ms a frame (about 45 fps), about 4 ms of it waiting for the GPU.

### GPU clock through ACPM (written, built, not yet run)

`a51gpu_clock=1` reads ACPM's channel table from the APM's SRAM (0x2039000 + 0x6f00) and sends FREQ_GET for DVFS domain 4
(g3d_cmu_cal_id 0xb040004) on channel 5. Nothing changes. `a51gpu_clock=2 a51gpu_mhz=N` sends FREQ_REQ for one of the
DT's levels (260..1053 MHz; default 572) and reads PLL_G3D back. The firmware sets the voltage with the clock, as on
Android (Samsung acpm_dvfs.c / acpm_ipc.c, copied to ref/acpm/). Test early-boot steps with a51-run.sh, whose log keeps
them: `EXTRA="a51gpu=4 a51gpu_clock=1" ./a51-run.sh 4 0 clk1 1`.

Clock step 1 on the phone (2026-11-14): the ACPM table reads fine. 6 plugins; 15 channels, 8 for the AP, at SRAM
+0x5ad8, 72 bytes each, exactly Samsung's struct ipc_channel. DVFS channel 5 is id 5, type 2, ap_poll 0, and has a
**1-element** queue of 16 bytes (AP→APM rear/front/base 0x73a8/0x73ac/0x73b0, APM→AP 0x7388/0x738c/0x7390, indices 0/0).
Channels 0, 1, 2, 4 and 7 have rings of 15, 3, 3, 3 and 5; channel 3 (type 2) has its queues in the mailbox's SR registers.
FREQ_GET (domain 4) through the doorbell alone (INTGR0 bit 16+5) got no answer in 50 ms: INTSR1 stayed 0 and the tx
slot didn't change. Samsung's acpm_ipc_send_data() can't send on a 1-slot ring either: its full test (front+1 mod len ==
rear) is always true there, and framework.h says the firmware sees a request only by rx_front != rx_rear. So either
the firmware treats type-2 channels in a way the published source doesn't show, or Android does something else.
Stopped there rather than poke the APM, which also runs CPU and memory power. Next: ground truth from Android (does GPU
DVFS work there: /sys/kernel/gpu/gpu_clock, dmesg acpm lines).

Android shows the missing piece (2026-11-14): its kernel uploads ACPM plugins at every boot ("Loading
exynos9610_acpm_fvp.fw firmware ... plugin(id = 2) attach done!", tmu as id 4; "dvfs_g3d - id : 4"), and Android's GPU
DVFS works (/sys/kernel/gpu: 11 levels, Interactive governor). The plugin files are built into Samsung's kernel image
(its firmware/ directory, 5120 bytes each). They're in fw-acpm/ (not in git), and userland/build.sh puts them in
root.apfs fw/. Clock steps now run in a thread of their own that waits for the small image; their result repeats with
the status line ("a51gpu: clock: ..."). Steps: 1 reads the plugin table (48-byte struct plugin) and channels; 2 uploads
and attaches the plugins Android attaches (channel 4, cmd 1<<25 | id<<26) and sends FREQ_GET; 3 sends FREQ_REQ
(a51gpu_mhz). Test: `./a51-ios.sh 120 a51gpu=8 a51gpu_keep=1 a51gpu_clock=1`, then =2, then =3.

Clock steps 1 and 2 on the phone (2026-11-14): step 1 found plugins 0, 1, 3 and 5 attached and 2 and 4 to attach (0x2b /
0x14), exactly the fvp and tmu plugins Android's kernel attaches. Step 2 uploaded both and the firmware answered both
attaches on channel 4, the first ACPM answers to HakiOS; iOS and the GPU ran normally after. But FREQ_GET on DVFS
channel 5 still gets no answer, and its descriptor is unchanged after the attach: 1 x 16, type 2, ap_poll 0. All
channels that answer are type 1 rings; the type-2 ones (3, 5, 6) have one slot (channel 3's in the mailbox's SR
registers). Samsung's published acpm_ipc.c has no path for a one-slot channel (its full test is always true there),
so Android must send DVFS requests some way that source doesn't show. Parked. The steps stay behind a51gpu_clock=N.
Leads: Android's /sys/kernel/debug/acpm_framework, ACPM's own log buffer in SRAM (log_buf_rear/front in acpm_framework),
or a newer Samsung acpm_ipc.c that handles type-2 channels.

### Where the lag comes from (profile, 2026-11-14)

GL=1 with RC_JOBS=rc.jobs.cputop (rwtouch swipes, cputop every 5 s), boot-args touch_lat=1 cpuload=100:
- CPU clocks as the bootloader leaves them: A53s at 1053 MHz (PLL con0 a0f30311; max 1850), A73s at 1456 MHz (a1500311;
  max 2400), both on their PLLs, divider 1. That is about 60% of their top speed; changing it needs ACPM, the same
  parked DVFS channel as the GPU (domains 2 and 3).
- Idle, the system kept about 2.3 processors busy: vmd 92-96%, launchd 60-76%, SpringBoard about 45%, backboardd about
  20%. vmd is visual voicemail: with CommCenter denied, it asks launchd for com.apple.commcenter.coretelephony.xpc in a
  tight loop, and launchd answers and logs each refusal. Fixed: mkjobs.py denies com.apple.voicemail.
- The window server's own frames averaged about 11 ms in this run (3.7 ms of it waiting for the GPU).
Next: rerun the same profile without vmd; then look at SpringBoard's idle 45%.
Without vmd (rerun): idle CPU went from about 235% to about 116% (launchd about 50%, SpringBoard about 45%, backboardd
about 19%), and frames averaged 10.4 ms. launchd's remaining log volume is small, so next: QUIET=1 (launchd's stderr
off the console) and no -v (a51-ios.sh VERBOSE=, so no kernel text console drawn over the screen).
QUIET=1 and no -v (VERBOSE= ./a51-ios.sh): idle CPU about 15% in all (backboardd about 11%, SpringBoard about 2%;
launchd off the list), against about 235% at first and 116% without vmd. So the console was most of launchd's and
SpringBoard's idle cost: the text console draws every line into the framebuffer, on the CPU, in the context of the
process that wrote it. Frames averaged 9.4 ms. While widgets start (95-125 s), Weather, Calendar, WorldClock and Stocks
widget extensions take up to 99% each; that settles by 145 s. The screen is clean (logs/a51-ios-273s.png: the home
screen drawn through the Mali, no console text; a screenshot from an earlier run had misled me).

### The DVFS channel, found (2026-11-14)

Channel 5 was the wrong channel. Type 2 is TYPE_BUFFER (fw_header framework.h): a one-slot buffer the APM posts notices
into, which Samsung's driver only ever reads (dequeue_policy; Motorola's exynos9610 tree shows the same). acpm_dvfs's
own DT node (channel 5) is for acpm_noti_mif, MIF notices to the AP. DVFS requests (exynos_acpm_set_rate) go through
acpm_dvfs.dev, which cal_if_init sets to the clock controller's node, clock-controller@0x12100000:
acpm-ipc-channel = 0, a 15-slot TYPE_QUEUE ring, polled. Step 2 now sends FREQ_GET on channel 0 for domains 2 (A53s),
3 (A73s) and 4 (GPU). Built, not yet run.
Step 2 on channel 0: the firmware answers at once (no timeout), but FREQ_GET gives 0 for all three domains. Samsung's
cpufreq (exynos-acme.c) only ever sends FREQ_REQ (cal_dfs_set_rate) and keeps the current rate itself, so FREQ_GET
probably isn't answered with a rate by this firmware. Step 3 now asks levels from the DT's tables (cpufreq domain@0/@1
mif-perf: A53s up to 1742 MHz, A73s up to 2314 MHz) and checks the PLLs. Default 1638 / 2080 / 572 MHz, not the
highest: there's no thermal control yet. Boot-args a51cpu0_khz, a51cpu1_khz, a51gpu_mhz.
Step 3 WORKS (2026-11-14): FREQ_REQ on channel 0 set all three. PLLs went from A53s 1053 → 1638 MHz (con0 a0bd0310),
A73s 1456 → 2080 MHz (a0f00310) and GPU 260 → 572 MHz, with all three requests answered. The 2-minute run stayed stable
at those speeds: 17160 GPU jobs, 0 failed, no resets, no panic. So the firmware also sets the voltages: nothing broke.
Boot with `a51gpu_clock=3` (needs a51gpu=8; the plugins come from fw/ of root.apfs). Not yet: a profile at these
speeds, any temperature watch (the tmu plugin is attached but nothing here reads it), and the top levels (1742 / 2314 /
1053).
Profile at the raised clocks (a51gpu_clock=3, same quiet GL rc.jobs.cputop build): frames averaged 7.75 ms against 9.4
ms at the bootloader's clocks (about 18% faster), waiting for the GPU 2.6 ms against 3.5, idle CPU about 11% against 15%,
and the widgets' start-up storm was over by about 130 s against about 145 s. Stable for the whole 5 minutes: 14704 GPU
jobs, 0 failed, no resets, no panic.

### Temperature watch (step 1: reading, built, not yet run)

TMU at 0x10070000 (dt BIG/LITTLE/G3D/ISP, one block): sensor 0 GPU (G3D sensors=1), 1 A53s (LITTLE and ISP, 2),
2 A73s (BIG, 4). Codes in 0x40 (sensors 0, 1) and 0x44 (sensor 2), 9 bits each; TRIMINFO(n) at n*4 (25 °C code bits 0-8,
85 °C code bits 9-17), two-point if TRIMINFO(0) bit 23; efuse fallback 0x37 (Samsung exynos_tmu.c). The status line now
says "a51gpu: temperatures: GPU, A53s, A73s" with the raw values. Read only; throttling comes once the readings look right.
Readings on the phone (5 min at 1638/2080/572 MHz, scripted swipes): 49-53 °C with the home screen just up, peaks of
GPU 61, A53s 60 and A73s 77 °C during the widgets' start-up, then 50-57 °C. Decoding checked by hand (sensor 0: code
0x12d, e25 0x114, e85 0x150, two-point: 50 °C). Samsung's limits (dt thermal-zones): big cores passive from 83 °C, active
95/100/105/110, hot 115; little cores from 81.
Step 2 (built, not yet run): after step 3, the clock thread watches once a second. Above 83 °C a domain drops one level
a second; below 75 it climbs back one level every 5 s up to what was asked; above 100 it falls straight to the
bootloader's speed. Never below that speed. A watch line repeats every 30 s ("watching: now ... hottest ... steps").
Temperature watch on the phone, at the top levels (a51cpu0_khz=1742000 a51cpu1_khz=2314000 a51gpu_mhz=1053, 5 min with
scripted swipes): the A73s crossed 83 °C under load (hottest 88 °C). The watch stepped them down as far as 1976 MHz and,
once below 75 °C, back up one level every 5 s to 2314 MHz: 8 steps down, 8 up. The A53s (hottest 63 °C) and the GPU
(64 °C) never needed it. Frames averaged 7.18 ms (7.75 at 1638/2080/572, 9.4 at the bootloader's speeds); 14074 GPU
jobs, 0 failed, no panic. So the top levels are safe to ask for with the watch running.

## WiFi: Samsung's integrated WLBT (S620 / "Maxwell"), research (2026-11-15)

- Hardware: dt scsc_wifibt@119c0000 (0x180 bytes; interrupts MBOX SPI 42, ALIVE 28, WDOG 72, CFG_REQ 73), with syscons
  baaw_p_wlbt 0x12050000, dbus_baaw 0x14c20000, pbus_baaw 0x14c30000, wlbt_remap 0x14c50000, boot_cfg 0x14c60000, and
  the PMU (WLBT_CTRL_NS / WLBT_CTRL_S / CENTRAL_SEQ_WLBT_*). Shared memory: dt reserved-memory wifibt_if, 16 MB, placed
  by Linux at boot (no fixed address), so HakiOS must find 16 MB of contiguous physical memory itself.
- Firmware from the phone's Android (fw-wifi-a51/, not in git): /vendor/etc/wifi/mx140.bin (1835952 bytes, "smxf"
  header, build S620_E9610_R3-1_Y23W37A), plus mx140_wlan.hcf / mx140_wlan_sw.hcf (config) and mx140_bt.hcf.
- Samsung's driver (ref/scsc, from the Exynos9611Development tree): drivers/misc/samsung/scsc (mxman.c,
  platform_mif_9610.c, mif_reg_S5E9610.h) and drivers/net/wireless/scsc (slsi: hip4.c, mlme.c, ...).
- Start-up as mxman_start + platform_mif_9610 do it:
  1. map the 16 MB; copy the firmware to its start; check the header and CRCs (fwhdr.c, fwimage.c)
  2. lay out the memory managers (miframman), mailboxes and interrupt bits; build an mxconf block (magic, version,
     the management transport's queues) in the shared memory
  3. mailboxes: 0 = firmware entry point, 1 = mxconf (as the core sees it), 2 = 0xbcdeedcb, 3 = start flags
  4. PMU: power on (WLBT_CTRL_NS MASK_PWR_REQ), release the reset, set WLBT_CTRL_S WLBT_START
  5. the core raises CFG_REQ; the AP then makes WLBT's TZPC non-secure (SMC_CMD_CONN_IF), programs WLBT_REMAP and
     the dbus/pbus BAAW windows (dbus window 0 → the 16 MB), sets boot_cfg (boot flag, keep-alive patch), writes
     BOOT_CFG_ACK and hands MASK_PWR_REQ back to the firmware
  6. wait for the firmware's MM_START_IND on the management transport; then services (WLAN) start over it
- Driver started (2026-11-15): devices/a51/drivers/wifi/hakios.a51wifi.c, CONFIG_HAKIOS_A51WIFI=y, boot-arg a51wifi=N.
  Step 1, read only: the PMU's WLBT registers and the AP-WLBT mailbox, then (in a thread, once the small image is
  there) mx140.bin's header (smxf magic, versions, lengths, entry point, build) with its header and constant-part
  CRC32s (ether_crc, as fwimage.c), and 16 MB of contiguous memory for the core (kmem_alloc_contig). The result
  repeats every 30 s. userland/build.sh copies ../fw-wifi-a51 into fw/. Built, not yet run.
- Step 1 on the phone (2026-11-15): mx140.bin is smxf, header v-, entry point 0x1a9 (Thumb, the core's R4), runtime
  3520552 bytes; header CRC right. Its constant part is empty (length = header length, 420; CRC 0xffffffff), and the CRC
  of everything after the header matches (0x6a1b47ee): the check now allows an empty constant part and checks that one
  too. The 16 MB were taken at 0x88b104000 (above 4 GB; the bus window registers take the address >> 12). PMU
  WLBT_CTRL_NS 0x3804a0, WLBT_CTRL_S 0, CENTRAL_SEQ_WLBT_STATUS 0: the core has not been started.
- Step 2 written (built, not yet run): the firmware copied into the 16 MB (cache cleaned), mailboxes 0 = entry point and
  2 = 0xbcdeedcb (1 and 3 left 0 until step 3 builds an mxconf), PMU power request / reset release / start, then the
  CFG_REQ interrupt (SPI 73 → ID 105, level) answered as platform_cfg_req_isr does: TZPC SMC 0x82000710, WLBT_REMAP,
  CHIP_VERSION_ID EMA bits, dbus window 0x80000000-0x81400000 → the 16 MB, the six pbus windows, boot_cfg flag, the
  keep-alive patch (Samsung's ka_patch[], 58 words, extracted from the local Samsung source into
  fw-wifi-a51/wlbt_ka_patch.bin, so it stays out of the repository) and BOOT_CFG_ACK; then 2 s of watching.
- Step 2 on the phone (2026-11-15): THE CORE STARTS. CFG_REQ came within the second and was answered (TZPC SMC answered
  0, success). Two seconds later mailbox 0 read 0x22220004 instead of the entry point written (0x1a9): the firmware ran
  and wrote it, most likely a progress or error code for the missing configuration (mailbox 1 was 0). PMU
  CENTRAL_SEQ_WLBT_STATUS reads 0. iOS ran normally. Next, step 3: an mxconf with the management transport (mxconf.h,
  mxmgmt_transport, cpacket_buffer, mifintrbit) in mailbox 1, the firmware's panic record read
  (fwhdr r4_panic_record_offset), and waiting for MM_START_IND.
- Step 3 written (built, not yet run): an mxconf (magic 0x79828486, v0.5, 240 bytes) and the management transport's
  two rings (4 × 128-byte packets each, plus read/write indices) laid out just after the firmware's runtime part
  (rounded up to 4 KB); mailbox 1 = the block's offset. Interrupt bits as Samsung's first-transport allocation: to-AP
  stream write bit 0 / read bit 1, from-AP stream read bit 1 / write bit 2 (bit 0 of the from-host side is the R4's
  panic bit). soc_revision from the chip id's low byte. After the CFG_REQ answer, the to-AP ring is watched up to 3 s
  for MM_START_IND (channel 0, message 0, arg = WLBT hardware revision); failing that, the firmware's panic record.
- Step 3, first run (2026-11-15): no MM_START_IND (to-AP ring indices stayed 0/0), but mailbox 0 went to 0x22220006,
  against 0x22220004 with no mxconf: the firmware read the block and got two stages further. Its panic record (at
  0x24398c) is all zeros: no crash, it waits. Added the transports Samsung also makes, with the bits its allocation gives
  them (A51 defconfig: SCSC_MX450_GDB_SUPPORT off): gdb R4 (2 KB × 4-byte packets each way; to-AP write 2 / read 3,
  from-AP read 3 / write 0 = panic bit), gdb M4 (to-AP write 4 / read 1 from the M4 set, from-AP read 5 / write 0),
  mxlog (16 KB × 4 bytes, to-AP write 6 / read 4). Layout grows to 64 KB. Built, not yet run.
- Step 3 with all transports (2026-11-15): **MM_START_IND received.** The firmware wrote one packet into the to-AP
  management ring (indices 0 → 1): channel 0 (Maxwell management), message 0 (MM_START_IND), arg 0 (the WLBT hardware
  revision Samsung's driver keeps as rf_hw_ver). So the WLBT core is fully started under HakiOS: firmware, mxconf, all
  seven streams, the CFG_REQ handshake. The missing gdb and log transports were what held it at stage 0x22220006.
  Next: the WLAN service. It's started over the service-management channel (srvman / servman_messages.h, channel 1),
  then Samsung's slsi driver speaks HIP4 with the firmware over shared memory: MIBs (the .hcf config), MLME requests
  (scan, connect) and data frames.

### WLAN service, research (2026-11-15)

- A service starts with one message on management channel 1 (service management, servman_messages.h): service id
  (SCSC_SERVICE_ID_WLAN = 1), SM_MSG_START_REQ (0), and a 32-bit offset in the 16 MB pointing at the service's own
  configuration; the firmware answers SM_MSG_START_CFM (1) on the same channel. Sending means writing a 128-byte packet
  into the from-AP management ring, moving its write index and raising from-host bit 2 (INTGR1).
- WLAN's configuration is the HIP4 region (drivers/net/wireless/scsc/hip4.h, copies in ref/slsi): about 2.1 MB in the
  shared memory. At its start, an 8 KB config area (struct hip4_hip_config_version_4/5: magic 0xcaba0401, version,
  length; firmware-filled API versions and build; host-filled buffer locations, the MIB location and size, interrupt
  bits, a scoreboard of queue indices and six 256-entry queues: control, data and receive-buffer, each direction). Then
  32 KB for the MIB (the WiFi configuration: the .hcf files), 1 MB of transmit data blocks and 64 KB of transmit
  control blocks (mbulk pools), and 512 KB or 1 MB for receive.
- Plan, step 4: lay out the HIP4 region after the mxconf layout (it fits in the first 8 MB beside the 3.5 MB of
  firmware), fill the host side of the config, put mx140_wlan.hcf (+ _sw.hcf) into the MIB area, send
  SM_MSG_START_REQ for WLAN, and wait for SM_MSG_START_CFM (and the firmware's fields filled in the config). After
  that, step 5: Samsung's HIP4 signalling (mbulk + queues) to send the first MLME request (a scan).
- Step 4 written (built, not yet run): the HIP4 region (0x21a000 bytes on the A51: no PCIe so 1 MB receive, no RX NAPI
  so one to-host bit for all queues, SMAPPER built in so config v4 is 227 bytes) after step 3's layout, 4 KB aligned.
  Control area as struct hip4_hip_control: init 0 (magic 0xcaaa0400; refs to config v4 at 160 and v5 at 32), scoreboard
  448, queues 1472 + n·1088. MIB = mx140_wlan.hcf then mx140_wlan_sw.hcf, unchanged (Samsung only hashes the header).
  Interrupt bits: to-host 7, from-host 5 (the next free after step 3's streams). Then SM_MSG_START_REQ (channel 1,
  service 1, message 0, the region's offset) into the from-AP ring with from-host bit 2, and SM_MSG_START_CFM awaited;
  the firmware's chosen config version and its fields (SAP versions, build) are read back.
- Step 4 on the phone (2026-11-15): the firmware took SM_MSG_START_REQ (from-AP ring read index 0 → 1) and accepted the
  HIP4 config (it wrote 4 into init.conf_hip4_ver: version 4). Mailbox 0 went to 0x11110011. But no SM_MSG_START_CFM in
  3 s, which is also Samsung's timeout. Added diagnostics for the next run: wait 10 s, then the panic record, all four
  to-AP slots, config v4's firmware-filled fields, and the log stream's indices and first words.
- Step 4 diagnostics (2026-11-15): the firmware **panicked** while starting WLAN. Its R4 panic record (format version 2,
  212 bytes) starts 2 d4 a8ff4 25002ae5 d 0 2090a2 11187. Mailbox INTSR0 0x420000: to-host bit 1 (it read our request)
  and bit 6 (the log stream: 152 entries written). Config v4: SAP MLME 0x0e07 filled, build 0. Samsung's driver only
  dumps a panic record (and checks its XOR checksum, seed 0xa5a5a5a5); it doesn't decode the fields. Next run dumps the
  whole record and the log ring as hex, repeated with the status. To decode the log: each entry is
  sync|level|len, timestamp, an offset into /vendor/etc/wifi/mx140/debug/log_strings.bin (the firmware's format
  strings; pull it from Android), then arguments (mxlog.c).
- The firmware's log, decoded (2026-11-15; log-strings.bin from /vendor/etc/wifi/mx140/debug/common, decoder in the
  session scratchpad; decoded copy logs/wifi-log-decoded.txt): APM IPC set up, chip version 0xa4800000, "WLBT
  Initialized early power via APM", LDOs; on our request "Received SM_MSG_START_REQ for service: 1", "Starting service:
  1", "WLBT RFIC LDOs setup via APM", then the panic, with no message of its own.
- Android's own WLAN start, for comparison (WiFi toggled off and on, dmesg): the same two .hcf files and the same HIP4
  sizes, but it loads only 4266 + 204 bytes of MIB, and SM_MSG_START_CFM comes 180 ms after the start, with SAP versions
  MLME 14.7, MA 14.1, debug 13.3, test 14.0. The difference: Samsung's slsi_mib_open_file skips each file's 8-byte
  header, and slsi_mib_get_platform then skips the leading platform keys (0xfffe version, 0xffff platform-name string,
  padded to 16 bits): 60 and 80 bytes. HakiOS loaded the files whole, so the firmware read header and platform text as
  configuration. Fixed (hcf_payload()); built, not yet run.
- Step 4 with the trimmed MIB (2026-11-15): **SM_MSG_START_CFM: the WLAN service runs.** MIB 4470 bytes (as Android),
  config version 4 taken, and the firmware's SAP versions are Android's: MLME 0x0e07 (14.7), MA 0x0e01 (14.1), debug
  0x0d03 (13.3), test 0x0e00 (14.0); headroom 248 and tailroom 26 for MA-UNITDATA. Next, step 5: HIP4 signalling
  (mbulk blocks in the transmit pools, the from-host control queue and the scoreboard, from-host bit 5; replies on the
  to-host queues with bit 7) to send a first MLME request, then a scan.

### HIP4 signalling, research (2026-11-15)

- After SM_MSG_START_CFM, Samsung's hip4_setup takes the config version the firmware chose (4 here), its unitdata head-
  and tailroom, and unmasks the to-host bit. From then on the host handles HIP4 interrupts.
- A request (scsc_wifi_transmit_frame): a transmit block from the control pool (64 KB at HIP+0x10a000; 32 slots of
  2 KB) holding a struct mbulk (next offset, flags, class, pool id, refcount, data / signal buffer sizes, length, head,
  chain offset), then the signal (fapi_signal_header: id, receiver PID, sender PID, firmware reference, then the
  signal's fields), 4 bytes into the signal buffer; any payload in the data buffer after it. Its offset goes into the
  from-host control queue (q[0].array[write]); the write index (one byte at scoreboard + 0) moves on; from-host bit 5
  is raised (INTGR1).
- Scoreboard bytes: the host owns 0 (FH_CTRL write), 1 (FH_DAT write), 2 (FH_RFB read), 3 (TH_CTRL read), 4 (TH_DAT
  read), 5 (TH_RFB write); the firmware owns the same queues' other index at 64..69.
- Still to read for step 5: mbulk_with_signal_alloc's layout, the FAPI signal ids and the MIB get encoding (fapi.h,
  mib.c) for a first MLME-GET.request, and the receive path (to-host control queue entries are offsets of the
  firmware's blocks; returning them goes through the to-host RFB queue).

### WiFi step 5: first MLME request (WORKS: MLME-ADD-VIF.cfm vif 1, result SUCCESS)

`a51wifi=5` sends MLME-ADD-VIF.req (0x2007; vif 1, station, address 02:a5:1c:00:00:01, sender PID 0xc001) after
SM_MSG_START_CFM. The signal goes in control-pool block 0 (HIP + 0x10a000): mbulk descriptor 20 bytes, because
mbulk_class is a packed enum (1 byte): flag 4 = 1, class 5 = 5, pool 6 = 1, data size 8, sig_bufsz 10 =
round_up(20 + 4 + len, 4) - 20, len 12, head 14 = sig_bufsz, chain 16; FAPI signal at +20+4. First try with a 23-byte
layout (class as 4 bytes): the firmware panicked 0x2a01 COMMON_HOSTIO_GENERIC; panic record word 2 is the offset of
its log string in log-strings.bin (mif_q.c:378), word 13 the block's firmware address (0x80000000 + offset).
With 20 bytes the same panic came back, now with the right head and size (words 9, 10). Disassembly of mx140.bin
(llvm-objdump --triple=thumbv7r, file offset = firmware address - 0x80000000; the panic string's offset is a literal
next to the check) showed the check: (flag & 0x24) must be 4, i.e. MBULK_F_OBOUND set and MBULK_F_FREE clear. Samsung's
hip4_skb_to_mbulk() ends with m->flag |= MBULK_F_OBOUND; flag is 0x05. Its offset
goes into FH_CTRL at scoreboard byte 0, then from-host bit 5. For 3 s every TH_CTRL entry (host read byte 3,
firmware write byte 67) is logged and handed back through TH_RFB (byte 5); a received signal also starts 4 bytes
into the block's signal buffer (hip4_mbulk_to_skb). Expected: MLME-ADD-VIF.cfm 0x2107, result 0.
Result on the phone (2026-11-15): seven DebugWord12_indication (0x8301) and then MLME-ADD-VIF.cfm (0x2107), vif 1,
result 0. Every to-host block was handed back through TH_RFB without complaint. Signal names are in
fw-wifi-a51/debug/symbols.dbg (section "signalid").

### WiFi step 6: passive scan (WORKS)

`a51wifi=6`, after MLME-ADD-VIF.cfm: MLME-ADD-SCAN.req (0x2005) as Samsung's slsi_mlme_add_scan (full source in
~/Desktop/kernel_samsung_universal9611/drivers/net/wireless/scsc/mlme.c): header vif 0, scan_id 0x0100 (interface 1 << 8),
scan_type 2 (full), device address, report mode 0x0004 (real time); data: the scan-timing element
(dd 11 00 16 32 01 01, zeros) and the channel list (dd len 00 16 32 01 02, then le16 MHz*2 and a policy byte per
channel, 0x01 = passive since no SSIDs are given). The data goes after the signal buffer plus the firmware's
headroom (config byte 28; tailroom byte 29), dat_bufsz = data + headroom + tailroom, refcount 1.

Answers: MLME-ADD-SCAN.cfm result 0, MLME-SCAN.ind (0x2300; channel_frequency = MHz*2 at signal 12, rssi at 14) and
MLME-SCAN-DONE.ind (0x2301). A scan report is a chain: the head block (flag 0xc5 = SIG | OBOUND | CHAIN_HEAD |
CHAIN) holds only the signal, chain_next (byte 16) points to the block with the beacon; every block of the chain is
handed back on TH_RFB (hip4_mbulk_to_skb). Result on the phone (2026-11-15): "TP-Link_6960" 5240 MHz -53 dBm, secured,
and a hidden BSS of the same router. Only 2-4 reports per scan: passive dwell with the default timing is short; an
active scan (wildcard SSID element, the supplicant's rates/HT elements) would see more.

### WiFi step 7: WPA2-Personal connect (WORKS)

`a51wifi=7` with `fw-wifi-a51/wifi.conf` (git-ignored; `ssid=`, `psk=` or `pmk=<64 hex>`; copied into root/fw by
userland/build.sh; never printed). The handshake's arithmetic is in drivers/wifi/hakios.a51wpa.c (SHA-1, HMAC,
PBKDF2, PRF-384, HMAC-SHA-1-128 MIC, AES key unwrap), built alone on the PC and checked against Python's hashlib/hmac,
RFC 3394's vector and an access point simulated in Python (both MICs, TK, GTK, key id, RSC; a wrong passphrase fails
at message 3). Signals, as Samsung's mlme.c / rx.c / cfg80211_ops.c:

- MLME-CONNECT.req 0x200b: vif 1, BSSID @12, authentication 0 (open) @18, MHz*2 @20; data: SSID element and our RSN
  element (30 14 01 00, 00-0f-ac-04 group, CCMP pairwise, PSK AKM, capabilities 0). Answers CONNECT.cfm 0, then
  MLME-CONNECT.ind 0 (associated).
- EAPOL from the AP arrives as MLME-RECEIVED-FRAME.ind 0x230d (data unit descriptor @12 = 1: the data is the 802.3
  frame), not as MA-UNITDATA.ind. Ours go out as MLME-SEND-FRAME.req 0x2010 (vif 1, host tag @12 in steps of 4, data
  unit descriptor 1 @14, message type @16: 2 for messages 1-3, 3 for message 4) with an 802.3 frame (to the BSSID,
  ethertype 888e). MLME-FRAME-TRANSMISSION.ind 0x230c (host tag @12) says message 4 went out.
- MLME-SETKEYS.req 0x2018: vif 1, length in bits @12, key id @14, type @16 (1 pairwise, 0 group), address @18, RSC @24
  (bytes as received), cipher suite @40 as the u32 0x000fac04 little endian; data: the key. BOTH keys use the AP's
  address ("Sta Group Key will use the peer address"); the group key against ff:ff:ff:ff:ff:ff is refused.
- Then MLME-CONNECT-RES 0x2200 (vif 1).

Result on the phone (2026-11-15): connected to TP-Link_6960, SETKEYS.cfm 0 and 0, 13 data frames (2424 bytes) in
20 s as MA-UNITDATA.ind, decrypted by the firmware (e.g. IPv4 from another station). Next: a network interface in
Darwin (ifnet over MA-UNITDATA.req/ind), DHCP.

### WiFi step 8: Darwin's Wi-Fi (WORKS: iOS online)

`a51wifi=8`. The Darwin side already existed in HakiOS: drivers/net/hakios.net.c makes en0 over any "link"
(sys/hakios.net.h), and hakios.sta.c / hakios.apple80211.c / hakios.wlan.cpp answer wifid over a "station boundary"
(sys/hakios.sta.h). Those three were part of the Qualcomm WIFI driver; they are now their own driver, WIFI_STATION,
required by both WIFI and A51WIFI (drivers/wifi/conf, stubs split in drivers/core/hakios.stubs.c, which also gives
hakios_wifi_tell when WIFI is not built).

hakios.a51wifi.c attaches en0 (link down) as soon as its thread runs; after step 5 it attaches the station side
(power, scan, heard, join, leave, state, channels) and its thread becomes the dispatcher (every 2 ms): to-host queues,
scan reports, joins (link_progress: keys after message 4, CONNECT-RES), data frames to en0 (802.3, and A-MSDU taken
apart), FH_RFB (host blocks handed back: a free map for the 32 control and 512 data blocks). en0's frames go out as
MA-UNITDATA.req 0x1000 (vif 1, host tag, priority 0, peer index 1, descriptor 1) in the data pool on FH_DAT (pool 0,
class 4). fw/wifi.conf is a standing join (hakios_sta_ask_join_standing).

Found on the way:
- The firmware panics with COMMON_HOSTIO_HOST_NOT_RESPONSIVE (mif_th.c:1638) if the host never clears the to-host
  interrupt: it re-raises it every 2 s and gives up after 10. The dispatcher writes INTCR0 bit 16+7 on every pass
  (unconditionally: INTSR0 reads masked). Steps 5-7 were too short to see it.
- The station VIF follows Samsung's life cycle: added before MLME-CONNECT.req, deleted after a failed join, a leave
  or a loss (cfg80211_ops.c slsi_connect, slsi_handle_disconnect).
- The router's 2.4 GHz side answered MLME-CONNECT.ind 0x30 (NOT_ALLOWED), twice; 5 GHz joins. Joins count 5 GHz 15 dB
  better and try the next candidate on a refusal. Why 2.4 GHz is refused is not known yet.
- Scans are passive: an active scan without the probe request's elements was followed by a silent firmware.

Result on the phone (2026-11-15): joined at boot from wifi.conf (5240 MHz), DHCP, the status bar's Wi-Fi icon, in 3
minutes 30338 frames (42.5 MB) received and 14060 (1.2 MB) sent; no firmware panic. 402 frames were dropped for want
of a free data block. wifid's later scans while joined hear only 2-3 networks.

Drops fixed: a from-host queue holds 255 entries (byte indices) but the data pool has 512 blocks, and hip_put never
checked for a full queue (hip4_q_add_signal returns -ENOSPC): in a burst the write index could lap the firmware's read
index, losing frames and leaking their blocks. hip_put now refuses a full queue (the block is freed), and en0's
transmit waits up to 10 ms (reclaiming) instead of dropping. Next run: 0 dropped of 10071 sent, 47 MB received, data
blocks busy at most 321 and back to 0.

Joining a network from Settings works too (2026-11-16): wifid asks the station side (hakios.apple80211.c) with the
passphrase as typed.

## Display: frames 38 ms -> 9.5 ms (2026-11-16)

Frames came at 13-20 a second, every one "a refresh late": MiniIOMobileFramebuffer scales the window server's 1125x2436
surface onto the 1080x2400 panel with the CPU, in the boot loader's framebuffer, and that took 38 ms. Found by
measuring, step by step (boot-arg fb_bench=1 and the frame report's new line):
- one layer a frame, the two surfaces of the window server (kernel memory, not a program's own);
- the three shares took the same 37 ms on A53s and A73s alike, no row over 1 ms (not descheduled);
- memory alone is fast: the surface read 1.3-4.4 GB/s, the framebuffer written 0.9-2.8 GB/s by one thread; drawRows
  alone, one thread, the whole panel in 6 ms;
- not the memory clock (MIF at 2093 MHz changed nothing; now opt-in, a51mif_khz=) and not the GPU driver's cache
  maintenance (mali_flush=0, a test switch, changed nothing);
- probes in the live frames: a source row read in 2.3 us, a panel row written in 44 us. Three processors writing the
  write-combined framebuffer at once got ~100 MB/s between them; one writes a row in 3.4 us.
configs/a51.config sets CONFIG_FB_THREADS=1 (a51-ios.sh now passes the .config's boot-arguments, as make run does).
Result: 9.5 ms a frame, 27-31 frames a second while swiping. What is left: the window server's own swaps come late
(CoreAnimation/GPU), and tearing (the copy is not synchronised with DECON's TE-triggered transfers; command mode,
psr_mode 2, trig_mode 0: HW_SW_TRIG_CONTROL 0x70, INTERRUPT_PENDING 0x4c FRAME_DONE bit 13, FRAME_COUNT 0x2a0).

## Display: no tearing (2026-11-16)

DECON (0x148b0000) in command mode with the hardware trigger on and unmasked (HW_SW_TRIG_CONTROL 0x1001) sends the
framebuffer on every TE, 60 a second (FRAME_COUNT and HW_TE_CNT both +60 in a second); the boot loader left interrupts
0x3011 enabled. FRAME_START and FRAME_DONE come together, near the end of a transfer. CONFIG_FB_DECON_TRIG=1: the
trigger is masked while a frame is written and armed when it is whole; the next frame first waits for a transfer to be
done. Seen on the phone: the tearing is gone. Still about 30 frames a second while swiping and laggy to the touch: most
swaps come late from the window server itself (next to look at), not the display's 9.5 ms.

## Display: two framebuffers, 48-62 frames a second (2026-11-16)

The one-buffer handshake with DECON (CONFIG_FB_DECON_TRIG) kept frames from tearing most of the time but made each frame
wait out a whole transfer, which locked gestures at 30 frames a second, and 7-14% of frames still saw a transfer while
written. Now two framebuffers (CONFIG_FB_DECON_DOUBLE, CONFIG_FB_DECON_SECOND=0xcaa00000):
- DECON reads through DPP channel dpp2, IDMA at 0x14881000 (IDMA_IN_BASE_ADDR_Y 0x40 = 0xca000000), taken at a frame's
  start after SHADOW_REG_UPDATE_REQ bit 31 (0x148b0060), which DECON clears when it has.
- The loader hands XNU only the largest free block (bank 3, above 4 GB): XNU has no memory below 4 GB, and the register
  is 32 bits. So the second buffer is reserved by UEFI: uefi/files/.../MemoryMapLib.c reserves 20 MB at 0xCA000000
  ("Display Reserved"; the RAM after it from 0xCB400000). Recovery image out/hakios-a51-fb2-RECOVERY.tar.md5 (the
  previous one, 10 MB reserved, is out/hakios-a51-kernel-RECOVERY.tar.md5: with it, fb_decon_second must not be set).
- Mapped through IOKit (IOMemoryDescriptor::withPhysicalAddress, write-combined): ml_io_map_wcomb panicked
  ("kmem_alloc ... failed with 3": the I/O map has no room for another 10 MB).
Result: 1270 frames handed over, none late; gestures 48-62 frames a second, 44% of frame steps over 16.7 ms (100%
before). What is left is the window server's own drawing, about 16 ms a frame (Mesa and CoreAnimation on the CPU).

