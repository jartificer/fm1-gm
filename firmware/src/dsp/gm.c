/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM sound engine (gm.h). One compilation unit, -O2 (tools/build.py O2_UNITS).
 *
 * A sample player. Each note plays one zone of the wavetable (gm_wavetable.h, generated from the
 * SoundFont assets/soundfont/fm1gm.sf2): its 4-bit ADPCM sample, decoded as it plays and
 * interpolated by an 8-point windowed sinc (linear under load) (above 1.5 points a frame, averaged: less aliasing),
 * its loop replaying exactly, through the zone's volume envelope (attack, hold, decay, sustain, release) and low-pass
 * filter with its modulation envelope. One sample covers the whole keyboard; the pitch stays put
 * (no built-in vibrato: the mod wheel adds it; a zone's tremolo where the SoundFont has one).
 *
 * The render works in chunks of up to GM_CHUNK frames: per chunk each voice works out its pitch,
 * envelopes and filter coefficients once, then runs a tight per-sample loop with the amplitude
 * ramped linearly across the chunk. */
#include "gm.h"
#include "fastmath.h"
#include "gm_names.h"
#include "gm_wave.h"
#include "gm_wavetable.h"

volatile float gm_ch_level[GM_NCH];
volatile uint8_t gm_voices_used, gm_drums_used;
volatile uint8_t gm_ch_prog[GM_NCH];
volatile uint8_t gm_voice_max = GM_NVOICE_DEF;
volatile uint32_t gm_resets;

#define INV_SR (1.0f / GM_SR)
#define MASTER (1.5f * 3.1623f)                 /* the mix level: about as loud as the synthesized engine was
                                                 * (x 10 dB: the SoundFont's zones are 10 dB down, for room to even them out) */

static inline float tsec(int32_t tc) { return fm_exp2f((float)tc * (1.0f / 1200.0f)); }   /* timecents -> s */
static inline float cents_hz(float c) { return 8.1757989f * fm_exp2f(c * (1.0f / 1200.0f)); }
/* per-chunk factor of a fall of 100 dB in t seconds (SoundFont decay and release) */
static inline float fall_k(float t, uint32_t n) { return fm_exp2f(-16.609640f * (float)n * INV_SR / t); }

/* ------------------------------------------------------------ channels --- */
typedef struct {
    uint8_t prog, vol, expr, pan, mod, sus, rev, bend_range;
    uint8_t rpn_msb, rpn_lsb;
    int16_t bend;                               /* -8192 .. 8191 */
    float lvl;                                  /* metering */
} chan_t;
static chan_t ch[GM_NCH];

static void chan_reset_ctl(chan_t *c)
{
    c->expr = 127;
    c->mod = 0;
    c->sus = 0;
    c->bend = 0;
    c->rpn_msb = c->rpn_lsb = 127;
}

static void chan_defaults(chan_t *c)
{
    c->prog = 0;
    c->vol = 100;
    c->pan = 64;
    c->rev = 40;
    c->bend_range = 2;
    chan_reset_ctl(c);
}

/* volume x expression, as GM recommends (40 log10 of each) */
static inline float chan_gain(const chan_t *c)
{
    float v = (float)c->vol * (1.0f / 127.0f), e = (float)c->expr * (1.0f / 127.0f);
    return v * v * e * e;
}

static void pan_gains(int32_t pan, float *l, float *r)  /* 0 .. 127, equal power */
{
    float p = (float)(pan < 0 ? 0 : pan > 127 ? 127 : pan) * (1.0f / 127.0f);
    *l = fm_sin_turns((1.0f - p) * 0.25f);
    *r = fm_sin_turns(p * 0.25f);
}

/* --------------------------------------------------------------- voices --- */
/* the interpolation: a windowed sinc over NTAP points (Kaiser, beta 5, cut off at the sample's own
 * Nyquist), its taps for NPH + 1 places between two points, interpolated between: flat to 0.3 of the
 * sample's rate, and the images of what lies below that 50-60 dB down (a cubic leaves them 20 dB
 * down, heard as a fizz over the sound) */
#define VEL_100 0.66966f                 /* the velocity curve's level at 100: (100/127) (0.3 + 0.7 x 100/127) */
#define TREM_DELAY 0.2f                         /* a zone's tremolo starts this long (s) into the note */
#define NTAP 8
#define NPH 32
static float sinc_tab[NPH + 1][NTAP];
/* under load (a render over LOAD_HI % of its time) every voice interpolates linearly, a third of the
 * work, until the renders have stayed under LOAD_LO % for CALM renders (3 s): heard as a little fizz,
 * not as a voice cut off (gm_shed stays for a render late even so) */
#define LOAD_HI 70u
#define LOAD_LO 45u
#define CALM 517u
static uint8_t lin_mode;
static uint32_t calm;

int gm_load(uint32_t pct)
{
    if (pct > LOAD_HI) {
        calm = 0;
        if (!lin_mode) {
            lin_mode = 1;
            return 1;
        }
    } else if (lin_mode && pct < LOAD_LO && ++calm >= CALM)
        lin_mode = 0;
    else if (pct >= LOAD_LO)
        calm = 0;
    return 0;
}

static float bessel_i0(float x)
{
    float sum = 1.0f, t = 1.0f;
    int k;
    for (k = 1; k < 24; k++) {
        t *= (x * 0.5f / (float)k) * (x * 0.5f / (float)k);
        sum += t;
    }
    return sum;
}

static void sinc_init(void)
{
    int p, i;
    for (p = 0; p <= NPH; p++) {
        float sum = 0.0f;
        for (i = 0; i < NTAP; i++) {
            float x = (float)(i - (NTAP / 2 - 1)) - (float)p / NPH, u = 2.0f * x / NTAP;
            float w = u * u < 1.0f ? bessel_i0(5.0f * fm_sqrtf(1.0f - u * u)) / bessel_i0(5.0f) : 0.0f;
            float sn = x == 0.0f ? 1.0f : fm_sinf(FM_PI * x) / (FM_PI * x);
            sinc_tab[p][i] = sn * w;
            sum += sn * w;
        }
        for (i = 0; i < NTAP; i++)
            sinc_tab[p][i] /= sum;          /* each place passes a steady level as it is */
    }
}

enum { ST_FREE, ST_ATT, ST_HOLD, ST_DEC, ST_REL };
typedef struct {
    uint8_t st, ch, note, vel;
    uint8_t held, sus;                          /* key down; let go while the pedal was down */
    uint8_t drum, eos;                          /* a drum voice; its one-shot sample has ended */
    uint8_t layer;                              /* which of a key's voices (Honky-tonk's two strings) */
    const gmw_zone_t *z;
    const uint8_t *data;
    uint32_t pos, len, ls, le;                  /* the next point to decode; the sample's end and loop */
    int32_t pred, idx, lpred, lidx;             /* the ADPCM decoder, and its state at the loop start */
    float hist[2 * NTAP];                       /* the last NTAP points, twice over (a ring read straight) */
    float frac;                                 /* where the output lies between the middle two */
    uint8_t hp;                                 /* the oldest point's place in hist */
    uint32_t age;
    float semi, ratio;                          /* pitch: semitones from the sample's own; its rate / GM_SR */
    float amp, a_inc, th, sus_l, td, tr;        /* volume envelope: now, attack step a frame, hold left (s),
                                                 * sustain level, decay and release (s to fall 100 dB) */
    float fe, fa, fd, fs, fc, fenv, fk, fg;     /* filter: modulation envelope (now; attack, decay s;
                                                 * sustain), cutoff and depth (cents), 1 / Q, gain */
    float vph;                                  /* the mod wheel's vibrato: phase (turns) */
    float t;                                    /* seconds since the note */
    float ic1, ic2;                             /* filter state */
    float vgain, pl, pr;                        /* velocity x the zone's gain; a drum's own pan */
} voice_t;
static voice_t vo[GM_NVOICE] GM_POOL;
static voice_t dr[GM_NDRUM] GM_POOL;
static uint32_t age_ctr;

static const int16_t IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
    88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
    658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t IMA_IDX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

/* reverb (Freeverb-style: four combs, two all-passes per side) */
#define NCOMB 4
static const uint16_t COMB_LEN[NCOMB] = {1116, 1188, 1277, 1356};
static const uint16_t AP_LEN[4] = {556, 441, 579, 421};
static float comb_buf[1116 + 1188 + 1277 + 1356] GM_POOL;
static float ap_buf[556 + 441 + 579 + 421] GM_POOL;
static float comb_z[NCOMB];
static uint16_t comb_i[NCOMB], ap_i[4];
static float *comb_p[NCOMB], *ap_p[4];

/* ---------------------------------------------------------------- init --- */
void gm_init(void)
{
    int i;
    float *q;
    sinc_init();
    for (i = 0, q = comb_buf; i < NCOMB; i++) {
        comb_p[i] = q;
        q += COMB_LEN[i];
    }
    for (i = 0, q = ap_buf; i < 4; i++) {
        ap_p[i] = q;
        q += AP_LEN[i];
    }
    gm_reset();
}

void gm_panic(void)
{
    int i;
    for (i = 0; i < GM_NVOICE; i++)
        vo[i].st = ST_FREE;
    for (i = 0; i < GM_NDRUM; i++)
        dr[i].st = ST_FREE;
    for (i = 0; i < (int)(sizeof comb_buf / sizeof comb_buf[0]); i++)
        comb_buf[i] = 0.0f;
    for (i = 0; i < (int)(sizeof ap_buf / sizeof ap_buf[0]); i++)
        ap_buf[i] = 0.0f;
    for (i = 0; i < NCOMB; i++)
        comb_z[i] = 0.0f;
}

void gm_reset(void)
{
    int i;
    for (i = 0; i < GM_NCH; i++) {
        chan_defaults(&ch[i]);
        gm_ch_prog[i] = 0;
    }
    gm_panic();
}

const char *gm_prog_name(int p) { return GM_NAME[p & 127]; }
const char *gm_family_name(int p) { return GM_FAMILY[(p & 127) >> 3]; }
const char *gm_drum_name(int n) { return n >= GM_DRUM_LO && n <= GM_DRUM_HI ? GM_DRUM_NAME[n - GM_DRUM_LO] : ""; }

/* -------------------------------------------------------------- notes --- */
static const gmw_zone_t *zone_for(int prog, int note)  /* the program's zone for the key (or the nearest) */
{
    uint32_t i, a = GMW_PROG[prog & 127], b = GMW_PROG[(prog & 127) + 1];
    if (a >= b)
        return 0;
    for (i = a; i < b; i++)
        if (note >= GMW_ZONE[i].lo && note <= GMW_ZONE[i].hi)
            return &GMW_ZONE[i];
    return note < GMW_ZONE[a].lo ? &GMW_ZONE[a] : &GMW_ZONE[b - 1];
}

static voice_t *voice_alloc(int c, int note, int layer)
{
    int i, best = -1, nv = gm_voice_max;
    float bl = 1e9f;
    if (nv < 8 || nv > GM_NVOICE)
        nv = GM_NVOICE_DEF;
    for (i = 0; i < GM_NVOICE; i++)             /* the same key again: restrike its voice */
        if (vo[i].st != ST_FREE && vo[i].ch == c && vo[i].note == note && vo[i].layer == layer)
            return &vo[i];
    for (i = 0; i < nv; i++)
        if (vo[i].st == ST_FREE)
            return &vo[i];
    for (i = 0; i < nv; i++)                    /* the quietest released voice */
        if ((vo[i].st == ST_REL || (!vo[i].held && !vo[i].sus)) && vo[i].amp < bl) {
            bl = vo[i].amp;
            best = i;
        }
    if (best < 0) {                             /* else the oldest */
        uint32_t old = 0;
        for (i = 0; i < nv; i++)
            if (age_ctr - vo[i].age >= old) {
                old = age_ctr - vo[i].age;
                best = i;
            }
    }
    return &vo[best];
}

/* start (or restrike) v on zone z: the sample from its first point, the envelopes from the top */
static void voice_start(voice_t *v, const gmw_zone_t *z, int c, int note, int vel, int drum, float cents)
{
    const gmw_smp_t *s = &GMW_SMP[z->smp];
    float vn = (float)vel * (1.0f / 127.0f), q;
    int32_t kd = 60 - note;
    int i;
    if (v->st == ST_FREE) {
        v->amp = 0.0f;
        for (i = 0; i < 2 * NTAP; i++)
            v->hist[i] = 0.0f;
        v->hp = 0;
        v->ic1 = v->ic2 = 0.0f;
    }
    v->z = z;
    v->ch = (uint8_t)c;
    v->note = (uint8_t)note;
    v->vel = (uint8_t)vel;
    v->held = 1;
    v->sus = 0;
    v->drum = (uint8_t)drum;
    v->eos = 0;
    v->age = ++age_ctr;
    v->data = GMW_DATA + s->off;
    v->pos = 0;
    v->len = s->len;
    v->ls = s->ls;
    v->le = s->le ? s->le : 0xFFFFFFFFu;
    v->pred = 0;
    v->idx = s->idx0;
    v->lpred = s->lpred;
    v->lidx = s->lidx;
    {                                           /* from where a restruck voice was: no click */
        float y = v->hist[v->hp + NTAP / 2 - 1];
        for (i = 0; i < 2 * NTAP; i++)
            v->hist[i] = y;
    }
    v->frac = 1.0f;
    v->semi = ((float)(note - z->root) * (float)z->kt + (float)z->tune + cents) * 0.01f;
    v->ratio = s->rate * INV_SR;
    v->st = ST_ATT;
    v->a_inc = 1.0f / (tsec(z->att) * GM_SR);
    v->th = tsec(z->hold + z->khold * kd);
    v->td = tsec(z->dec + z->kdec * kd);
    v->sus_l = z->sus >= 960 ? 0.0f : fm_db2lin((float)z->sus * -0.1f);
    v->tr = tsec(z->rel);
    v->fc = (float)z->fc;
    v->fenv = (float)z->fenv;
    v->fe = 0.0f;
    v->fa = tsec(z->fatt);
    v->fd = tsec(z->fdec);
    v->fs = 1.0f - (float)z->fsus * 0.001f;
    q = fm_db2lin((float)z->q * 0.1f - 3.01f);  /* resonance, as FluidSynth: 0 cB is Q 0.707 */
    v->fk = 1.0f / q;
    v->fg = q > 1.0f ? fm_rsqrtf(q) : 1.0f;
    v->vph = 0.0f;
    v->t = 0.0f;
    /* velocity: up to 100 (where the levels were matched) a gentle curve, so soft parts stay a little
     * louder than in FluidSynth; above it 40 log10, as FluidSynth, so hard hits are as loud as there */
    v->vgain = (vel <= 100 ? vn * (0.3f + 0.7f * vn) : VEL_100 * (float)(vel * vel) * 1e-4f) * z->gain;
    v->pl = v->pr = 1.0f;
    if (drum)
        pan_gains(64 + z->pan, &v->pl, &v->pr);
}

/* Honky-tonk Piano: two strings a key, a little out of tune with each other (each this far off, in
 * cents: about 3 Hz of beat at middle C), the piano's sample played twice */
#define HONKY 3
#define HONKY_CENTS 9.0f

static void note_on(int c, int note, int vel)
{
    const gmw_zone_t *z;
    int i, n = ch[c].prog == HONKY ? 2 : 1;
    if (note > GM_NOTE_MAX)                     /* above C9: ignored (the samples have nothing up there) */
        return;
    z = zone_for(ch[c].prog, note);
    for (i = 0; z && i < n; i++) {
        voice_t *v = voice_alloc(c, note, i);
        voice_start(v, z, c, note, vel, 0, n == 1 ? 0.0f : i ? HONKY_CENTS : -HONKY_CENTS);
        v->layer = (uint8_t)i;
        if (n > 1)
            v->vgain *= 0.71f;                  /* the pair as loud as one */
    }
}

static void note_off(int c, int note)
{
    int i;
    for (i = 0; i < GM_NVOICE; i++) {
        voice_t *v = &vo[i];
        if (v->st == ST_FREE || v->ch != c || v->note != note || !v->held)
            continue;
        v->held = 0;
        if (ch[c].sus)
            v->sus = 1;
        else
            v->st = ST_REL;
    }
}

void gm_shed(void)
{
    int i, q = -1;
    for (i = 0; i < GM_NVOICE; i++)
        if (vo[i].st != ST_FREE && (q < 0 || vo[i].amp * vo[i].vgain < vo[q].amp * vo[q].vgain))
            q = i;
    if (q >= 0) {
        vo[q].st = ST_REL;
        vo[q].held = vo[q].sus = 0;
        vo[q].tr = 0.02f;
    }
}

static void sustain_off(int c)
{
    int i;
    for (i = 0; i < GM_NVOICE; i++)
        if (vo[i].st != ST_FREE && vo[i].ch == c && vo[i].sus) {
            vo[i].sus = 0;
            vo[i].st = ST_REL;
        }
}

static void chan_off(int c, int now)            /* all notes off (release), or all sound off (now) */
{
    int i;
    for (i = 0; i < GM_NVOICE; i++)
        if (vo[i].st != ST_FREE && vo[i].ch == c) {
            vo[i].held = vo[i].sus = 0;
            vo[i].st = now ? ST_FREE : ST_REL;
        }
    if (c == GM_DRUM_CH && now)
        for (i = 0; i < GM_NDRUM; i++)
            dr[i].st = ST_FREE;
}

static void drum_hit(int note, int vel)
{
    const gmw_zone_t *z;
    voice_t *x = 0;
    int i;
    if (note < GM_DRUM_LO || note > GM_DRUM_HI || GMW_DRUM[note - GM_DRUM_LO] < 0)
        return;
    z = &GMW_ZONE[GMW_DRUM[note - GM_DRUM_LO]];
    for (i = 0; i < GM_NDRUM; i++) {
        if (dr[i].st == ST_FREE)
            continue;
        if (dr[i].note == note)
            x = &dr[i];                         /* the same drum again: that voice */
        else if (z->excl && dr[i].z->excl == z->excl) {
            dr[i].st = ST_REL;                  /* the open hat stops when the closed one hits */
            dr[i].tr = 0.03f;
        }
    }
    if (!x)
        for (i = 0; i < GM_NDRUM; i++)
            if (dr[i].st == ST_FREE) {
                x = &dr[i];
                break;
            }
    if (!x) {                                   /* the quietest */
        float q = 1e9f;
        for (i = 0; i < GM_NDRUM; i++)
            if (dr[i].amp * dr[i].vgain < q) {
                q = dr[i].amp * dr[i].vgain;
                x = &dr[i];
            }
    }
    voice_start(x, z, GM_DRUM_CH, note, vel, 1, 0.0f);
    x->held = 0;                                /* the drums ring out by themselves */
}

/* ---------------------------------------------------------------- MIDI --- */
void gm_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    int c = (int)(st & 15u);
    chan_t *cp = &ch[c];
    d1 &= 127u;
    d2 &= 127u;
    switch (st & 0xF0u) {
    case 0x90:
        if (d2 && c == GM_DRUM_CH)
            drum_hit((int)d1, (int)d2);
        else if (d2)
            note_on(c, (int)d1, (int)d2);
        else if (c != GM_DRUM_CH)              /* velocity 0 is a note off */
            note_off(c, (int)d1);
        break;
    case 0x80:
        if (c != GM_DRUM_CH)                   /* the drums ring out by themselves */
            note_off(c, (int)d1);
        break;
    case 0xB0:
        switch (d1) {
        case 1: cp->mod = (uint8_t)d2; break;
        case 6:                                /* data entry: RPN 0, the bend range */
            if (cp->rpn_msb == 0 && cp->rpn_lsb == 0)
                cp->bend_range = (uint8_t)(d2 > 24u ? 24u : d2);
            break;
        case 7: cp->vol = (uint8_t)d2; break;
        case 10: cp->pan = (uint8_t)d2; break;
        case 11: cp->expr = (uint8_t)d2; break;
        case 64:
            cp->sus = d2 >= 64u;
            if (!cp->sus)
                sustain_off(c);
            break;
        case 91: cp->rev = (uint8_t)d2; break;
        case 100: cp->rpn_lsb = (uint8_t)d2; break;
        case 101: cp->rpn_msb = (uint8_t)d2; break;
        case 120: chan_off(c, 1); break;
        case 121:
            chan_reset_ctl(cp);
            sustain_off(c);
            break;
        case 123: case 124: case 125: case 126: case 127:
            chan_off(c, 0);
            break;
        default: break;
        }
        break;
    case 0xC0:
        cp->prog = (uint8_t)d1;
        gm_ch_prog[c] = (uint8_t)d1;
        break;
    case 0xE0:
        cp->bend = (int16_t)((int32_t)(d1 | d2 << 7) - 8192);
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------- render --- */
static float mix_l[GM_CHUNK], mix_r[GM_CHUNK], mix_rev[GM_CHUNK], vsrc[GM_CHUNK];

/* 1 / n for the points decoded in one frame: at most INC_MAX, and one more on a note's first frame
 * (frac starts at 1). A read past the end here was the full-scale noise on the highest notes. */
#define INC_MAX 12.0f
static const float INV_N[14] = {1.0f, 1.0f, 1.0f / 2, 1.0f / 3, 1.0f / 4, 1.0f / 5, 1.0f / 6, 1.0f / 7, 1.0f / 8,
                                1.0f / 9, 1.0f / 10, 1.0f / 11, 1.0f / 12, 1.0f / 13};

/* one voice for n frames, added to the mix; 0 when it has finished */
static int voice_render(voice_t *v, uint32_t n)
{
    const gmw_zone_t *z = v->z;
    const chan_t *c = &ch[v->ch];
    float dt = (float)n * INV_SR, a0 = v->amp, a1, da, inc, semi, vib, gl, gr, gain;
    float a1c = 0.0f, a2c = 0.0f, a3c = 0.0f, k = 0.0f;
    uint32_t j;
    int filt = z->fc + (z->fenv > 0 ? z->fenv : 0) < 13000;
    /* amplitude */
    switch (v->st) {
    case ST_ATT:
        a1 = a0 + v->a_inc * (float)n;
        if (a1 >= 1.0f) {
            a1 = 1.0f;
            v->st = ST_HOLD;
        }
        break;
    case ST_HOLD:
        a1 = 1.0f;
        v->th -= dt;
        if (v->th <= 0.0f)
            v->st = ST_DEC;
        break;
    case ST_DEC:
        a1 = a0 * fall_k(v->td, n);
        if (a1 < v->sus_l)
            a1 = v->sus_l;
        break;
    default:
        a1 = a0 * fall_k(v->tr, n);
        break;
    }
    if ((v->st == ST_REL || (v->st == ST_DEC && v->sus_l == 0.0f)) && a1 < 3e-4f) {
        v->st = ST_FREE;
        a1 = 0.0f;
    }
    v->amp = a1;
    da = (a1 - a0) / (float)n;
    /* pitch: the key, bend, and the mod wheel's vibrato */
    vib = (float)c->mod * (50.0f / 127.0f);
    semi = v->semi + (float)c->bend * (float)c->bend_range * (1.0f / 8192.0f);
    if (vib != 0.0f) {
        v->vph += 5.5f * dt;
        v->vph -= fm_floorf(v->vph);
        semi += vib * 0.01f * fm_sin_turns(v->vph);
    }
    inc = v->ratio * fm_exp2f(semi * (1.0f / 12.0f));
    if (inc > INC_MAX)
        inc = INC_MAX;
    /* the filter, with its modulation envelope (linear: attack, then decay to the sustain) */
    if (filt) {
        float fc, g;
        if (v->t < v->fa)
            v->fe = v->t / v->fa;
        else {
            v->fe = 1.0f - (v->t - v->fa) / v->fd;
            if (v->fe < v->fs)
                v->fe = v->fs;
        }
        fc = cents_hz(v->fc + v->fenv * v->fe);
        if (fc > 0.42f * GM_SR)
            fc = 0.42f * GM_SR;
        if (fc < 20.0f)
            fc = 20.0f;
        g = fm_tanf(FM_PI * fc * INV_SR);
        k = v->fk;
        a1c = 1.0f / (1.0f + g * (g + k));
        a2c = g * a1c;
        a3c = g * a2c;
    }
    v->t += dt;
    gain = v->vgain * chan_gain(c) * MASTER * (filt ? v->fg : 1.0f);
    if (z->trem && v->t > TREM_DELAY)           /* the zone's tremolo (Tremolo Strings' bowing) */
        gain *= fm_db2lin((float)(z->trem >> 4) * 0.5f *
                          fm_sin_turns((v->t - TREM_DELAY) * (float)(z->trem & 15u) * 2.0f));
    pan_gains(c->pan, &gl, &gr);
    if (v->drum) {
        gl *= v->pl * 1.41f;
        gr *= v->pr * 1.41f;
    }
    {   /* the sample into src[]; then the filter, the amplitude and the mix */
        float frac = v->frac, ic1 = v->ic1, ic2 = v->ic2, amp = a0, *hist = v->hist;
        uint32_t hp = v->hp;
        float rev = (float)c->rev * (1.0f / 127.0f), gwl, gwr, gwv;
        float *src = vsrc;
        uint32_t pos = v->pos, len = v->len, ls = v->ls, le = v->le;
        int32_t pred = v->pred, idx = v->idx;
        const uint8_t *data = v->data;
        int box = inc >= 1.5f;                  /* above 1.5 points a frame: the mean of them (less aliasing) */
        int lin = lin_mode;
        for (j = 0; j < n; j++) {
            float acc = 0.0f;
            uint32_t cnt = 0;
            frac += inc;
            while (frac >= 1.0f) {              /* the next point: one ADPCM nibble */
                uint32_t b, nb;
                int32_t step, dq;
                frac -= 1.0f;
                if (pos == le) {
                    pos = ls;
                    pred = v->lpred;
                    idx = v->lidx;
                }
                if (pos >= len) {
                    hist[hp] = hist[hp + NTAP] = 0.0f;
                    hp = hp + 1 == NTAP ? 0 : hp + 1;
                    v->eos = 1;
                    continue;
                }
                b = data[pos >> 1];
                nb = pos & 1u ? b >> 4 : b & 15u;
                pos++;
                step = IMA_STEP[idx];
                dq = step >> 3;
                if (nb & 4u)
                    dq += step;
                if (nb & 2u)
                    dq += step >> 1;
                if (nb & 1u)
                    dq += step >> 2;
                pred += nb & 8u ? -dq : dq;
                pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
                idx += IMA_IDX[nb & 7u];
                idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
                {
                    float y = (float)pred * (1.0f / 32768.0f);
                    hist[hp] = hist[hp + NTAP] = y;
                    hp = hp + 1 == NTAP ? 0 : hp + 1;
                    acc += y;
                }
                cnt++;
            }
            if (box && cnt > 1 && cnt < sizeof INV_N / sizeof INV_N[0])
                src[j] = acc * INV_N[cnt];
            else if (lin) {
                const float *h = hist + hp + NTAP / 2 - 1;
                src[j] = h[0] + (h[1] - h[0]) * frac;
            } else {
                float ph = frac * NPH, o = 0.0f;
                int k = (int)ph, i;
                const float *a = sinc_tab[k], *b = sinc_tab[k + 1 > NPH ? NPH : k + 1], *h = hist + hp;
                ph -= (float)k;
                for (i = 0; i < NTAP; i++)
                    o += h[i] * (a[i] + (b[i] - a[i]) * ph);
                src[j] = o;
            }
        }
        v->pos = pos;
        v->pred = pred;
        v->idx = idx;
        if (filt)                               /* TPT state-variable low-pass */
            for (j = 0; j < n; j++) {
                float v3 = src[j] - ic2, v1 = a1c * ic1 + a2c * v3, v2 = ic2 + a2c * ic1 + a3c * v3;
                ic1 = 2.0f * v1 - ic1;
                ic2 = 2.0f * v2 - ic2;
                src[j] = v2;
            }
        gwl = gain * gl;
        gwr = gain * gr;
        gwv = gain * rev;
        for (j = 0; j < n; j++) {
            float o;
            amp += da;
            o = src[j] * amp;
            mix_l[j] += o * gwl;
            mix_r[j] += o * gwr;
            mix_rev[j] += o * gwv;
        }
        v->hp = (uint8_t)hp;
        v->frac = frac;
        v->ic1 = fm_flush(ic1);
        v->ic2 = fm_flush(ic2);
        ch[v->ch].lvl += (a0 + a1) * 0.5f * gain * 0.6f;   /* the meter: the envelope, not the waveform */
    }
    if (v->eos) {                               /* a one-shot that has played out */
        int i, quiet = 1;
        for (i = 0; i < NTAP; i++)
            quiet &= v->hist[i] == 0.0f;
        if (quiet)
            v->st = ST_FREE;
    }
    return v->st != ST_FREE;
}

/* the safety net: a NaN or an infinity anywhere in the voices or the reverb reaches the chunk's sum
 * (a NaN times 0 is still a NaN), and would stay in the reverb until a reboot, at full scale after the
 * clipper (0.9.8's noise on the highest notes). Then the chunk is dropped and the engine goes quiet:
 * a click, not the noise. */
static int not_finite(float x)
{
    fm_bits_t b;
    b.f = x;
    return (b.u & 0x7F800000u) == 0x7F800000u;
}

#ifdef OM_HOST
void gm_test_poison(int what)                   /* tests only: a NaN in a voice's filter (0) or the reverb (1) */
{
    int i;
    float nan;
    fm_bits_t b;
    b.u = 0x7FC00000u;
    nan = b.f;
    if (what)
        comb_buf[0] = nan;
    else
        for (i = 0; i < GM_NVOICE; i++)
            if (vo[i].st != ST_FREE)
                vo[i].ic1 = vo[i].hist[0] = vo[i].hist[NTAP] = nan;
}
#endif

void gm_render(float *l, float *r, uint32_t n)
{
    uint32_t i, j, nv = 0, nd = 0;
    float sum = 0.0f;
    if (n > GM_CHUNK)
        n = GM_CHUNK;
    for (j = 0; j < n; j++)
        mix_l[j] = mix_r[j] = mix_rev[j] = 0.0f;
    for (i = 0; i < GM_NCH; i++)
        ch[i].lvl = 0.0f;
    for (i = 0; i < GM_NVOICE; i++)
        if (vo[i].st != ST_FREE)
            nv += (uint32_t)voice_render(&vo[i], n);
    for (i = 0; i < GM_NDRUM; i++)
        if (dr[i].st != ST_FREE)
            nd += (uint32_t)voice_render(&dr[i], n);
    for (i = 0; i < GM_NCH; i++) {              /* meters: fast up, slow down */
        float lv = ch[i].lvl * 3.0f, m = gm_ch_level[i];
        gm_ch_level[i] = lv > m ? lv : m * 0.995f;
    }
    gm_voices_used = (uint8_t)nv;
    gm_drums_used = (uint8_t)nd;
    /* reverb */
    for (j = 0; j < n; j++) {
        float in = mix_rev[j] * 0.12f, acc = 0.0f, ol, orr;
        for (i = 0; i < NCOMB; i++) {
            float *b = comb_p[i], y = b[comb_i[i]];
            comb_z[i] = y * 0.75f + comb_z[i] * 0.25f;      /* damping */
            b[comb_i[i]] = in + comb_z[i] * 0.84f;
            if (++comb_i[i] >= COMB_LEN[i])
                comb_i[i] = 0;
            acc += y;
        }
        ol = acc;
        orr = acc;
        for (i = 0; i < 4; i++) {
            float *b = ap_p[i], y = b[ap_i[i]], *o = i < 2 ? &ol : &orr;
            b[ap_i[i]] = *o + y * 0.5f;
            *o = y - *o;
            if (++ap_i[i] >= AP_LEN[i])
                ap_i[i] = 0;
        }
        mix_l[j] += ol * 0.3f;
        mix_r[j] += orr * 0.3f;
        sum += mix_l[j] + mix_r[j];
    }
    for (i = 0; i < NCOMB; i++) {
        sum += comb_z[i];
        comb_z[i] = fm_flush(comb_z[i]);
    }
    if (not_finite(sum)) {
        gm_panic();
        gm_resets++;
        return;
    }
    for (j = 0; j < n; j++) {
        l[j] += mix_l[j];
        r[j] += mix_r[j];
    }
}
