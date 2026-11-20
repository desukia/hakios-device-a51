#!/usr/bin/env python3
"""
patch-ld64.py - Neutralize assertion crashes in cctools ld64 on Linux.

In Apple's official ld64, debug assertions are compiled out (-DNDEBUG), so
encountering typeLastSection, typeFirstSection, or typeTempLTO simply executes:
    return S_REGULAR; // 0x0
However, community cross-builds of cctools ld64 (such as sbingner's llvm-project)
were compiled without -DNDEBUG. When linking static binaries like the XNU kernel
(mach.development.bcm2837.unstripped), ld64 hits:
    ld64: ./HeaderAndLoadCommands.hpp:969: ... Assertion `0 && "typeLastSection should not make it to final linked image"' failed.

This script patches the 25-byte __assert_fail call sites inside
HeaderAndLoadCommandsAtom<A>::sectionFlags with an in-place atomic return:
    31 c0       xor eax, eax  ; return 0 (S_REGULAR)
    5b          pop rbx       ; restore callee-saved registers
    41 5c       pop r12
    41 5d       pop r13
    41 5e       pop r14
    41 5f       pop r15
    c3          ret
    90 ... 90   nop (x13)
Total length = 12 bytes + 13 bytes = 25 bytes.
This restores the exact stack frame, sets the return value to S_REGULAR (0),
and returns cleanly to the caller without any stack corruption or crash.
"""

import os
import struct
import sys

# 12-byte atomic return sequence + 13 NOPs = 25 bytes
INLINE_RETURN_0 = bytes.fromhex("31c05b415c415d415e415fc3") + b"\x90" * 13

TARGET_NEEDLES = [
    ("typeLastSection", bytes.fromhex("bf49de6600")),
    ("typeFirstSection", bytes.fromhex("bf08de6600")),
    ("typeTempLTO", bytes.fromhex("bf74dd6600")),
]


def patch_binary(path):
    real_path = os.path.realpath(path)
    if not os.path.isfile(real_path):
        print(f"[!] Arquivo não encontrado: {path} ({real_path})")
        return False

    with open(real_path, "rb") as f:
        data = bytearray(f.read())

    total_patched = 0
    for name, needle in TARGET_NEEDLES:
        idx = data.find(needle)
        while idx != -1:
            # Verifica se o bloco de 25 bytes termina com call (0xe8)
            if idx + 25 <= len(data) and data[idx + 20] == 0xe8:
                data[idx : idx + 25] = INLINE_RETURN_0
                total_patched += 1
                print(f"[✓] {name} em 0x{idx:x} neutralizado com retorno atômico S_REGULAR")
            elif idx + 25 <= len(data) and data[idx : idx + 12] == bytes.fromhex("31c05b415c415d415e415fc3"):
                print(f"[*] {name} em 0x{idx:x} já está com patch atômico.")
                total_patched += 1
            idx = data.find(needle, idx + 1)

    # Fallback genérico: busca qualquer assert_fail associado a "should not make it to final linked image"
    if total_patched == 0:
        str_needle = b"should not make it to final linked image"
        if str_needle in data:
            print("[*] Buscando referências genéricas à asserção...")
            str_idx = data.find(str_needle)
            while str_idx != -1:
                prefix_idx = data.rfind(b'0 && "', max(0, str_idx - 30), str_idx)
                target_str_idx = prefix_idx if prefix_idx != -1 else str_idx
                va = 0x400000 + target_str_idx
                va_bytes = b"\xbf" + struct.pack("<I", va)
                call_idx = data.find(va_bytes)
                while call_idx != -1:
                    if call_idx + 25 <= len(data) and data[call_idx + 20] == 0xe8:
                        data[call_idx : call_idx + 25] = INLINE_RETURN_0
                        total_patched += 1
                        print(f"[✓] Asserção genérica em 0x{call_idx:x} neutralizada com retorno atômico")
                    call_idx = data.find(va_bytes, call_idx + 1)
                str_idx = data.find(str_needle, str_idx + 1)

    if total_patched > 0:
        with open(real_path, "wb") as f:
            f.write(data)
        os.chmod(real_path, 0o755)
        print(f"[✓] {real_path}: {total_patched} asserções neutralizadas com sucesso!")
        return True
    else:
        print(f"[!] Nenhuma asserção precisou de patch em {real_path} (já corrigido ou formato diferente).")
        return False


def main():
    targets = sys.argv[1:]
    if not targets:
        default_theos = os.path.expanduser("~/theos/toolchain/linux/iphone/bin/ld")
        targets = [default_theos, default_theos + "64"]

    success = False
    for t in targets:
        if os.path.exists(t):
            print(f"[*] Processando {t}...")
            if patch_binary(t):
                success = True

    return 0 if success else 1


if __name__ == "__main__":
    main()
