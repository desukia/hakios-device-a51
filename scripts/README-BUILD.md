# Guia de Compilação Automatizada - HakiOS para Galaxy A51 (SM-A515F)

Este diretório contém dois métodos automatizados para compilar todo o projeto HakiOS (Kernel XNU, Mu-Silicium UEFI, Userland do iOS 16.5 e a imagem `.tar.md5` para o Odin).

---

## Método 1: GitHub Actions (Recomendado - 100% na Nuvem)

Este método **não consome espaço no seu computador** e roda nos servidores da nuvem do GitHub com link de gigabit e Linux Ubuntu.

### Como usar:
1. Suba as alterações para o seu repositório no GitHub:
   ```bash
   git add .
   git commit -m "fix: integrate A51 build patches and uefi"
   git push
   ```
2. No seu repositório no GitHub, clique na aba **Actions**.
3. No menu à esquerda, selecione o workflow **"Build HakiOS iOS 16.5 for Samsung Galaxy A51 (SM-A515F)"**.
4. Clique no botão **"Run workflow"** e confirme em **"Run workflow"**.
5. O GitHub executará automaticamente:
   - Limpeza do disco para liberar mais de 45 GB.
   - Instalação de compiladores Clang, LLVM, GCC ARM64, Theos com cctools e SDK iPhoneOS 16.5.
   - Clone do repositório base `hakios-xnu-kernel`.
   - Download do IPSW oficial do iOS 16.5 (`iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw`) direto dos servidores da Apple.
   - Descompactação das imagens APFS.
   - Clone e compilação do Mu-Silicium UEFI para o Exynos 9611 (A51).
   - Aplicação dos patches de Wi-Fi (`WIFI_STATION`) e stubs do kernel.
   - Configuração de `a51_defconfig`, compilação do loader e do Kernel XNU.
   - Geração da userland flashboot APFS e empacotamento em `hakios-a51-fb2-RECOVERY.tar.md5`.
6. Ao finalizar, baixe o arquivo compilado na seção **Artifacts** na parte inferior da página da execução.

---

## Método 2: Script Local (Linux ou WSL2)

Caso prefira rodar localmente, certifique-se de:
1. Ter uma distribuição Linux (Ubuntu 22.04 ou 24.04).
2. Ter **pelo menos 35 GB de espaço livre**.

### Como executar:
```bash
chmod +x scripts/build-a51-all.sh
./scripts/build-a51-all.sh
```

---

## Como Iniciar o iOS no A51 após a Compilação

1. Coloque o Galaxy A51 em **Download Mode**:
   - Desligue o telefone.
   - Segure `Volume UP + Volume DOWN` e conecte o cabo USB no PC.
   - Pressione `Volume UP` na tela azul de aviso.
2. No PC, abra o **Odin3**:
   - Coloque o arquivo `hakios-a51-fb2-RECOVERY.tar.md5` no slot **AP**.
   - Clique em **Start**.
3. Inicialização:
   - Reinicie o aparelho segurando `Power + Volume UP` com o cabo USB conectado para acionar o recovery.
   - O UEFI iniciará e carregará o iOS com os argumentos otimizados:
     `root_apfs=ufs_darwin ufs=2 bootcpu=4 cpus=7 touchscreen=1 a51gpu=8 a51gpu_keep=1 a51gpu_clock=3`
