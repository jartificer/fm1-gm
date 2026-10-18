/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM UI: the panel and the screen. Part of the unity build (after gfx.c and settings.c); the
 * host simulator includes it too.
 *
 *   keys           play the selected track's sound (and record into it); on a drum track the keys
 *                  are the GM kit, from Kick 2 (note 35) up
 *   OCT- / OCT+    the keys' octave (both held 5 s: update mode)
 *   PLAY           play / stop (from the top)
 *   REC            arm real-time recording into the selected track (overdub, quantized); from stop,
 *                  PLAY then starts with a bar of count-in
 *   SEQ            step recording (stopped): each key / chord is a step at the cursor; ARP a rest,
 *                  LFO back a step (clearing it); SEQ again to leave
 *   ENV / LFO      mute / solo the selected track
 *   ARP            undo (again: redo) the last recording or edit; held 1 s: erase the selected track
 *   HOME EDIT FX SEL GLO   the pages: SONG, TRACK, MIX, SONGS, SETUP (EDIT, GLO again: their next
 *                  knob page; HOME again: BAR); on SONGS, SEL again loads the song highlighted, ARP
 *                  held twice deletes it, SEL held twice starts a new song (undo: ARP)
 *   BAR            one bar of the selected track, close up: KNOB 1 the bar, 2 the step, 3 loop the
 *                  bar (playback and recording stay in it), 4 quantize (the step's size). ENV deletes
 *                  the step's notes (held: deletes the bar, every track), LFO copies the bar (held:
 *                  pastes it over this one), ARP held clears the bar; ARP undoes any of them. SEQ
 *                  step-records from the step.
 *   SAVE           saves the song to its slot (on SONGS: to the slot highlighted)
 *   SELECT         the track (on SONGS: the slot)   ALGORITHM the track's program   PRESETS the tempo
 *   KNOB 1-4       the page's four values */

enum { V_SONG, V_TRACK, V_MIX, V_SONGS, V_SETUP, V_BAR, NVIEWS };
static const char *const VIEW_NAME[NVIEWS] = {"Song", "Track", "Mix", "Songs", "Setup", "Bar"};

/* the knobs: what each page's four do */
enum {
    K_NONE,
    K_TEMPO, K_BARS, K_QUANT, K_METRO,
    K_FAMILY, K_PROG, K_VOL, K_PAN, K_CHAN, K_REV, K_TRANSPOSE, K_KEYVEL,
    K_MIX0, K_MIX1, K_MIX2, K_MIX3,
    K_MIDIIN, K_MIDIOUT, K_CLOCK, K_COUNTIN, K_POLY, K_METROVOL, K_LEDS, K_THEME,
    K_SLOT, K_BAR, K_STEP, K_LOOP,
};
#define NKPAGES 2
static const uint8_t VIEW_KNOB[NVIEWS][NKPAGES][4] = {
    {{K_TEMPO, K_BARS, K_QUANT, K_METRO}, {K_TEMPO, K_BARS, K_QUANT, K_METRO}},
    {{K_FAMILY, K_PROG, K_VOL, K_PAN}, {K_CHAN, K_REV, K_TRANSPOSE, K_KEYVEL}},
    {{K_MIX0, K_MIX1, K_MIX2, K_MIX3}, {K_MIX0, K_MIX1, K_MIX2, K_MIX3}},
    {{K_SLOT, K_NONE, K_NONE, K_NONE}, {K_SLOT, K_NONE, K_NONE, K_NONE}},
    {{K_MIDIIN, K_MIDIOUT, K_CLOCK, K_COUNTIN}, {K_POLY, K_METROVOL, K_LEDS, K_THEME}},
    {{K_BAR, K_STEP, K_LOOP, K_QUANT}, {K_BAR, K_STEP, K_LOOP, K_QUANT}},
};

/* the screen's colours (Setup, COLOURS): one accent per meaning. Dark is the default; the two
 * colour-blind ones take their accents from Okabe and Ito's palette (red-green: no red against
 * green, play is blue and record vermilion) and, for blue-yellow, from the red-cyan axis that
 * tritan vision keeps */
typedef struct {
    const char *name;
    uint16_t bg, panel, line, dim, text;
    uint16_t acc;                      /* values, the selected thing */
    uint16_t selc, sel_t;              /* the selected track; its fill */
    uint16_t play, rec, drum;
} theme_t;
static const theme_t THEME[NTHEMES] = {
    {"DARK", RGB(16, 18, 24), RGB(30, 34, 44), RGB(50, 56, 70), RGB(112, 120, 136), RGB(228, 232, 240),
     RGB(255, 172, 64), RGB(64, 136, 255), RGB(30, 56, 104), RGB(84, 214, 124), RGB(244, 72, 72), RGB(214, 120, 255)},
    {"LIGHT", RGB(236, 236, 232), RGB(216, 218, 222), RGB(184, 188, 198), RGB(100, 106, 118), RGB(16, 18, 24),
     RGB(196, 96, 0), RGB(0, 88, 208), RGB(184, 206, 244), RGB(0, 136, 56), RGB(208, 24, 24), RGB(136, 48, 192)},
    {"HI-CON", RGB(0, 0, 0), RGB(28, 28, 28), RGB(140, 140, 140), RGB(208, 208, 208), RGB(255, 255, 255),
     RGB(255, 255, 0), RGB(0, 255, 255), RGB(0, 84, 100), RGB(0, 255, 0), RGB(255, 48, 48), RGB(255, 96, 255)},
    {"RED-GRN", RGB(16, 18, 24), RGB(30, 34, 44), RGB(50, 56, 70), RGB(112, 120, 136), RGB(228, 232, 240),
     RGB(240, 228, 66), RGB(86, 180, 233), RGB(20, 60, 92), RGB(40, 140, 210), RGB(213, 94, 0), RGB(204, 121, 167)},
    {"BLU-YEL", RGB(16, 18, 24), RGB(30, 34, 44), RGB(50, 56, 70), RGB(112, 120, 136), RGB(228, 232, 240),
     RGB(255, 120, 120), RGB(0, 184, 204), RGB(0, 58, 68), RGB(236, 236, 236), RGB(204, 0, 40), RGB(255, 176, 208)},
};
static const theme_t *thm = &THEME[0];
#define K_BG (thm->bg)
#define K_PANEL (thm->panel)
#define K_LINE (thm->line)
#define K_DIM (thm->dim)
#define K_TEXT (thm->text)
#define K_ACC (thm->acc)
#define K_SELC (thm->selc)
#define K_SEL_T (thm->sel_t)
#define K_PLAYC (thm->play)
#define K_RECC (thm->rec)
#define K_DRUMC (thm->drum)

#define LONG_MS 1000u
#define KEY_BASE 53                    /* F3, the lowest key, at octave 0 */
#define DRUM_BASE 35                   /* the lowest key on a drum track: Kick 2 */

static struct {
    uint8_t view, kpage[NVIEWS];
    uint32_t btn, keys, btn_used;
    uint32_t btn_t[NB];                /* when each button went down */
    uint32_t enc_t[NE];
    int8_t oct;
    uint8_t slot_cur;                  /* the SONGS page's cursor */
    uint8_t bar, loop;                 /* the BAR page's bar (0..), and its loop on */
    uint8_t key_note[NKEYS], key_trk[NKEYS];   /* what each key held plays (the track may change meanwhile) */
    uint8_t midi_trk[128];             /* MIDI in (TRACK mode): the track each held note went to, +1 */
    uint8_t last_drum;                 /* the drum played last (its name on the TRACK page) */
    uint32_t saved_changes;            /* seq_changes when the song was last loaded / saved */
    uint8_t set_dirty;
    uint32_t act_t;
    char msg[2][24];
    uint32_t msg_until;
    int8_t touched;
    uint32_t touch_until;
    uint8_t confirm;                   /* a press waiting for its second: C_LOAD, C_SAVE */
    uint32_t confirm_until;
    uint8_t pend;                      /* a load / save waiting for the sequencer to stop: P_* */
    uint8_t pend_slot;
    uint32_t pend_t;
    uint32_t frame;
    uint32_t sig[3];
} ui;
enum { C_NONE, C_LOAD, C_SAVE, C_DEL, C_INIT };
enum { P_NONE, P_LOAD, P_SAVE, P_DEL };

static void say(const char *a, const char *b)
{
    uint32_t i;
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1400u;
}
void ui_say(const char *a, const char *b) { say(a, b); }

static void itoa_s(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    if (v < 0)
        *b++ = '-';
    do {
        t[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u);
    while (n)
        *b++ = t[--n];
    *b = 0;
}
static void itoa2(uint32_t v, char *b)          /* two digits */
{
    b[0] = (char)('0' + v / 10u % 10u);
    b[1] = (char)('0' + v % 10u);
    b[2] = 0;
}
static char *cat(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    *d = 0;
    return d;
}

static const seq_trk_t *sel_trk(void) { return &seq_song.trk[seq_sel]; }
static int is_drum(const seq_trk_t *t) { return t->chan == GM_DRUM_CH; }
static int song_dirty(void) { return seq_changes != ui.saved_changes; }

static void post(uint32_t op, uint32_t a, uint32_t b, uint32_t c, int32_t w)
{
    if (seq_post(op, a, b, c, w) != 0)
        say("BUSY", "TRY AGAIN");
}

/* the BAR page: its bar, and the step (the sequencer's step cursor, a quantize step long) */
static uint32_t step_ticks(void)
{
    uint32_t q = (uint32_t)seq_quant_ticks(seq_song.quant);
    return q < 6u ? 12u : q;                    /* as seq.c's step_len: quantize off, 1/16s */
}
static int bar_cur(void) { return ui.bar < seq_song.bars ? ui.bar : seq_song.bars - 1; }
static void bar_go(int bar, uint32_t step)      /* the bar and the step in it (0..): to the sequencer */
{
    ui.bar = (uint8_t)bar;
    post(SC_LOOP, ui.view == V_BAR && ui.loop, 0, 0, bar);
    post(SC_CURSOR, 0, 0, 0, (int32_t)((uint32_t)bar * SEQ_BAR + step * step_ticks()));
}

/* -------------------------------------------------------------- knobs --- */
static int knob_get(int k)
{
    const seq_trk_t *t = sel_trk();
    switch (k) {
    case K_TEMPO: return seq_song.tempo;
    case K_BARS: return seq_song.bars;
    case K_QUANT: return seq_song.quant;
    case K_METRO: return set.metro;
    case K_FAMILY: return t->prog >> 3;
    case K_PROG: return t->prog;
    case K_VOL: return t->vol;
    case K_PAN: return t->pan - 64;
    case K_CHAN: return t->chan + 1;
    case K_REV: return t->rev;
    case K_TRANSPOSE: return t->transpose;
    case K_KEYVEL: return t->vel;
    case K_MIX0: case K_MIX1: case K_MIX2: case K_MIX3:
        return seq_song.trk[(seq_sel & ~3u) + (uint32_t)(k - K_MIX0)].vol;
    case K_MIDIIN: return set.midi_in;
    case K_MIDIOUT: return set.midi_out;
    case K_CLOCK: return set.clock_out;
    case K_COUNTIN: return set.countin;
    case K_POLY: return set.poly;
    case K_METROVOL: return set.metro_vol;
    case K_LEDS: return set.leds;
    case K_THEME: return set.theme;
    case K_SLOT: return ui.slot_cur + 1;
    case K_BAR: return bar_cur() + 1;
    case K_STEP: {
        uint32_t b0 = (uint32_t)bar_cur() * SEQ_BAR;
        return seq_step_tick >= b0 && seq_step_tick < b0 + SEQ_BAR ? (int)((seq_step_tick - b0) / step_ticks()) + 1 : 1;
    }
    case K_LOOP: return ui.loop;
    default: return 0;
    }
}

static void knob_range(int k, int *lo, int *hi)
{
    static const struct {
        uint8_t k;
        int16_t lo, hi;
    } R[] = {
        {K_TEMPO, 30, 300}, {K_BARS, 1, SEQ_MAXBARS}, {K_QUANT, 0, SEQ_NQUANT - 1}, {K_METRO, 0, 2},
        {K_FAMILY, 0, 15}, {K_PROG, 0, 127}, {K_VOL, 0, 127}, {K_PAN, -64, 63}, {K_CHAN, 1, 16},
        {K_REV, 0, 127}, {K_TRANSPOSE, -24, 24}, {K_KEYVEL, 1, 127}, {K_MIX0, 0, 127}, {K_MIX1, 0, 127},
        {K_MIX2, 0, 127}, {K_MIX3, 0, 127}, {K_MIDIIN, 0, 1}, {K_MIDIOUT, 0, 1}, {K_CLOCK, 0, 1},
        {K_COUNTIN, 0, 1}, {K_POLY, 8, GM_NVOICE}, {K_METROVOL, 0, 10}, {K_LEDS, 0, 1}, {K_THEME, 0, NTHEMES - 1},
        {K_SLOT, 1, SEQ_NSONGS}, {K_LOOP, 0, 1},
    };
    uint32_t i;
    *lo = *hi = 0;
    if (k == K_BAR || k == K_STEP) {
        *lo = 1;
        *hi = k == K_BAR ? seq_song.bars : (int)(SEQ_BAR / step_ticks());
        return;
    }
    for (i = 0; i < sizeof R / sizeof R[0]; i++)
        if (R[i].k == k) {
            *lo = R[i].lo;
            *hi = R[i].hi;
        }
}

static const char *knob_name(int k)
{
    static char mix[8];
    switch (k) {
    case K_TEMPO: return "TEMPO";
    case K_BARS: return "BARS";
    case K_QUANT: return "QUANT";
    case K_METRO: return "CLICK";
    case K_FAMILY: return "FAMILY";
    case K_PROG: return "PROGRAM";
    case K_VOL: return "VOLUME";
    case K_PAN: return "PAN";
    case K_CHAN: return "CHANNEL";
    case K_REV: return "REVERB";
    case K_TRANSPOSE: return "TRANSP";
    case K_KEYVEL: return "VELOCITY";
    case K_MIX0: case K_MIX1: case K_MIX2: case K_MIX3:
        mix[0] = 'T';
        mix[1] = 'R';
        mix[2] = ' ';
        itoa_s((int32_t)(seq_sel & ~3u) + (k - K_MIX0) + 1, mix + 3);
        return mix;
    case K_MIDIIN: return "MIDI IN";
    case K_MIDIOUT: return "MIDI OUT";
    case K_CLOCK: return "CLOCK";
    case K_COUNTIN: return "COUNT-IN";
    case K_POLY: return "VOICES";
    case K_METROVOL: return "CLK VOL";
    case K_LEDS: return "LIGHTS";
    case K_THEME: return "COLOURS";
    case K_SLOT: return "SLOT";
    case K_BAR: return "BAR";
    case K_STEP: return "STEP";
    case K_LOOP: return "LOOP";
    default: return "";
    }
}

static void knob_text(int k, int v, char *b)    /* knob k's value v as it shows */
{
    const seq_trk_t *t = sel_trk();
    static const char *const OFFON[2] = {"OFF", "ON"};
    switch (k) {
    case K_QUANT: cat(b, seq_quant_name(v)); return;
    case K_METRO: cat(b, v == 0 ? "OFF" : v == 1 ? "REC" : "ON"); return;
    case K_FAMILY:
    case K_PROG:
        if (is_drum(t)) {
            cat(b, k == K_FAMILY ? "Drums" : "Kit");
            return;
        }
        if (k == K_PROG) {
            itoa_s(v + 1, b);
            return;
        }
        cat(b, gm_family_name(v * 8));
        return;
    case K_PAN:
        if (v == 0)
            cat(b, "C");
        else {
            b[0] = v < 0 ? 'L' : 'R';
            itoa_s(v < 0 ? -v : v, b + 1);
        }
        return;
    case K_TRANSPOSE:
        if (v > 0) {
            b[0] = '+';
            itoa_s(v, b + 1);
        } else {
            itoa_s(v, b);
        }
        return;
    case K_MIDIIN: cat(b, v ? "MULTI" : "TRACK"); return;
    case K_MIDIOUT: case K_CLOCK: case K_LEDS: case K_LOOP: cat(b, OFFON[v & 1]); return;
    case K_COUNTIN: cat(b, v ? "1 BAR" : "OFF"); return;
    case K_THEME: cat(b, THEME[v % NTHEMES].name); return;
    default: itoa_s(v, b); return;
    }
}

static void knob_set(int k, int v)
{
    int lo, hi;
    knob_range(k, &lo, &hi);
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    if (v == knob_get(k))
        return;
    switch (k) {
    case K_TEMPO: post(SC_SONG, SP_TEMPO, 0, 0, v); break;
    case K_BARS: post(SC_SONG, SP_BARS, 0, 0, v); break;
    case K_QUANT: post(SC_SONG, SP_QUANT, 0, 0, v); break;
    case K_FAMILY:
        if (!is_drum(sel_trk()))
            post(SC_TRK, seq_sel, TP_PROG, 0, v * 8);
        break;
    case K_PROG:
        if (!is_drum(sel_trk()))
            post(SC_TRK, seq_sel, TP_PROG, 0, v);
        break;
    case K_VOL: post(SC_TRK, seq_sel, TP_VOL, 0, v); break;
    case K_PAN: post(SC_TRK, seq_sel, TP_PAN, 0, v + 64); break;
    case K_CHAN: post(SC_TRK, seq_sel, TP_CHAN, 0, v - 1); break;
    case K_REV: post(SC_TRK, seq_sel, TP_REV, 0, v); break;
    case K_TRANSPOSE: post(SC_TRK, seq_sel, TP_TRANSPOSE, 0, v); break;
    case K_KEYVEL: post(SC_TRK, seq_sel, TP_VEL, 0, v); break;
    case K_MIX0: case K_MIX1: case K_MIX2: case K_MIX3:
        post(SC_TRK, (seq_sel & ~3u) + (uint32_t)(k - K_MIX0), TP_VOL, 0, v);
        break;
    case K_SLOT: ui.slot_cur = (uint8_t)(v - 1); break;
    case K_BAR: bar_go(v - 1, 0); break;
    case K_STEP: post(SC_CURSOR, 0, 0, 0, (int32_t)((uint32_t)bar_cur() * SEQ_BAR + (uint32_t)(v - 1) * step_ticks())); break;
    case K_LOOP:
        ui.loop = (uint8_t)v;
        post(SC_LOOP, (uint32_t)v, 0, 0, bar_cur());
        break;
    default: {   /* the settings */
        uint8_t *p = k == K_METRO ? &set.metro : k == K_MIDIIN ? &set.midi_in : k == K_MIDIOUT ? &set.midi_out :
                     k == K_CLOCK ? &set.clock_out : k == K_COUNTIN ? &set.countin : k == K_POLY ? &set.poly :
                     k == K_METROVOL ? &set.metro_vol : k == K_LEDS ? &set.leds : k == K_THEME ? &set.theme : 0;
        if (!p)
            return;
        *p = (uint8_t)v;
        if (k == K_THEME) {             /* every part redraws in the new colours */
            thm = &THEME[set.theme];
            ui.sig[0] = ui.sig[1] = ui.sig[2] = 0;
        }
        if (k == K_MIDIOUT && !v)
            post(SC_PANIC, 0, 0, 0, 0);
        settings_apply();
        ui.set_dirty = 1;
        break;
    }
    }
}

/* knob acceleration by speed: a slow turn is one step a detent, a quick one up to 6 */
static uint8_t accel_off;              /* the host simulator's "spin": exact steps */
static int32_t accel(int role, int32_t s, int range)
{
    uint32_t now = plat_ms(), dt = now - ui.enc_t[role], a = (uint32_t)(s < 0 ? -s : s), m;
    ui.enc_t[role] = now;
    if (range <= 24 || !a || accel_off)
        return s;
    if (a > 1)
        dt /= a;
    m = dt < 15u ? 6u : dt < 30u ? 4u : dt < 60u ? 2u : 1u;
    return s * (int32_t)m;
}

static void turn(int role, int k, int32_t e)
{
    int lo, hi;
    if (!e || k == K_NONE)
        return;
    knob_range(k, &lo, &hi);
    knob_set(k, knob_get(k) + accel(role, e, hi - lo));
}

/* ------------------------------------------------------------- notes --- */
static int key_to_note(int key)
{
    const seq_trk_t *t = sel_trk();
    int n = (is_drum(t) ? DRUM_BASE : KEY_BASE) + key + 12 * ui.oct;
    return n < 0 ? 0 : n > 127 ? 127 : n;
}

static void key_down(int key)
{
    int n = key_to_note(key);
    ui.key_note[key] = (uint8_t)n;
    ui.key_trk[key] = seq_sel;
    if (is_drum(sel_trk()))
        ui.last_drum = (uint8_t)n;
    post(SC_NOTE_ON, seq_sel, (uint32_t)n, sel_trk()->vel, 0);
}

static void key_up(int key) { post(SC_NOTE_OFF, ui.key_trk[key], ui.key_note[key], 0, 0); }

static void keys_release(void)                  /* before the track or the octave changes under held keys */
{
    uint32_t i;
    for (i = 0; i < NKEYS; i++)
        if (ui.keys >> i & 1u)
            key_up((int)i);
    ui.keys = 0;
}

/* MIDI in (USB and the TRS jack). TRACK: every channel plays the selected track; MULTI: the engine
 * as a 16-channel GM module, and notes on the selected track's channel record. Start / stop: PLAY. */
static void midi_in(void)
{
    uint32_t pkt;
    while (plat_midi_in(&pkt)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu, ty = st & 0xF0u;
        if (st == 0xFAu) {
            post(SC_PLAY, 0, 0, 0, 0);
            continue;
        }
        if (st == 0xFCu) {
            post(SC_STOP, 0, 0, 0, 0);
            continue;
        }
        if (st < 0x80u || st >= 0xF0u)
            continue;
        ui.act_t = plat_ms();
        if (set.midi_in) {
            post(SC_MIDI, st, d1, d2, 0);
            continue;
        }
        if (ty == 0x90u && d2) {
            ui.midi_trk[d1] = (uint8_t)(seq_sel + 1u);
            if (is_drum(sel_trk()))
                ui.last_drum = (uint8_t)d1;
            post(SC_NOTE_ON, seq_sel, d1, sel_trk()->vel, 0);   /* as the keys: the track's velocity */
        } else if (ty == 0x80u || ty == 0x90u) {
            if (ui.midi_trk[d1])
                post(SC_NOTE_OFF, ui.midi_trk[d1] - 1u, d1, 0, 0);
            ui.midi_trk[d1] = 0;
        } else if (ty != 0xC0u) {                   /* controllers, bend, pressure: the track's channel */
            post(SC_MIDI, ty | sel_trk()->chan, d1, d2, 0);
        }
    }
}

/* ----------------------------------------------------------- buttons --- */
static void set_view(int v)
{
    if (ui.view == v && (v == V_TRACK || v == V_SETUP))
        ui.kpage[v] ^= 1u;                      /* the page's other knobs */
    if (v == V_SONGS && ui.view != V_SONGS)
        ui.slot_cur = set.song;
    if (v == V_SONG && ui.view == V_SONG)       /* HOME again: the bar */
        v = V_BAR;
    else if (v == V_SONG && ui.view == V_BAR)
        v = V_SONG;
    if (v == V_BAR && ui.view != V_BAR) {       /* at the bar playing, or the step cursor's */
        uint32_t t = seq_playing ? seq_tick : seq_step_tick;
        ui.view = V_BAR;
        bar_go((int)(t / SEQ_BAR < seq_song.bars ? t / SEQ_BAR : 0u), t % SEQ_BAR / step_ticks());
    } else if (v != V_BAR && ui.view == V_BAR) {
        post(SC_LOOP, 0, 0, 0, 0);              /* the loop is the bar page's */
    }
    ui.view = (uint8_t)v;
    ui.confirm = C_NONE;
}

static void select_track(int t)
{
    if (t < 0)
        t = 0;
    if (t > SEQ_NTRK - 1)
        t = SEQ_NTRK - 1;
    if (t == seq_sel)
        return;
    keys_release();
    post(SC_SELECT, (uint32_t)t, 0, 0, 0);
    seq_sel = (uint8_t)t;                       /* for the screen at once; the render sets it too */
}

static void request(int what, int slot)         /* a load or save: once the sequencer has stopped */
{
    if (seq_step)
        post(SC_STEP, 0, 0, 0, 0);
    if (seq_rec)
        post(SC_REC, 0, 0, 0, 0);
    post(SC_STOP, 0, 0, 0, 0);
    keys_release();
    ui.pend = (uint8_t)what;
    ui.pend_slot = (uint8_t)slot;
    ui.pend_t = plat_ms();
}

static void pending(void)
{
    char t[8];
    int rc;
    if (!ui.pend || seq_playing || !seq_idle()) {
        if (ui.pend && plat_ms() - ui.pend_t > 3000u) {
            ui.pend = P_NONE;
            say("BUSY", "NOT DONE");
        }
        return;
    }
    seq_lock = 1;
    t[0] = 0;
    itoa2(ui.pend_slot + 1u, t);
    if (ui.pend == P_SAVE) {
        rc = song_save_slot(ui.pend_slot);
        seq_lock = 0;
        if (rc == 0) {
            set.song = ui.pend_slot;
            ui.set_dirty = 1;
            ui.saved_changes = seq_changes;
            say("SAVED", t);
        } else {
            say(rc == -10 ? "NO ROOM" : "SAVE FAILED", t);
        }
    } else if (ui.pend == P_DEL) {
        rc = song_delete_slot(ui.pend_slot);
        seq_lock = 0;
        if (ui.pend_slot == set.song)          /* the song stays here, now not saved anywhere */
            ui.saved_changes = seq_changes - 1u;
        say(rc == 0 ? "DELETED" : "DELETE FAILED", t);
    } else {
        rc = song_load_slot(ui.pend_slot);
        seq_lock = 0;
        seq_changes++;
        ui.saved_changes = seq_changes;
        set.song = ui.pend_slot;
        ui.set_dirty = 1;
        post(SC_GMRESET, 0, 0, 0, 0);
        post(SC_SELECT, 0, 0, 0, 0);
        seq_sel = 0;
        say(rc == 0 ? "LOADED" : rc > 0 ? "NEW SONG" : "UNREADABLE: NEW", t);
    }
    ui.pend = P_NONE;
}

static void button(int b, int held_long)
{
    switch (b) {
    case B_HOME: set_view(V_SONG); break;
    case B_EDIT: set_view(V_TRACK); break;
    case B_FX: set_view(V_MIX); break;
    case B_GLO: set_view(V_SETUP); break;
    case B_SEL:
        if (ui.view == V_SONGS && held_long) {     /* a new song: held twice */
            if (ui.confirm != C_INIT) {
                ui.confirm = C_INIT;
                ui.confirm_until = plat_ms() + 4000u;
                say("NEW SONG?", "HOLD SEL AGAIN");
            } else {
                ui.confirm = C_NONE;
                keys_release();
                post(SC_INIT, 0, 0, 0, 0);
                post(SC_GMRESET, 0, 0, 0, 0);
                post(SC_SELECT, 0, 0, 0, 0);
                seq_sel = 0;
                say("NEW SONG", "ARP: UNDO");
            }
        } else if (ui.view != V_SONGS) {
            set_view(V_SONGS);
        } else if (song_dirty() && ui.confirm != C_LOAD) {
            ui.confirm = C_LOAD;
            ui.confirm_until = plat_ms() + 4000u;
            say("NOT SAVED!", "SEL: LOAD ANYWAY");
        } else {
            ui.confirm = C_NONE;
            request(P_LOAD, ui.slot_cur);
        }
        break;
    case B_SAVE: {
        int slot = ui.view == V_SONGS ? ui.slot_cur : set.song;
        if (slot != set.song && slot_info[slot].used && ui.confirm != C_SAVE) {
            ui.confirm = C_SAVE;
            ui.confirm_until = plat_ms() + 4000u;
            say("SLOT IN USE", "SAVE: OVERWRITE");
        } else {
            ui.confirm = C_NONE;
            request(P_SAVE, slot);
        }
        break;
    }
    case B_PLAY:
        if (seq_playing)
            post(SC_STOP, 0, 0, 0, 0);
        else
            post(SC_PLAY, 0, 0, 0, 0);
        break;
    case B_REC:
        if (seq_step)
            break;
        post(SC_REC, !seq_rec, 0, 0, 0);
        break;
    case B_SEQ:
        if (seq_playing && !seq_step) {
            say("STEP RECORDING", "STOP FIRST");
            break;
        }
        if (!seq_step && seq_rec)
            post(SC_REC, 0, 0, 0, 0);
        post(SC_STEP, !seq_step, 0, 0, 0);
        break;
    case B_ENV:
        if (ui.view == V_BAR && held_long && !seq_step) {
            char t[8] = "BAR ";
            itoa_s(bar_cur() + 1, t + 4);
            post(SC_BAR_DELETE, 0, 0, 0, bar_cur());
            say("BAR DELETED", t);
        } else if (ui.view == V_BAR || seq_step) {
            post(SC_STEP_DELETE, seq_sel, 0, 0, 0);
            say("STEP DELETED", "");
        } else {
            post(SC_TRK, seq_sel, TP_MUTE, 0, !sel_trk()->mute);
        }
        break;
    case B_LFO:
        if (seq_step)
            post(SC_STEP_BACK, 0, 0, 0, 0);
        else if (ui.view == V_BAR && held_long) {
            post(SC_BAR_PASTE, seq_sel, 0, 0, bar_cur());
            say("PASTED", "");
        } else if (ui.view == V_BAR) {
            post(SC_BAR_COPY, seq_sel, 0, 0, bar_cur());
            say("BAR COPIED", "");
        } else
            post(SC_TRK, seq_sel, TP_SOLO, 0, !sel_trk()->solo);
        break;
    case B_ARP:
        if (seq_step) {
            post(SC_STEP_REST, 0, 0, 0, 0);
        } else if (held_long && ui.view == V_SONGS) {
            char t[4];
            itoa2(ui.slot_cur + 1u, t);
            if (!slot_info[ui.slot_cur].used) {
                say("EMPTY SLOT", t);
            } else if (ui.confirm != C_DEL) {
                ui.confirm = C_DEL;
                ui.confirm_until = plat_ms() + 4000u;
                say("DELETE SONG?", "HOLD ARP AGAIN");
            } else {
                ui.confirm = C_NONE;
                request(P_DEL, ui.slot_cur);
            }
        } else if (held_long && ui.view == V_BAR) {
            post(SC_BAR_CLEAR, seq_sel, 0, 0, bar_cur());
            say("BAR CLEARED", "");
        } else if (held_long) {
            char t[8] = "TRACK ";
            itoa_s(seq_sel + 1, t + 6);
            post(SC_ERASE, seq_sel, 0, 0, 0);
            say("ERASED", t);
        } else if (seq_undo_ok) {
            post(SC_UNDO, 0, 0, 0, 0);
            say("UNDO", "");
        } else {
            say("NOTHING TO UNDO", "");
        }
        break;
    case B_OCTDN:
    case B_OCTUP: {
        int o = ui.oct + (b == B_OCTUP ? 1 : -1);
        char t[4];
        if (o < -3 || o > 3)
            break;
        keys_release();
        ui.oct = (int8_t)o;
        if (o > 0) {
            t[0] = '+';
            itoa_s(o, t + 1);
        } else {
            itoa_s(o, t);
        }
        say("OCTAVE", t);
        break;
    }
    default:
        break;
    }
}

static int has_long(int b)                      /* a button with a held action here */
{
    return b == B_ARP || (ui.view == V_BAR && !seq_step && (b == B_ENV || b == B_LFO)) ||
           (ui.view == V_SONGS && b == B_SEL);
}

static void input(void)
{
    uint32_t btn = plat_buttons(), keys = plat_keys(), ch, i, now = plat_ms();
    int32_t e;
    ch = btn ^ ui.btn;
    ui.btn = btn;
    for (i = 0; i < NB; i++) {
        uint32_t m = 1u << i;
        if (ch & m) {
            if (btn & m) {
                ui.btn_used &= ~m;
                ui.btn_t[i] = now;
                if (i == B_PLAY || i == B_REC)      /* transport on the press */
                    button((int)i, 0);
            } else if (!(ui.btn_used & m) && i != B_PLAY && i != B_REC) {
                button((int)i, 0);
            }
        } else if ((btn & m) && !(ui.btn_used & m) && has_long((int)i) && now - ui.btn_t[i] >= LONG_MS) {
            ui.btn_used |= m;                   /* held: its long action, and no short one on release */
            button((int)i, 1);
        }
    }
    if ((btn & ((1u << B_OCTDN) | (1u << B_OCTUP))) == ((1u << B_OCTDN) | (1u << B_OCTUP)))
        ui.btn_used |= (1u << B_OCTDN) | (1u << B_OCTUP);   /* both: the update-mode hold, not octaves */
    ch = keys ^ ui.keys;
    for (i = 0; i < NKEYS; i++)
        if (ch >> i & 1u) {
            if (keys >> i & 1u)
                key_down((int)i);
            else
                key_up((int)i);
        }
    ui.keys = keys;
    if (btn || keys || ch)
        ui.act_t = now;
    midi_in();
    if ((e = plat_enc(EN_SELECT)) != 0) {
        if (ui.view == V_SONGS)
            knob_set(K_SLOT, ui.slot_cur + 1 + (e > 0 ? 1 : -1));
        else
            select_track(seq_sel + (e > 0 ? 1 : -1));
        ui.act_t = now;
    }
    if ((e = plat_enc(EN_ALGO)) != 0) {
        turn(EN_ALGO, K_PROG, e);
        ui.touched = 4;
        ui.touch_until = now + 1500u;
    }
    if ((e = plat_enc(EN_PRESET)) != 0)
        turn(EN_PRESET, K_TEMPO, e);
    for (i = 0; i < 4; i++)
        if ((e = plat_enc(EN_K1 + (int)i)) != 0) {
            turn(EN_K1 + (int)i, VIEW_KNOB[ui.view][ui.kpage[ui.view]][i], e);
            ui.touched = (int8_t)i;
            ui.touch_until = now + 1200u;
            ui.act_t = now;
        }
    if (ui.confirm && now > ui.confirm_until)
        ui.confirm = C_NONE;
}

/* ------------------------------------------------------------- screen --- */
/* three bands: the header (28 px), the page (144), the knobs (68) */
#define HDR_H 28
#define MAIN_Y 28
#define MAIN_H 144
#define KNB_Y 172
#define KNB_H 68

static uint32_t hash(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

static void cv_round(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)
{
    static const uint8_t IN[7][6] = {{0}, {1}, {1, 0}, {2, 1, 0}, {2, 1, 0, 0}, {3, 2, 1, 0, 0}, {4, 2, 1, 1, 0, 0}};
    int32_t j;
    if (r > 6)
        r = 6;
    if (r > h / 2)
        r = h / 2;
    cv_rect(x, y + r, w, h - 2 * r, c);
    for (j = 0; j < r; j++) {
        int32_t d = IN[r][j];
        cv_rect(x + d, y + j, w - 2 * d, 1, c);
        cv_rect(x + d, y + h - 1 - j, w - 2 * d, 1, c);
    }
}

static void text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

/* text clipped to w pixels (a trailing '.' when cut) */
static void text_fit(int32_t x, int32_t y, int32_t w, const felucca_font_t *f, const char *s, uint16_t c)
{
    char b[32];
    int n = 0;
    while (s[n] && n < 31) {
        b[n] = s[n];
        n++;
    }
    b[n] = 0;
    while (n > 1 && text_w(f, b) > w) {
        b[--n] = 0;
        b[n - 1] = '.';
    }
    cv_text(x, y, f, b, c);
}

static void pos_text(uint32_t tick, char *b)    /* "bar.beat" */
{
    b = b + (itoa_s((int32_t)(tick / SEQ_BAR + 1u), b), 0);
    while (*b)
        b++;
    *b++ = '.';
    itoa_s((int32_t)(tick % SEQ_BAR / SEQ_PPQN + 1u), b);
}

static void track_name(int t, char *b)
{
    const seq_trk_t *k = &seq_song.trk[t];
    cat(b, is_drum(k) ? "GM Drum Kit" : gm_prog_name(k->prog));
}

/* the battery at the header's right end (ending at x1): 0..3 bars, a bolt when USB powers the
 * FM-1, red when empty; the x it starts at (x1 when not read yet) */
static int32_t draw_battery(int32_t x1)
{
    static const char *const BOLT[6] = {"......###.....", ".....###......", "....#########.", ".#########....",
                                        "......###.....", ".....###......"};
    int lv = plat_battery(), i, j, x = x1 - 20;
    uint16_t c = lv == 0 ? K_RECC : lv == 1 ? K_ACC : K_TEXT;
    if (lv < 0)
        return x1;
    cv_rect(x, 7, 18, 1, K_DIM);              /* the outline: 18 x 10, and the cap */
    cv_rect(x, 16, 18, 1, K_DIM);
    cv_rect(x, 7, 1, 10, K_DIM);
    cv_rect(x + 17, 7, 1, 10, K_DIM);
    cv_rect(x + 18, 10, 2, 4, K_DIM);
    if (lv == 4) {
        for (i = 0; i < 6; i++)
            for (j = 0; j < 14; j++)
                if (BOLT[i][j] == '#')
                    cv_rect(x + 2 + j, 9 + i, 1, 1, K_ACC);
    } else
        for (i = 0; i < lv; i++)
            cv_rect(x + 2 + i * 5, 9, 4, 6, c);
    if (lv == 0)
        cv_rect(x + 2, 9, 2, 6, c);
    return x;
}

static void draw_header(void)
{
    char t[24];
    int msg = plat_ms() < ui.msg_until;
    uint32_t h = hash(hash(hash(2166136261u, ui.view), seq_playing | seq_rec << 1 | seq_step << 2 | seq_counting << 3 |
                                                       (uint32_t)song_dirty() << 4 | (uint32_t)msg << 5),
                      (uint32_t)seq_song.tempo | (uint32_t)set.song << 12 | (uint32_t)(plat_battery() + 1) << 20 |
                      (uint32_t)set.theme << 24);
    if (msg)
        h = hash(hash(h, (uint32_t)ui.msg[0][0] << 8 | ui.msg[0][1]), ui.msg_until);
    if (seq_playing)
        h = hash(h, seq_tick / SEQ_PPQN);
    if (seq_step)
        h = hash(h, seq_step_tick);
    h = hash(h, ui.frame / 16u & (seq_counting || ui.pend));
    if (h == ui.sig[0])
        return;
    ui.sig[0] = h;
    cv_begin(240, HDR_H, K_BG);
    if (msg) {
        int32_t x = cv_text(8, 6, &FONT_B, ui.msg[0], K_TEXT);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], K_ACC);
    } else {
        int32_t x;
        t[0] = 'S';
        itoa2(set.song + 1u, t + 1);
        x = cv_text(8, 6, &FONT_B, t, song_dirty() ? K_ACC : K_TEXT);
        if (song_dirty())
            x = cv_text(x, 6, &FONT_B, "*", K_ACC);
        cv_text(x + 8, 6, &FONT_B, VIEW_NAME[ui.view], K_DIM);
        x = draw_battery(232) - 8;
        itoa_s(seq_song.tempo, t);
        x -= text_w(&FONT_S, t);
        cv_text(x, 6, &FONT_S, t, K_TEXT);
        if (seq_step) {
            pos_text(seq_step_tick, t);
            x -= 8 + text_w(&FONT_S, t);
            cv_text(x, 6, &FONT_S, t, K_ACC);
            x -= 8 + text_w(&FONT_XS, "STEP");
            cv_round(x - 4, 6, text_w(&FONT_XS, "STEP") + 8, 16, 5, K_ACC);
            cv_text(x, 7, &FONT_XS, "STEP", K_BG);
        } else {
            pos_text(seq_playing && !seq_counting ? seq_tick : 0, t);
            x -= 8 + text_w(&FONT_S, t);
            cv_text(x, 6, &FONT_S, t, seq_playing ? K_TEXT : K_DIM);
            x -= 16;
            if (seq_rec)                        /* a red dot, blinking through the count-in */
                cv_round(x, 9, 10, 10, 5, seq_counting && (ui.frame / 16u & 1u) ? K_BG : K_RECC);
            else if (seq_playing)
                cv_round(x, 9, 10, 10, 5, K_PLAYC);
            else {
                int32_t j;
                for (j = 0; j < 5; j++)
                    cv_rect(x + 2 + j, 9 + j, 1, 11 - 2 * j, K_DIM);
            }
        }
    }
    cv_blit(0, 0);
}

static uint16_t mix(uint16_t a, uint16_t b, int t)   /* t 0..16: a -> b */
{
    int r = ((a >> 11) * (16 - t) + (b >> 11) * t) / 16, g = (((a >> 5) & 63) * (16 - t) + ((b >> 5) & 63) * t) / 16,
        bl = ((a & 31) * (16 - t) + (b & 31) * t) / 16;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

static int lvl16(int chan)
{
    int v = (int)(gm_ch_level[chan & 15] * 16.0f);
    return v > 16 ? 16 : v;
}

static uint32_t main_sig(void)
{
    uint32_t h = hash(hash(2166136261u, ui.view | ui.kpage[ui.view] << 4 | (uint32_t)set.theme << 8),
                      seq_sel | (uint32_t)ui.slot_cur << 8);
    int t;
    h = hash(h, seq_changes);
    for (t = 0; t < SEQ_NTRK; t++)
        h = hash(h, (uint32_t)lvl16(seq_song.trk[t].chan));
    if (seq_playing)
        h = hash(h, seq_tick / 6u);
    h = hash(h, seq_step_tick | (uint32_t)seq_step << 16 | (uint32_t)ui.last_drum << 20);
    if (ui.view == V_SETUP)
        h = hash(hash(h, plat_cpu_pct()), gm_voices_used | gm_drums_used << 8 | plat_xruns() << 16);
    if (ui.view == V_BAR)
        h = hash(h, (uint32_t)bar_cur() | (uint32_t)seq_loop << 8 | (uint32_t)seq_clip_n << 9 | seq_song.quant << 24);
    if (ui.view == V_SONGS)
        h = hash(h, plat_song_free());
    return h;
}

/* SONG: the 16 tracks, two columns of eight; the loop's progress at the bottom */
static void draw_song(void)
{
    int t;
    for (t = 0; t < SEQ_NTRK; t++) {
        const seq_trk_t *k = &seq_song.trk[t];
        int32_t x = (t / 8) * 120 + 2, y = (t % 8) * 17 + 1, w = 116;
        int sel = t == seq_sel, lv = lvl16(k->chan), quiet = k->mute || (k->solo == 0 && 0);
        char b[24];
        uint16_t bg = sel ? K_SEL_T : K_PANEL, fg = k->mute ? K_DIM : K_TEXT;
        cv_round(x, y, w, 15, 3, bg);
        if (lv)
            cv_round(x, y, 3 + lv * 2, 15, 3, mix(bg, is_drum(k) ? K_DRUMC : K_PLAYC, 6 + lv / 2));
        itoa2((uint32_t)t + 1u, b);
        cv_text(x + 4, y + 1, &FONT_XS, b, sel ? K_TEXT : K_DIM);
        b[0] = 0;
        track_name(t, b);
        text_fit(x + 22, y + 1, 78, &FONT_XS, b, fg);
        if (k->solo)
            cv_text(x + w - 11, y + 1, &FONT_XS, "S", K_ACC);
        else if (k->mute)
            cv_text(x + w - 11, y + 1, &FONT_XS, "M", K_RECC);
        else if (k->nev)
            cv_round(x + w - 9, y + 5, 5, 5, 2, is_drum(k) ? K_DRUMC : K_SELC);
        (void)quiet;
    }
    {
        uint32_t len = seq_song_ticks(), p = seq_playing && !seq_counting ? seq_tick : 0;
        int32_t w = (int32_t)(236u * p / len);
        cv_rect(2, 139, 236, 3, K_LINE);
        if (seq_playing)
            cv_rect(2, 139, w < 2 ? 2 : w, 3, seq_rec ? K_RECC : K_PLAYC);
    }
}

/* TRACK: the selected track, big, and its notes over the loop */
static void draw_track(void)
{
    const seq_trk_t *k = sel_trk();
    char b[32], *q;
    int32_t x;
    uint32_t s = 0, i, len = seq_song_ticks();
    int lo = 127, hi = 0;
    itoa2((uint32_t)seq_sel + 1u, b);
    x = cv_text(8, 0, &FONT_L, b, K_SELC);
    b[0] = 0;
    q = cat(b, "CH ");
    itoa_s(k->chan + 1, q);
    cv_text(x + 10, 3, &FONT_S, b, K_DIM);
    if (is_drum(k)) {
        cv_text(x + 10, 18, &FONT_B, "GM Drum Kit", K_DRUMC);
        if (ui.last_drum)
            cv_text(8, 38, &FONT_S, gm_drum_name(ui.last_drum), K_TEXT);
    } else {
        b[0] = 0;
        itoa_s(k->prog + 1, b);
        q = cat(b + str_len(b), " ");
        cat(q, gm_prog_name(k->prog));
        text_fit(x + 10, 18, 230 - x - 10, &FONT_B, b, K_TEXT);
        cv_text(8, 38, &FONT_S, gm_family_name(k->prog), K_DIM);
    }
    b[0] = 0;
    q = b;
    itoa_s(k->nev, q);
    cat(q + str_len(q), k->nev == 1 ? " note" : " notes");
    cv_text(240 - 8 - text_w(&FONT_S, b), 38, &FONT_S, b, seq_full ? K_RECC : K_DIM);
    /* the piano roll: 224 x 80 at (8, 58) */
    cv_round(4, 58, 232, 84, 4, K_PANEL);
    for (i = 0; i < (uint32_t)seq_sel; i++)
        s += seq_song.trk[i].nev;
    for (i = s; i < s + k->nev; i++) {
        if (seq_song.ev[i].note < lo)
            lo = seq_song.ev[i].note;
        if (seq_song.ev[i].note > hi)
            hi = seq_song.ev[i].note;
    }
    if (hi - lo < 12) {
        int m = (hi + lo) / 2;
        lo = k->nev ? m - 6 : 54;
        hi = lo + 12;
    }
    {
        uint32_t b4;
        for (b4 = 0; b4 < seq_song.bars && seq_song.bars <= 32u; b4++)   /* bar lines */
            cv_rect(8 + (int32_t)(224u * b4 * SEQ_BAR / len), 60, 1, 80, K_LINE);
    }
    for (i = s; i < s + k->nev; i++) {
        const seq_ev_t *e = &seq_song.ev[i];
        int32_t ex, ew, ey;
        if (e->tick >= len)
            continue;
        ex = 8 + (int32_t)(224u * e->tick / len);
        ew = (int32_t)(224u * e->len / len);
        ey = 60 + 78 - (int32_t)((e->note - lo) * 76 / (hi - lo)) - 2;
        cv_rect(ex, ey, ew < 2 ? 2 : ew, 3, is_drum(k) ? K_DRUMC : K_ACC);
    }
    if (seq_playing || seq_step) {
        uint32_t p = seq_step ? seq_step_tick : seq_tick;
        cv_rect(8 + (int32_t)(224u * p / len), 60, 1, 80, seq_step ? K_ACC : seq_rec ? K_RECC : K_PLAYC);
    }
    if (seq_step)
        cv_text(10, 124, &FONT_XS, "ARP rest   LFO back   SEQ done", K_DIM);
}

/* BAR: one bar of the selected track, close up: the steps, the step selected, the notes */
#define BAR_HELP1 "ENV del step (held: bar)  ARP undo"
#define BAR_HELP2 "LFO copy (held: paste)  ARP held: clear"
#define SONGS_HELP "SEL load (held: new)  SAVE  ARP held: del"
static void draw_bar(void)
{
    const seq_trk_t *k = sel_trk();
    char b[32], *q;
    uint32_t bar = (uint32_t)bar_cur(), b0 = bar * SEQ_BAR, st = step_ticks(), ns = SEQ_BAR / st, i, s = 0;
    uint32_t cs = seq_step_tick >= b0 && seq_step_tick < b0 + SEQ_BAR ? (seq_step_tick - b0) / st : 0xFFFFu;
    int32_t x;
    int lo = 127, hi = 0;
    b[0] = 0;
    q = cat(b, "Bar ");
    itoa_s((int32_t)bar + 1, q);
    q = cat(q + str_len(q), "/");
    itoa_s(seq_song.bars, q);
    x = cv_text(8, 0, &FONT_B, b, K_TEXT);
    if (seq_loop) {
        cv_round(x + 8, 2, text_w(&FONT_XS, "LOOP") + 8, 16, 5, K_PLAYC);
        x = cv_text(x + 12, 3, &FONT_XS, "LOOP", K_BG) + 4;
    }
    b[0] = 0;
    itoa2((uint32_t)seq_sel + 1u, b);
    q = cat(b + 2, " ");
    track_name(seq_sel, q);
    text_fit(x + 10, 3, 230 - x - 10, &FONT_XS, b, K_SELC);
    for (i = 0; i < (uint32_t)seq_sel; i++)
        s += seq_song.trk[i].nev;
    for (i = s; i < s + k->nev; i++) {          /* the notes' span in this bar, an octave at least */
        const seq_ev_t *e = &seq_song.ev[i];
        if (e->tick < b0 || e->tick >= b0 + SEQ_BAR)
            continue;
        lo = e->note < lo ? e->note : lo;
        hi = e->note > hi ? e->note : hi;
    }
    if (hi < lo) {
        lo = 54;
        hi = 66;
    } else if (hi - lo < 12) {
        lo = (hi + lo) / 2 - 6;
        hi = lo + 12;
    }
    cv_round(4, 20, 232, 90, 4, K_PANEL);
    for (i = 0; i < ns; i++) {                  /* the steps: the beats brighter, the step selected lit */
        int32_t sx = 8 + (int32_t)(224u * i / ns), sw = (int32_t)(224u * (i + 1u) / ns) - (int32_t)(224u * i / ns);
        if (i == cs)
            cv_rect(sx, 22, sw, 86, seq_step ? mix(K_PANEL, K_ACC, 5) : K_SEL_T);
        cv_rect(sx, 22, 1, 86, i * st % SEQ_PPQN == 0 ? K_DIM : K_LINE);
    }
    for (i = s; i < s + k->nev; i++) {
        const seq_ev_t *e = &seq_song.ev[i];
        int32_t ex, ew, ey;
        uint32_t end;
        if (e->tick < b0 || e->tick >= b0 + SEQ_BAR)
            continue;
        end = e->tick + e->len > b0 + SEQ_BAR ? b0 + SEQ_BAR : e->tick + e->len;
        ex = 8 + (int32_t)(224u * (e->tick - b0) / SEQ_BAR);
        ew = (int32_t)(224u * (end - e->tick) / SEQ_BAR);
        ey = 22 + 81 - (int32_t)((e->note - lo) * 80 / (hi - lo));
        cv_rect(ex + 1, ey, ew < 3 ? 2 : ew - 1, 4, is_drum(k) ? K_DRUMC : K_ACC);
    }
    if (seq_playing && !seq_counting && seq_tick >= b0 && seq_tick < b0 + SEQ_BAR)
        cv_rect(8 + (int32_t)(224u * (seq_tick - b0) / SEQ_BAR), 22, 1, 86, seq_rec ? K_RECC : K_PLAYC);
    cv_text(6, 114, &FONT_XS, BAR_HELP1, K_DIM);
    cv_text(6, 128, &FONT_XS, BAR_HELP2, K_DIM);
}

/* MIX: sixteen strips; the four the knobs move are marked */
static void draw_mix(void)
{
    int t, bank = seq_sel / 4;
    for (t = 0; t < SEQ_NTRK; t++) {
        const seq_trk_t *k = &seq_song.trk[t];
        int32_t x = 2 + t * 15, h = (int32_t)k->vol * 96 / 127, lv = lvl16(k->chan) * 96 / 16;
        char b[4];
        int inb = t / 4 == bank;
        cv_round(x, 6, 13, 100, 3, inb ? K_SEL_T : K_PANEL);
        if (lv)
            cv_rect(x + 2, 104 - lv, 9, lv, mix(K_PANEL, K_PLAYC, 8));
        cv_rect(x + 1, 104 - h, 11, 2, k->mute ? K_DIM : t == seq_sel ? K_ACC : K_TEXT);
        itoa_s(t + 1, b);
        text_c(x + 7, 110, &FONT_XS, b, t == seq_sel ? K_SELC : K_DIM);
        if (k->solo)
            text_c(x + 7, 124, &FONT_XS, "S", K_ACC);
        else if (k->mute)
            text_c(x + 7, 124, &FONT_XS, "M", K_RECC);
    }
}

/* SONGS: the slots, sixteen at a time (the page of the one highlighted), and the room left */
static void draw_songs(void)
{
    int s, first = ui.slot_cur / 16 * 16;
    char b[40], *q;
    for (s = first; s < first + 16 && s < SEQ_NSONGS; s++) {
        const slot_info_t *i = &slot_info[s];
        int32_t x = ((s - first) / 8) * 120 + 2, y = ((s - first) % 8) * 14 + 1;
        int cur = s == ui.slot_cur, in_use = s == set.song;
        cv_round(x, y, 116, 13, 3, cur ? K_SEL_T : K_PANEL);
        itoa2((uint32_t)s + 1u, b);
        cv_text(x + 4, y, &FONT_XS, b, in_use ? K_ACC : K_DIM);
        if (i->used) {
            b[0] = 0;                           /* bars and notes ("4b 120n") */
            itoa_s(i->bars, b);
            q = cat(b + str_len(b), "b ");
            itoa_s(i->nev, q);
            cat(q + str_len(q), "n");
            cv_text(x + 22, y, &FONT_XS, b, cur ? K_TEXT : K_DIM);
        } else {
            cv_text(x + 22, y, &FONT_XS, "empty", K_DIM);
        }
        if (in_use)
            cv_round(x + 116 - 9, y + 4, 5, 5, 2, song_dirty() ? K_ACC : K_PLAYC);
    }
    b[0] = 0;
    q = cat(b, "page ");
    itoa_s(first / 16 + 1, q);
    q = cat(q + str_len(q), "/");
    itoa_s(SEQ_NSONGS / 16, q);
    q = cat(q + str_len(q), "   free ");
    itoa_s((int32_t)(plat_song_free() * PLAT_SECTOR_BYTES / 1024u), q);
    q = cat(q + str_len(q), " of ");
    itoa_s((int32_t)(plat_song_sectors() * PLAT_SECTOR_BYTES / 1024u), q);
    cat(q + str_len(q), " KB");
    cv_text(6, 114, &FONT_XS, b, K_DIM);
    cv_text(6, 128, &FONT_XS, SONGS_HELP, K_DIM);
}

static void draw_setup(void)
{
    char b[40], *q;
    cv_text(8, 4, &FONT_B, "FM1-GM " OM_VERSION, K_TEXT);
    cv_text(8, 24, &FONT_XS, "General MIDI sequencer, 16 tracks", K_DIM);
    b[0] = 0;
    q = cat(b, "CPU ");
    itoa_s((int32_t)plat_cpu_pct(), q);
    q = cat(q + str_len(q), "%   voices ");
    itoa_s(gm_voices_used, q);
    q = cat(q + str_len(q), "+");
    itoa_s(gm_drums_used, q);
    q = cat(q + str_len(q), "   xruns ");
    itoa_s((int32_t)plat_xruns(), q);
    cv_text(8, 48, &FONT_S, b, K_TEXT);
    b[0] = 0;
    q = cat(b, "song ");
    itoa_s(seq_song.nev, q);
    q = cat(q + str_len(q), " notes, ");
    itoa_s((int32_t)song_bytes(&seq_song), q);
    cat(q + str_len(q), " B");
    cv_text(8, 70, &FONT_S, b, K_DIM);
    cv_text(8, 96, &FONT_XS, ui.kpage[V_SETUP] ? "GLO: MIDI settings" : "GLO: voices, click, lights, colours", K_DIM);
    cv_text(8, 112, &FONT_XS, "OCT- + OCT+ held 5 s: update mode", K_DIM);
}

static void draw_main(void)
{
    uint32_t h = main_sig();
    if (h == ui.sig[1])
        return;
    ui.sig[1] = h;
    cv_begin(240, MAIN_H, K_BG);
    switch (ui.view) {
    case V_SONG: draw_song(); break;
    case V_TRACK: draw_track(); break;
    case V_MIX: draw_mix(); break;
    case V_SONGS: draw_songs(); break;
    case V_BAR: draw_bar(); break;
    default: draw_setup(); break;
    }
    cv_blit(0, MAIN_Y);
}

static void draw_knobs(void)
{
    const uint8_t *kn = VIEW_KNOB[ui.view][ui.kpage[ui.view]];
    uint32_t h = hash(hash(2166136261u, ui.view | (uint32_t)ui.kpage[ui.view] << 4 | (uint32_t)ui.touched << 8 |
                                            (uint32_t)set.theme << 16), seq_sel), i;
    int now_touch = plat_ms() < ui.touch_until;
    for (i = 0; i < 4; i++)
        h = hash(h, (uint32_t)knob_get(kn[i]));
    h = hash(hash(h, (uint32_t)now_touch), sel_trk()->prog);
    if (h == ui.sig[2])
        return;
    ui.sig[2] = h;
    cv_begin(240, KNB_H, K_BG);
    cv_round(4, 2, 232, KNB_H - 6, 6, K_PANEL);
    if (now_touch && ui.touched == 4 && !is_drum(sel_trk())) {   /* the ALGORITHM knob: the program, whole */
        char b[32], *q;
        b[0] = 0;
        itoa_s(sel_trk()->prog + 1, b);
        q = cat(b + str_len(b), " ");
        cat(q, gm_prog_name(sel_trk()->prog));
        text_c(120, 8, &FONT_XS, "PROGRAM", K_DIM);
        text_c(120, 26, &FONT_M, b, K_ACC);
        cv_blit(0, KNB_Y);
        return;
    }
    for (i = 0; i < 4; i++) {
        int k = kn[i], lo, hi, v;
        int32_t x = (int32_t)i * 60, cx = x + 30, bw;
        int hot = now_touch && ui.touched == (int)i;
        char t[24];
        if (k == K_NONE)
            continue;
        if (i)
            cv_rect(x, 14, 1, KNB_H - 30, K_LINE);
        knob_range(k, &lo, &hi);
        v = knob_get(k);
        text_c(cx, 6, &FONT_XS, knob_name(k), hot ? K_TEXT : K_DIM);
        t[0] = 0;
        knob_text(k, v, t);
        {
            const char *q = t;
            int lower = 0;
            for (; *q; q++)
                lower |= *q >= 'a' && *q <= 'z';
            if (lower || text_w(&FONT_M, t) > 56)
                text_c(cx, 26, &FONT_B, t, hot ? K_ACC : K_TEXT);
            else
                text_c(cx, 22, &FONT_M, t, hot ? K_ACC : K_TEXT);
        }
        bw = hi > lo ? (int32_t)(44 * (v - lo) / (hi - lo)) : 0;
        cv_round(cx - 22, 50, 44, 6, 3, K_LINE);
        if (lo < 0) {
            int32_t m = cx - 22 + 44 * (0 - lo) / (hi - lo), e = cx - 22 + bw;
            cv_round(e < m ? e : m, 50, (e < m ? m - e : e - m) + 6, 6, 3, hot ? K_ACC : K_SELC);
        } else if (bw) {
            cv_round(cx - 22, 50, bw < 6 ? 6 : bw, 6, 3, hot ? K_ACC : K_SELC);
        }
    }
    cv_blit(0, KNB_Y);
}

static void leds(void)
{
    uint32_t b = 0, k = 0;
    static const uint8_t VIEW_BTN[NVIEWS] = {B_HOME, B_EDIT, B_FX, B_SEL, B_GLO, B_HOME};
    int blink = (ui.frame / 8u) & 1u;
    b |= 1u << VIEW_BTN[ui.view];
    if (seq_playing && !seq_counting)
        b |= seq_tick % SEQ_PPQN < SEQ_PPQN / 2 ? 1u << B_PLAY : 0u;
    if (seq_rec && (!seq_counting || blink))
        b |= 1u << B_REC;
    if (seq_step)
        b |= 1u << B_SEQ;
    if (sel_trk()->mute)
        b |= 1u << B_ENV;
    if (sel_trk()->solo)
        b |= 1u << B_LFO;
    if (seq_undo_ok && !seq_step)
        b |= 1u << B_ARP;
    if (song_dirty())
        b |= blink ? 1u << B_SAVE : 0u;
    if (ui.oct < 0)
        b |= 1u << B_OCTDN;
    if (ui.oct > 0)
        b |= 1u << B_OCTUP;
    k = ui.keys;
    if (!set.leds) {
        b = 0;
        k = 0;
    }
    plat_play_red(seq_rec);
    plat_glow(set.leds);
    plat_leds(b, k);
}

/* the settings save by themselves, a few seconds after a change, when nothing is touched and the
 * sequencer is stopped (a flash erase silences the audio for a moment) */
#define AUTOSAVE_QUIET 4000u
static void autosave(void)
{
    if (!ui.set_dirty || seq_playing || ui.btn || ui.keys || plat_ms() - ui.act_t < AUTOSAVE_QUIET)
        return;
    if (settings_save() == 0)
        ui.set_dirty = 0;
}

void ui_init(void)
{
    uint8_t keep = seq_sel;
    memset(&ui, 0, sizeof ui);
    ui.touched = -1;
    ui.saved_changes = seq_changes;
    ui.slot_cur = set.song;
    seq_sel = keep;
    thm = &THEME[set.theme < NTHEMES ? set.theme : 0];
    lcd_fill(0, 0, 240, 240, K_BG);
}

static void bar_follow(void)                    /* step recording off the bar: the page goes with it */
{
    if (ui.view == V_BAR && seq_step && seq_idle() && seq_step_tick / SEQ_BAR != (uint32_t)bar_cur() &&
        seq_step_tick / SEQ_BAR < seq_song.bars)
        ui.bar = (uint8_t)(seq_step_tick / SEQ_BAR);
}

void ui_frame(void)
{
    input();
    bar_follow();
    pending();
    autosave();
    draw_header();
    draw_main();
    draw_knobs();
    leds();
    ui.frame++;
}

void ui_input_only(void) { input(); }

#ifdef OM_HOST
/* the host's "fitcheck": every knob's name and value, the families beside a track's note count, the
 * lines of help, in the room the screen gives them. Prints what does not fit; returns how many. */
static int fit1(const char *what, const felucca_font_t *f, const char *t, int32_t room)
{
    int32_t w = text_w(f, t);
    if (w <= room)
        return 0;
    printf("does not fit: %s \"%s\" %d px, room %d\n", what, t, (int)w, (int)room);
    return 1;
}
int ui_fit_check(void)
{
    int bad = 0, k, v, lo, hi, p;
    char t[48];
    uint8_t keep = seq_song.bars;
    seq_song.bars = SEQ_MAXBARS;                 /* the widest the values get */
    for (k = K_TEMPO; k <= K_LOOP; k++) {
        const char *q;
        int lower;
        bad += fit1("knob name", &FONT_XS, knob_name(k), 58);
        knob_range(k, &lo, &hi);
        if (k == K_STEP)
            hi = SEQ_BAR / 6;
        for (v = lo; v <= hi; v++) {
            t[0] = 0;
            knob_text(k, v, t);
            for (q = t, lower = 0; *q; q++)
                lower |= *q >= 'a' && *q <= 'z';
            if (lower || text_w(&FONT_M, t) > 56)   /* as draw_knobs: the big font, else the bold */
                bad += fit1("knob value", &FONT_B, t, 58);
        }
    }
    seq_song.bars = keep;
    for (p = 0; p < 128; p += 8)                 /* TRACK: the family, then the notes at the right */
        bad += fit1("family (TRACK)", &FONT_S, gm_family_name(p), 232 - 16 - text_w(&FONT_S, "8192 notes"));
    bad += fit1("BAR help", &FONT_XS, BAR_HELP1, 228);
    bad += fit1("BAR help", &FONT_XS, BAR_HELP2, 228);
    bad += fit1("SONGS help", &FONT_XS, SONGS_HELP, 228);
    bad += fit1("SONGS row", &FONT_XS, "128b 8192n", 116 - 22 - 10);
    bad += fit1("SETUP", &FONT_S, "CPU 100%   voices 48+16   xruns 9999", 232);
    bad += fit1("SETUP", &FONT_S, "song 8192 notes, 32768 B", 232);
    bad += fit1("SONGS free", &FONT_XS, "page 4/4   free 281 of 281 KB", 228);
    return bad;
}
#endif
