# Samsung Galaxy A51 (SM-A515F) (a51)

A HakiOS device repository: what is true of this device and of no other. The kernel, the loader, the drivers and the tools are
the common repository's; the exynos9611 family's facts are in `soc/`, carried with the device. How the three are put together:
[docs/porting/repositories.md](https://github.com/oosexgang/hakios-xnu-kernel/blob/main/docs/porting/repositories.md).

| File | What |
|---|---|
| `metal.args` | what `tools/metal-run.sh` gives this phone: its cores, its panel's rate, the threads that copy frames, the touch poll |
| `device.conf` | what the device is, and how far each part of it works: its page in the documentation is made from it |
| `configs/a51_defconfig` | the common kernel, the SoC's fragment, this device's: `make a51_defconfig` |
| `configs/a51.config` | what is this device's own: the drivers built in, the arguments it needs |
| `soc/` | its SoC family's layer (exynos9611): `soc.conf`, `configs/exynos9611.config`, the SoC's drivers; carried here so that this one repository is all the device needs |
| `drivers/` | drivers of its own, if it needs any (`drivers/<subsystem>/conf`, as in the common repository) |

Every phone driver of the common repository is left out to begin with: their addresses and pins are the phone they were written
for. Put one back in `configs/a51.config` when it has been made to work here, and say so in `device.conf`.

Check with `tools/hakios-device.py check a51`.

## Running iOS on it

The phone boots iOS 16.5 from its darwin partition, from HakiOS's loader (PORTING.md has how that partition was made).
The small image for that, with OpenGL ES on the Mali and the console quiet:

    QUIET=1 GL=1 CRYPTEXES=flash FLASHBOOT=1 REST=1 userland/build.sh

and the boot-arguments for the GPU, the clocks and the touch screen: `touchscreen=1 a51gpu=8 a51gpu_keep=1 a51gpu_clock=3`
(a51gpu_clock=3 asks ACPM for the top CPU and GPU speeds and watches the temperature; it needs Samsung's ACPM plugins in
../fw-acpm when the image is built, see PORTING.md). Leave `-v` out, and keep
`QUIET=1`: launchd's output and the kernel's text console, drawn into the framebuffer by the CPU line by line, kept about
a processor busy with the system idle (measured: about 116% of the CPU idle with them, about 15% without).
