/*
 * processor_arm.h - ARMv8-M (Cortex-M33) processor identification
 *
 * Copyright (C) 2026 The pTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 *
 * Replaces bios/processor_arm.h on ARMv8-M: there is no CP15, the CPUID
 * register is memory mapped in the System Control Block.  Its layout
 * (implementer, variant, architecture, part number, revision) is the same
 * as the A-profile MIDR, so bios.c's CPU name table works unchanged.
 */

#ifndef PROCESSOR_ARM_H
#define PROCESSOR_ARM_H

typedef unsigned long uint32_t;     /* as in bios/processor_arm.h */

#define ARMV8M_SCB_CPUID_REG (*(volatile ULONG *)0xe000ed00)

static inline ULONG __attribute__((__const__)) read_cpuid_id(void)
{
    return ARMV8M_SCB_CPUID_REG;
}

void invalidate_data_cache_all(void);
void flush_data_cache_all(void);
void flush_branch_target_cache(void);

#endif /* PROCESSOR_ARM_H */
