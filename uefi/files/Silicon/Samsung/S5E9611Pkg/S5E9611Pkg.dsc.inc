##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  #
  # Timer
  #
  USE_PHYSICAL_TIMER                         = 0

  #
  # KEP
  # Exynos 9611: GIC-400 (GICv2) at the same addresses as the 7885; 26 MHz arch timer, virtual timer PPI 11 (INTID 27).
  #
  HAS_CNTFRQ_EL0_EMPTY_ERRATA                = 0

!include SamsungPkg/SamsungPkg.dsc.inc

[BuildOptions]
  *_*_AARCH64_CC_FLAGS = -march=armv8-a+crypto

[PcdsFixedAtBuild]
  #
  # ACPI
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemTableId|0x2031313639453553
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemRevision|0x9611

  #
  # CPU
  #
  gArmPlatformTokenSpaceGuid.PcdCoreCount|8
  gArmPlatformTokenSpaceGuid.PcdClusterCount|2

  #
  # GIC
  #
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x12301000
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0x12302000

  #
  # GPIO (order = CtrlNum in GpioBankLib)
  # List Contents: 0x11850000 (ALIVE), 0x14A60000 (DISPAUD), 0x13490000 (FSYS), 0x139B0000 (TOP)
  #
  gSamsungPkgTokenSpaceGuid.PcdGpioCtrlAddr|{ 0x00, 0x00, 0x85, 0x11, 0x00, 0x00, 0xA6, 0x14, 0x00, 0x00, 0x49, 0x13, 0x00, 0x00, 0x9B, 0x13 }

  #
  # SPEEDY
  # List Contents: 0x11A10000 (speedy@11a10000, S2MPU09)
  #
  gSamsungPkgTokenSpaceGuid.PcdSpeedyBusAddr|{ 0x00, 0x00, 0xA1, 0x11 }

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorModel|"Exynos (TM) 9611 @ 2.31 GHz"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorPartNumber|"S5E9611"

[LibraryClasses]
  #
  # SEC Libraries
  #
  PlatformSecLib|S5E9611Pkg/Library/PlatformSecLib/PlatformSecLib.inf

  #
  # IO Libraries
  #
  GpioBankLib|S5E9611Pkg/Library/GpioBankLib/GpioBankLib.inf

  #
  # Storage Libraries
  #
  PlatformUfsLib|S5E9611Pkg/Library/PlatformUfsLib/PlatformUfsLib.inf

  #
  # USB Libraries
  #
  PlatformUsbfnLib|S5E9611Pkg/Library/PlatformUsbfnLib/PlatformUsbfnLib.inf
