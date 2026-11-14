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
