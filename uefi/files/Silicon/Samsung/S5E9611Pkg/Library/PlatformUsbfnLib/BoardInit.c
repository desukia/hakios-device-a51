/**
  USB 2.0 device mode for the Exynos 9611 (Galaxy A51: USB 2.0 only, "maximum-speed = high-speed").

  Where the numbers come from:
    - the phone's device tree: usb@13200000 (DWC3 at 0x13200000, snps,dis-u2-freeclk-exists-quirk,
      adj-sof-accuracy = 0, is_not_vbus_pad), phy@131D0000 (USBCON v3.0.0 "phy_version = 0x300",
      common_block_disable = 1, use_io_for_ovc = 0, pmu_offset 0x704, pmu_mask 0x3), usb_hs_tune (device column)
    - Samsung's kernel (universal9611): phy-exynos-usb3p1.c phy_exynos_usb_v3p1_enable() and _pipe_ovrd(),
      the sequence below follows them step for step for a 3.0.0 USBCON (minor 0: CLKRST_PHY_* reset bits, the
      link reset first); phy-exynos-usbdrd.c (PMU: the mask written to leave isolation, 0 to enter it);
      cal-if/exynos9610 (CMU_USB 0x13030000 gates, CMU_TOP USB gates; CG_VAL bit 21, MANUAL bit 20)
    - the structure is S5E9830Pkg's PlatformUsbfnLib (the same usb3p1 PHY family)

  The USB block has no power domain on the 9611 (no pd-usb node), so only its clock gates are made sure of.
**/
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PlatformUsbfnLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Device/Dwc3.h>

#include "PlatformUsbRegisters.h"

#define EXYNOS9611_PMU_BASE             0x11860000UL
#define EXYNOS9611_PMU_USBDRD_PHY_CTRL  (EXYNOS9611_PMU_BASE + 0x0704)
#define EXYNOS9611_PMU_USBDRD_PHY_MASK  0x3U

#define EXYNOS9611_USBDRD_LINK_BASE     0x13200000UL
#define EXYNOS9611_USBDRD_CTRL_BASE     0x131D0000UL

#define CMU_TOP_BASE                    0x12100000UL
#define CMU_USB_BASE                    0x13030000UL
#define CMU_GATE_FORCE_ON               (BIT21 | BIT20)   // CG_VAL | MANUAL

STATIC CONST UINTN mUsbClockGates[] = {
  CMU_TOP_BASE + 0x2090,        // GATE_CLKCMU_USB_BUS
  CMU_TOP_BASE + 0x2098,        // GATE_CLKCMU_USB_USB30DRD
  CMU_USB_BASE + 0x2034,        // USB30DRD ACLK_PHYCTRL_20
  CMU_USB_BASE + 0x2038,        // USB30DRD ACLK_PHYCTRL_30_0
  CMU_USB_BASE + 0x203C,        // USB30DRD ACLK_PHYCTRL_30_1
  CMU_USB_BASE + 0x2040,        // USB30DRD BUS_CLK_EARLY
  CMU_USB_BASE + 0x2044,        // USB30DRD REF_CLK
};

#define USBCON(Offset)                  (EXYNOS9611_USBDRD_CTRL_BASE + (Offset))

STATIC
VOID
UsbClocksOn (VOID)
{
  for (UINTN i = 0; i < ARRAY_SIZE (mUsbClockGates); i++) {
    MmioOr32 (mUsbClockGates[i], CMU_GATE_FORCE_ON);
  }
}

STATIC
VOID
UsbPhyIsolation (IN BOOLEAN PowerOn)
{
  // exynos_usbdrd_utmi_phy_isol(): val = on ? 0 : mask, where "on" means isolated
  MmioAndThenOr32 (EXYNOS9611_PMU_USBDRD_PHY_CTRL, ~EXYNOS9611_PMU_USBDRD_PHY_MASK,
                   PowerOn ? EXYNOS9611_PMU_USBDRD_PHY_MASK : 0);
}

STATIC
VOID
UsbPhyForceQChannel (VOID)
{
  // exynos_cal_usbphy_q_ch(regs_base, 1)
  UINT32 Register;

  Register  = MmioRead32 (USBCON (EXYNOS_USBCON_LINK_CTRL));
  Register |= LINKCTRL_DIS_QACT_ID0 | LINKCTRL_DIS_QACT_VBUS_VALID | LINKCTRL_DIS_QACT_BVALID | LINKCTRL_DIS_QACT_LINKGATE;
  Register &= ~LINKCTRL_FORCE_QACT;

  gBS->Stall (500);
  MmioWrite32 (USBCON (EXYNOS_USBCON_LINK_CTRL), Register);
  gBS->Stall (500);

  Register  = MmioRead32 (USBCON (EXYNOS_USBCON_LINK_CTRL));
  Register |= LINKCTRL_FORCE_QACT;
  gBS->Stall (500);
  MmioWrite32 (USBCON (EXYNOS_USBCON_LINK_CTRL), Register);
}

STATIC
VOID
UsbLinkReset (VOID)
{
  MmioOr32 (USBCON (EXYNOS_USBCON_CLKRST), CLKRST_LINK_SW_RST);
  gBS->Stall (10);
  MmioAnd32 (USBCON (EXYNOS_USBCON_CLKRST), ~(UINT32)CLKRST_LINK_SW_RST);
}

STATIC
VOID
UsbPhySoftResetAssert (VOID)
{
  // phy_sw_rst_high(), USBCON minor version 0
  MmioOr32 (USBCON (EXYNOS_USBCON_CLKRST), CLKRST_PHY_SW_RST | CLKRST_PHY_RST_SEL | CLKRST_PORT_RST);
}

STATIC
VOID
UsbPhySoftResetDeassert (VOID)
{
  // phy_sw_rst_low(), USBCON minor version 0
  MmioAndThenOr32 (USBCON (EXYNOS_USBCON_CLKRST), ~(UINT32)(CLKRST_PHY_SW_RST | CLKRST_PORT_RST), CLKRST_PHY_RST_SEL);
}

STATIC
VOID
UsbPhyPowerEnable (IN BOOLEAN Enable)
{
  // phy_power_en(), USBCON 3.0.0: the 2.0 PHY's SIDDQ
  if (Enable) {
    MmioAnd32 (USBCON (EXYNOS_USBCON_HSP_TEST), ~(UINT32)HSP_TEST_SIDDQ);
  } else {
    MmioOr32 (USBCON (EXYNOS_USBCON_HSP_TEST), HSP_TEST_SIDDQ);
  }
}

STATIC
VOID
UsbPhyTune (VOID)
{
  UINT32 Register = MmioRead32 (USBCON (EXYNOS_USBCON_HSP_TUNE));

  Register &= ~(HSP_TUNE_TXVREF_MASK | HSP_TUNE_TXPREEMPA_MASK | HSP_TUNE_TXPREEMPA_PLUS | HSP_TUNE_TXRES_MASK |
                HSP_TUNE_TXRISE_MASK | HSP_TUNE_HSXV_MASK | HSP_TUNE_TXFSLS_MASK | HSP_TUNE_SQRX_MASK |
                HSP_TUNE_COMPDIS_MASK | HSP_TUNE_OTG_MASK);
  Register |= HSP_TUNE_TXVREF (EXYNOS9611_TUNE_TX_VREF);
  Register |= HSP_TUNE_TXPREEMPA (EXYNOS9611_TUNE_TX_PRE_EMP);       // tx_pre_emp_plus = 0
  Register |= HSP_TUNE_TXRES (EXYNOS9611_TUNE_TX_RES);
  Register |= HSP_TUNE_TXRISE (EXYNOS9611_TUNE_TX_RISE);
  Register |= HSP_TUNE_HSXV (EXYNOS9611_TUNE_TX_HSXV);
  Register |= HSP_TUNE_TXFSLS (EXYNOS9611_TUNE_TX_FSLS);
  Register |= HSP_TUNE_SQRX (EXYNOS9611_TUNE_RX_SQRX);
  Register |= HSP_TUNE_COMPDIS (EXYNOS9611_TUNE_COMPDIS);
  Register |= HSP_TUNE_OTG (EXYNOS9611_TUNE_OTG);

  MmioWrite32 (USBCON (EXYNOS_USBCON_HSP_TUNE), Register);
}

STATIC
VOID
UsbPhyPipe3Override (VOID)
{
  // phy_exynos_usb_v3p1_pipe_ovrd(): no SuperSpeed PHY on this port, so the link is told PIPE3 is idle
  MmioAndThenOr32 (USBCON (EXYNOS_USBCON_LINK_CTRL), ~(UINT32)LINKCTRL_PIPE3_FORCE_PHY_STATUS,
                   LINKCTRL_PIPE3_FORCE_EN | LINKCTRL_PIPE3_FORCE_RX_ELEC_IDLE);
  MmioOr32 (USBCON (EXYNOS_USBCON_COMBO_PMA_CTRL), PMA_LOW_PWR);
}

EFI_STATUS
UsbfnPlatformGetData (OUT USBFN_PLATFORM_DATA *PlatformData)
{
  if (PlatformData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  PlatformData->ControllerBase        = EXYNOS9611_USBDRD_LINK_BASE;
  PlatformData->MaximumSpeed          = UsbBusSpeedHigh;
  PlatformData->SuperSpeedPhyDisabled = TRUE;
  PlatformData->UtmiWidth             = UsbfnUtmiWidth16Bit;   // Samsung's HS PHY (as the 9830's)
  PlatformData->Usb2FreeClockExists   = FALSE;                 // snps,dis-u2-freeclk-exists-quirk
  PlatformData->FrameLengthAdjustMask = 0;                     // adj-sof-accuracy = 0: GFLADJ left alone
  PlatformData->FrameLengthAdjustValue = 0;
  PlatformData->NumEndpoints          = 0;                     // from GHWPARAMS3
  PlatformData->VendorId              = 0x04E8;
  PlatformData->ProductId             = 0x2143;

  return EFI_SUCCESS;
}

EFI_STATUS
UsbfnPlatformPhyInit (VOID)
{
  UINT32 Register;

  UsbClocksOn ();
  UsbPhyIsolation (TRUE);
  gBS->Stall (10);

  Register = MmioRead32 (USBCON (EXYNOS_USBCON_CTRL_VER));
  if (Register == 0xFFFFFFFF || Register == 0) {
    DEBUG ((EFI_D_ERROR, "USB: USBCON does not answer (0x%x)\n", Register));
    return EFI_DEVICE_ERROR;
  }

  UsbPhyForceQChannel ();
  UsbLinkReset ();
  UsbPhySoftResetAssert ();

  Register  = MmioRead32 (USBCON (EXYNOS_USBCON_UTMI));
  Register &= ~(UTMI_FORCE_SUSPEND | UTMI_FORCE_SLEEP | UTMI_DP_PULLDOWN | UTMI_DM_PULLDOWN);
  MmioWrite32 (USBCON (EXYNOS_USBCON_UTMI), Register);

  // common_block_disable = 1
  MmioOr32 (USBCON (EXYNOS_USBCON_HSP), HSP_EN_UTMISUSPEND | HSP_COMMONONN);

  gBS->Stall (100);

  // no VBUS pad: VBUS and B-session are forced valid, the bus filter bypassed
  MmioOr32 (USBCON (EXYNOS_USBCON_LINK_CTRL), LINKCTRL_BUS_FILTER_BYPASS (0xF));
  MmioOr32 (USBCON (EXYNOS_USBCON_UTMI), UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID);
  MmioOr32 (USBCON (EXYNOS_USBCON_HSP), HSP_VBUSVLDEXTSEL | HSP_VBUSVLDEXT);

  UsbPhyTune ();
  UsbPhyPowerEnable (TRUE);
  gBS->Stall (10);                 // before POR low
  UsbPhySoftResetDeassert ();
  gBS->Stall (75);                 // PHYCLOCK guaranteed after this

  // use_io_for_ovc = 0
  MmioOr32 (USBCON (EXYNOS_USBCON_LINK_PORT), LINKPORT_HUB_PORT_SEL_OCD_U3 | LINKPORT_HUB_PORT_SEL_OCD_U2);

  UsbPhyPipe3Override ();

  return EFI_SUCCESS;
}

EFI_STATUS
UsbfnPlatformPhyExit (VOID)
{
  MmioAndThenOr32 (USBCON (EXYNOS_USBCON_UTMI), ~(UINT32)(UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID),
                   UTMI_FORCE_SUSPEND | UTMI_FORCE_SLEEP);
  MmioAnd32 (USBCON (EXYNOS_USBCON_HSP), ~(UINT32)(HSP_VBUSVLDEXTSEL | HSP_VBUSVLDEXT));

  UsbPhyPowerEnable (FALSE);
  UsbPhySoftResetAssert ();
  UsbPhyIsolation (FALSE);

  return EFI_SUCCESS;
}

BOOLEAN
UsbfnPlatformIsVbusPresent (VOID)
{
  // is_not_vbus_pad: no VBUS sense; a line state other than SE0 means a host is pulling the lines
  return (BOOLEAN)(HSP_TEST_LINESTATE (MmioRead32 (USBCON (EXYNOS_USBCON_HSP_TEST))) != 0);
}
