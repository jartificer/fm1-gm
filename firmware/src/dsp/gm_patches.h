/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM: the 128 General MIDI programs and the GM drum kit, as voice recipes for gm.c.
 * Included by gm.c only. The names are the GM Level 1 sound set's; the sounds are FM1-GM's own,
 * synthesized (no samples), so they are recognisable stand-ins, not copies of any sound module.
 *
 * Times are codes: t(code) = 1 ms * 2^(code / 10), so 0 = 1 ms, 50 = 32 ms, 70 = 128 ms,
 * 90 = 0.5 s, 100 = 1 s, 110 = 2 s, 120 = 4 s, 130 = 8 s. A decay or release takes about t(code)
 * to fall by 43 dB. */
#pragma once
#include <stdint.h>

enum { WV_FM, WV_SAW, WV_PULSE, WV_NOISE };
enum {
    F_KS = 1,       /* key scaling: higher notes decay faster (pianos, plucked strings, mallets) */
    F_HP = 2,       /* the filter is a high-pass */
    F_BP = 4,       /* the filter is a band-pass */
    F_VIBNOW = 8,   /* vibrato from the start (else it fades in after a quarter second) */
    F_IXAMP = 16,   /* the FM index follows the amplitude (brass: brighter as it gets louder) */
    F_DRIVE = 32,   /* soft clipping in the voice (overdriven guitars) */
    F_BREATH = 64,  /* breath noise in the source (flutes) */
};

typedef struct {
    uint8_t wave;   /* WV_* */
    int8_t oct;     /* octave shift */
    uint8_t rc;     /* carrier / oscillator pitch ratio x2 (2 = the note) */
    uint8_t rm;     /* FM: modulator ratio x2; PULSE: pulse width % */
    uint8_t idx;    /* FM index at the attack, radians x10 */
    uint8_t isus;   /* FM index it settles to, % of the attack's (above 100: it grows) */
    uint8_t idec;   /* ... in this time (code) */
    uint8_t fb;     /* FM modulator feedback, % */
    uint8_t det;    /* second oscillator: detune, cents (0 and iv 0: no second oscillator) */
    int8_t iv;      /* second oscillator: interval, semitones */
    uint8_t o2;     /* second oscillator: level, % */
    uint8_t cut;    /* filter cutoff, as a MIDI note at middle C (tracks the key by half); 0 = no filter */
    uint8_t res;    /* resonance, % */
    int8_t fenv;    /* filter envelope depth, semitones */
    uint8_t fdec;   /* filter envelope decay (code) */
    uint8_t a, d, s, r;   /* amplitude: attack, decay (codes), sustain %, release (code) */
    uint8_t vib;    /* vibrato depth, cents */
    uint8_t trem;   /* tremolo depth, % */
    uint8_t trate;  /* tremolo rate, Hz x10 */
    int8_t pe;      /* pitch envelope: start offset, semitones (decays to 0) */
    uint8_t pdec;   /* ... in this time (code) */
    uint8_t lvl;    /* level, % */
    uint8_t flags;  /* F_* */
} gm_patch_t;

#define FM WV_FM
#define SAW WV_SAW
#define PUL WV_PULSE
#define NOI WV_NOISE
/* wave oct rc rm idx isus idec fb det iv o2 cut res fenv fdec  a  d   s   r  vib trem trate pe pdec lvl flags */
static const gm_patch_t GM_PATCH[128] = {
    /* Piano */
    {FM, 0, 2, 2, 22, 20, 75, 10, 4, 0, 60, 0, 0, 0, 0, 0, 120, 0, 72, 0, 0, 0, 0, 0, 85, F_KS},
    {FM, 0, 2, 2, 30, 30, 75, 20, 4, 0, 60, 0, 0, 0, 0, 0, 118, 0, 70, 0, 0, 0, 0, 0, 85, F_KS},
    {FM, 0, 2, 2, 26, 25, 70, 15, 7, 0, 80, 0, 0, 0, 0, 0, 118, 0, 70, 0, 0, 0, 0, 0, 80, F_KS},
    {FM, 0, 2, 2, 22, 20, 75, 10, 18, 0, 100, 0, 0, 0, 0, 0, 115, 0, 70, 0, 0, 0, 0, 0, 75, F_KS},
    {FM, 0, 2, 2, 16, 15, 72, 0, 3, 0, 50, 0, 0, 0, 0, 0, 115, 0, 75, 0, 0, 0, 0, 0, 90, F_KS},
    {FM, 0, 2, 2, 25, 5, 60, 25, 5, 12, 25, 0, 0, 0, 0, 0, 112, 0, 75, 0, 0, 0, 0, 0, 85, F_KS},
    {FM, 0, 2, 2, 25, 40, 80, 70, 0, 0, 0, 0, 0, 0, 0, 0, 105, 0, 50, 0, 0, 0, 0, 0, 70, F_KS},
    {PUL, 0, 2, 25, 0, 0, 0, 0, 0, 0, 0, 70, 30, 36, 70, 0, 95, 0, 50, 0, 0, 0, 0, 0, 75, F_KS},
    /* Chromatic percussion */
    {FM, 1, 2, 8, 10, 0, 60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 105, 0, 80, 0, 0, 0, 0, 0, 80, F_KS},
    {FM, 1, 2, 7, 15, 0, 55, 0, 0, 0, 0, 0, 0, 0, 0, 0, 110, 0, 90, 0, 0, 0, 0, 0, 75, 0},
    {FM, 1, 2, 8, 12, 0, 60, 0, 6, 0, 40, 0, 0, 0, 0, 0, 100, 0, 85, 0, 0, 0, 0, 0, 80, F_KS},
    {FM, 0, 2, 8, 8, 10, 70, 0, 0, 0, 0, 0, 0, 0, 0, 0, 115, 0, 90, 0, 30, 55, 0, 0, 85, 0},
    {FM, 0, 2, 8, 14, 0, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 85, 0, 60, 0, 0, 0, 0, 0, 95, F_KS},
    {FM, 1, 2, 7, 18, 0, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 75, 0, 55, 0, 0, 0, 0, 0, 85, F_KS},
    {FM, 0, 2, 7, 28, 10, 90, 0, 3, 0, 50, 0, 0, 0, 0, 0, 125, 0, 110, 0, 0, 0, 0, 0, 70, 0},
    {FM, 0, 2, 2, 20, 10, 70, 30, 8, 0, 80, 0, 0, 0, 0, 0, 105, 0, 70, 0, 0, 0, 0, 0, 75, F_KS},
    /* Organ */
    {FM, 0, 2, 2, 6, 100, 0, 0, 0, 12, 60, 0, 0, 0, 0, 2, 0, 100, 50, 8, 0, 0, 0, 0, 70, F_VIBNOW},
    {FM, 0, 2, 2, 14, 40, 60, 0, 0, 19, 50, 0, 0, 0, 0, 2, 0, 100, 50, 0, 0, 0, 0, 0, 70, 0},
    {FM, 0, 2, 2, 10, 100, 0, 50, 0, 12, 70, 0, 0, 0, 0, 2, 0, 100, 45, 10, 0, 0, 0, 0, 65, F_VIBNOW},
    {FM, 0, 2, 2, 12, 100, 0, 30, 4, 12, 80, 0, 0, 0, 0, 60, 0, 100, 85, 0, 0, 0, 0, 0, 65, 0},
    {PUL, 0, 2, 40, 0, 0, 0, 0, 5, 0, 60, 80, 0, 0, 0, 50, 0, 100, 60, 0, 0, 0, 0, 0, 65, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 12, 0, 100, 85, 10, 0, 0, 40, 0, 100, 55, 0, 15, 60, 0, 0, 60, 0},
    {PUL, 0, 2, 30, 0, 0, 0, 0, 0, 0, 0, 75, 20, 12, 70, 40, 0, 100, 50, 15, 0, 0, 0, 0, 70, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 6, 0, 100, 80, 10, 0, 0, 40, 0, 100, 55, 0, 0, 0, 0, 0, 60, 0},
    /* Guitar */
    {FM, 0, 2, 2, 14, 5, 55, 10, 0, 0, 0, 0, 0, 0, 0, 0, 105, 0, 60, 0, 0, 0, 0, 0, 90, F_KS},
    {FM, 0, 2, 2, 22, 8, 60, 25, 5, 0, 60, 0, 0, 0, 0, 0, 108, 0, 60, 0, 0, 0, 0, 0, 80, F_KS},
    {FM, 0, 2, 2, 10, 20, 70, 0, 0, 0, 0, 0, 0, 0, 0, 0, 100, 10, 60, 0, 0, 0, 0, 0, 90, F_KS},
    {FM, 0, 2, 2, 16, 20, 70, 20, 0, 0, 0, 0, 0, 0, 0, 0, 110, 0, 60, 0, 0, 0, 0, 0, 85, F_KS},
    {FM, 0, 2, 2, 14, 0, 35, 0, 0, 0, 0, 0, 0, 0, 0, 0, 60, 0, 40, 0, 0, 0, 0, 0, 95, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 6, 0, 100, 85, 20, 12, 90, 2, 115, 40, 60, 6, 0, 0, 0, 0, 55, F_DRIVE},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 10, 0, 100, 90, 25, 0, 0, 2, 115, 60, 60, 6, 0, 0, 0, 0, 50, F_DRIVE},
    {FM, 1, 2, 4, 4, 0, 70, 0, 0, 0, 0, 0, 0, 0, 0, 0, 105, 0, 70, 0, 0, 0, 0, 0, 80, 0},
    /* Bass */
    {FM, 0, 2, 2, 10, 0, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 95, 0, 50, 0, 0, 0, 0, 0, 100, F_KS},
    {FM, 0, 2, 2, 14, 10, 60, 15, 0, 0, 0, 0, 0, 0, 0, 0, 105, 0, 45, 0, 0, 0, 0, 0, 100, 0},
    {FM, 0, 2, 2, 20, 15, 50, 35, 0, 0, 0, 0, 0, 0, 0, 0, 100, 0, 45, 0, 0, 0, 0, 0, 90, 0},
    {FM, 0, 2, 2, 8, 40, 80, 0, 0, 0, 0, 0, 0, 0, 0, 10, 110, 30, 50, 10, 0, 0, 0, 0, 100, 0},
    {FM, 0, 2, 2, 30, 5, 35, 40, 0, 0, 0, 0, 0, 0, 0, 0, 90, 0, 45, 0, 0, 0, 0, 0, 85, 0},
    {FM, 0, 2, 4, 30, 5, 30, 30, 0, 0, 0, 0, 0, 0, 0, 0, 90, 0, 45, 0, 0, 0, 0, 0, 85, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 60, 50, 36, 75, 0, 100, 40, 45, 0, 0, 0, 0, 0, 90, 0},
    {PUL, 0, 2, 50, 0, 0, 0, 0, 8, 0, 100, 55, 40, 30, 70, 0, 95, 50, 45, 0, 0, 0, 0, 0, 80, 0},
    /* Strings */
    {SAW, 0, 2, 0, 0, 0, 0, 0, 5, 0, 60, 88, 10, 0, 0, 65, 0, 100, 75, 18, 0, 0, 0, 0, 70, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 5, 0, 60, 82, 10, 0, 0, 65, 0, 100, 75, 15, 0, 0, 0, 0, 70, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 5, 0, 60, 75, 10, 0, 0, 68, 0, 100, 75, 14, 0, 0, 0, 0, 75, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 5, 0, 60, 66, 10, 0, 0, 70, 0, 100, 75, 10, 0, 0, 0, 0, 80, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 8, 0, 80, 82, 10, 0, 0, 60, 0, 100, 75, 0, 70, 80, 0, 0, 70, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 70, 10, 24, 55, 0, 75, 0, 50, 0, 0, 0, 0, 0, 90, F_KS},
    {FM, 0, 2, 2, 10, 0, 60, 5, 0, 0, 0, 0, 0, 0, 0, 0, 110, 0, 90, 0, 0, 0, 0, 0, 90, F_KS},
    {FM, 0, 2, 3, 10, 0, 70, 0, 0, 0, 0, 0, 0, 0, 0, 0, 105, 0, 90, 0, 0, 0, 2, 60, 100, 0},
    /* Ensemble */
    {SAW, 0, 2, 0, 0, 0, 0, 0, 14, 0, 100, 85, 5, 0, 0, 70, 0, 100, 90, 6, 0, 0, 0, 0, 60, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 14, 0, 100, 80, 5, 0, 0, 90, 0, 100, 100, 6, 0, 0, 0, 0, 60, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 16, 0, 100, 90, 15, 12, 95, 75, 0, 100, 90, 0, 0, 0, 0, 0, 55, 0},
    {PUL, 0, 2, 45, 0, 0, 0, 0, 14, 0, 100, 80, 10, 0, 0, 85, 0, 100, 95, 0, 0, 0, 0, 0, 55, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 10, 0, 100, 72, 40, 0, 0, 85, 0, 100, 95, 12, 0, 0, 0, 0, 60, 0},
    {FM, 0, 2, 2, 4, 100, 0, 0, 8, 0, 80, 0, 0, 0, 0, 80, 0, 100, 90, 14, 0, 0, 0, 0, 75, 0},
    {PUL, 0, 2, 50, 0, 0, 0, 0, 10, 0, 100, 76, 45, 0, 0, 75, 0, 100, 90, 10, 0, 0, 0, 0, 55, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 25, 12, 100, 100, 0, -24, 70, 0, 85, 0, 70, 0, 0, 0, 0, 0, 70, 0},
    /* Brass */
    {FM, 0, 2, 2, 22, 120, 70, 20, 0, 0, 0, 0, 0, 0, 0, 45, 0, 90, 55, 12, 0, 0, 0, 0, 75, F_IXAMP},
    {FM, 0, 2, 2, 16, 110, 75, 10, 0, 0, 0, 0, 0, 0, 0, 55, 0, 90, 60, 10, 0, 0, 0, 0, 80, F_IXAMP},
    {FM, 0, 2, 2, 10, 110, 75, 0, 0, 0, 0, 0, 0, 0, 0, 55, 0, 90, 60, 6, 0, 0, 0, 0, 90, F_IXAMP},
    {FM, 0, 2, 2, 14, 100, 0, 40, 0, 0, 0, 86, 50, 0, 0, 40, 0, 85, 55, 10, 0, 0, 0, 0, 75, 0},
    {FM, 0, 2, 2, 8, 120, 80, 0, 0, 0, 0, 76, 0, 0, 0, 65, 0, 95, 75, 8, 0, 0, 0, 0, 80, F_IXAMP},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 10, 0, 100, 76, 10, 18, 80, 50, 90, 85, 60, 0, 0, 0, 0, 0, 60, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 12, 0, 100, 70, 30, 36, 85, 30, 95, 70, 60, 0, 0, 0, 0, 0, 60, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 18, 0, 100, 64, 20, 30, 100, 60, 0, 80, 75, 0, 0, 0, 0, 0, 60, 0},
    /* Reed */
    {PUL, 0, 2, 30, 0, 0, 0, 0, 0, 0, 0, 92, 25, 12, 70, 40, 0, 90, 50, 16, 0, 0, 0, 0, 70, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 84, 30, 15, 75, 42, 0, 90, 50, 14, 0, 0, 0, 0, 70, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 78, 30, 15, 75, 45, 0, 90, 50, 14, 0, 0, 0, 0, 60, F_DRIVE},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 70, 25, 12, 75, 45, 0, 90, 50, 10, 0, 0, 0, 0, 75, 0},
    {PUL, 0, 2, 15, 0, 0, 0, 0, 0, 0, 0, 95, 35, 0, 0, 45, 0, 100, 50, 15, 0, 0, 0, 0, 75, 0},
    {PUL, 0, 2, 20, 0, 0, 0, 0, 0, 0, 0, 86, 30, 0, 0, 50, 0, 100, 50, 12, 0, 0, 0, 0, 75, 0},
    {PUL, 0, 2, 25, 0, 0, 0, 0, 0, 0, 0, 70, 30, 0, 0, 50, 0, 100, 50, 8, 0, 0, 0, 0, 80, 0},
    {PUL, 0, 2, 50, 0, 0, 0, 0, 0, 0, 0, 84, 10, 0, 0, 40, 0, 100, 50, 8, 0, 0, 0, 0, 70, 0},
    /* Pipe */
    {FM, 1, 2, 2, 3, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 50, 0, 100, 50, 14, 0, 0, 0, 0, 80, F_BREATH},
    {FM, 0, 2, 2, 4, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 50, 0, 100, 50, 14, 0, 0, 0, 0, 85, F_BREATH},
    {FM, 0, 2, 2, 6, 100, 0, 10, 0, 0, 0, 0, 0, 0, 0, 40, 0, 100, 50, 6, 0, 0, 0, 0, 85, F_BREATH},
    {FM, 0, 2, 2, 2, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 60, 0, 100, 55, 12, 0, 0, 0, 0, 85, F_BREATH},
    {FM, 0, 2, 2, 1, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 70, 0, 100, 60, 6, 0, 0, 0, 0, 85, F_BREATH},
    {FM, 0, 2, 2, 6, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 65, 0, 100, 60, 20, 0, 0, 0, 0, 80, F_BREATH},
    {FM, 1, 2, 2, 0, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 40, 0, 100, 50, 20, 0, 0, 0, 0, 70, F_VIBNOW},
    {FM, 0, 2, 2, 1, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 45, 0, 100, 50, 10, 0, 0, 0, 0, 85, 0},
    /* Synth lead */
    {PUL, 0, 2, 50, 0, 0, 0, 0, 0, 0, 0, 110, 0, 0, 0, 0, 0, 100, 45, 10, 0, 0, 0, 0, 55, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 8, 0, 100, 105, 15, 0, 0, 0, 0, 100, 45, 10, 0, 0, 0, 0, 50, 0},
    {PUL, 0, 2, 30, 0, 0, 0, 0, 0, 12, 40, 0, 0, 0, 0, 30, 0, 100, 50, 15, 0, 0, 0, 0, 55, F_BREATH},
    {FM, 0, 2, 2, 6, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 20, 0, 100, 50, 8, 0, 0, 0, 0, 80, F_BREATH},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 10, 0, 100, 95, 30, 24, 80, 0, 100, 60, 50, 6, 0, 0, 0, 0, 45, F_DRIVE},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 6, 0, 100, 80, 50, 0, 0, 30, 0, 100, 50, 15, 0, 0, 0, 0, 55, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 0, 7, 100, 100, 10, 0, 0, 0, 0, 100, 45, 0, 0, 0, 0, 0, 45, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 0, -12, 100, 90, 25, 24, 85, 0, 100, 70, 45, 0, 0, 0, 0, 0, 45, 0},
    /* Synth pad */
    {FM, 0, 2, 6, 20, 20, 100, 0, 8, 0, 80, 0, 0, 0, 0, 80, 115, 70, 105, 0, 0, 0, 0, 0, 65, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 12, 0, 100, 64, 10, 0, 0, 95, 0, 100, 105, 0, 0, 0, 0, 0, 70, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 14, 0, 100, 78, 20, 24, 90, 20, 100, 70, 80, 0, 0, 0, 0, 0, 55, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 12, 0, 100, 70, 40, 0, 0, 95, 0, 100, 105, 8, 0, 0, 0, 0, 60, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 8, 0, 100, 72, 30, 12, 100, 100, 0, 100, 105, 0, 0, 0, 0, 0, 60, 0},
    {FM, 0, 2, 7, 18, 60, 110, 0, 10, 0, 80, 0, 0, 0, 0, 90, 0, 100, 105, 0, 0, 0, 0, 0, 60, 0},
    {FM, 0, 2, 2, 8, 100, 0, 0, 12, 0, 100, 0, 0, 0, 0, 100, 0, 100, 110, 6, 0, 0, 0, 0, 65, F_BREATH},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 14, 0, 100, 55, 50, 48, 125, 100, 0, 100, 110, 0, 0, 0, 0, 0, 60, 0},
    /* Synth effects */
    {FM, 1, 2, 9, 20, 0, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 95, 0, 90, 0, 0, 0, 0, 0, 65, F_BREATH},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 12, 0, 100, 70, 20, 24, 115, 95, 0, 100, 110, 0, 0, 0, 0, 0, 60, 0},
    {FM, 0, 2, 9, 14, 10, 80, 0, 6, 12, 50, 0, 0, 0, 0, 0, 115, 20, 105, 0, 0, 0, 0, 0, 70, 0},
    {FM, 0, 2, 2, 18, 30, 90, 20, 8, 0, 80, 0, 0, 0, 0, 50, 115, 50, 100, 0, 0, 0, 0, 0, 65, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 10, 12, 50, 110, 10, 0, 0, 70, 110, 70, 100, 0, 0, 0, 0, 0, 50, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 20, 0, 100, 60, 70, 24, 120, 110, 0, 100, 100, 0, 40, 30, 0, 0, 55, 0},
    {FM, 0, 2, 4, 12, 30, 90, 0, 0, 0, 0, 0, 0, 0, 0, 60, 110, 40, 115, 0, 30, 25, 0, 0, 65, 0},
    {FM, 0, 2, 13, 30, 50, 110, 0, 0, 0, 0, 0, 0, 0, 0, 70, 0, 100, 100, 60, 0, 0, 0, 0, 55, F_VIBNOW},
    /* Ethnic */
    {FM, 0, 2, 2, 22, 30, 85, 70, 0, 0, 0, 0, 0, 0, 0, 0, 112, 0, 80, 0, 0, 0, 0, 0, 70, F_KS},
    {FM, 0, 2, 2, 26, 0, 45, 40, 0, 0, 0, 0, 0, 0, 0, 0, 85, 0, 50, 0, 0, 0, 0, 0, 75, F_KS},
    {FM, 0, 2, 2, 24, 0, 40, 60, 0, 0, 0, 0, 0, 0, 0, 0, 85, 0, 50, 0, 0, 0, 0, 0, 70, F_KS},
    {FM, 0, 2, 6, 16, 5, 60, 20, 0, 0, 0, 0, 0, 0, 0, 0, 100, 0, 60, 0, 0, 0, 0, 0, 80, F_KS},
    {FM, 0, 2, 8, 8, 0, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 90, 0, 60, 0, 0, 0, 0, 0, 90, F_KS},
    {PUL, 0, 2, 20, 0, 0, 0, 0, 4, 0, 50, 100, 40, 0, 0, 30, 0, 100, 50, 0, 0, 0, 0, 0, 55, 0},
    {SAW, 0, 2, 0, 0, 0, 0, 0, 4, 0, 40, 92, 20, 0, 0, 50, 0, 100, 60, 20, 0, 0, 0, 0, 65, 0},
    {PUL, 0, 2, 18, 0, 0, 0, 0, 0, 0, 0, 100, 40, 0, 0, 40, 0, 100, 50, 18, 0, 0, 0, 0, 60, 0},
    /* Percussive */
    {FM, 1, 2, 9, 16, 0, 60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 105, 0, 90, 0, 0, 0, 0, 0, 70, 0},
    {FM, 0, 2, 5, 12, 0, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 80, 0, 60, 0, 0, 0, 0, 0, 80, 0},
    {FM, 0, 2, 7, 14, 10, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 95, 0, 70, 0, 0, 0, 1, 50, 80, F_KS},
    {FM, 1, 2, 7, 10, 0, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 50, 0, 30, 0, 0, 0, 0, 0, 90, 0},
    {FM, -1, 2, 3, 12, 0, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 85, 0, 80, 0, 0, 0, 12, 50, 100, 0},
    {FM, 0, 2, 2, 4, 0, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 85, 0, 70, 0, 0, 0, 12, 55, 100, 0},
    {FM, 0, 2, 2, 1, 0, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 80, 0, 70, 0, 0, 0, 24, 60, 100, 0},
    {NOI, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 100, 20, 0, 0, 115, 0, 100, 30, 0, 0, 0, 0, 0, 55, F_HP},
    /* Sound effects */
    {NOI, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 90, 60, 0, 0, 10, 60, 0, 40, 0, 0, 0, 0, 0, 60, F_BP},
    {NOI, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 80, 30, 0, 0, 50, 0, 100, 60, 0, 0, 0, 0, 0, 50, F_BP},
    {NOI, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 90, 0, 0, 0, 105, 0, 100, 110, 0, 80, 2, 0, 0, 70, 0},
    {FM, 2, 2, 2, 1, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 20, 0, 100, 50, 0, 90, 150, 7, 40, 60, 0},
    {FM, 1, 2, 4, 6, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 100, 40, 0, 100, 200, 0, 0, 55, 0},
    {NOI, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 60, 50, 0, 0, 60, 0, 100, 80, 0, 100, 120, 0, 0, 80, 0},
    {NOI, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 95, 20, 0, 0, 100, 0, 100, 100, 0, 60, 90, 0, 0, 60, F_BP},
    {NOI, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 100, 0, -40, 60, 0, 80, 0, 80, 0, 0, 0, 0, 0, 100, 0},
};
#undef FM
#undef SAW
#undef PUL
#undef NOI

static const char *const GM_NAME[128] = {
    "Acoustic Grand", "Bright Piano", "Electric Grand", "Honky-tonk", "E.Piano 1", "E.Piano 2", "Harpsichord",
    "Clavi",
    "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Perc. Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica",
    "Tango Accordion",
    "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar", "Muted Guitar", "Overdrive Gtr",
    "Distortion Gtr", "Gtr Harmonics",
    "Acoustic Bass", "Finger Bass", "Pick Bass", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1",
    "Synth Bass 2",
    "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato", "Harp", "Timpani",
    "Strings 1", "Strings 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice",
    "Orchestra Hit",
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1",
    "Synth Brass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Square Lead", "Saw Lead", "Calliope Lead", "Chiff Lead", "Charang Lead", "Voice Lead", "Fifths Lead",
    "Bass + Lead",
    "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad", "Bowed Pad", "Metallic Pad", "Halo Pad",
    "Sweep Pad",
    "Rain", "Soundtrack", "Crystal", "Atmosphere", "Brightness", "Goblins", "Echoes", "Sci-fi",
    "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum",
    "Reverse Cymbal",
    "Gtr Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone", "Helicopter", "Applause",
    "Gunshot",
};

static const char *const GM_FAMILY[16] = {
    "Piano", "Chromatic Perc", "Organ", "Guitar", "Bass", "Strings", "Ensemble", "Brass",
    "Reed", "Pipe", "Synth Lead", "Synth Pad", "Synth FX", "Ethnic", "Percussive", "Sound FX",
};

/* ---------------------------------------------------------------------------- drums --- */
enum { T_NONE, T_SINE, T_METAL, T_SQ2 };       /* the tone: none, a swept sine, six squares, two squares */
enum { N_NONE, N_LP, N_BP, N_HP };             /* the noise (and a metal / square tone's) filter */
enum {
    D_CLAP = 1,     /* the noise in three quick bursts, then the tail */
    D_RATTLE = 2,   /* the noise chopped at ~35 Hz (guiro, vibraslap) */
    D_WOBBLE = 4,   /* the tone warbles (whistles) */
    D_FTONE = 8,    /* the tone goes through the filter too */
};
typedef struct {
    uint8_t tone;           /* T_* */
    uint16_t f0, f1;        /* tone pitch: start, end (Hz) */
    uint8_t pdec;           /* pitch sweep time (code) */
    uint8_t tdec, tlvl;     /* tone decay (code), level % */
    uint8_t nmode;          /* N_* */
    uint16_t nf;            /* filter frequency, Hz */
    uint8_t nq;             /* filter Q x10 */
    uint8_t ndec, nlvl;     /* noise decay (code), level % */
    int8_t pan;             /* -64 .. 63 */
    uint8_t choke;          /* a hit cuts the others of its group: 1 hi-hats, 2 triangles, 3 whistles, 4 guiros,
                             * 5 cuicas */
    uint8_t flags;          /* D_* */
} gm_drum_t;

#define S T_SINE
#define M T_METAL
#define Q T_SQ2
/*  tone  f0    f1  pdec tdec tlvl nmode  nf  nq ndec nlvl pan choke flags */
static const gm_drum_t GM_DRUM[GM_DRUM_HI - GM_DRUM_LO + 1] = {
    {S, 140, 48, 45, 85, 100, N_LP, 2000, 7, 30, 15, 0, 0, 0},           /* 35 Acoustic Bass Drum */
    {S, 170, 52, 42, 80, 100, N_LP, 3000, 7, 25, 20, 0, 0, 0},           /* 36 Bass Drum 1 */
    {S, 1800, 1700, 30, 35, 60, N_BP, 2500, 20, 32, 50, 0, 0, 0},        /* 37 Side Stick */
    {S, 330, 185, 35, 55, 55, N_BP, 4500, 5, 72, 80, 0, 0, 0},           /* 38 Acoustic Snare */
    {T_NONE, 0, 0, 0, 0, 0, N_BP, 1200, 15, 70, 90, 0, 0, D_CLAP},       /* 39 Hand Clap */
    {S, 280, 170, 35, 50, 50, N_BP, 6000, 4, 66, 85, 0, 0, 0},           /* 40 Electric Snare */
    {S, 110, 78, 60, 88, 90, N_LP, 800, 7, 30, 10, -40, 0, 0},           /* 41 Low Floor Tom */
    {M, 400, 400, 0, 45, 35, N_HP, 8000, 7, 45, 60, 25, 1, D_FTONE},     /* 42 Closed Hi-Hat */
    {S, 130, 92, 60, 88, 90, N_LP, 900, 7, 30, 10, -30, 0, 0},           /* 43 High Floor Tom */
    {M, 400, 400, 0, 40, 30, N_HP, 7000, 7, 40, 45, 25, 1, D_FTONE},     /* 44 Pedal Hi-Hat */
    {S, 150, 108, 60, 86, 90, N_LP, 1000, 7, 30, 10, -15, 0, 0},         /* 45 Low Tom */
    {M, 400, 400, 0, 95, 35, N_HP, 7500, 7, 95, 60, 25, 1, D_FTONE},     /* 46 Open Hi-Hat */
    {S, 175, 125, 60, 85, 90, N_LP, 1100, 7, 30, 10, -5, 0, 0},          /* 47 Low-Mid Tom */
    {S, 200, 145, 60, 84, 90, N_LP, 1200, 7, 30, 10, 10, 0, 0},          /* 48 Hi-Mid Tom */
    {M, 450, 450, 0, 120, 30, N_HP, 5000, 5, 122, 70, -30, 0, D_FTONE},  /* 49 Crash Cymbal 1 */
    {S, 230, 170, 60, 82, 90, N_LP, 1300, 7, 30, 10, 25, 0, 0},          /* 50 High Tom */
    {M, 500, 500, 0, 118, 40, N_HP, 9000, 7, 110, 30, 30, 0, D_FTONE},   /* 51 Ride Cymbal 1 */
    {M, 350, 350, 0, 115, 40, N_BP, 3500, 10, 115, 70, -35, 0, D_FTONE}, /* 52 Chinese Cymbal */
    {M, 700, 700, 0, 115, 55, N_HP, 2000, 7, 90, 15, 30, 0, D_FTONE},    /* 53 Ride Bell */
    {M, 900, 900, 0, 60, 20, N_HP, 7000, 10, 70, 70, 35, 0, D_FTONE},    /* 54 Tambourine */
    {M, 550, 550, 0, 100, 30, N_HP, 6000, 7, 100, 70, 20, 0, D_FTONE},   /* 55 Splash Cymbal */
    {Q, 560, 560, 0, 70, 70, N_BP, 800, 30, 0, 0, -10, 0, D_FTONE},      /* 56 Cowbell */
    {M, 420, 420, 0, 122, 30, N_HP, 4500, 5, 125, 75, 30, 0, D_FTONE},   /* 57 Crash Cymbal 2 */
    {T_NONE, 0, 0, 0, 0, 0, N_BP, 3000, 30, 100, 60, 0, 0, D_RATTLE},    /* 58 Vibraslap */
    {M, 470, 470, 0, 116, 40, N_HP, 8000, 7, 105, 35, -25, 0, D_FTONE},  /* 59 Ride Cymbal 2 */
    {S, 420, 400, 30, 55, 80, N_BP, 2000, 7, 25, 15, 20, 0, 0},          /* 60 Hi Bongo */
    {S, 300, 285, 30, 60, 80, N_BP, 1800, 7, 25, 15, 15, 0, 0},          /* 61 Low Bongo */
    {S, 340, 320, 30, 40, 80, N_BP, 1800, 7, 25, 20, -15, 0, 0},         /* 62 Mute Hi Conga */
    {S, 330, 318, 30, 70, 80, N_BP, 1800, 7, 25, 15, -15, 0, 0},         /* 63 Open Hi Conga */
    {S, 230, 220, 30, 75, 80, N_BP, 1500, 7, 25, 15, -20, 0, 0},         /* 64 Low Conga */
    {S, 520, 480, 30, 70, 70, N_BP, 3000, 7, 55, 30, 30, 0, 0},          /* 65 High Timbale */
    {S, 380, 350, 30, 72, 70, N_BP, 2600, 7, 55, 30, 30, 0, 0},          /* 66 Low Timbale */
    {Q, 920, 920, 0, 72, 70, N_BP, 1800, 20, 0, 0, -25, 0, D_FTONE},     /* 67 High Agogo */
    {Q, 650, 650, 0, 72, 70, N_BP, 1300, 20, 0, 0, -25, 0, D_FTONE},     /* 68 Low Agogo */
    {T_NONE, 0, 0, 0, 0, 0, N_HP, 9000, 5, 50, 60, 25, 0, 0},            /* 69 Cabasa */
    {T_NONE, 0, 0, 0, 0, 0, N_HP, 10000, 7, 40, 60, -25, 0, 0},          /* 70 Maracas */
    {S, 2400, 2400, 0, 55, 50, N_NONE, 0, 0, 0, 0, 40, 3, D_WOBBLE},     /* 71 Short Whistle */
    {S, 2100, 2100, 0, 100, 50, N_NONE, 0, 0, 0, 0, 40, 3, D_WOBBLE},    /* 72 Long Whistle */
    {T_NONE, 0, 0, 0, 0, 0, N_BP, 3500, 20, 55, 70, -30, 4, D_RATTLE},   /* 73 Short Guiro */
    {T_NONE, 0, 0, 0, 0, 0, N_BP, 3000, 20, 90, 70, -30, 4, D_RATTLE},   /* 74 Long Guiro */
    {S, 2500, 2500, 0, 35, 80, N_NONE, 0, 0, 0, 0, 20, 0, 0},            /* 75 Claves */
    {S, 1600, 1500, 20, 35, 80, N_BP, 1600, 7, 20, 20, 25, 0, 0},        /* 76 Hi Wood Block */
    {S, 1150, 1080, 20, 38, 80, N_BP, 1200, 7, 20, 20, 25, 0, 0},        /* 77 Low Wood Block */
    {S, 700, 500, 40, 45, 70, N_NONE, 0, 0, 0, 0, -20, 5, 0},            /* 78 Mute Cuica */
    {S, 400, 600, 60, 70, 70, N_NONE, 0, 0, 0, 0, -20, 5, 0},            /* 79 Open Cuica */
    {S, 4500, 4500, 0, 50, 60, N_HP, 9000, 7, 30, 10, -30, 2, 0},        /* 80 Mute Triangle */
    {S, 4500, 4500, 0, 110, 60, N_HP, 9000, 7, 40, 10, -30, 2, 0},       /* 81 Open Triangle */
};
#undef S
#undef M
#undef Q

static const char *const GM_DRUM_NAME[GM_DRUM_HI - GM_DRUM_LO + 1] = {
    "Kick 2", "Kick 1", "Side Stick", "Snare 1", "Hand Clap", "Snare 2", "Low Floor Tom", "Closed Hat",
    "High Floor Tom", "Pedal Hat", "Low Tom", "Open Hat", "Low-Mid Tom", "Hi-Mid Tom", "Crash 1", "High Tom",
    "Ride 1", "China", "Ride Bell", "Tambourine", "Splash", "Cowbell", "Crash 2", "Vibraslap", "Ride 2",
    "Hi Bongo", "Low Bongo", "Mute Conga", "Open Conga", "Low Conga", "Hi Timbale", "Low Timbale",
    "Hi Agogo", "Low Agogo", "Cabasa", "Maracas", "Short Whistle", "Long Whistle", "Short Guiro",
    "Long Guiro", "Claves", "Hi Woodblock", "Low Woodblock", "Mute Cuica", "Open Cuica", "Mute Triangle",
    "Open Triangle",
};
