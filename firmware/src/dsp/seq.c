/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM sequencer (seq.h). One compilation unit, -O2 (tools/build.py O2_UNITS); it runs in the
 * audio interrupt, as the engine does.
 *
 * Playback: each track keeps pos[t], the index of its first note not yet played this pass (its
 * notes before pos[t] have ticks <= the last tick played). A tick plays the note-offs due, then
 * every audible track's notes at that tick. The loop end cuts what still sounds.
 * Recording: a note's start is the tick nearest the moment it was played, quantized; it is written
 * when it ends (its length known), so it lands behind the playback position and does not play
 * twice; a note quantized ahead of the position is stepped over for this pass. */
#include "seq.h"
#include "fastmath.h"

song_t seq_song GM_POOL;
static song_t undo GM_POOL;
seq_cfg_t seq_cfg = {0, 0, 1, 6, 1};
volatile uint8_t seq_lock;
volatile uint8_t seq_playing, seq_rec, seq_step, seq_counting, seq_full, seq_sel, seq_undo_ok;
volatile uint16_t seq_tick, seq_step_tick;
volatile uint32_t seq_changes;

static const uint8_t QT[SEQ_NQUANT] = {1, 48, 24, 16, 12, 8, 6};
static const char *const QN[SEQ_NQUANT] = {"OFF", "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
int seq_quant_ticks(int q) { return QT[(unsigned)q < SEQ_NQUANT ? q : 0]; }
const char *seq_quant_name(int q) { return QN[(unsigned)q < SEQ_NQUANT ? q : 0]; }

/* ------------------------------------------------------------- commands --- */
typedef struct {
    uint8_t op, a, b, c;
    int32_t w;
} cmd_t;
#define NCMD 256u
static cmd_t cmdq[NCMD];
static volatile uint32_t cq_w, cq_r;

int seq_post(uint32_t op, uint32_t a, uint32_t b, uint32_t c, int32_t w)
{
    cmd_t *q;
    if (cq_w - cq_r >= NCMD)
        return -1;
    q = &cmdq[cq_w % NCMD];
    q->op = (uint8_t)op;
    q->a = (uint8_t)a;
    q->b = (uint8_t)b;
    q->c = (uint8_t)c;
    q->w = w;
    __asm__ volatile("" ::: "memory");          /* the slot before the index */
    cq_w++;
    return 0;
}
int seq_idle(void) { return cq_w == cq_r; }

/* --------------------------------------------------------------- song --- */
static const uint8_t DEF_PROG[SEQ_NTRK] = {0, 33, 48, 4, 24, 16, 61, 73, 81, 0, 89, 11, 52, 27, 65, 38};

void song_defaults(song_t *s)
{
    uint32_t i;
    uint8_t *b = (uint8_t *)s;
    for (i = 0; i < sizeof *s; i++)
        b[i] = 0;
    s->magic = SONG_MAGIC;
    s->format = SONG_FORMAT;
    s->tempo = 120;
    s->bars = 4;
    s->quant = 4;                               /* 1/16 */
    for (i = 0; i < SEQ_NTRK; i++) {
        seq_trk_t *t = &s->trk[i];
        t->chan = (uint8_t)i;
        t->prog = DEF_PROG[i];
        t->vol = 100;
        t->pan = 64;
        t->rev = 40;
        t->vel = 100;
    }
}

int song_valid(const song_t *s, uint32_t len)
{
    uint32_t i, n = 0;
    if (len < SONG_HEAD_BYTES || s->magic != SONG_MAGIC || s->format != SONG_FORMAT)
        return 0;
    if (s->tempo < 30 || s->tempo > 300 || s->bars < 1 || s->bars > SEQ_MAXBARS || s->quant >= SEQ_NQUANT ||
        s->nev > SEQ_MAXEV || len < song_bytes(s))
        return 0;
    for (i = 0; i < SEQ_NTRK; i++) {
        const seq_trk_t *t = &s->trk[i];
        if (t->chan > 15 || t->prog > 127 || t->vol > 127 || t->pan > 127 || t->rev > 127 || t->vel < 1 ||
            t->vel > 127 || t->mute > 1 || t->solo > 1 || t->transpose < -24 || t->transpose > 24)
            return 0;
        n += t->nev;
    }
    if (n != s->nev)
        return 0;
    for (i = 0; i < s->nev; i++) {
        const seq_ev_t *e = &s->ev[i];
        if (e->note > 127 || e->vel < 1 || e->vel > 127 || e->len < 1 || e->tick >= SEQ_MAXBARS * SEQ_BAR)
            return 0;
    }
    return 1;
}

static song_t *const S = &seq_song;
uint32_t seq_song_ticks(void) { return (uint32_t)S->bars * SEQ_BAR; }

static uint32_t trk_start(int t)
{
    uint32_t i, k = 0;
    for (i = 0; i < (uint32_t)t; i++)
        k += S->trk[i].nev;
    return k;
}

static void copy_song(song_t *d, const song_t *s)
{
    uint32_t n = song_bytes(s) / 4u, i;          /* both are word-aligned; the size is a multiple of 2 */
    const uint32_t *a = (const uint32_t *)s;
    uint32_t *b = (uint32_t *)d;
    for (i = 0; i < n; i++)
        b[i] = a[i];
    if (song_bytes(s) & 2u)
        ((uint16_t *)d)[2u * n] = ((const uint16_t *)s)[2u * n];
}

static void snapshot(void)                      /* the undo copy: before a recording pass or an erase */
{
    copy_song(&undo, S);
    seq_undo_ok = 1;
}

static void changed(void) { seq_changes++; }

/* ------------------------------------------------------------- output --- */
static void out3(uint32_t st, uint32_t d1, uint32_t d2)   /* the engine, and MIDI out when it is on */
{
    gm_midi(st, d1, d2);
    if (seq_cfg.midi_out)
        seq_midi_out(st, d1, d2);
}

static void apply_track(int t)
{
    const seq_trk_t *k = &S->trk[t];
    uint32_t c = k->chan;
    if (c != GM_DRUM_CH)
        out3(0xC0u | c, k->prog, 0);
    out3(0xB0u | c, 7, k->vol);
    out3(0xB0u | c, 10, k->pan);
    out3(0xB0u | c, 91, k->rev);
}

static void apply_all(void)
{
    int t;
    for (t = SEQ_NTRK - 1; t >= 0; t--)         /* tracks sharing a channel: the lowest one wins */
        apply_track(t);
}

/* ---------------------------------------------------------- playback --- */
#define NACT 96
typedef struct {
    uint8_t on, trk, chan, note;
    uint32_t off;                               /* the absolute tick it ends at */
} act_t;
static act_t act[NACT];
static uint16_t pos[SEQ_NTRK];
static uint32_t cur;                            /* the next tick to play */
static uint32_t last = 0xFFFFFFFFu;             /* the tick played last (none yet) */
static uint32_t abs_t;                          /* ticks played since PLAY: the note-offs' clock */
static float acc, spt;                          /* samples since the last tick; samples per tick */
static uint32_t preroll;                        /* count-in ticks left */
static float click_amp, click_ph, click_inc;

static float samples_per_tick(void) { return GM_SR * 60.0f / ((float)S->tempo * (float)SEQ_PPQN); }

static int audible(int t)
{
    int i, solo = 0;
    for (i = 0; i < SEQ_NTRK; i++)
        solo |= S->trk[i].solo;
    return solo ? S->trk[t].solo : !S->trk[t].mute;
}

static int out_note(const seq_trk_t *k, int note)
{
    if (k->chan != GM_DRUM_CH)
        note += k->transpose;
    return note < 0 ? 0 : note > 127 ? 127 : note;
}

static void act_off(act_t *a)
{
    if (a->on) {
        out3(0x80u | a->chan, a->note, 0);
        a->on = 0;
    }
}

static void tracks_off(int t)                   /* t < 0: every track */
{
    int i;
    for (i = 0; i < NACT; i++)
        if (act[i].on && (t < 0 || act[i].trk == t))
            act_off(&act[i]);
}

static void act_add(int t, int chan, int note, uint32_t off)
{
    int i, best = 0;
    for (i = 0; i < NACT; i++)                  /* the same note again on that channel: end it first */
        if (act[i].on && act[i].chan == chan && act[i].note == note)
            act_off(&act[i]);
    for (i = 0; i < NACT; i++) {
        if (!act[i].on) {
            best = i;
            break;
        }
        if (act[i].off < act[best].off)
            best = i;
    }
    act_off(&act[best]);
    act[best].on = 1;
    act[best].trk = (uint8_t)t;
    act[best].chan = (uint8_t)chan;
    act[best].note = (uint8_t)note;
    act[best].off = off;
}

static void seek(uint32_t tick)                 /* pos[] for "the next tick is tick" */
{
    uint32_t t, s = 0;
    for (t = 0; t < SEQ_NTRK; t++) {
        uint32_t e = s + S->trk[t].nev, i = s;
        while (i < e && S->ev[i].tick < tick)
            i++;
        pos[t] = (uint16_t)i;
        s = e;
    }
}

static void click(int down)
{
    if (!seq_cfg.metro_vol || (!preroll && (seq_cfg.metro == 0 || (seq_cfg.metro == 1 && !seq_rec))))
        return;                                 /* the count-in always clicks */
    click_amp = (float)seq_cfg.metro_vol * 0.03f * (down ? 1.0f : 0.6f);
    click_ph = 0.0f;
    click_inc = (down ? 2000.0f : 1500.0f) / GM_SR;
}

static void play_tick(void)
{
    uint32_t t, s = 0, len = seq_song_ticks(), i;
    if (preroll) {                              /* the count-in: clicks only */
        uint32_t k = SEQ_BAR - preroll;
        if (k % SEQ_PPQN == 0)
            click(k == 0);
        if (--preroll == 0) {
            seq_counting = 0;
            if (seq_cfg.clock_out)
                seq_midi_out(0xFA, 0, 0);
        }
        return;
    }
    if (cur >= len) {                           /* the loop: back to the top */
        cur = 0;
        tracks_off(-1);
    }
    if (cur == 0)
        seek(0);
    for (i = 0; i < NACT; i++)
        if (act[i].on && act[i].off <= abs_t)
            act_off(&act[i]);
    for (t = 0; t < SEQ_NTRK; t++) {
        const seq_trk_t *k = &S->trk[t];
        uint32_t e = s + k->nev;
        int on = audible((int)t);
        while (pos[t] < e && S->ev[pos[t]].tick <= cur) {
            const seq_ev_t *ev = &S->ev[pos[t]];
            if (on && ev->tick == cur) {
                int n = out_note(k, ev->note);
                out3(0x90u | k->chan, (uint32_t)n, ev->vel);
                act_add((int)t, k->chan, n, abs_t + ev->len);
            }
            pos[t]++;
        }
        s = e;
    }
    if (cur % SEQ_PPQN == 0)
        click(cur % SEQ_BAR == 0);
    if (seq_cfg.clock_out && cur % 2u == 0)    /* 24 a quarter note */
        seq_midi_out(0xF8, 0, 0);
    seq_tick = (uint16_t)cur;
    last = cur;
    cur++;
    abs_t++;
}

static void start(void)
{
    uint32_t i;
    for (i = 0; i < NACT; i++)
        act[i].on = 0;
    cur = 0;
    last = 0xFFFFFFFFu;
    abs_t = 0;
    spt = samples_per_tick();
    acc = spt;                                  /* the first tick now */
    seek(0);
    apply_all();
    seq_playing = 1;
    preroll = 0;
    seq_counting = 0;
    if (seq_rec) {
        snapshot();
        if (seq_cfg.countin) {
            preroll = SEQ_BAR;
            seq_counting = 1;
        }
    }
    if (seq_cfg.clock_out && !preroll)
        seq_midi_out(0xFA, 0, 0);
}

/* --------------------------------------------------------- recording --- */
typedef struct {
    uint8_t on, trk, vel;
    uint16_t tick;                              /* where it lands (quantized) */
    uint32_t abs;                               /* when it started: abs_t and the fraction */
    float frac;
} pend_t;
static pend_t pend[128];

static int quant(void) { return seq_quant_ticks(S->quant); }

/* insert (or, the same note at the same tick, replace) a note in track t */
static void ev_insert(int t, uint32_t tick, int note, int vel, uint32_t len)
{
    uint32_t s = trk_start(t), e = s + S->trk[t].nev, i, j, k;
    if (len < 1)
        len = 1;
    if (len > 0xFFFFu)
        len = 0xFFFFu;
    for (i = s; i < e && S->ev[i].tick <= tick; i++)
        if (S->ev[i].tick == tick && S->ev[i].note == note) {
            S->ev[i].vel = (uint8_t)vel;
            S->ev[i].len = (uint16_t)len;
            changed();
            return;
        }
    if (S->nev >= SEQ_MAXEV) {
        seq_full = 1;
        return;
    }
    for (j = S->nev; j > i; j--)
        S->ev[j] = S->ev[j - 1];
    S->ev[i].tick = (uint16_t)tick;
    S->ev[i].note = (uint8_t)note;
    S->ev[i].vel = (uint8_t)vel;
    S->ev[i].len = (uint16_t)len;
    S->nev++;
    S->trk[t].nev++;
    for (k = (uint32_t)t + 1; k < SEQ_NTRK; k++)
        pos[k]++;
    /* behind the playback position (or quantized just ahead of it): not again this pass */
    if (i < pos[t] || (i == pos[t] && seq_playing && (last == 0xFFFFFFFFu || tick <= last + (uint32_t)quant())))
        pos[t]++;
    changed();
}

static void ev_remove(uint32_t i, int t)
{
    uint32_t j, k;
    for (j = i; j + 1u < S->nev; j++)
        S->ev[j] = S->ev[j + 1];
    S->nev--;
    S->trk[t].nev--;
    for (k = (uint32_t)t + 1; k < SEQ_NTRK; k++)
        pos[k]--;
    if (i < pos[t])
        pos[t]--;
    seq_full = 0;
    changed();
}

static void erase_track(int t)
{
    uint32_t s = trk_start(t), n = S->trk[t].nev, j, k;
    if (!n)
        return;
    tracks_off(t);
    for (j = s; j + n < S->nev; j++)
        S->ev[j] = S->ev[j + n];
    S->nev = (uint16_t)(S->nev - n);
    S->trk[t].nev = 0;
    for (k = 0; k < SEQ_NTRK; k++)
        pos[k] = (uint16_t)(k < (uint32_t)t ? pos[k] : k == (uint32_t)t ? s : pos[k] - n);
    seq_full = 0;
    changed();
}

/* the position now, in ticks: the tick played last plus the fraction since */
static void now_pos(uint32_t *tick, uint32_t *abs, float *frac)
{
    *frac = spt > 0.0f ? acc / spt : 0.0f;
    if (*frac > 1.0f)
        *frac = 1.0f;
    *tick = last == 0xFFFFFFFFu ? 0u : last;
    *abs = abs_t ? abs_t - 1u : 0u;
}

static uint32_t step_len(void)
{
    uint32_t q = (uint32_t)quant();
    return q < 6u ? 12u : q;                    /* quantize off: steps of a 1/16 */
}
static uint32_t step_held[4];                   /* the keys down while step recording (128 bits) */
static uint8_t step_any_note;                   /* a note went in at this step */
static int held_any(void) { return step_held[0] | step_held[1] | step_held[2] | step_held[3]; }

static void rec_on(int t, int note, int vel)
{
    pend_t *p = &pend[note & 127];
    uint32_t tick, abs, len = seq_song_ticks(), q = (uint32_t)quant(), r;
    float frac;
    if (seq_step) {                             /* step recording: at the cursor, a step long */
        uint32_t sl = step_len();
        ev_insert(t, seq_step_tick, note, vel, sl - sl / 8u);
        step_held[(note >> 5) & 3] |= 1u << (note & 31);
        step_any_note = 1;
        return;
    }
    if (!seq_rec || !seq_playing || preroll)
        return;
    now_pos(&tick, &abs, &frac);
    r = tick + (frac >= 0.5f);
    if (q > 1u)
        r = (r + q / 2u) / q * q;
    p->on = 1;
    p->trk = (uint8_t)t;
    p->vel = (uint8_t)vel;
    p->tick = (uint16_t)(r % len);
    p->abs = abs;
    p->frac = frac;
}

static void rec_off(int note)
{
    pend_t *p = &pend[note & 127];
    uint32_t tick, abs, len;
    float frac, d;
    if (seq_step) {
        step_held[(note >> 5) & 3] &= ~(1u << (note & 31));
        if (!held_any() && step_any_note) {     /* the chord is let go: the cursor moves on */
            step_any_note = 0;
            seq_step_tick = (uint16_t)((seq_step_tick + step_len()) % seq_song_ticks());
        }
        return;
    }
    if (!p->on)
        return;
    p->on = 0;
    now_pos(&tick, &abs, &frac);
    d = (float)(abs - p->abs) + (frac - p->frac);
    len = d < 1.0f ? 1u : (uint32_t)(d + 0.5f);
    if (len > seq_song_ticks())
        len = seq_song_ticks();
    ev_insert(p->trk, p->tick, note, p->vel, len);
}

static void rec_flush(void)                     /* stop: the notes still held end now */
{
    int n;
    for (n = 0; n < 128; n++)
        if (pend[n].on)
            rec_off(n);
}

static void step_back(void)
{
    uint32_t sl = step_len(), len = seq_song_ticks(), t0, s, e, i;
    int t = seq_sel;
    t0 = (seq_step_tick + len - sl) % len;
    seq_step_tick = (uint16_t)t0;
    s = trk_start(t);
    e = s + S->trk[t].nev;
    for (i = s; i < e;) {
        if (S->ev[i].tick >= t0 && S->ev[i].tick < t0 + sl) {
            ev_remove(i, t);
            e--;
        } else {
            i++;
        }
    }
}

/* ------------------------------------------------------------ commands --- */
static void track_param(int t, int p, int32_t v)
{
    seq_trk_t *k = &S->trk[t];
    switch (p) {
    case TP_CHAN:
        tracks_off(t);
        k->chan = (uint8_t)(v & 15);
        apply_track(t);
        break;
    case TP_PROG:
        k->prog = (uint8_t)(v & 127);
        if (k->chan != GM_DRUM_CH)
            out3(0xC0u | k->chan, k->prog, 0);
        break;
    case TP_VOL:
        k->vol = (uint8_t)fm_clampf((float)v, 0.0f, 127.0f);
        out3(0xB0u | k->chan, 7, k->vol);
        break;
    case TP_PAN:
        k->pan = (uint8_t)fm_clampf((float)v, 0.0f, 127.0f);
        out3(0xB0u | k->chan, 10, k->pan);
        break;
    case TP_REV:
        k->rev = (uint8_t)fm_clampf((float)v, 0.0f, 127.0f);
        out3(0xB0u | k->chan, 91, k->rev);
        break;
    case TP_VEL:
        k->vel = (uint8_t)fm_clampf((float)v, 1.0f, 127.0f);
        break;
    case TP_MUTE:
    case TP_SOLO: {
        int i;
        if (p == TP_MUTE)
            k->mute = (uint8_t)(v != 0);
        else
            k->solo = (uint8_t)(v != 0);
        for (i = 0; i < SEQ_NTRK; i++)
            if (!audible(i))
                tracks_off(i);
        break;
    }
    case TP_TRANSPOSE:
        tracks_off(t);
        k->transpose = (int8_t)fm_clampf((float)v, -24.0f, 24.0f);
        break;
    default:
        return;
    }
    changed();
}

static void song_param(int p, int32_t v)
{
    switch (p) {
    case SP_TEMPO:
        S->tempo = (uint16_t)fm_clampf((float)v, 30.0f, 300.0f);
        spt = samples_per_tick();
        if (acc > spt)
            acc = spt;
        break;
    case SP_BARS:
        S->bars = (uint8_t)fm_clampf((float)v, 1.0f, (float)SEQ_MAXBARS);
        if (seq_step_tick >= seq_song_ticks())
            seq_step_tick = 0;
        break;
    case SP_QUANT:
        S->quant = (uint8_t)((uint32_t)v < SEQ_NQUANT ? v : 0);
        break;
    default:
        return;
    }
    changed();
}

static void live_on(int t, int note, int vel)
{
    const seq_trk_t *k = &S->trk[t];
    out3(0x90u | k->chan, (uint32_t)out_note(k, note), (uint32_t)vel);
    rec_on(t, note, vel);
}

static void live_off(int t, int note)
{
    const seq_trk_t *k = &S->trk[t];
    out3(0x80u | k->chan, (uint32_t)out_note(k, note), 0);
    rec_off(note);
}

static void stop(void)
{
    rec_flush();
    tracks_off(-1);
    if (seq_playing && seq_cfg.clock_out)
        seq_midi_out(0xFC, 0, 0);
    seq_playing = 0;
    seq_counting = 0;
    preroll = 0;
    seq_tick = 0;
}

static void exec(const cmd_t *c)
{
    int t = c->a & 15;
    switch (c->op) {
    case SC_NOTE_ON:
        live_on(t, c->b & 127, c->c ? c->c & 127 : 1);
        break;
    case SC_NOTE_OFF:
        live_off(t, c->b & 127);
        break;
    case SC_MIDI: {
        uint32_t st = c->a, ty = st & 0xF0u;
        out3(st, c->b, c->c);
        if ((ty == 0x90u || ty == 0x80u) && (st & 15u) == S->trk[seq_sel].chan) {
            if (ty == 0x90u && c->c)
                rec_on(seq_sel, c->b & 127, c->c & 127);
            else
                rec_off(c->b & 127);
        }
        break;
    }
    case SC_PLAY:
        seq_step = 0;
        start();
        break;
    case SC_STOP:
        stop();
        break;
    case SC_REC:
        if (c->a && !seq_rec && seq_playing)
            snapshot();                         /* punching in while it plays */
        if (!c->a)
            rec_flush();
        seq_rec = (uint8_t)(c->a != 0);
        break;
    case SC_STEP:
        if (c->a && !seq_playing) {
            snapshot();
            seq_step = 1;
            step_held[0] = step_held[1] = step_held[2] = step_held[3] = 0;
            step_any_note = 0;
            if (seq_step_tick >= seq_song_ticks())
                seq_step_tick = 0;
        } else {
            seq_step = 0;
        }
        break;
    case SC_STEP_REST:
        if (seq_step)
            seq_step_tick = (uint16_t)((seq_step_tick + step_len()) % seq_song_ticks());
        break;
    case SC_STEP_BACK:
        if (seq_step)
            step_back();
        break;
    case SC_SELECT:
        if (t != seq_sel)
            rec_flush();
        seq_sel = (uint8_t)t;
        break;
    case SC_TRK:
        track_param(t, c->b, c->w);
        break;
    case SC_SONG:
        song_param(c->a, c->w);
        break;
    case SC_ERASE:
        snapshot();
        erase_track(t);
        break;
    case SC_UNDO:
        if (seq_undo_ok) {
            tracks_off(-1);
            rec_flush();
            {                                   /* swap, word by word: undo again is redo */
                uint32_t n = sizeof(song_t) / 4u, i;
                uint32_t *a = (uint32_t *)S, *b = (uint32_t *)&undo;
                for (i = 0; i < n; i++) {
                    uint32_t x = a[i];
                    a[i] = b[i];
                    b[i] = x;
                }
            }
            seek(seq_playing ? cur : 0);
            spt = samples_per_tick();
            apply_all();
            seq_full = 0;
            changed();
        }
        break;
    case SC_APPLY:
        tracks_off(-1);
        seek(seq_playing ? cur : 0);
        spt = samples_per_tick();
        apply_all();
        break;
    case SC_PANIC:
        tracks_off(-1);
        gm_panic();
        break;
    case SC_GMRESET:
        tracks_off(-1);
        gm_reset();
        apply_all();
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------- render --- */
void seq_init(void)
{
    song_defaults(&seq_song);
    seq_undo_ok = 0;
    gm_init();
    apply_all();
}

void seq_render(int32_t *out, uint32_t n, uint32_t gain_q12)
{
    static float l[GM_CHUNK], r[GM_CHUNK];
    const float gain = (float)gain_q12 * (1.0f / 4096.0f) * 1.2f;
    if (!seq_lock)
        while (cq_r != cq_w) {
            exec(&cmdq[cq_r % NCMD]);
            cq_r++;
        }
    while (n) {
        uint32_t k = n > GM_CHUNK ? GM_CHUNK : n, j;
        if (seq_playing && !seq_lock) {
            acc += (float)k;
            while (acc >= spt && seq_playing) {
                acc -= spt;
                play_tick();
            }
        }
        for (j = 0; j < k; j++)
            l[j] = r[j] = 0.0f;
        gm_render(l, r, k);
        if (click_amp > 1e-4f) {
            for (j = 0; j < k; j++) {
                float c = click_amp * fm_sin_turns(click_ph);
                click_ph += click_inc;
                click_amp *= 0.9985f;
                l[j] += c;
                r[j] += c;
            }
        }
        for (j = 0; j < k; j++) {
            out[2 * j] = (int32_t)(fm_tanhf(l[j] * gain) * 8300000.0f);
            out[2 * j + 1] = (int32_t)(fm_tanhf(r[j] * gain) * 8300000.0f);
        }
        out += 2 * k;
        n -= k;
    }
}
