#!/usr/bin/env bash
# ==============================================================================
# HakiOS Galaxy A51 (SM-A515F) - Automated Build & Setup Pipeline
# Compila Kernel XNU, Mu-Silicium UEFI e Userland iOS 16.5
# ==============================================================================

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}================================================================${NC}"
echo -e "${BLUE}   HakiOS Builder - Samsung Galaxy A51 (iOS 16.5 / Exynos 9611) ${NC}"
echo -e "${BLUE}================================================================${NC}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${SCRIPT_DIR}/build_workspace"
HAKIOS_REPO="https://github.com/oosexgang/hakios-xnu-kernel.git"
IPSW_URL="https://updates.cdn-apple.com/2023SpringFCS/fullrestores/032-85130/6AB0D10A-8AF9-4F6C-8CF5-A1B315CABE88/iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw"
IPSW_NAME="iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw"

# 1. Checagem de Espaço em Disco (mínimo 30 GB)
AVAILABLE_KB=$(df -k "$SCRIPT_DIR" | awk 'NR==2 {print $4}')
AVAILABLE_GB=$(( AVAILABLE_KB / 1024 / 1024 ))
echo -e "${BLUE}[*] Espaço livre detectado:${NC} ${AVAILABLE_GB} GB"
if [ "$AVAILABLE_GB" -lt 30 ]; then
    echo -e "${YELLOW}[!] AVISO: Recomenda-se pelo menos 30-35 GB livres para o IPSW + XNU + APFS.${NC}"
fi

# 2. Instalação de dependências (se tiver sudo/apt)
if command -v apt-get &>/dev/null; then
    echo -e "${BLUE}[*] Instalando dependências do sistema...${NC}"
    sudo apt-get update -qq || true
    sudo apt-get install -y --no-install-recommends \
        build-essential clang llvm lld bison flex \
        gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
        device-tree-compiler libssl-dev libelf-dev bc cpio \
        kmod curl wget unzip xz-utils git python3 python3-pip \
        python3-setuptools python3-wheel 7zip lz4 util-linux || true
    pip3 install --break-system-packages edk2-pytool-extensions edk2-pytool-library lzfse || true
fi

# 3. Setup do Theos e SDK do iOS 16.5 (se ainda não existir)
export THEOS="${THEOS:-$HOME/theos}"
if [ ! -d "$THEOS/sdks/iPhoneOS16.5.sdk" ] || [ ! -x "$THEOS/toolchain/linux/iphone/bin/ld" ]; then
    echo -e "${BLUE}[*] Configurando Theos e SDK iPhoneOS 16.5...${NC}"
    mkdir -p "$THEOS/toolchain/linux/iphone"
    curl -LO https://github.com/sbingner/llvm-project/releases/latest/download/linux-ios-arm64e-clang-toolchain.tar.lzma
    tar -xf linux-ios-arm64e-clang-toolchain.tar.lzma -C "$THEOS/toolchain/linux/iphone/"
    rm -f linux-ios-arm64e-clang-toolchain.tar.lzma

    git clone --depth 1 https://github.com/theos/sdks.git /tmp/theos-sdks
    mkdir -p "$THEOS/sdks"
    cp -r /tmp/theos-sdks/iPhoneOS16.5.sdk "$THEOS/sdks/"
    rm -rf /tmp/theos-sdks
fi

# 4. Preparação do diretório de trabalho
mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

if [ ! -d "hakios-xnu-kernel" ]; then
    echo -e "${BLUE}[*] Clonando hakios-xnu-kernel...${NC}"
    git clone --depth 1 "$HAKIOS_REPO" hakios-xnu-kernel
else
    echo -e "${GREEN}[✓] Repositório hakios-xnu-kernel já presente.${NC}"
fi

cd hakios-xnu-kernel

# 5. Injetar arquivos do dispositivo (hakios-device-a51)
echo -e "${BLUE}[*] Copiando camada de dispositivo A51 para devices/a51...${NC}"
mkdir -p devices/a51
cp -r "${SCRIPT_DIR}/configs" devices/a51/
cp -r "${SCRIPT_DIR}/drivers" devices/a51/
cp -r "${SCRIPT_DIR}/soc" devices/a51/
cp -r "${SCRIPT_DIR}/uefi" devices/a51/
cp "${SCRIPT_DIR}/device.conf" devices/a51/
cp "${SCRIPT_DIR}/metal.args" devices/a51/
cp "${SCRIPT_DIR}/PORTING.md" devices/a51/
cp "${SCRIPT_DIR}/README.md" devices/a51/

# 6. Download do IPSW do iOS 16.5
if [ ! -f "$IPSW_NAME" ]; then
    echo -e "${BLUE}[*] Baixando IPSW oficial do iOS 16.5 (Build 20F66)...${NC}"
    curl -C - -L -o "$IPSW_NAME" "$IPSW_URL"
else
    echo -e "${GREEN}[✓] IPSW $IPSW_NAME já baixado.${NC}"
fi

# 7. Descompactar arquivos de sistema (Userland)
echo -e "${BLUE}[*] Descompactando imagens APFS do IPSW...${NC}"
umask 022
make unpack IPSW="$IPSW_NAME"

# 8. Compilar o UEFI
echo -e "${BLUE}[*] Compilando Mu-Silicium UEFI para Exynos 9611 / A51...${NC}"
if [ -f uefi/apply.sh ]; then
    bash uefi/apply.sh a51
fi
python3 build_uefi.py -d a51 || echo -e "${YELLOW}[!] Continuando caso UEFI já tenha sido gerado...${NC}"

# 9. Configurar e Compilar Kernel + Loader
echo -e "${BLUE}[*] Configurando e compilando kernel XNU para A51...${NC}"
make a51_defconfig
make loader
make kernel

# 10. Gerar imagem Userland (OpenGL ES Panfrost + Flashboot)
echo -e "${BLUE}[*] Gerando imagens de inicialização (flashboot.apfs)...${NC}"
umask 022
QUIET=1 GL=1 CRYPTEXES=flash FLASHBOOT=1 REST=1 userland/build.sh

# 11. Empacotar imagem final para Odin (RECOVERY .tar.md5)
echo -e "${BLUE}[*] Empacotando imagem de recovery Odin...${NC}"
mkdir -p out
if [ -f mkxnuimg.py ]; then
    python3 mkxnuimg.py --device a51 -o out/recovery.img || python3 mkxnuimg.py -o out/recovery.img
fi

if [ -f out/recovery.img ]; then
    cd out
    tar -H ustar -c recovery.img > hakios-a51-fb2-RECOVERY.tar
    md5sum -t hakios-a51-fb2-RECOVERY.tar >> hakios-a51-fb2-RECOVERY.tar
    mv hakios-a51-fb2-RECOVERY.tar hakios-a51-fb2-RECOVERY.tar.md5
    echo -e "${GREEN}================================================================${NC}"
    echo -e "${GREEN}[✓] BUILD CONCLUÍDO COM SUCESSO!${NC}"
    echo -e "${GREEN}Arquivo gerado para Odin: ${WORK_DIR}/hakios-xnu-kernel/out/hakios-a51-fb2-RECOVERY.tar.md5${NC}"
    echo -e "${GREEN}================================================================${NC}"
else
    echo -e "${YELLOW}[!] recovery.img não foi gerado automaticamente. Verifique os logs acima.${NC}"
fi
