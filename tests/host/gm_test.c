/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM engine and sequencer, on the host: every program and drum sounds (and stops), levels stay in
 * range, the worst case's cost; the sequencer's timing, recording, quantize, loop, mute, erase, undo
 * and step recording, read off what it sends to MIDI out.
 *   tests/host/gm_test [WAVDIR]     (WAVDIR: a WAV of every program and of the drum kit) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gm.h"
#include "seq.h"

static int fails;
#define CHECK(c, ...)                                                                                       \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                     \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
            fails++;                                                                                         \
        }                                                                                                    \
    } while (0)

/* MIDI out, as the sequencer sends it: a log with the frame it went out at */
typedef struct {
    uint32_t frame, st, d1, d2;
} msg_t;
static msg_t mlog[8192];
static int nlog;
static uint32_t frame_now;
void seq_midi_out(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (nlog < 8192 && st != 0xF8) {
        mlog[nlog].frame = frame_now;
        mlog[nlog].st = st;
        mlog[nlog].d1 = d1;
        mlog[nlog].d2 = d2;
        nlog++;
    }
}

static int32_t buf[2 * 256];
static double peak, sumsq;
static long nsamp;
static FILE *wav;

static void wav_open(const char *path)
{
    static const uint8_t hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                                    1, 0, 2, 0, 0x44, 0xAC, 0, 0, 0x10, 0xB1, 2, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a',
                                    0, 0, 0, 0};
    wav = fopen(path, "wb");
    if (wav)
        fwrite(hdr, 1, 44, wav);
}
static void wav_close(void)
{
    long n;
    uint8_t b[4];
    if (!wav)
        return;
    n = ftell(wav) - 44;
    fseek(wav, 4, SEEK_SET);
    b[0] = (uint8_t)(n + 36), b[1] = (uint8_t)((n + 36) >> 8), b[2] = (uint8_t)((n + 36) >> 16), b[3] = (uint8_t)((n + 36) >> 24);
    fwrite(b, 1, 4, wav);
    fseek(wav, 40, SEEK_SET);
    b[0] = (uint8_t)n, b[1] = (uint8_t)(n >> 8), b[2] = (uint8_t)(n >> 16), b[3] = (uint8_t)(n >> 24);
    fwrite(b, 1, 4, wav);
    fclose(wav);
    wav = 0;
}

static void run(uint32_t frames)                /* the render, 256 frames at a time, as the device */
{
    while (frames) {
        uint32_t n = frames > 256 ? 256 : frames, i;
        seq_render(buf, n, 4096);
        for (i = 0; i < 2 * n; i++) {
            double v = buf[i] / 8388608.0;
            if (v != v)
                v = 9.0;                        /* NaN */
            if (fabs(v) > peak)
                peak = fabs(v);
            sumsq += v * v;
            nsamp++;
            if (wav) {
                int16_t s = (int16_t)(buf[i] >> 8);
                fwrite(&s, 2, 1, wav);
            }
        }
        frame_now += n;
        frames -= n;
    }
}
static void meter_reset(void)
{
    peak = sumsq = 0;
    nsamp = 0;
}
static double rms(void) { return nsamp ? sqrt(sumsq / (double)nsamp) : 0; }

static void post(uint32_t op, uint32_t a, uint32_t b, uint32_t c, int32_t w)
{
    CHECK(seq_post(op, a, b, c, w) == 0, "queue full");
}
static void midi(uint32_t st, uint32_t d1, uint32_t d2) { post(SC_MIDI, st, d1, d2, 0); }

/* ----------------------------------------------------------------- engine --- */
static void test_programs(const char *dir)
{
    int p;
    char path[512];
    for (p = 0; p < 128; p++) {
        double on_rms, tail;
        midi(0xC0, (uint32_t)p, 0);
        if (dir) {
            snprintf(path, sizeof path, "%s/prog%03d.wav", dir, p);
            wav_open(path);
        }
        meter_reset();
        midi(0x90, 60, 100);
        midi(0x90, 64, 90);
        midi(0x90, 67, 90);
        run(44100 / 2);
        on_rms = rms();
        CHECK(peak < 1.0, "program %d (%s): peak %.3f", p, gm_prog_name(p), peak);
        CHECK(on_rms > 0.005 || p == 119, "program %d (%s) is silent (rms %.4f)", p, gm_prog_name(p), on_rms);
        midi(0x80, 60, 0);
        midi(0x80, 64, 0);
        midi(0x80, 67, 0);
        run(44100 * 3);
        meter_reset();
        run(44100 * 4);                         /* long releases end, and the reverb tails off */
        tail = peak;
        CHECK(tail < 0.002, "program %d (%s) does not stop (%.4f)", p, gm_prog_name(p), tail);
        CHECK(gm_voices_used == 0, "program %d: %d voices left", p, gm_voices_used);
        wav_close();
        if (getenv("GM_VERBOSE"))
            printf("  %3d %-16s rms %.3f\n", p, gm_prog_name(p), on_rms);
    }
    midi(0xC0, 0, 0);
}

static void test_drums(const char *dir)
{
    int n;
    char path[512];
    if (dir) {
        snprintf(path, sizeof path, "%s/drums.wav", dir);
        wav_open(path);
    }
    for (n = GM_DRUM_LO; n <= GM_DRUM_HI; n++) {
        meter_reset();
        midi(0x99, (uint32_t)n, 110);
        run(44100 / 3);
        CHECK(peak > 0.01 && peak < 1.0, "drum %d (%s): peak %.3f", n, gm_drum_name(n), peak);
    }
    run(44100 * 4);
    meter_reset();
    run(44100);
    CHECK(peak < 0.002, "drums do not stop (%.4f)", peak);
    wav_close();
    /* the closed hat cuts the open one */
    midi(0x99, 46, 120);
    run(4410);
    midi(0x99, 42, 120);
    run(44100 / 2);
    meter_reset();
    run(4410);
    CHECK(peak < 0.01, "open hat not choked (%.4f)", peak);
}

/* a held note settles into its waveform and stays put: no loop seam, no wobble, in any register */
static void test_sustain(void)
{
    static const int PROG[] = {16, 19, 80, 81};     /* organs, square and saw leads: no fade of their own */
    int p, k, w;
    midi(0xB0, 91, 0);
    for (p = 0; p < 4; p++) {
        midi(0xC0, (uint32_t)PROG[p], 0);
        for (k = 48; k <= 96; k += 24) {
            double lo = 1e9, hi = -1e9;
            midi(0x90, (uint32_t)k, 100);
            run(44100 * 3 / 2);                         /* low down, the attack plays slower */
            for (w = 0; w < 6; w++) {                   /* 1.5 .. 2.7 s, in 0.2 s windows */
                double db;
                meter_reset();
                run(8820);
                db = 20 * log10(rms() + 1e-9);
                lo = db < lo ? db : lo;
                hi = db > hi ? db : hi;
            }
            CHECK(lo > -60.0 && hi - lo < 0.5, "program %d (%s) key %d: held level %.1f .. %.1f dB",
                  PROG[p], gm_prog_name(PROG[p]), k, lo, hi);
            midi(0x80, (uint32_t)k, 0);
            run(44100 * 2);
        }
    }
    midi(0xB0, 121, 0);
    midi(0xB0, 91, 40);
    midi(0xC0, 0, 0);
}

static void test_controllers(void)
{
    double a, b;
    midi(0xC0, 80, 0);                          /* square lead: steady */
    midi(0x90, 69, 100);
    run(22050);
    meter_reset();
    run(11025);
    a = rms();
    midi(0xB0, 7, 50);                          /* volume down */
    run(2205);
    meter_reset();
    run(11025);
    b = rms();
    CHECK(b < a * 0.3, "CC7 50 does not quieten (%.4f -> %.4f)", a, b);
    midi(0xB0, 7, 100);
    midi(0xB0, 64, 127);                        /* sustain: the note holds after its note off */
    midi(0x80, 69, 0);
    run(22050);
    meter_reset();
    run(4410);
    CHECK(rms() > a * 0.5, "sustain pedal does not hold (%.4f)", rms());
    midi(0xB0, 64, 0);
    run(44100);
    meter_reset();
    run(4410);
    CHECK(peak < 0.01, "sustain off does not release (%.4f)", peak);
    midi(0xB0, 121, 0);
    midi(0xC0, 0, 0);
    run(44100 * 3);
}

static void test_polyphony_cost(void)
{
    int i, p;
    clock_t t0;
    double us;
    static const int HEAVY[] = {89, 48, 92, 0, 81};   /* filtered pads, strings, vibrato, piano, lead */
    for (p = 0; p < 5; p++) {
        midi(0xC0 | (uint32_t)p, (uint32_t)HEAVY[p], 0);
        for (i = 0; i < 5; i++)
            midi(0x90 | (uint32_t)p, (uint32_t)(48 + p * 5 + i * 3), 100);
    }
    for (i = 0; i < 8; i++)
        midi(0x99, (uint32_t)(36 + i * 4), 100);
    run(256);
    CHECK(gm_voices_used == gm_voice_max, "%d voices sounding, expected %d", gm_voices_used, gm_voice_max);
    t0 = clock();
    run(44100);
    us = (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC;
    printf("  worst case: %d voices + %d drums, %.1f us of host CPU per second of audio (%.2f%%)\n",
           gm_voices_used, gm_drums_used, us, us / 1e4);
    CHECK(peak < 1.0, "full polyphony clips (%.3f)", peak);
    post(SC_PANIC, 0, 0, 0, 0);
    for (p = 0; p < 5; p++)
        midi(0xB0 | (uint32_t)p, 121, 0);
    post(SC_GMRESET, 0, 0, 0, 0);
    run(256);
}

/* ------------------------------------------------------------- sequencer --- */
#define SPT(bpm) (44100.0 * 60.0 / ((bpm) * SEQ_PPQN))     /* frames a tick */

static int log_find(uint32_t st, uint32_t d1, int from)
{
    int i;
    for (i = from; i < nlog; i++)
        if (mlog[i].st == st && mlog[i].d1 == d1)
            return i;
    return -1;
}

static void test_record_play(void)
{
    uint32_t t0;
    int i, k;
    double spt = SPT(120);
    seq_cfg.midi_out = 1;
    seq_cfg.countin = 0;
    seq_cfg.metro = 0;
    post(SC_SELECT, 0, 0, 0, 0);
    post(SC_SONG, SP_BARS, 0, 0, 1);            /* one bar, 192 ticks, ~2 s at 120 */
    post(SC_SONG, SP_QUANT, 0, 0, 4);           /* 1/16 = 12 ticks */
    post(SC_REC, 1, 0, 0, 0);
    post(SC_PLAY, 0, 0, 0, 0);
    run(256);
    CHECK(seq_playing && seq_rec, "not playing / recording");
    t0 = frame_now - 256;                       /* tick 0 went out at the first block */
    /* play C4 a little after tick 12 (a 16th), hold it for ~ an 8th; then E4 near tick 50 (-> 48) */
    run((uint32_t)(spt * 13.5) - (frame_now - t0));
    post(SC_NOTE_ON, 0, 60, 100, 0);
    run((uint32_t)(spt * 24));
    post(SC_NOTE_OFF, 0, 60, 0, 0);
    run((uint32_t)(spt * (50 - 37.5)));
    post(SC_NOTE_ON, 0, 64, 90, 0);
    run((uint32_t)(spt * 6));
    post(SC_NOTE_OFF, 0, 64, 0, 0);
    run(256);
    CHECK(seq_song.trk[0].nev == 2, "track 1 has %d notes, expected 2", seq_song.trk[0].nev);
    if (seq_song.trk[0].nev == 2) {
        CHECK(seq_song.ev[0].tick == 12 && seq_song.ev[0].note == 60, "C4 at %d", seq_song.ev[0].tick);
        CHECK(abs((int)seq_song.ev[0].len - 24) <= 2, "C4 length %d", seq_song.ev[0].len);
        CHECK(seq_song.ev[1].tick == 48 && seq_song.ev[1].note == 64, "E4 at %d", seq_song.ev[1].tick);
    }
    post(SC_REC, 0, 0, 0, 0);
    /* the next pass plays both, on time */
    nlog = 0;
    run((uint32_t)(spt * 192 * 1.05));
    i = log_find(0x90, 60, 0);
    k = log_find(0x90, 64, 0);
    CHECK(i >= 0 && k >= 0, "the recorded notes did not play");
    if (i >= 0 && k >= 0) {
        int32_t d = (int32_t)(mlog[k].frame - mlog[i].frame) - (int32_t)(spt * 36);
        int off = log_find(0x80, 60, i);
        CHECK(abs(d) <= 256 + 32, "C4 -> E4 is %d frames off 36 ticks", d);
        CHECK(off > 0 && abs((int)(mlog[off].frame - mlog[i].frame) - (int)(spt * seq_song.ev[0].len)) <= 64,
              "C4 note off at the wrong time");
    }
    /* no note played twice in that pass */
    CHECK(log_find(0x90, 60, i + 1) < 0 || mlog[log_find(0x90, 60, i + 1)].frame - mlog[i].frame > spt * 180,
          "C4 played twice in a pass");
    /* mute: nothing from track 1 */
    post(SC_TRK, 0, TP_MUTE, 0, 1);
    run(256);
    nlog = 0;
    run((uint32_t)(spt * 200));
    CHECK(log_find(0x90, 60, 0) < 0, "a muted track played");
    post(SC_TRK, 0, TP_MUTE, 0, 0);
    /* transpose: +2 */
    post(SC_TRK, 0, TP_TRANSPOSE, 0, 2);
    run(256);
    nlog = 0;
    run((uint32_t)(spt * 200));
    CHECK(log_find(0x90, 62, 0) >= 0 && log_find(0x90, 60, 0) < 0, "transpose not applied");
    post(SC_TRK, 0, TP_TRANSPOSE, 0, 0);
    /* channel: track 1 on channel 3 */
    post(SC_TRK, 0, TP_CHAN, 0, 2);
    run(256);
    nlog = 0;
    run((uint32_t)(spt * 200));
    CHECK(log_find(0x92, 60, 0) >= 0, "track 1 not on channel 3");
    post(SC_TRK, 0, TP_CHAN, 0, 0);
    post(SC_STOP, 0, 0, 0, 0);
    run(256);
    CHECK(!seq_playing, "still playing");
    /* erase, undo (back), undo again (redo) */
    post(SC_ERASE, 0, 0, 0, 0);
    run(256);
    CHECK(seq_song.trk[0].nev == 0 && seq_song.nev == 0, "erase left %d notes", seq_song.nev);
    post(SC_UNDO, 0, 0, 0, 0);
    run(256);
    CHECK(seq_song.trk[0].nev == 2, "undo: %d notes", seq_song.trk[0].nev);
    post(SC_UNDO, 0, 0, 0, 0);
    run(256);
    CHECK(seq_song.trk[0].nev == 0, "redo: %d notes", seq_song.trk[0].nev);
    post(SC_UNDO, 0, 0, 0, 0);
    run(256);
}

static void test_step(void)
{
    int i;
    /* step recording on track 10 (the drums): kick, rest, snare, kick+hat */
    post(SC_SELECT, 9, 0, 0, 0);
    post(SC_STEP, 1, 0, 0, 0);
    run(256);
    CHECK(seq_step, "not in step mode");
    post(SC_NOTE_ON, 9, 36, 100, 0);
    post(SC_NOTE_OFF, 9, 36, 0, 0);
    post(SC_STEP_REST, 0, 0, 0, 0);
    post(SC_NOTE_ON, 9, 38, 100, 0);
    post(SC_NOTE_OFF, 9, 38, 0, 0);
    post(SC_NOTE_ON, 9, 36, 100, 0);
    post(SC_NOTE_ON, 9, 42, 100, 0);
    post(SC_NOTE_OFF, 9, 36, 0, 0);
    post(SC_NOTE_OFF, 9, 42, 0, 0);
    run(256);
    CHECK(seq_song.trk[9].nev == 4, "drum track: %d notes", seq_song.trk[9].nev);
    CHECK(seq_step_tick == 48, "cursor at %d, expected 48", seq_step_tick);
    {
        uint32_t s = seq_song.nev - seq_song.trk[9].nev;    /* track 10 is after 1..9 (and 11..16 are empty) */
        static const uint16_t T[4] = {0, 24, 36, 36};
        for (i = 0; i < 4 && i < seq_song.trk[9].nev; i++)
            CHECK(seq_song.ev[s + (uint32_t)i].tick == T[i], "step note %d at %d", i, seq_song.ev[s + (uint32_t)i].tick);
    }
    post(SC_STEP_BACK, 0, 0, 0, 0);             /* clears the kick + hat step */
    run(256);
    CHECK(seq_song.trk[9].nev == 2 && seq_step_tick == 36, "step back: %d notes, cursor %d", seq_song.trk[9].nev,
          seq_step_tick);
    post(SC_STEP, 0, 0, 0, 0);
    run(256);
}

static const uint8_t *rd_buf;
static uint32_t rd_n, rd_i;
static int rd_get(void) { return rd_i < rd_n ? rd_buf[rd_i++] : -1; }

static int trk_first(int t)
{
    int i, k = 0;
    for (i = 0; i < t; i++)
        k += seq_song.trk[i].nev;
    return k;
}

/* bar editing on track 3: loop a bar, copy, paste, clear, delete it; delete a step */
static void test_bars(void)
{
    int s;
    post(SC_SONG, SP_BARS, 0, 0, 4);
    post(SC_SONG, SP_QUANT, 0, 0, 4);
    post(SC_SELECT, 2, 0, 0, 0);
    post(SC_LOOP, 1, 0, 0, 1);                 /* bar 2 only */
    post(SC_CURSOR, 0, 0, 0, SEQ_BAR + 12);
    post(SC_STEP, 1, 0, 0, 0);
    run(256);
    CHECK(seq_step_tick == SEQ_BAR + 12, "cursor at %d, expected %d", seq_step_tick, SEQ_BAR + 12);
    for (s = 0; s < 16; s++) {                 /* a bar of 1/16s, and the cursor wraps in the bar */
        post(SC_NOTE_ON, 2, (uint32_t)(60 + s), 100, 0);
        post(SC_NOTE_OFF, 2, (uint32_t)(60 + s), 0, 0);
    }
    post(SC_STEP, 0, 0, 0, 0);
    run(256);
    s = trk_first(2);
    CHECK(seq_song.trk[2].nev == 16, "bar 2: %d notes", seq_song.trk[2].nev);
    CHECK(seq_step_tick == SEQ_BAR + 12 && seq_song.ev[s].tick == SEQ_BAR && seq_song.ev[s + 15].tick == SEQ_BAR + 180,
          "the cursor did not wrap in the bar: %d, first %d, last %d", seq_step_tick, seq_song.ev[s].tick,
          seq_song.ev[s + 15].tick);
    post(SC_BAR_COPY, 2, 0, 0, 1);
    post(SC_BAR_PASTE, 2, 0, 0, 3);
    run(256);
    CHECK(seq_clip_n == 16 && seq_song.trk[2].nev == 32, "copy, paste: clip %d, %d notes", seq_clip_n,
          seq_song.trk[2].nev);
    post(SC_BAR_PASTE, 2, 0, 0, 3);             /* again over it: replaces, does not add */
    run(256);
    CHECK(seq_song.trk[2].nev == 32, "paste over: %d notes", seq_song.trk[2].nev);
    post(SC_BAR_CLEAR, 2, 0, 0, 3);
    run(256);
    CHECK(seq_song.trk[2].nev == 16, "clear: %d notes", seq_song.trk[2].nev);
    post(SC_UNDO, 0, 0, 0, 0);
    run(256);
    CHECK(seq_song.trk[2].nev == 32, "undo the clear: %d notes", seq_song.trk[2].nev);
    post(SC_BAR_DELETE, 0, 0, 0, 0);            /* bar 1 out: bar 2 is bar 1 */
    run(256);
    s = trk_first(2);
    CHECK(seq_song.bars == 3 && seq_song.trk[2].nev == 32 && seq_song.ev[s].tick == 0 &&
          seq_song.ev[s + 16].tick == 2 * SEQ_BAR, "delete bar: %d bars, %d notes, first at %d, bar 3 at %d",
          seq_song.bars, seq_song.trk[2].nev, seq_song.ev[s].tick, seq_song.ev[s + 16].tick);
    CHECK(seq_loop_bar == 1, "the loop bar is %d", seq_loop_bar);
    /* the step at tick 12 (note 60, the bar's first step before): deleted, then undone */
    post(SC_LOOP, 0, 0, 0, 0);
    post(SC_CURSOR, 0, 0, 0, 12);
    post(SC_STEP_DELETE, 2, 0, 0, 0);
    run(256);
    s = trk_first(2) + 1;
    CHECK(seq_song.trk[2].nev == 31 && seq_song.ev[s].tick == 24, "delete a step: %d notes, next at %d",
          seq_song.trk[2].nev, seq_song.ev[s].tick);
    post(SC_UNDO, 0, 0, 0, 0);
    run(256);
    s = trk_first(2) + 1;
    CHECK(seq_song.trk[2].nev == 32 && seq_song.ev[s].tick == 12 && seq_song.ev[s].note == 60,
          "undo the step delete: %d notes, %d at %d", seq_song.trk[2].nev, seq_song.ev[s].note, seq_song.ev[s].tick);
    post(SC_LOOP, 1, 0, 0, 1);
    post(SC_PLAY, 0, 0, 0, 0);                  /* looped: playback stays in the bar */
    run(GM_SR * 3);
    CHECK(seq_playing && seq_tick >= SEQ_BAR && seq_tick < 2 * SEQ_BAR, "looped bar: tick %d", seq_tick);
    post(SC_STOP, 0, 0, 0, 0);
    post(SC_LOOP, 0, 0, 0, 0);
    post(SC_ERASE, 2, 0, 0, 0);
    run(256);
}

static void test_full(void)
{
    int i;
    post(SC_SELECT, 1, 0, 0, 0);
    post(SC_SONG, SP_BARS, 0, 0, SEQ_MAXBARS);
    post(SC_SONG, SP_QUANT, 0, 0, 6);
    post(SC_STEP, 1, 0, 0, 0);
    for (i = 0; i < SEQ_MAXEV + 10; i++) {
        post(SC_NOTE_ON, 1, (uint32_t)(40 + i % 40), 100, 0);
        post(SC_NOTE_OFF, 1, (uint32_t)(40 + i % 40), 0, 0);
        if (i % 64 == 0)
            run(256);
    }
    run(256);
    CHECK(seq_song.nev == SEQ_MAXEV && seq_full, "full song: %d notes, full %d", seq_song.nev, seq_full);
    {   /* the stream: back to the same song, and small */
        static uint8_t buf[SEQ_MAXEV * 8];
        static song_t back;
        uint32_t n = song_bytes(&seq_song);
        CHECK(n <= sizeof buf, "stream %u B", n);
        song_stream(&seq_song, buf, 0, n);
        rd_buf = buf;
        rd_n = n;
        rd_i = 0;
        CHECK(song_parse(&back, rd_get) == 0, "the full song's stream does not parse");
        CHECK(!memcmp(&back, &seq_song, song_ram_bytes(&seq_song)), "the stream does not give the song back");
        CHECK(n < 3u * SEQ_MAXEV, "a full song is %u B (%.2f B a note)", n, (double)n / SEQ_MAXEV);
        printf("full song: %u notes, %u B in flash (%.2f B a note)\n", (unsigned)seq_song.nev, n,
               (double)n / seq_song.nev);
        rd_n = n - 1;                           /* cut short: refused */
        rd_i = 0;
        CHECK(song_parse(&back, rd_get) != 0, "a truncated stream parses");
        buf[0] ^= 0x40;                         /* not this format: refused */
        rd_n = n;
        rd_i = 0;
        CHECK(song_parse(&back, rd_get) != 0, "a stream of another format parses");
    }
    post(SC_STEP, 0, 0, 0, 0);
    post(SC_ERASE, 1, 0, 0, 0);
    run(256);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : 0;
    seq_init();
    test_programs(dir);
    test_drums(dir);
    test_sustain();
    test_controllers();
    test_polyphony_cost();
    test_record_play();
    test_step();
    test_bars();
    test_full();
    printf("song in RAM: %u B header + %u B a note, %u B full\n", (unsigned)SONG_HEAD_BYTES,
           (unsigned)sizeof(seq_ev_t), (unsigned)sizeof(song_t));
    if (fails)
        printf("%d FAILED\n", fails);
    else
        printf("gm_test: all passed\n");
    return fails != 0;
}
