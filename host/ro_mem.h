/* SPDX-License-Identifier: GPL-3.0-only */
/* Host check for the FM-1's flash rule: what is programmed must come from RAM. On the device a
 * const (.rodata) sits in the XIP flash, which cannot be read while the flash is being programmed:
 * the CPU stalls until the watchdog reboots it. On the host such a source is in a read-only mapping,
 * so a program from one is reported (and the run fails) instead of passing unnoticed. */
#ifndef RO_MEM_H
#define RO_MEM_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int ro_mem(const void *p)                 /* 1: p is in a mapping the program cannot write */
{
#ifdef __linux__
    char line[512];
    uintptr_t a = (uintptr_t)p;
    int ro = 0;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perm[5];
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perm) == 3 && a >= lo && a < hi) {
            ro = perm[1] != 'w';
            break;
        }
    }
    fclose(f);
    return ro;
#else
    (void)p;
    return 0;
#endif
}
#endif
