#!/bin/sh
#
# check-no-fpu.sh - the kernel must not touch the floating point unit
#
# Copyright (C) 2026 The pTOS development team
#
# This software is licenced under the GNU Public License.
# Please see LICENSE.TXT for further information.
#
# On the RP2350 the exception frame stays the small one -- 8 words, not
# 26 -- because nothing in the operating system ever sets FPCA.  That
# matters: this port builds and edits exception frames by hand, in
# proc_go() and in the SVC redirect, and they all expect the small one.
# It is also what makes it safe to leave automatic floating point
# stacking off, since no handler of ours can then disturb the registers
# of a program in the middle of a calculation.
#
# So the assumption is checked instead of trusted.  Programs and the
# real-time runtime use the unit as much as they like; the kernel must
# not, apart from the two instructions in the AES context switch that
# carry a program's s16..s31 across a process switch.
#
#   usage: check-no-fpu.sh OBJDUMP ALLOWED.o OBJECT...
#
# Exits non-zero, naming the file and the instruction, if any other
# object contains one.

OBJDUMP="$1"
ALLOWED="$2"
shift 2

status=0

for obj in "$@"; do
    case "$obj" in
        *"$ALLOWED") continue ;;
    esac

    found=`"$OBJDUMP" -d "$obj" 2>/dev/null |
           awk -F'\t' 'NF >= 3 { split($3, a, " "); print a[1] }' |
           grep -E '^(v[a-z]|fld|fst)' | sort -u | tr '\n' ' '`

    if [ -n "$found" ]; then
        echo "### $obj uses the floating point unit: $found" >&2
        status=1
    fi
done

if [ $status -ne 0 ]; then
    echo "###" >&2
    echo "### The kernel must not touch the FPU: with FPCA set, an" >&2
    echo "### exception lays down 26 words instead of 8, and this port" >&2
    echo "### builds such frames by hand (proc_go, the SVC redirect)." >&2
    echo "### See Kconfig.machine, and $ALLOWED for the two exceptions." >&2
fi

exit $status
