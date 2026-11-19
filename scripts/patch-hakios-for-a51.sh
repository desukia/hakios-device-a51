#!/usr/bin/env bash
# ==============================================================================
# Patch upstream hakios-xnu-kernel with Samsung Galaxy A51 requirements:
# 1. Split WIFI_STATION driver in drivers/wifi/conf
# 2. Update hakios.stubs.c to separate WIFI and WIFI_STATION stubs
# 3. Register FB_DECON symbols in configs/symbols.tsv and configs/boot-args.lst
# 4. Add --device support to mkxnuimg.py for Exynos 9611 (A51)
# ==============================================================================

set -euo pipefail

TARGET_DIR="${1:-.}"
cd "$TARGET_DIR"

echo "[*] Aplicando patch de WIFI_STATION em drivers/wifi/conf..."
cat << 'EOF' > drivers/wifi/conf
# drivers/wifi/conf: Wi-Fi.
# CONFIG_HAKIOS_<NAME>=y in a defconfig builds a driver into the kernel; "standard" files are always built.
# tools/hakios-config.py reads this file; docs/drivers-config.md says what each word means and how it reaches XNU.

driver WIFI_STATION
	file    bsd/dev/hakios.sta.c
	file    bsd/dev/hakios.apple80211.c
	file    iokit/Kernel/hakios.wlan.cpp iokitcpp
	help    Darwin station boundary and Apple80211 surface

driver WIFI
	file    bsd/dev/hakios.wifi.c
	file    bsd/dev/hakios.wlfw.c
	file    bsd/dev/hakios.lx.c
	requires QMI WIFI_STATION
	help    Wi-Fi of the WCN6750 (wifi=1, wifi_step=N): the DSP that runs the radio's firmware, Linux's ath11k
	help    (its object, made by tools/mk-ath11k.sh, is linked in only with this), a station, and the Apple80211
	help    surface Darwin's wifid talks to. Needs fw-wifi/ from the phone
EOF

echo "[*] Aplicando patch em drivers/core/hakios.stubs.c e hakios.stubs.cpp..."
python3 - << 'PY'
c_path = "drivers/core/hakios.stubs.c"
with open(c_path, "r", encoding="utf-8") as f:
    c_content = f.read()

proto_target = "void HakiosWLANStart(const char *interface, const uint8_t *address);"
proto_replacement = """void HakiosWLANStart(const char *interface, const uint8_t *address);
int hakios_wifi_tell(const char *format, ...);"""

if proto_target in c_content and "int hakios_wifi_tell" not in c_content:
    c_content = c_content.replace(proto_target, proto_replacement)

target = """#ifndef HAKIOS_WIFI
void hakios_wifi_init(void) {}
void hakios_wifi_stop(void) {}
void hakios_wifi_quiesce(void) {}
errno_t hakios_apple80211_ioctl(__unused u_long command, __unused void *data) { return EOPNOTSUPP; }
void HakiosWLANStart(__unused const char *interface, __unused const uint8_t *address) {}
#endif"""

replacement = """#ifndef HAKIOS_WIFI
void hakios_wifi_init(void) {}
void hakios_wifi_stop(void) {}
void hakios_wifi_quiesce(void) {}
int
hakios_wifi_tell(__unused const char *format, ...)
{
	return 0;
}
#endif

#ifndef HAKIOS_WIFI_STATION
errno_t hakios_apple80211_ioctl(__unused u_long command, __unused void *data) { return EOPNOTSUPP; }
void HakiosWLANStart(__unused const char *interface, __unused const uint8_t *address) {}
#endif"""

if target in c_content:
    c_content = c_content.replace(target, replacement)
elif "void hakios_wifi_tell" in c_content:
    # Caso já tenha sido substituído na versão antiga com void
    old_void = "void hakios_wifi_tell(__unused const char *what) {}"
    new_int = """int
hakios_wifi_tell(__unused const char *format, ...)
{
	return 0;
}"""
    c_content = c_content.replace(old_void, new_int)

with open(c_path, "w", encoding="utf-8") as f:
    f.write(c_content)
print("[✓] hakios.stubs.c atualizado com sucesso!")

cpp_path = "drivers/core/hakios.stubs.cpp"
with open(cpp_path, "r", encoding="utf-8") as f:
    cpp_content = f.read()

cpp_target = "#ifndef HAKIOS_ADRENO"
cpp_replacement = "#if !defined(HAKIOS_ADRENO) && !defined(HAKIOS_A51GPU)"
if cpp_target in cpp_content:
    cpp_content = cpp_content.replace(cpp_target, cpp_replacement)
    with open(cpp_path, "w", encoding="utf-8") as f:
        f.write(cpp_content)
    print("[✓] hakios.stubs.cpp atualizado com sucesso!")
else:
    print("[!] Trecho em hakios.stubs.cpp já modificado ou ausente.")
PY

echo "[*] Registrando símbolos FB_DECON em configs/symbols.tsv e configs/boot-args.lst..."
python3 - << 'PY'
tsv_path = "configs/symbols.tsv"
lst_path = "configs/boot-args.lst"

decon_tsv = [
    "FB_DECON_TRIG\targ\tfb_decon_trig\tGPU and display\tk\tMiniIOMobileFramebuffer.cpp\thardware DECON trigger",
    "FB_DECON_DOUBLE\targ\tfb_decon_double\tGPU and display\tk\tMiniIOMobileFramebuffer.cpp\ttwo framebuffers",
    "FB_DECON_SECOND\thex\tfb_decon_second\tGPU and display\tk\tMiniIOMobileFramebuffer.cpp\tsecond framebuffer address",
    "FB_DECON_THIRD\thex\tfb_decon_third\tGPU and display\tk\tMiniIOMobileFramebuffer.cpp\tthird framebuffer address",
]

decon_args = ["fb_decon_trig", "fb_decon_double", "fb_decon_second", "fb_decon_third"]

with open(tsv_path, "r", encoding="utf-8") as f:
    tsv_content = f.read()

missing_tsv = [line for line in decon_tsv if line.split("\t")[0] not in tsv_content]
if missing_tsv:
    with open(tsv_path, "a", encoding="utf-8") as f:
        f.write("\n" + "\n".join(missing_tsv) + "\n")
    print(f"[✓] Adicionados {len(missing_tsv)} símbolos em {tsv_path}")

with open(lst_path, "r", encoding="utf-8") as f:
    lst_content = f.read().splitlines()

missing_args = [arg for arg in decon_args if arg not in lst_content]
if missing_args:
    with open(lst_path, "a", encoding="utf-8") as f:
        f.write("\n" + "\n".join(missing_args) + "\n")
    print(f"[✓] Adicionados {len(missing_args)} boot-args em {lst_path}")
PY

echo "[*] Atualizando mkxnuimg.py para suportar --device a51..."
cat << 'EOF' > mkxnuimg.py
#!/usr/bin/env python3
"""
Pack UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for fastboot or Odin recovery flash.
"""
import argparse, gzip, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output', default=None)
ap.add_argument('--mode', default='RELEASE')
ap.add_argument('--device', default='a51')
args = ap.parse_args()

if args.device == 'a51':
    FD_BASE, FD_SIZE = 0xA0000000, 0x200000
    PAYLOAD_BASE, PAYLOAD_MAX = 0xC1000000, 0x04000000
    pkg = 'a51Pkg'
    dtb_file = 'a51.dtb'
    header_version = '2'
    if args.output is None:
        args.output = os.path.join(HERE, 'out', 'recovery.img')
else:
    FD_BASE, FD_SIZE = 0x9FD00000, 0x200000
    PAYLOAD_BASE, PAYLOAD_MAX = 0xF0000000, 0x04000000
    pkg = 'redwoodPkg'
    dtb_file = 'redwood.dtb'
    header_version = '1'
    if args.output is None:
        args.output = os.path.join(HERE, 'out', 'xnu-redwood.img')

os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)

fd_path = os.path.join(MU, f'Build/{pkg}/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if not os.path.isfile(fd_path):
    alt_fd = os.path.join(MU, f'Build/{pkg}/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd')
    if os.path.isfile(alt_fd):
        fd_path = alt_fd
    else:
        print(f"AVISO: {fd_path} nao encontrado ainda.")

shim_dir = os.path.join(MU, 'BootShim')
for f in ('BootShim.elf', 'BootShim.bin'):
    try: os.unlink(os.path.join(shim_dir, f))
    except (FileNotFoundError, OSError): pass

if os.path.isdir(shim_dir):
    try:
        subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                               f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'],
                              cwd=shim_dir, stdout=subprocess.DEVNULL)
    except Exception as e:
        print(f"make BootShim: {e}")

dtb_path = os.path.join(MU, 'Resources/DTBs', dtb_file)
if not os.path.isfile(dtb_path):
    dtb_path = os.path.join(HERE, 'devices', args.device, 'uefi/files/Resources/DTBs', dtb_file)

ramdisk_path = os.path.join(MU, 'Resources/ramdisk')
if not os.path.isfile(ramdisk_path):
    with open(os.path.join(HERE, 'out', 'empty_ramdisk'), 'wb') as rf:
        rf.write(b'\0' * 1024)
    ramdisk_path = os.path.join(HERE, 'out', 'empty_ramdisk')

mkbootimg = os.path.join(MU, 'Resources/Scripts/mkbootimg.py')
if not os.path.isfile(mkbootimg):
    mkbootimg = 'mkbootimg'

shim_bin = os.path.join(shim_dir, 'BootShim.bin')
shim = open(shim_bin, 'rb').read() if os.path.isfile(shim_bin) else b'\0'*1024
fd = open(fd_path, 'rb').read() if os.path.isfile(fd_path) else b'\0'*FD_SIZE

blob = shim + fd
if args.payload and os.path.isfile(args.payload):
    data = open(args.payload, 'rb').read()
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes')

kernel_img = args.output + '.kernel~'
if header_version == '2':
    open(kernel_img, 'wb').write(gzip.compress(blob, 6))
    cmd = [sys.executable, mkbootimg, '--kernel', kernel_img, '--ramdisk', ramdisk_path,
           '--header_version', '2', '--base', '0x10000000', '--kernel_offset', '0x00008000',
           '--ramdisk_offset', '0x01000000', '--tags_offset', '0x00000100',
           '--dtb', dtb_path, '-o', args.output]
else:
    dtb = open(dtb_path, 'rb').read() if os.path.isfile(dtb_path) else b''
    open(kernel_img, 'wb').write(gzip.compress(blob, 6) + dtb)
    cmd = [sys.executable, mkbootimg, '--kernel', kernel_img, '--ramdisk', ramdisk_path,
           '--header_version', '1', '-o', args.output]

try:
    subprocess.check_call(cmd)
    print(f"[✓] Imagem gerada: {args.output}")
finally:
    if os.path.isfile(kernel_img):
        os.unlink(kernel_img)
EOF
chmod +x mkxnuimg.py
echo "[✓] mkxnuimg.py atualizado com sucesso!"
