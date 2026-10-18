/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM sound engine (gm.h). One compilation unit, -O2 (tools/build.py O2_UNITS).
 *
 * The render works in chunks of up to GM_CHUNK frames: per chunk each voice works out its pitch,
 * envelopes and filter coefficients once, then runs a tight per-sample loop with the amplitude
 * ramped linearly across the chunk. */
#include "gm.h"
#include "fastmath.h"
#include "gm_patches.h"

volatile float gm_ch_level[GM_NCH];
volatile uint8_t gm_voices_used, gm_drums_used;
volatile uint8_t gm_ch_prog[GM_NCH];
volatile uint8_t gm_voice_max = GM_NVOICE_DEF;

#define INV_SR (1.0f / GM_SR)
#define SIN_N 1024
static float sin_t[SIN_N + 1] GM_POOL;

/* sine of x turns, any x > -8 (FM phases plus a modulation of a few turns) */
static inline float sin_turns(float x)
{
    float u = (x + 8.0f) * (float)SIN_N;
    int32_t i = (int32_t)u;
    float fr = u - (float)i;
    i &= SIN_N - 1;
    return sin_t[i] + (sin_t[i + 1] - sin_t[i]) * fr;
}

static uint32_t rng_s = 0x1234567u;
static inline float noise(void)                 /* white, -1 .. 1 */
{
    rng_s = rng_s * 1664525u + 1013904223u;
    return (float)(int32_t)rng_s * (1.0f / 2147483648.0f);
}

static inline float tcode(uint32_t c) { return 0.001f * fm_exp2f((float)c * 0.1f); }   /* seconds */
/* per-chunk factor of an exponential that falls 43 dB (x 1/148) in t seconds: e^(-5 n / (t sr)) */
static inline float decay_k(float t, uint32_t n) { return fm_exp2f(-7.2134752f * (float)n * INV_SR / t); }
static inline float note_hz(float note) { return 440.0f * fm_exp2f((note - 69.0f) * (1.0f / 12.0f)); }

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
enum { ST_FREE, ST_ATT, ST_DEC, ST_REL };
typedef struct {
    uint8_t st, ch, note, vel;
    uint8_t held, sus;                          /* key down; let go while the pedal was down */
    uint8_t two;                                /* a second oscillator */
    const gm_patch_t *p;
    uint32_t age;
    float base;                                 /* the note, with the patch's octave */
    float amp, a_inc, sus_l, tdec, trel;        /* amplitude envelope */
    float ix, ix_to, tix;                       /* FM index, radians: now, where it goes, how fast */
    float fe, tfe;                              /* filter envelope 1 -> 0 */
    float pe, tpe;                              /* pitch envelope, semitones -> 0 */
    float ph1, ph2, phm, fbz;                   /* phases (turns); the modulator's last output */
    float vlfo, tlfo, t;                        /* vibrato / tremolo phases; seconds since the note */
    float ic1, ic2;                             /* filter state */
    float vgain;                                /* velocity x level */
} voice_t;
static voice_t vo[GM_NVOICE] GM_POOL;
static uint32_t age_ctr;

/* drums */
typedef struct {
    uint8_t on, note;
    const gm_drum_t *d;
    float t, gain, pl, pr;
    float tamp, tk, namp, nk;                   /* tone / noise envelopes and their per-chunk factors */
    float pe, pk;                               /* pitch sweep 1 -> 0 */
    float ph[6];
    float g, k, ic1, ic2;                       /* filter */
} drum_t;
static drum_t dr[GM_NDRUM];
static const float METAL_R[6] = {1.0f, 1.4827f, 1.8003f, 2.5460f, 2.6303f, 3.8967f};   /* TR-808 ratios */

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
    for (i = 0; i <= SIN_N; i++)
        sin_t[i] = fm_sin_turns((float)i / (float)SIN_N);
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
        dr[i].on = 0;
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

static void note_on(int c, int note, int vel)
{
    const gm_patch_t *p = &GM_PATCH[ch[c].prog & 127];
    voice_t *v = voice_alloc(c, note);
    float vn = (float)vel * (1.0f / 127.0f), ks = 1.0f;
    int restrike = v->st != ST_FREE && v->ch == c && v->note == note;
    if (p->flags & F_KS)
        ks = fm_exp2f(-(float)(note - 60) * (1.0f / 24.0f));
    v->p = p;
    v->ch = (uint8_t)c;
    v->note = (uint8_t)note;
    v->vel = (uint8_t)vel;
    v->held = 1;
    v->sus = 0;
    v->age = ++age_ctr;
    v->two = p->det || p->iv;
    v->base = (float)(note + 12 * p->oct);
    if (!restrike) {
        v->amp = 0.0f;
        v->ph1 = v->ph2 = v->phm = v->fbz = 0.0f;
        v->ic1 = v->ic2 = 0.0f;
        v->vlfo = 0.0f;
        v->tlfo = 0.25f;                        /* tremolo starts at full level */
    }
    v->st = ST_ATT;
    v->a_inc = 1.0f / (tcode(p->a) * GM_SR);
    v->sus_l = (float)p->s * 0.01f;
    v->tdec = p->d ? tcode(p->d) * ks : 0.001f;
    v->trel = tcode(p->r) * (ks > 1.0f ? 1.0f : ks);
    /* the FM index: brighter when played harder */
    v->ix = (float)p->idx * 0.1f * (0.55f + 0.45f * vn);
    v->ix_to = v->ix * (float)p->isus * 0.01f;
    v->tix = p->idec ? tcode(p->idec) * ks : 0.0f;
    if (!p->idec)
        v->ix = v->ix_to;
    v->fe = 1.0f;
    v->tfe = tcode(p->fdec);
    v->pe = (float)p->pe;
    v->tpe = tcode(p->pdec);
    v->t = 0.0f;
    v->vgain = vn * (0.3f + 0.7f * vn) * (float)p->lvl * 0.01f;
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
        vo[q].trel = 0.01f;
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
            dr[i].on = 0;
}

static void drum_hit(int note, int vel)
{
    const gm_drum_t *d;
    drum_t *x = 0;
    float vn = (float)vel * (1.0f / 127.0f), f;
    int i;
    if (note < GM_DRUM_LO || note > GM_DRUM_HI)
        return;
    d = &GM_DRUM[note - GM_DRUM_LO];
    for (i = 0; i < GM_NDRUM; i++) {
        if (dr[i].on && d->choke && dr[i].d->choke == d->choke)
            dr[i].on = 0;                       /* the open hat stops when the closed one hits */
        if (dr[i].on && dr[i].note == note)
            x = &dr[i];                         /* the same drum again: that voice */
    }
    if (!x)
        for (i = 0; i < GM_NDRUM; i++)
            if (!dr[i].on) {
                x = &dr[i];
                break;
            }
    if (!x) {                                   /* the quietest */
        float q = 1e9f;
        for (i = 0; i < GM_NDRUM; i++)
            if (dr[i].tamp + dr[i].namp < q) {
                q = dr[i].tamp + dr[i].namp;
                x = &dr[i];
            }
    }
    x->on = 1;
    x->note = (uint8_t)note;
    x->d = d;
    x->t = 0.0f;
    x->gain = vn * (0.25f + 0.75f * vn);
    pan_gains(64 + d->pan, &x->pl, &x->pr);
    x->tamp = (float)d->tlvl * 0.01f;
    x->namp = (float)d->nlvl * 0.01f;
    x->tk = d->tdec ? tcode(d->tdec) : 0.001f;
    x->nk = d->ndec ? tcode(d->ndec) : 0.001f;
    x->pe = 1.0f;
    x->pk = d->pdec ? tcode(d->pdec) : 0.001f;
    for (i = 0; i < 6; i++)
        x->ph[i] = 0.0f;
    x->ic1 = x->ic2 = 0.0f;
    f = d->nf ? (float)d->nf : 1000.0f;
    if (f > 0.45f * GM_SR)
        f = 0.45f * GM_SR;
    x->g = fm_tanf(FM_PI * f * INV_SR);
    x->k = 10.0f / (d->nq ? (float)d->nq : 7.0f);
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

static inline float saw_blep(float t, float inc)    /* a saw, -1 .. 1, polyBLEP at the wrap */
{
    float s = 2.0f * t - 1.0f, d;
    if (t < inc) {
        d = t / inc;
        s -= d + d - d * d - 1.0f;
    } else if (t > 1.0f - inc) {
        d = (t - 1.0f) / inc;
        s -= d * d + d + d + 1.0f;
    }
    return s;
}

/* one melodic voice for n frames, added to the mix; 0 when it has finished */
static int voice_render(voice_t *v, uint32_t n)
{
    const gm_patch_t *p = v->p;
    const chan_t *c = &ch[v->ch];
    float dt = (float)n * INV_SR, a0 = v->amp, a1, da, f, inc1, inc2 = 0.0f, incm, vib, ix, g = 0.0f, k = 0.0f;
    float a1c = 0.0f, a2c = 0.0f, a3c = 0.0f, gl, gr, gain, trem = 1.0f, fb, o2;
    uint32_t j;
    int filt = p->cut != 0;
    /* amplitude */
    switch (v->st) {
    case ST_ATT:
        a1 = a0 + v->a_inc * (float)n;
        if (a1 >= 1.0f) {
            a1 = 1.0f;
            v->st = ST_DEC;
        }
        break;
    case ST_DEC:
        a1 = v->sus_l + (a0 - v->sus_l) * decay_k(v->tdec, n);
        break;
    default:
        a1 = a0 * decay_k(v->trel, n);
        break;
    }
    if ((v->st == ST_REL || (v->st == ST_DEC && v->sus_l == 0.0f)) && a1 < 3e-4f) {
        v->st = ST_FREE;
        a1 = 0.0f;
    }
    v->amp = a1;
    da = (a1 - a0) / (float)n;
    /* pitch: the note, bend, pitch envelope, vibrato (the patch's, faded in, and the mod wheel) */
    vib = (float)p->vib;
    if (!(p->flags & F_VIBNOW))
        vib *= fm_clampf((v->t - 0.25f) * 2.0f, 0.0f, 1.0f);
    vib += (float)c->mod * (50.0f / 127.0f);
    v->vlfo += 5.5f * dt;
    if (v->vlfo >= 1.0f)
        v->vlfo -= 1.0f;
    f = v->base + (float)c->bend * (float)c->bend_range * (1.0f / 8192.0f) + v->pe +
        vib * 0.01f * fm_sin_turns(v->vlfo);
    if (v->pe != 0.0f)
        v->pe *= decay_k(v->tpe, n);
    f = note_hz(f);
    inc1 = f * (float)p->rc * 0.5f * INV_SR;
    if (v->two)
        inc2 = inc1 * fm_exp2f(((float)p->iv * 100.0f + (float)p->det) * (1.0f / 1200.0f));
    incm = f * (float)p->rm * 0.5f * INV_SR;
    if (inc1 > 0.45f)
        inc1 = 0.45f;
    if (inc2 > 0.45f)
        inc2 = 0.45f;
    /* the FM index */
    if (v->tix > 0.0f)
        v->ix = v->ix_to + (v->ix - v->ix_to) * decay_k(v->tix, n);
    ix = v->ix * (1.0f / FM_TWO_PI);            /* in turns */
    if (p->flags & F_IXAMP)
        ix *= 0.3f + 0.7f * a1;
    fb = (float)p->fb * (0.01f / FM_TWO_PI);
    /* the filter */
    if (filt) {
        float cn = (float)p->cut + (v->base - 60.0f) * 0.5f + (float)p->fenv * v->fe + ((float)v->vel - 64.0f) * 0.1f;
        float fc = note_hz(cn), q;
        v->fe *= decay_k(v->tfe, n);
        if (fc > 0.42f * GM_SR)
            fc = 0.42f * GM_SR;
        if (fc < 20.0f)
            fc = 20.0f;
        g = fm_tanf(FM_PI * fc * INV_SR);
        q = (float)p->res * 0.01f;
        k = 2.0f - 1.9f * q;
        a1c = 1.0f / (1.0f + g * (g + k));
        a2c = g * a1c;
        a3c = g * a2c;
    }
    /* tremolo */
    if (p->trem) {
        v->tlfo += (float)p->trate * 0.1f * dt;
        v->tlfo -= fm_floorf(v->tlfo);
        trem = 1.0f - (float)p->trem * 0.005f * (1.0f - fm_sin_turns(v->tlfo));
    }
    v->t += dt;
    gain = v->vgain * chan_gain(c) * trem;
    pan_gains(c->pan, &gl, &gr);
    o2 = v->two ? (float)p->o2 * 0.01f : 0.0f;
    {   /* the source into src[], one tight loop per kind; then filter, amplitude and the mix */
        float ph1 = v->ph1, ph2 = v->ph2, phm = v->phm, fbz = v->fbz, ic1 = v->ic1, ic2 = v->ic2, amp = a0;
        float norm = 1.0f / (1.0f + o2), rev = (float)c->rev * (1.0f / 127.0f), lv = 0.0f, gwl, gwr, gwv;
        float *src = vsrc;
        switch (p->wave) {
        case WV_FM:
            if (o2 == 0.0f) {
                for (j = 0; j < n; j++) {
                    float m = sin_turns(phm + fbz * fb);
                    fbz = m;
                    src[j] = sin_turns(ph1 + m * ix);
                    ph1 += inc1;
                    ph1 -= (float)(ph1 >= 1.0f);
                    phm += incm;
                    phm -= fm_floorf(phm);
                }
            } else {
                for (j = 0; j < n; j++) {
                    float m = sin_turns(phm + fbz * fb);
                    fbz = m;
                    m *= ix;
                    src[j] = (sin_turns(ph1 + m) + o2 * sin_turns(ph2 + m)) * norm;
                    ph1 += inc1;
                    ph1 -= (float)(ph1 >= 1.0f);
                    ph2 += inc2;
                    ph2 -= (float)(ph2 >= 1.0f);
                    phm += incm;
                    phm -= fm_floorf(phm);
                }
            }
            break;
        case WV_SAW:
            for (j = 0; j < n; j++) {
                src[j] = saw_blep(ph1, inc1);
                ph1 += inc1;
                ph1 -= (float)(ph1 >= 1.0f);
            }
            if (o2 != 0.0f)
                for (j = 0; j < n; j++) {
                    src[j] = (src[j] + o2 * saw_blep(ph2, inc2)) * norm;
                    ph2 += inc2;
                    ph2 -= (float)(ph2 >= 1.0f);
                }
            break;
        case WV_PULSE: {
            float w = (float)p->rm * 0.01f, dc = (1.0f - 2.0f * w) * 0.5f, sh = 1.0f - w;
            for (j = 0; j < n; j++) {
                float t2 = ph1 + sh;
                t2 -= (float)(t2 >= 1.0f);
                src[j] = (saw_blep(ph1, inc1) - saw_blep(t2, inc1)) * 0.5f + dc;
                ph1 += inc1;
                ph1 -= (float)(ph1 >= 1.0f);
            }
            if (o2 != 0.0f)
                for (j = 0; j < n; j++) {
                    src[j] = (src[j] + o2 * 0.5f * saw_blep(ph2, inc2)) * norm;
                    ph2 += inc2;
                    ph2 -= (float)(ph2 >= 1.0f);
                }
            break;
        }
        default:
            for (j = 0; j < n; j++)
                src[j] = noise();
            break;
        }
        if (p->flags & F_BREATH) {              /* soft noise: low-passed, about 4 kHz */
            static float bz;
            for (j = 0; j < n; j++) {
                bz += 0.45f * (noise() - bz);
                src[j] += bz * 0.09f;
            }
            bz = fm_flush(bz);
        }
        if (filt) {                             /* TPT state-variable filter */
            if (p->flags & F_HP) {
                for (j = 0; j < n; j++) {
                    float x = src[j], v3 = x - ic2, v1 = a1c * ic1 + a2c * v3, v2 = ic2 + a2c * ic1 + a3c * v3;
                    ic1 = 2.0f * v1 - ic1;
                    ic2 = 2.0f * v2 - ic2;
                    src[j] = x - k * v1 - v2;
                }
            } else if (p->flags & F_BP) {
                for (j = 0; j < n; j++) {
                    float v3 = src[j] - ic2, v1 = a1c * ic1 + a2c * v3, v2 = ic2 + a2c * ic1 + a3c * v3;
                    ic1 = 2.0f * v1 - ic1;
                    ic2 = 2.0f * v2 - ic2;
                    src[j] = v1 * k;
                }
            } else {
                for (j = 0; j < n; j++) {
                    float v3 = src[j] - ic2, v1 = a1c * ic1 + a2c * v3, v2 = ic2 + a2c * ic1 + a3c * v3;
                    ic1 = 2.0f * v1 - ic1;
                    ic2 = 2.0f * v2 - ic2;
                    src[j] = v2;
                }
            }
        }
        if (p->flags & F_DRIVE)
            for (j = 0; j < n; j++)
                src[j] = fm_tanhf(src[j] * 3.0f) * 0.6f;
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
        lv = (a0 + a1) * 0.5f * gain * 0.6f;    /* the meter: the envelope, not the waveform */
        v->ph1 = ph1;
        v->ph2 = ph2;
        v->phm = phm;
        v->fbz = fbz;
        v->ic1 = fm_flush(ic1);
        v->ic2 = fm_flush(ic2);
        ch[v->ch].lvl += lv;
    }
    return v->st != ST_FREE;
}

static int drum_render(drum_t *x, uint32_t n)
{
    const gm_drum_t *d = x->d;
    const chan_t *c = &ch[GM_DRUM_CH];
    float dt = (float)n * INV_SR, t0 = x->tamp, n0 = x->namp, t1, n1, dta, dna, f, inc, gain, gl, gr, cl, cr;
    float g = x->g, k = x->k, a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2, rev, lv = 0.0f;
    float ic1 = x->ic1, ic2 = x->ic2;
    uint32_t j, i;
    int tone = d->tone, ftone = d->flags & D_FTONE, nm = d->nmode;
    t1 = t0 * decay_k(x->tk, n);
    n1 = n0 * decay_k(x->nk, n);
    if (t1 < 2e-4f && n1 < 2e-4f) {
        x->on = 0;
        return 0;
    }
    dta = (t1 - t0) / (float)n;
    dna = (n1 - n0) / (float)n;
    f = (float)d->f1 + ((float)d->f0 - (float)d->f1) * x->pe;
    x->pe *= decay_k(x->pk, n);
    if (d->flags & D_WOBBLE)
        f *= 1.0f + 0.04f * fm_sin_turns(x->t * 22.0f);
    inc = f * INV_SR;
    {   /* the noise's shape: clap bursts, rattle */
        float gate = 1.0f;
        if (d->flags & D_CLAP && x->t < 0.03f) {
            float ph = x->t * 100.0f;
            gate = 1.6f * (1.0f - (ph - fm_floorf(ph)));
        } else if (d->flags & D_RATTLE) {
            float ph = x->t * 35.0f;
            gate = ph - fm_floorf(ph) < 0.4f ? 1.0f : 0.15f;
        }
        n0 *= gate;
        dna *= gate;
    }
    gain = x->gain * chan_gain(c);
    pan_gains(c->pan, &cl, &cr);                /* the channel's pan moves the whole kit */
    gl = x->pl * cl * 1.41f;
    gr = x->pr * cr * 1.41f;
    rev = (float)c->rev * (1.0f / 127.0f);
    for (j = 0; j < n; j++) {
        float ts = 0.0f, ns = 0.0f, s, out;
        if (tone == T_SINE) {
            ts = sin_turns(x->ph[0]);
            x->ph[0] += inc;
            if (x->ph[0] >= 1.0f)
                x->ph[0] -= 1.0f;
        } else if (tone == T_METAL) {
            for (i = 0; i < 6; i++) {
                ts += x->ph[i] < 0.5f ? 0.25f : -0.25f;
                x->ph[i] += inc * METAL_R[i];
                if (x->ph[i] >= 1.0f)
                    x->ph[i] -= 1.0f;
            }
        } else if (tone == T_SQ2) {
            ts = (x->ph[0] < 0.5f ? 0.5f : -0.5f) + (x->ph[1] < 0.5f ? 0.5f : -0.5f);
            x->ph[0] += inc;
            x->ph[1] += inc * 1.5085f;
            if (x->ph[0] >= 1.0f)
                x->ph[0] -= 1.0f;
            if (x->ph[1] >= 1.0f)
                x->ph[1] -= 1.0f;
        }
        ts *= t0;
        t0 += dta;
        if (n0 > 0.0f)
            ns = noise() * n0;
        n0 += dna;
        s = ftone ? ts + ns : ns;
        if (nm != N_NONE) {
            float v3 = s - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            s = nm == N_LP ? v2 : nm == N_BP ? v1 * k : s - k * v1 - v2;
        }
        if (!ftone)
            s += ts;
        out = s * gain;
        mix_l[j] += out * gl;
        mix_r[j] += out * gr;
        mix_rev[j] += out * rev;
        lv += fm_fabsf(out);
    }
    x->ic1 = fm_flush(ic1);
    x->ic2 = fm_flush(ic2);
    x->tamp = t1;
    x->namp = n1;
    x->t += dt;
    ch[GM_DRUM_CH].lvl += lv / (float)n;
    return 1;
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
        if (dr[i].on)
            nd += (uint32_t)drum_render(&dr[i], n);
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
