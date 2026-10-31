/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-TRAX: instruments (docs/DESIGN.md, 4).
 *
 *   FM   the YM2612's patch, register for register, and optional arpeggio and pitch macros
 *   PSG  macros: volume, arpeggio, pitch, noise mode (the PSG has no other timbre)
 *   DAC  an 8-bit sample on FM6: which, its root note, its rate, loop
 *
 * A macro is a sequence of up to MACRO_LEN values played one per tick, repeating from its loop
 * point while the note is held and going on from its release point when it is let go. */
#pragma once
#include <stdint.h>

enum { INS_EMPTY, INS_FM, INS_PSG, INS_DAC };
enum { MAC_VOL, MAC_ARP, MAC_PITCH, MAC_NOISE, NMAC };
#define MACRO_LEN 32
#define MACRO_NONE 0xFF

typedef struct {
    uint8_t len;                       /* 0: the macro is off */
    uint8_t loop, rel;                 /* MACRO_NONE: none */
    int8_t v[MACRO_LEN];
} macro_t;

typedef struct {
    uint8_t ar, d1r, d2r, rr;          /* 0-31, 0-31, 0-31, 0-15 */
    uint8_t sl, tl;                    /* 0-15, 0-127 */
    uint8_t ks, mul, dt;               /* 0-3, 0-15, 0-7 (4-7: negative) */
    uint8_t am, ssg;                   /* 0-1; 0, or 8-15 (SSG-EG on, mode 0-7) */
} fm_op_t;

typedef struct {
    uint8_t alg, fb, ams, pms;
    fm_op_t op[4];                     /* OP1..OP4 (the algorithm's numbering, not the registers') */
} fm_patch_t;

typedef struct {
    uint8_t sample;                    /* index into the sample bank */
    uint8_t root;                      /* the note that plays it at its own rate (1..96) */
    uint8_t loop;
    uint8_t pitched;                   /* 0: every note at the sample's rate (as most drivers) */
} dac_ins_t;

typedef struct {
    uint8_t type;                      /* INS_* */
    char name[15];
    union {
        fm_patch_t fm;
        dac_ins_t dac;
    } u;
    macro_t mac[NMAC];
} ins_t;

void ins_init(ins_t *i, int type);
int ins_is_carrier(int alg, int op);   /* the operator sounds (rather than modulates) in the algorithm */
