/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM's sample tables: the types. The tables themselves (gm_wavetable.h) are generated at build
 * time by tools/gen_wavetable.py from the SoundFont assets/soundfont/fm1gm.sf2.
 *
 * Samples are 4-bit IMA ADPCM, two points a byte (low nibble first), peak-normalised. A zone is a
 * key range of one program (or one drum note) and carries the SoundFont generators gm.c follows,
 * in SoundFont units: timecents (t = 2^(tc / 1200) s), cents, centibels. */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t off;                  /* first byte in GMW_DATA */
    uint32_t len;                  /* points */
    uint32_t ls, le;               /* loop [ls, le); le 0: a one-shot */
    int16_t lpred;                 /* the decoder's state at ls: predictor, */
    uint8_t lidx;                  /* step index */
    uint8_t idx0;                  /* the step index at point 0 */
    float rate;                    /* Hz */
} gmw_smp_t;

typedef struct {
    uint16_t smp;
    uint8_t lo, hi, root, excl;    /* key range; root key; drums: exclusive class (a hit cuts the others) */
    int8_t pan;                    /* drums: -64 .. 63 */
    uint8_t trem;                  /* tremolo: depth (0.5 dB steps, high nibble), rate (2 Hz steps, low); 0: none */
    int16_t tune;                  /* cents */
    int16_t kt;                    /* key tracking, cents per key (100: normal) */
    int16_t att, hold, dec, sus, rel;   /* volume envelope: timecents (dec, rel: to fall 100 dB); sus: cB */
    int16_t kdec, khold;           /* timecents per key below 60 added to dec, hold */
    int16_t fc, q;                 /* low-pass: cutoff (absolute cents, 13500: off), resonance (cB) */
    int16_t fenv, fatt, fdec, fsus;     /* modulation envelope to the cutoff: cents; timecents; 0.1 % */
    float gain;                    /* sample peak x attenuation, linear */
} gmw_zone_t;
