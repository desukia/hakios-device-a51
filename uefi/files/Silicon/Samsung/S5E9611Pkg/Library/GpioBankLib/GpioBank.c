/**
  GPIO banks of the Exynos 9611 (shares its pin banks with the Exynos 9610).

  Source: Samsung's kernel for universal9611, drivers/pinctrl/samsung/pinctrl-exynos-arm64.c,
  exynos9610_pin_banks0 (ALIVE), 2 (DISPAUD), 3 (FSYS) and 4 (TOP). CMGP (26 single-pin banks) and
  SHUB are left out: nothing in UEFI uses them.
**/
#include <Library/GpioBankLib.h>

STATIC
EFI_GPIO_BANK
mGpioBanks[] = {
  // ALIVE (0x11850000): etc0 at 0x000 is not a GPIO bank
  { .CtrlNum = 0, .Id = BANK_ID_A, .Number = 0, .Offset = 0x020 },
  { .CtrlNum = 0, .Id = BANK_ID_A, .Number = 1, .Offset = 0x040 },  // keys: vol up 5, vol down 6, power 7
  { .CtrlNum = 0, .Id = BANK_ID_A, .Number = 2, .Offset = 0x060 },
  { .CtrlNum = 0, .Id = BANK_ID_Q, .Number = 0, .Offset = 0x080 },

  // DISPAUD (0x14A60000)
  { .CtrlNum = 1, .Id = BANK_ID_B, .Number = 0, .Offset = 0x000 },
  { .CtrlNum = 1, .Id = BANK_ID_B, .Number = 1, .Offset = 0x020 },
  { .CtrlNum = 1, .Id = BANK_ID_B, .Number = 2, .Offset = 0x040 },

  // FSYS (0x13490000)
  { .CtrlNum = 2, .Id = BANK_ID_F, .Number = 0, .Offset = 0x000 },
  { .CtrlNum = 2, .Id = BANK_ID_F, .Number = 1, .Offset = 0x020 },
  { .CtrlNum = 2, .Id = BANK_ID_F, .Number = 2, .Offset = 0x040 },

  // TOP (0x139B0000)
  { .CtrlNum = 3, .Id = BANK_ID_P, .Number = 0, .Offset = 0x000 },
  { .CtrlNum = 3, .Id = BANK_ID_P, .Number = 1, .Offset = 0x020 },
  { .CtrlNum = 3, .Id = BANK_ID_P, .Number = 2, .Offset = 0x040 },
  { .CtrlNum = 3, .Id = BANK_ID_C, .Number = 0, .Offset = 0x060 },
  { .CtrlNum = 3, .Id = BANK_ID_C, .Number = 1, .Offset = 0x080 },
  { .CtrlNum = 3, .Id = BANK_ID_C, .Number = 2, .Offset = 0x0A0 },
  { .CtrlNum = 3, .Id = BANK_ID_G, .Number = 0, .Offset = 0x0C0 },
  { .CtrlNum = 3, .Id = BANK_ID_G, .Number = 1, .Offset = 0x0E0 },
  { .CtrlNum = 3, .Id = BANK_ID_G, .Number = 2, .Offset = 0x100 },
  { .CtrlNum = 3, .Id = BANK_ID_G, .Number = 3, .Offset = 0x120 },
  { .CtrlNum = 3, .Id = BANK_ID_G, .Number = 4, .Offset = 0x140 },
};

VOID
GetGpioBanks (
  OUT EFI_GPIO_BANK **Bank,
  OUT UINT8          *Count)
{
  *Bank  = mGpioBanks;
  *Count = ARRAY_SIZE (mGpioBanks);
}
