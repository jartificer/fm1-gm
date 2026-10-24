/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM sound engine: a 16-channel General MIDI synthesizer.
 *
 *   128 melodic programs (GM Level 1 order and names, gm_patches.h), each a small voice recipe:
 *   2-operator FM, two detuned saws, a pulse or noise, through a state-variable filter, with
 *   amp / filter / FM-index / pitch envelopes, vibrato and tremolo. The sounds are fixed (not
 *   editable on the device).
 *
 *   One GM drum kit (notes 35..81, gm_patches.h) on channel 10, synthesized: tone + noise voices.
 *
 *   GM_NVOICE melodic voices and GM_NDRUM drum voices; per channel: program, volume (CC7),
 *   expression (CC11), pan (CC10), modulation (CC1, vibrato), sustain (CC64), reverb send (CC91),
 *   pitch bend with RPN 0 bend range, all sound / notes off, reset all controllers.
 *   One reverb on the mix.
 *
 * Threads: everything here runs in the audio interrupt (seq.c calls it). Floats only, no libm. */
#pragma once
#include <stdint.h>

#define GM_SR 44100.0f
/* GM Level 1 asks for 24 voices, or 16 melodic + 8 percussion: the default is the latter
 * (gm_voice_max, the POLYPHONY setting, takes the melodic voices up to GM_NVOICE) */
#define GM_NVOICE 24
#define GM_NVOICE_DEF 16
#define GM_NDRUM 8
#define GM_NCH 16
#define GM_DRUM_CH 9                   /* channel 10 */
#define GM_CHUNK 32                    /* frames per gm_render call, at most */
#define GM_DRUM_LO 35
#define GM_DRUM_HI 81
#define GM_NOTE_MAX 120                /* C9 (middle C is C4, 60): higher notes are ignored */

/* big zero-initialised buffers go in the .pool RAM region on the device (app.ld) */
#ifdef OM_HOST
#define GM_POOL
#else
#define GM_POOL __attribute__((section(".pool")))
#endif

void gm_init(void);
void gm_reset(void);                   /* GM reset: every channel to its defaults, everything quiet */
void gm_panic(void);                   /* everything quiet, now (the controllers stay) */
void gm_midi(uint32_t st, uint32_t d1, uint32_t d2);   /* one channel message */
void gm_shed(void);                    /* the render ran late: the quietest voice fades out fast */
int gm_load(uint32_t pct);              /* the last render's share of its time (%): under load the voices
                                         * interpolate linearly for a while; 1 if that just started */
/* add n (<= GM_CHUNK) frames of the mix to l / r; reverb included */
void gm_render(float *l, float *r, uint32_t n);

const char *gm_prog_name(int prog);    /* "Acoustic Grand", 0..127 */
const char *gm_drum_name(int note);    /* "Kick 1", or "" outside 35..81 */
const char *gm_family_name(int prog);  /* "Piano", "Organ", ... */

/* state for the screen (written by the render, only read elsewhere) */
extern volatile float gm_ch_level[GM_NCH];     /* loudness of what each channel plays, 0..~1 */
extern volatile uint8_t gm_voices_used, gm_drums_used;
extern volatile uint8_t gm_ch_prog[GM_NCH];
extern volatile uint8_t gm_voice_max;          /* melodic voices in use, 8..GM_NVOICE (the main loop sets it) */
extern volatile uint32_t gm_resets;            /* times the safety net found a NaN / infinity / absurd level and went quiet */
#ifdef OM_HOST
void gm_test_poison(int what);                 /* tests only: a NaN in a voice (0) or the reverb (1); 3e32 in a voice (2) */
#endif
