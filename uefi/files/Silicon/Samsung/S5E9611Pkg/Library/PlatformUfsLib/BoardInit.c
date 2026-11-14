/**
  UFS board init for the Exynos 9611 (Galaxy A51: one lane, HS-G3 rate B, as the Android kernel's log shows).

  Where the numbers come from:
    - the phone's device tree, ufs@0x13520000: HCI 0x13520000, vendor HCI 0x13521100, UniPro 0x13510000,
      UFSP 0x13530000, ufs-phy 0x13524000; ufs-dma-coherency offset 0x1010 mask/val 0x300 in the FSYS sysreg
      (system-controller@13410000); pins ufs-refclk-out gpf0-0 and ufs-rst-n gpf0-1, function 3, no pull;
      ufs,pmd-attr-lane = 1, ufs,pmd-attr-gear = 3
    - Samsung's kernel (universal9611): ufs-exynos.c (PHY isolation: PMU + 0x724 bit 0, its default when the
      tree has no ufs-phy-iso node, which the A51's has not; the device reset through HCI_GPIO_OUT, which
      UfsDxe does itself), cal-if/exynos9610 (UFS_EMBD = 133.25 MHz: MUX_CLKCMU_FSYS_UFS_EMBD select 1 =
      PLL_SHARED0 / 4 = 399.75 MHz, CLKCMU_FSYS_UFS_EMBD divratio 2 = / 3; TOP CMU 0x12100000 + 0x1048 / 0x1850,
      FSYS CMU 0x13400000 + 0x160 bit 4 for the user mux)

  S-Boot has the UFS running when it starts us (it read this image from it); setting the same clocks again is
  harmless and keeps the init from depending on it.
**/
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UfsHostBridge.h>

#include <Protocol/EFIGpio.h>

#define EXYNOS9611_UFS_BASE                 0x13520000UL
#define EXYNOS9611_UFS_VS_BASE              0x13521100UL
#define EXYNOS9611_UNIPRO_BASE              0x13510000UL
#define EXYNOS9611_PHY_PMA_BASE             0x13524000UL

#define EXYNOS9611_PMU_BASE                 0x11860000UL
#define EXYNOS9611_PMU_UFS_PHY_CONTROL      (EXYNOS9611_PMU_BASE + 0x724)

#define EXYNOS9611_SYSREG_FSYS_BASE         0x13410000UL
#define EXYNOS9611_SYSREG_FSYS_COHERENCY    (EXYNOS9611_SYSREG_FSYS_BASE + 0x1010)
#define EXYNOS9611_UFS_COHERENCY_MASK       0x300U

#define CMU_TOP_BASE                        0x12100000UL
#define MUX_CLKCMU_FSYS_UFS_EMBD            (CMU_TOP_BASE + 0x1048)   // SELECT [1:0], BUSY [16]
#define DIV_CLKCMU_FSYS_UFS_EMBD            (CMU_TOP_BASE + 0x1850)   // DIVRATIO [3:0], BUSY [16]
#define CMU_FSYS_BASE                       0x13400000UL
#define MUX_CLKCMU_FSYS_UFS_EMBD_USER       (CMU_FSYS_BASE + 0x0160)  // MUX_SEL [4], BUSY [7]

#define UFS_UNIPRO_CLK                      133250000UL               // 1599 MHz / 4 / 3
#define CNT_VAL_1US_MASK                    0x3FFU
#define UFSHCI_VS_1US_TO_CNT_VAL            0x110CU
#define UFSHCI_VS_UFSHCI_V2P1_CTRL          0x118CU
#define IA_TICK_SEL                         (1U << 16)

#define CMU_BUSY_TIMEOUT_US                 1000

STATIC EFI_GPIO_PROTOCOL *mGpioProtocol;

//
// UfsDxe switches the device's supply through a GPIO it is given (on the 9810, XBOOTLDO). The A51's UFS VCC
// is a fixed PMIC regulator (DT: vcc-fixed-regulator) that S-Boot leaves on, so the driver's write goes here.
//
STATIC UINT32 mNoDevicePowerSwitch = 1;

STATIC
VOID
CmuWaitNotBusy (
  IN UINTN  Register,
  IN UINT32 BusyBit)
{
  for (UINTN i = 0; i < CMU_BUSY_TIMEOUT_US; i++) {
    if (!(MmioRead32 (Register) & BusyBit)) {
      return;
    }
    MicroSecondDelay (1);
  }

  DEBUG ((EFI_D_ERROR, "UFS: CMU 0x%x stays busy\n", Register));
}

STATIC
VOID
UfsSetUniProClk (struct UfsHost *Ufs)
{
  // TOP: UFS_EMBD = PLL_SHARED0_DIV4 / 3
  MmioAndThenOr32 (MUX_CLKCMU_FSYS_UFS_EMBD, ~0x3U, 0x1);
  CmuWaitNotBusy (MUX_CLKCMU_FSYS_UFS_EMBD, BIT16);
  MmioAndThenOr32 (DIV_CLKCMU_FSYS_UFS_EMBD, ~0xFU, 0x2);
  CmuWaitNotBusy (DIV_CLKCMU_FSYS_UFS_EMBD, BIT16);

  // FSYS: take UFS_EMBD from CMU_TOP instead of OSCCLK
  MmioOr32 (MUX_CLKCMU_FSYS_UFS_EMBD_USER, BIT4);
  CmuWaitNotBusy (MUX_CLKCMU_FSYS_UFS_EMBD_USER, BIT7);

  // 1 us tick for the HCI (exynos_ufs_fit_aggr_timeout)
  MmioOr32 ((UINTN)(Ufs->IoAddr + UFSHCI_VS_UFSHCI_V2P1_CTRL), IA_TICK_SEL);
  MmioWrite32 ((UINTN)(Ufs->IoAddr + UFSHCI_VS_1US_TO_CNT_VAL), (UFS_UNIPRO_CLK / 1000000) & CNT_VAL_1US_MASK);
}

EFI_STATUS
UfsBoardInit (struct UfsHost *Ufs)
{
  EFI_STATUS Status;

  Status = gBS->LocateProtocol (&gEfiGpioProtocolGuid, NULL, (VOID *)&mGpioProtocol);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate GPIO Protocol! Status = %r\n", Status));
    return Status;
  }

  DEBUG ((EFI_D_INFO, "UFS: Exynos 9611 board init\n"));

  Ufs->IoAddr      = (VOID *)(UINTN)EXYNOS9611_UFS_BASE;
  Ufs->VsAddr      = (VOID *)(UINTN)EXYNOS9611_UFS_VS_BASE;
  Ufs->UniProAddr  = (VOID *)(UINTN)EXYNOS9611_UNIPRO_BASE;
  Ufs->PhyPma      = (VOID *)(UINTN)EXYNOS9611_PHY_PMA_BASE;

  Ufs->DevPwrAddr  = (VOID *)&mNoDevicePowerSwitch;
  Ufs->DevPwrShift = 0;
  Ufs->PhyIsoAddr  = (VOID *)(UINTN)EXYNOS9611_PMU_UFS_PHY_CONTROL;

  Ufs->MclkRate    = UFS_UNIPRO_CLK;
  Ufs->GearMode    = 3;

  UfsSetUniProClk (Ufs);

  // gpf0-0 REFCLK_OUT, gpf0-1 RST_N: function 3, no pull
  Status = mGpioProtocol->SetPull (BANK_ID_F, 0, 0, PULL_NONE);
  if (!EFI_ERROR (Status)) {
    Status = mGpioProtocol->SetPull (BANK_ID_F, 0, 1, PULL_NONE);
  }
  if (!EFI_ERROR (Status)) {
    Status = mGpioProtocol->SetFunction (BANK_ID_F, 0, 0, FUNCTION_3);
  }
  if (!EFI_ERROR (Status)) {
    Status = mGpioProtocol->SetFunction (BANK_ID_F, 0, 1, FUNCTION_3);
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "UFS: configuring gpf0-0/1 failed! Status = %r\n", Status));
    return Status;
  }

  // IO coherency for the UFS DMA (the tree marks the node dma-coherent)
  MmioAndThenOr32 (EXYNOS9611_SYSREG_FSYS_COHERENCY, ~EXYNOS9611_UFS_COHERENCY_MASK, EXYNOS9611_UFS_COHERENCY_MASK);

  return EFI_SUCCESS;
}
