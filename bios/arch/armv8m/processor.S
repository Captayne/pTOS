/*
 * processor.S - processor and cache support for ARMv8-M (Cortex-M33)
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * The Cortex-M33 on the RP2350 has no data or instruction cache of its
 * own; the only cache is the XIP cache in front of the flash, which pTOS
 * never writes to.  So all cache maintenance is a no-op apart from the
 * barriers needed after writing code to RAM.  The FPU is left disabled:
 * the kernel is built for soft float (see CPUFLAGS in Kconfig.machine).
 */

#include "asmdefs.h"

        .globl  C_SYM(processor_init)
        .globl  C_SYM(instruction_cache_kludge)
        .globl  C_SYM(invalidate_instruction_cache)
        .globl  C_SYM(flush_prefetch_buffer)
        .globl  C_SYM(flush_branch_target_cache)
        .globl  C_SYM(flush_data_cache)
        .globl  C_SYM(invalidate_data_cache)
        .globl  C_SYM(invalidate_data_cache_all)
        .globl  C_SYM(flush_data_cache_all)

        .text

        .balign 4
ASM_FUNC(processor_init):
        b       C_SYM(detect_cpu)

        .balign 4
ASM_FUNC(instruction_cache_kludge):
ASM_FUNC(invalidate_instruction_cache):
ASM_FUNC(flush_prefetch_buffer):
ASM_FUNC(flush_branch_target_cache):
        dsb
        isb
        bx      lr

        .balign 4
ASM_FUNC(flush_data_cache):
ASM_FUNC(invalidate_data_cache):
ASM_FUNC(invalidate_data_cache_all):
ASM_FUNC(flush_data_cache_all):
        dsb
        bx      lr
