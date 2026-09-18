#!/usr/bin/env python3
#
# elf2uf2.py - convert a linked RP2350 ELF image into a UF2 file
#
# Copyright (C) 2026 The pTOS development team
#
# This file is distributed under the GPL, version 2 or at your
# option any later version.  See doc/license.txt for details.
#
# Every PT_LOAD segment with file contents that lies in the XIP flash
# window is written out as 256-byte UF2 payload blocks tagged with the
# RP2350 ARM Secure family ID, which is what the bootrom's BOOTSEL drive
# expects for an image executed in place from flash.
#
# Usage: elf2uf2.py input.elf output.uf2
#

import struct
import sys

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
RP2350_ARM_S_FAMILY_ID = 0xE48BFF59

FLASH_START = 0x10000000
FLASH_END = 0x11000000
PAGE_SIZE = 256

PT_LOAD = 1


def load_segments(data):
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        sys.exit("elf2uf2: not a little-endian ELF32 file")
    (e_phoff,) = struct.unpack_from("<I", data, 28)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 42)
    segments = []
    for i in range(e_phnum):
        (p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags,
         p_align) = struct.unpack_from("<8I", data, e_phoff + i * e_phentsize)
        if p_type != PT_LOAD or p_filesz == 0:
            continue
        if not (FLASH_START <= p_paddr and p_paddr + p_filesz <= FLASH_END):
            sys.exit("elf2uf2: loadable segment at 0x%08x (0x%x bytes) is "
                     "outside the flash window" % (p_paddr, p_filesz))
        segments.append((p_paddr, data[p_offset:p_offset + p_filesz]))
    if not segments:
        sys.exit("elf2uf2: no loadable segments")
    return segments


def build_pages(segments):
    pages = {}
    for addr, blob in segments:
        for i, byte in enumerate(blob):
            a = addr + i
            base = a & ~(PAGE_SIZE - 1)
            page = pages.setdefault(base, bytearray(PAGE_SIZE))
            page[a - base] = byte
    return pages


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: elf2uf2.py input.elf output.uf2")
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    pages = build_pages(load_segments(data))
    addrs = sorted(pages)
    out = bytearray()
    for n, addr in enumerate(addrs):
        header = struct.pack("<8I", UF2_MAGIC_START0, UF2_MAGIC_START1,
                             UF2_FLAG_FAMILY_ID_PRESENT, addr, PAGE_SIZE, n,
                             len(addrs), RP2350_ARM_S_FAMILY_ID)
        payload = bytes(pages[addr]) + bytes(476 - PAGE_SIZE)
        out += header + payload + struct.pack("<I", UF2_MAGIC_END)
    with open(sys.argv[2], "wb") as f:
        f.write(out)
    print("# %s: %d blocks, 0x%08x-0x%08x" % (sys.argv[2], len(addrs),
          addrs[0], addrs[-1] + PAGE_SIZE))


if __name__ == "__main__":
    main()
