/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM sound engine (gm.h). One compilation unit, -O2 (tools/build.py O2_UNITS).
 *
 * A sample player. Each note plays one zone of the wavetable (gm_wavetable.h, generated from the
 * SoundFont assets/soundfont/fm1gm.sf2): its 4-bit ADPCM sample, decoded as it plays and
 * interpolated linearly, through the zone's volume envelope (attack, hold, decay, sustain,
 * release), low-pass filter with its modulation envelope, vibrato and tremolo.
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

#define INV_SR (1.0f / GM_SR)
#define MASTER 1.5f                             /* the mix level: about as loud as the synthesized engine was */

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
enum { ST_FREE, ST_ATT, ST_HOLD, ST_DEC, ST_REL };
typedef struct {
    uint8_t st, ch, note, vel;
    uint8_t held, sus;                          /* key down; let go while the pedal was down */
    uint8_t drum, eos;                          /* a drum voice; its one-shot sample has ended */
    const gmw_zone_t *z;
    const uint8_t *data;
    uint32_t pos, len, ls, le;                  /* the next point to decode; the sample's end and loop */
    int32_t pred, idx, lpred, lidx;             /* the ADPCM decoder, and its state at the loop start */
    float y0, y1, frac;                         /* the two points the output lies between, and where */
    uint32_t age;
    float semi, ratio;                          /* pitch: semitones from the sample's own; its rate / GM_SR */
    float amp, a_inc, th, sus_l, td, tr;        /* volume envelope: now, attack step a frame, hold left (s),
                                                 * sustain level, decay and release (s to fall 100 dB) */
    float fe, fa, fd, fs, fc, fenv, fk, fg;     /* filter: modulation envelope (now; attack, decay s;
                                                 * sustain), cutoff and depth (cents), 1 / Q, gain */
    float vib, vrate, vdel, vph;                /* vibrato: cents, Hz, delay (s), phase (turns) */
    float trem, trate, tdel, tph;               /* tremolo: dB, Hz, delay (s), phase */
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

static voice_t *voice_alloc(int c, int note)
{
    int i, best = -1, nv = gm_voice_max;
    float bl = 1e9f;
    if (nv < 8 || nv > GM_NVOICE)
        nv = GM_NVOICE_DEF;
    for (i = 0; i < GM_NVOICE; i++)             /* the same key again: restrike its voice */
        if (vo[i].st != ST_FREE && vo[i].ch == c && vo[i].note == note)
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
static void voice_start(voice_t *v, const gmw_zone_t *z, int c, int note, int vel, int drum)
{
    const gmw_smp_t *s = &GMW_SMP[z->smp];
    float vn = (float)vel * (1.0f / 127.0f), q;
    int32_t kd = 60 - note;
    if (v->st == ST_FREE) {
        v->amp = 0.0f;
        v->y1 = 0.0f;
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
    v->y0 = v->y1;                              /* from where a restruck voice was: no click */
    v->frac = 1.0f;
    v->semi = ((float)(note - z->root) * (float)z->kt + (float)z->tune) * 0.01f;
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
    v->vib = (float)z->vib;
    v->vrate = z->vib ? cents_hz((float)z->vfreq) : 5.5f;   /* the mod wheel alone: 5.5 Hz */
    v->vdel = tsec(z->vdel);
    v->vph = 0.0f;
    v->trem = (float)z->trem * 0.1f;
    v->trate = cents_hz((float)z->tfreq);
    v->tdel = tsec(z->tdel);
    v->tph = 0.0f;
    v->t = 0.0f;
    v->vgain = vn * (0.3f + 0.7f * vn) * z->gain;
    v->pl = v->pr = 1.0f;
    if (drum)
        pan_gains(64 + z->pan, &v->pl, &v->pr);
}

static void note_on(int c, int note, int vel)
{
    const gmw_zone_t *z = zone_for(ch[c].prog, note);
    if (z)
        voice_start(voice_alloc(c, note), z, c, note, vel, 0);
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
    voice_start(x, z, GM_DRUM_CH, note, vel, 1);
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

/* one voice for n frames, added to the mix; 0 when it has finished */
static int voice_render(voice_t *v, uint32_t n)
{
    const gmw_zone_t *z = v->z;
    const chan_t *c = &ch[v->ch];
    float dt = (float)n * INV_SR, a0 = v->amp, a1, da, inc, semi, vib, gl, gr, gain, trem = 1.0f;
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
    /* pitch: the key, bend, vibrato (the zone's, after its delay, and the mod wheel) */
    vib = (v->t >= v->vdel ? v->vib : 0.0f) + (float)c->mod * (50.0f / 127.0f);
    v->vph += v->vrate * dt;
    v->vph -= fm_floorf(v->vph);
    semi = v->semi + (float)c->bend * (float)c->bend_range * (1.0f / 8192.0f);
    if (vib != 0.0f)
        semi += vib * 0.01f * fm_sin_turns(v->vph);
    inc = v->ratio * fm_exp2f(semi * (1.0f / 12.0f));
    if (inc > 8.0f)
        inc = 8.0f;
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
    /* tremolo */
    if (v->trem != 0.0f && v->t >= v->tdel) {
        v->tph += v->trate * dt;
        v->tph -= fm_floorf(v->tph);
        trem = fm_db2lin(-v->trem * fm_sin_turns(v->tph));
    }
    v->t += dt;
    gain = v->vgain * chan_gain(c) * trem * MASTER * (filt ? v->fg : 1.0f);
    pan_gains(c->pan, &gl, &gr);
    if (v->drum) {
        gl *= v->pl * 1.41f;
        gr *= v->pr * 1.41f;
    }
    {   /* the sample into src[]; then the filter, the amplitude and the mix */
        float y0 = v->y0, y1 = v->y1, frac = v->frac, ic1 = v->ic1, ic2 = v->ic2, amp = a0;
        float rev = (float)c->rev * (1.0f / 127.0f), gwl, gwr, gwv;
        float *src = vsrc;
        uint32_t pos = v->pos, len = v->len, ls = v->ls, le = v->le;
        int32_t pred = v->pred, idx = v->idx;
        const uint8_t *data = v->data;
        for (j = 0; j < n; j++) {
            frac += inc;
            while (frac >= 1.0f) {              /* the next point: one ADPCM nibble */
                uint32_t b, nb;
                int32_t step, dq;
                frac -= 1.0f;
                y0 = y1;
                if (pos == le) {
                    pos = ls;
                    pred = v->lpred;
                    idx = v->lidx;
                }
                if (pos >= len) {
                    y1 = 0.0f;
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
                y1 = (float)pred * (1.0f / 32768.0f);
            }
            src[j] = y0 + (y1 - y0) * frac;
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
        v->y0 = y0;
        v->y1 = y1;
        v->frac = frac;
        v->ic1 = fm_flush(ic1);
        v->ic2 = fm_flush(ic2);
        ch[v->ch].lvl += (a0 + a1) * 0.5f * gain * 0.6f;   /* the meter: the envelope, not the waveform */
    }
    if (v->eos && v->y0 == 0.0f)                /* a one-shot that has played out */
        v->st = ST_FREE;
    return v->st != ST_FREE;
}

void gm_render(float *l, float *r, uint32_t n)
{
    uint32_t i, j, nv = 0, nd = 0;
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
        l[j] += mix_l[j] + ol * 0.3f;
        r[j] += mix_r[j] + orr * 0.3f;
    }
    for (i = 0; i < NCOMB; i++)
        comb_z[i] = fm_flush(comb_z[i]);
}
