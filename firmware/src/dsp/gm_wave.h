/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM's instrument tables: the types. The tables themselves (gm_wavetable.h) are generated at
 * build time by tools/gen_wavetable.py from assets/soundfont/fm1gm.sf2 and fm1gm_sustain.json.
 *
 * An instrument is one sample, its attack, then a sustain: a waveform (a single-cycle table at
 * GMW_LEVELS band limits) or filtered noise. Samples are 4-bit IMA ADPCM, two points a byte (low
 * nibble first), peak-normalised, played once (no loops). A zone (a program, or one drum note)
 * carries the SoundFont generators gm.c follows, in SoundFont units: timecents
 * (t = 2^(tc / 1200) s), cents, centibels. */
#pragma once
#include <stdint.h>

#define GMW_TAB_N 256               /* points a cycle (each table has one more: the first again) */
#define GMW_LEVELS 7                /* level L holds 64 >> L harmonics */
#define GMW_NONE 255                /* sustain: none (a one-shot) */
#define GMW_NOISE 254               /* sustain: noise */

typedef struct {
    uint32_t off;                  /* first byte in GMW_DATA */
    uint32_t len;                  /* points */
    uint8_t idx0;                  /* the step index at point 0 */
    float rate;                    /* Hz */
    uint8_t shape;                 /* the sustain: GMW_TAB index, GMW_NOISE or GMW_NONE */
    uint32_t xs, xe;               /* the crossfade from the sample into it: points */
    float ph0, cps;                /* the waveform's phase at point 0 (turns), cycles a point */
    float gain;                    /* its level, on the sample's (decoded) scale */
    float hp, lp;                  /* noise: one-pole high-pass and low-pass, Hz at the sample's pitch */
} gmw_smp_t;

typedef struct {
    uint16_t smp;
    uint8_t lo, hi, root, excl;    /* key range; root key; drums: exclusive class (a hit cuts the others) */
    int8_t pan;                    /* drums: -64 .. 63 */
    int16_t tune;                  /* cents */
    int16_t kt;                    /* key tracking, cents per key (100: normal) */
    int16_t att, hold, dec, sus, rel;   /* volume envelope: timecents (dec, rel: to fall 100 dB); sus: cB */
    int16_t kdec, khold;           /* timecents per key below 60 added to dec, hold */
    int16_t fc, q;                 /* low-pass: cutoff (absolute cents, 13500: off), resonance (cB) */
    int16_t fenv, fatt, fdec, fsus;     /* modulation envelope to the cutoff: cents; timecents; 0.1 % */
    float gain;                    /* sample peak x attenuation, linear */
} gmw_zone_t;
