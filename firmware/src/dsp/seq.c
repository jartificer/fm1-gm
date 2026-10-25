/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM sequencer (seq.h). One compilation unit, -O2 (tools/build.py O2_UNITS); it runs in the
 * audio interrupt, as the engine does.
 *
 * Playback: each track keeps pos[t], the index of its first note not yet played this pass (its
 * notes before pos[t] have ticks <= the last tick played). A tick plays the note-offs due, then
 * every audible track's notes at that tick. The loop end cuts what still sounds.
 * Recording: a note's start is the tick nearest the moment it was played, quantized; it is written
 * when it ends (its length known), so it lands behind the playback position and does not play
 * twice; a note quantized ahead of the position is stepped over for this pass. A key pressed in
 * the count-in and still held when recording begins starts at the top.
 * The loop is the song, or one bar (SC_LOOP): playback, recording and the step cursor wrap in it. */
#include "seq.h"
#include "fastmath.h"

song_t seq_song GM_POOL;
static song_t undo GM_POOL;
seq_cfg_t seq_cfg = {0, 0, 1, 6, 1};
volatile uint8_t seq_lock;
volatile uint8_t seq_playing, seq_rec, seq_step, seq_counting, seq_full, seq_sel, seq_undo_ok;
volatile uint16_t seq_tick, seq_step_tick;
volatile uint32_t seq_changes;
volatile uint8_t seq_loop, seq_loop_bar;
volatile uint16_t seq_clip_n;
static seq_ev_t clip[SEQ_CLIP] GM_POOL;         /* ticks from the bar's start */

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

/* ------------------------------------------------------- the stream --- *
 *   format (SONG_FORMAT), tempo (2 bytes, low first), bars, quantize;
 *   16 tracks: channel, program, volume, pan, reverb, velocity, mute | solo << 1, transpose;
 *   16 tracks: notes (a variable-length number), then each note: ticks since the note before (the
 *   first: since 0), the note | 0x80 when its length is the note before's, else its length.
 * Variable-length numbers are MIDI's: 7 bits a byte, the high first, bit 7 set on all but the last. */
typedef struct {
    uint8_t *d;
    uint32_t from, n, i;
} wr_t;

static void put(wr_t *w, uint32_t b)
{
    if (w->d && w->i >= w->from && w->i - w->from < w->n)
        w->d[w->i - w->from] = (uint8_t)b;
    w->i++;
}

static void put_vlq(wr_t *w, uint32_t v)
{
    uint8_t b[5];
    int k = 0;
    do
        b[k++] = (uint8_t)(v & 127u);
    while ((v >>= 7) != 0);
    while (k--)
        put(w, b[k] | (k ? 128u : 0u));
}

uint32_t song_stream(const song_t *s, uint8_t *dst, uint32_t from, uint32_t n)
{
    wr_t w = {dst, from, n, 0};
    uint32_t t, e = 0;
    put(&w, SONG_FORMAT);
    put(&w, s->tempo & 255u);
    put(&w, s->tempo >> 8);
    put(&w, s->bars);
    put(&w, s->quant);
    for (t = 0; t < SEQ_NTRK; t++) {
        const seq_trk_t *k = &s->trk[t];
        put(&w, k->chan);
        put(&w, k->prog);
        put(&w, k->vol);
        put(&w, k->pan);
        put(&w, k->rev);
        put(&w, k->vel);
        put(&w, (uint32_t)k->mute | (uint32_t)k->solo << 1);
        put(&w, (uint8_t)k->transpose);
    }
    for (t = 0; t < SEQ_NTRK; t++) {
        uint32_t j, prev = 0, plen = 0, ne = s->trk[t].nev;
        put_vlq(&w, ne);
        for (j = 0; j < ne && e < SEQ_MAXEV; j++, e++) {
            const seq_ev_t *v = &s->ev[e];
            put_vlq(&w, v->tick >= prev ? v->tick - prev : 0u);
            put(&w, v->note | (v->len == plen ? 128u : 0u));
            if (v->len != plen)
                put_vlq(&w, v->len);
            prev = v->tick;
            plen = v->len;
        }
    }
    return w.i;
}

static int (*rd)(void);
static uint8_t rd_bad;

static uint32_t get8(void)
{
    int c = rd_bad ? -2 : rd();
    if (c < 0) {
        rd_bad = 1;
        return 0;
    }
    return (uint32_t)c;
}

static uint32_t get_vlq(void)
{
    uint32_t v = 0, k;
    for (k = 0; k < 3u; k++) {                  /* nothing in a song needs more than 21 bits */
        uint32_t c = get8();
        v = v << 7 | (c & 127u);
        if (!(c & 128u))
            return v;
    }
    rd_bad = 1;
    return 0;
}

int song_parse(song_t *s, int (*get)(void))
{
    uint32_t t, e = 0;
    rd = get;
    rd_bad = 0;
    song_defaults(s);
    if (get8() != SONG_FORMAT)
        return -1;
    s->tempo = (uint16_t)get8();
    s->tempo = (uint16_t)(s->tempo | get8() << 8);
    s->bars = (uint8_t)get8();
    s->quant = (uint8_t)get8();
    if (s->tempo < 30 || s->tempo > 300 || s->bars < 1 || s->bars > SEQ_MAXBARS || s->quant >= SEQ_NQUANT)
        return -1;
    for (t = 0; t < SEQ_NTRK; t++) {
        seq_trk_t *k = &s->trk[t];
        uint32_t f;
        k->chan = (uint8_t)get8();
        k->prog = (uint8_t)get8();
        k->vol = (uint8_t)get8();
        k->pan = (uint8_t)get8();
        k->rev = (uint8_t)get8();
        k->vel = (uint8_t)get8();
        f = get8();
        k->mute = (uint8_t)(f & 1u);
        k->solo = (uint8_t)(f >> 1 & 1u);
        k->transpose = (int8_t)(uint8_t)get8();
        if (k->chan > 15 || k->prog > 127 || k->vol > 127 || k->pan > 127 || k->rev > 127 || k->vel < 1 ||
            k->vel > 127 || f > 3 || k->transpose < -24 || k->transpose > 24)
            return -1;
    }
    for (t = 0; t < SEQ_NTRK && !rd_bad; t++) {
        uint32_t j, ne = get_vlq(), tick = 0, plen = 0;
        if (ne > SEQ_MAXEV - e)
            return -1;
        s->trk[t].nev = (uint16_t)ne;
        for (j = 0; j < ne && !rd_bad; j++, e++) {
            seq_ev_t *v = &s->ev[e];
            uint32_t nb, len;
            tick += get_vlq();
            nb = get8();
            len = nb & 128u ? plen : get_vlq();
            if (tick >= SEQ_MAXBARS * SEQ_BAR || len < 1 || len > 0xFFFFu)
                return -1;
            v->tick = (uint16_t)tick;
            v->note = (uint8_t)(nb & 127u);
            v->len = (uint16_t)len;
            plen = len;
        }
    }
    s->nev = (uint16_t)e;
    if (rd_bad || rd() != -1)                   /* every byte, and no more */
        return -1;
    return 0;
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
    uint32_t n = song_ram_bytes(s) / 4u, i;      /* both are word-aligned; the size is a multiple of 2 */
    const uint32_t *a = (const uint32_t *)s;
    uint32_t *b = (uint32_t *)d;
    for (i = 0; i < n; i++)
        b[i] = a[i];
    if (song_ram_bytes(s) & 2u)
        ((uint16_t *)d)[2u * n] = ((const uint16_t *)s)[2u * n];
}

static void snapshot(void)                      /* the undo copy: before a recording pass or an edit */
{
    copy_song(&undo, S);
    seq_undo_ok = 1;
}

static void changed(void) { seq_changes++; }

int seq_import(int (*get)(void))
{
    copy_song(&undo, S);
    if (song_parse(S, get) != 0) {
        copy_song(S, &undo);
        seq_undo_ok = 0;
        return -1;
    }
    seq_undo_ok = 1;
    seq_full = 0;
    changed();
    return 0;
}

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

/* the loop: the song, or bar seq_loop_bar */
static uint32_t reg_lo(void) { return seq_loop && seq_loop_bar < S->bars ? (uint32_t)seq_loop_bar * SEQ_BAR : 0u; }
static uint32_t reg_hi(void) { return seq_loop && seq_loop_bar < S->bars ? reg_lo() + SEQ_BAR : seq_song_ticks(); }
static uint32_t reg_wrap(uint32_t t)
{
    uint32_t lo = reg_lo(), hi = reg_hi();
    return t < lo ? lo : t < hi ? t : lo + (t - lo) % (hi - lo);
}

static float samples_per_tick(void) { return GM_SR * 60.0f / ((float)S->tempo * (float)SEQ_PPQN); }

static int audible(int t)
{
    int i, solo = 0;
    for (i = 0; i < SEQ_NTRK; i++)
        solo |= S->trk[i].solo;
    return solo ? S->trk[t].solo : !S->trk[t].mute;
}

static int out_note(const seq_trk_t *k, int note)   /* the drums too: +6 puts Kick 2 back on the lowest key */
{
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
    uint32_t t, s = 0, i;
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
    if (cur >= reg_hi() || cur < reg_lo()) {    /* the loop: back to its top */
        cur = reg_lo();
        tracks_off(-1);
        seek(cur);
    }
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
                out3(0x90u | k->chan, (uint32_t)n, k->vel);
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

static void rec_flush(void);

static void start(void)
{
    uint32_t i;
    if (seq_playing)                            /* again while playing (MIDI Start): the notes still held end */
        rec_flush();                            /* here, before abs_t goes back to 0 under them */
    for (i = 0; i < NACT; i++)
        act[i].on = 0;
    cur = reg_lo();
    last = 0xFFFFFFFFu;
    abs_t = 0;
    spt = samples_per_tick();
    acc = spt;                                  /* the first tick now */
    seek(cur);
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
    uint8_t on, trk;
    uint16_t tick;                              /* where it lands (quantized) */
    uint32_t abs;                               /* when it started: abs_t and the fraction */
    float frac;
} pend_t;
static pend_t pend[128];

static int quant(void) { return seq_quant_ticks(S->quant); }

/* insert (or, the same note at the same tick, replace) a note in track t */
static void ev_insert(int t, uint32_t tick, int note, uint32_t len)
{
    uint32_t s = trk_start(t), e = s + S->trk[t].nev, i, j, k;
    if (len < 1)
        len = 1;
    if (len > 0xFFFFu)
        len = 0xFFFFu;
    for (i = s; i < e && S->ev[i].tick <= tick; i++)
        if (S->ev[i].tick == tick && S->ev[i].note == note) {
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
    S->ev[i].rsv = 0;
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

static void rec_on(int t, int note)
{
    pend_t *p = &pend[note & 127];
    uint32_t tick, abs, q = (uint32_t)quant(), r;
    float frac;
    if (seq_step) {                             /* step recording: at the cursor, a step long */
        uint32_t sl = step_len();
        ev_insert(t, seq_step_tick, note, sl - sl / 8u);
        step_held[(note >> 5) & 3] |= 1u << (note & 31);
        step_any_note = 1;
        return;
    }
    if (!seq_rec || !seq_playing)
        return;
    p->on = 1;
    p->trk = (uint8_t)t;
    if (preroll) {                              /* during the count-in: at the top, if still held then */
        p->tick = (uint16_t)reg_lo();
        p->abs = 0;
        p->frac = 0.0f;
        return;
    }
    now_pos(&tick, &abs, &frac);
    r = tick + (frac >= 0.5f);
    if (q > 1u)
        r = (r + q / 2u) / q * q;
    p->tick = (uint16_t)reg_wrap(r);
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
            seq_step_tick = (uint16_t)reg_wrap(seq_step_tick + step_len());
        }
        return;
    }
    if (!p->on)
        return;
    p->on = 0;
    if (preroll)                                /* let go before recording began */
        return;
    now_pos(&tick, &abs, &frac);
    d = (float)(abs - p->abs) + (frac - p->frac);
    len = d < 1.0f ? 1u : (uint32_t)(d + 0.5f);
    if (len > seq_song_ticks())
        len = seq_song_ticks();
    ev_insert(p->trk, p->tick, note, len);
}

static void rec_flush(void)                     /* stop: the notes still held end now */
{
    int n;
    for (n = 0; n < 128; n++)
        if (pend[n].on)
            rec_off(n);
}

static void step_clear(int t, uint32_t t0, uint32_t sl)   /* track t's notes that start in t0 .. t0 + sl - 1 */
{
    uint32_t s = trk_start(t), e = s + S->trk[t].nev, i;
    for (i = s; i < e;) {
        if (S->ev[i].tick >= t0 && S->ev[i].tick < t0 + sl) {
            ev_remove(i, t);
            e--;
        } else {
            i++;
        }
    }
}

static void step_back(void)
{
    uint32_t sl = step_len(), lo = reg_lo(), len = reg_hi() - lo, t0;
    t0 = lo + (reg_wrap(seq_step_tick) - lo + len - sl % len) % len;
    seq_step_tick = (uint16_t)t0;
    step_clear(seq_sel, t0, sl);
}

/* a note's length in steps of sl: what step_len() notes are (a step less its eighth), rounded */
static uint32_t len_steps(uint32_t len, uint32_t sl)
{
    uint32_t n = (len + sl / 8u + sl / 2u) / sl;
    return n ? n : 1u;
}

/* track t's notes (note 128: all) that start in the step at the step cursor: d steps longer */
static void step_len_add(int t, int note, int32_t d)
{
    static uint32_t mark_changes, mark_at = 0xFFFFFFFFu;
    uint32_t sl = step_len(), t0 = reg_wrap(seq_step_tick), s = trk_start(t), e = s + S->trk[t].nev, i, pass;
    uint32_t mx = seq_song_ticks() / sl, at = (uint32_t)t << 16 | t0;
    for (pass = 0; pass < 2; pass++)            /* 0: is anything to change; 1: change it */
        for (i = s; i < e && S->ev[i].tick < t0 + sl; i++) {
            seq_ev_t *v = &S->ev[i];
            int32_t n;
            uint32_t len;
            if (v->tick < t0 || (note < 128 && v->note != note))
                continue;
            n = (int32_t)len_steps(v->len, sl) + d;
            n = n < 1 ? 1 : n > (int32_t)mx ? (int32_t)mx : n;
            len = (uint32_t)n * sl - sl / 8u;
            if (len == v->len)
                continue;
            if (!pass) {                        /* the first change here since any other: the undo copy */
                if (mark_at != at || mark_changes != seq_changes)
                    snapshot();
                break;
            }
            v->len = (uint16_t)len;
            changed();
            mark_at = at;
            mark_changes = seq_changes;
        }
}

/* -------------------------------------------------------- bar editing --- */
static void bar_clear(int t, uint32_t bar)      /* track t's notes in the bar */
{
    tracks_off(t);
    step_clear(t, bar * SEQ_BAR, SEQ_BAR);
}

static void bar_copy(int t, uint32_t bar)
{
    uint32_t s = trk_start(t), e = s + S->trk[t].nev, i, lo = bar * SEQ_BAR, n = 0;
    for (i = s; i < e && n < SEQ_CLIP; i++)
        if (S->ev[i].tick >= lo && S->ev[i].tick < lo + SEQ_BAR) {
            clip[n] = S->ev[i];
            clip[n].tick = (uint16_t)(S->ev[i].tick - lo);
            n++;
        }
    seq_clip_n = (uint16_t)n;
}

static void bar_paste(int t, uint32_t bar)
{
    uint32_t i, lo = bar * SEQ_BAR;
    if (bar >= S->bars)
        return;
    bar_clear(t, bar);
    for (i = 0; i < seq_clip_n; i++)
        ev_insert(t, lo + clip[i].tick, clip[i].note, clip[i].len);
}

static void bar_delete(uint32_t bar)            /* every track: the bar out, the bars after it up one */
{
    uint32_t t, i, lo = bar * SEQ_BAR;
    if (bar >= S->bars)
        return;
    tracks_off(-1);
    for (t = 0; t < SEQ_NTRK; t++)
        bar_clear((int)t, bar);
    for (i = 0; i < S->nev; i++)
        if (S->ev[i].tick >= lo + SEQ_BAR)
            S->ev[i].tick = (uint16_t)(S->ev[i].tick - SEQ_BAR);
    if (S->bars > 1)
        S->bars--;
    if (seq_loop_bar >= S->bars)
        seq_loop_bar = (uint8_t)(S->bars - 1u);
    if (seq_step_tick >= seq_song_ticks())
        seq_step_tick = 0;
    if (cur >= seq_song_ticks())
        cur = reg_lo();
    seek(cur);
    changed();
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
        if (seq_loop_bar >= S->bars)
            seq_loop_bar = (uint8_t)(S->bars - 1u);
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
    rec_on(t, note);
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
                rec_on(seq_sel, c->b & 127);
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
            seq_step_tick = (uint16_t)reg_wrap(seq_step_tick);
        } else {
            seq_step = 0;
        }
        break;
    case SC_STEP_REST:
        if (seq_step)
            seq_step_tick = (uint16_t)reg_wrap(seq_step_tick + step_len());
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
                uint32_t na = song_ram_bytes(S), nb = song_ram_bytes(&undo), n = ((na > nb ? na : nb) + 3u) / 4u, i;
                uint32_t *a = (uint32_t *)S, *b = (uint32_t *)&undo;
                for (i = 0; i < n; i++) {
                    uint32_t x = a[i];
                    a[i] = b[i];
                    b[i] = x;
                }
            }
            if (seq_loop_bar >= S->bars)
                seq_loop_bar = (uint8_t)(S->bars - 1u);
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
    case SC_LOOP:
        seq_loop = (uint8_t)(c->a != 0);
        seq_loop_bar = (uint8_t)((uint32_t)c->w < S->bars ? (uint32_t)c->w : S->bars - 1u);
        break;
    case SC_CURSOR:
        seq_step_tick = (uint16_t)reg_wrap((uint32_t)c->w < seq_song_ticks() ? (uint32_t)c->w : 0u);
        break;
    case SC_BAR_CLEAR:
        snapshot();
        bar_clear(t, (uint32_t)c->w);
        break;
    case SC_BAR_COPY:
        bar_copy(t, (uint32_t)c->w);
        break;
    case SC_BAR_PASTE:
        snapshot();
        bar_paste(t, (uint32_t)c->w);
        break;
    case SC_BAR_DELETE:
        snapshot();
        bar_delete((uint32_t)c->w);
        break;
    case SC_INIT:
        stop();
        seq_step = 0;
        seq_rec = 0;
        snapshot();
        song_defaults(S);
        seq_loop = 0;
        seq_loop_bar = 0;
        seq_step_tick = 0;
        seq_full = 0;
        seek(0);
        spt = samples_per_tick();
        apply_all();
        changed();
        break;
    case SC_STEP_DELETE:
        snapshot();
        tracks_off(t);
        step_clear(t, reg_wrap(seq_step_tick), step_len());
        break;
    case SC_STEP_LEN:
        step_len_add(t, c->b > 127 ? 128 : c->b, c->w);
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
