/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM sequencer: 16 tracks of recorded notes, each on one of the 16 MIDI channels, played
 * through the GM engine (gm.h) and, optionally, MIDI out.
 *
 *   A song is a loop of 1..128 bars of 4/4 at 48 ticks a quarter note, at 30..300 BPM. Each track
 *   has its channel, GM program, volume, pan, reverb send, transpose, key velocity, mute and solo,
 *   and its notes (tick, note, velocity, length). Up to SEQ_MAXEV notes per song, shared by the
 *   tracks; a song is one flash object (plat.h OBJ_SONG0 + slot).
 *
 *   Real-time recording (overdub, quantized on input) while the loop plays, with an optional
 *   one-bar count-in; step recording while stopped; one level of undo; erase a track.
 *
 * Threads: seq_render() runs in the audio interrupt and owns the song and the engine. The main
 * loop changes them only through seq_post() (a command ring), and reads the song and the state
 * below for the screen. While seq_lock is set the render leaves the song alone (no commands, no
 * playback): the main loop loads or saves the song then. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "gm.h"

#define SEQ_NTRK 16
#define SEQ_PPQN 48
#define SEQ_BAR (SEQ_PPQN * 4)
#define SEQ_MAXBARS 128
#define SEQ_MAXEV 1280
#define SEQ_NSONGS 16
#define SONG_MAGIC 0x53474D46u          /* "FMGS" */
#define SONG_FORMAT 1u

typedef struct {
    uint16_t tick;                      /* 0 .. bars * SEQ_BAR - 1 */
    uint8_t note, vel;
    uint16_t len;                       /* ticks, >= 1 */
} seq_ev_t;

typedef struct {
    uint8_t chan;                       /* 0..15 (9 = the drums) */
    uint8_t prog;                       /* GM program 0..127 (ignored on channel 10) */
    uint8_t vol, pan, rev;              /* CC7, CC10, CC91 */
    uint8_t vel;                        /* velocity of notes played on the panel's keys */
    uint8_t mute, solo;
    int8_t transpose;                   /* semitones, -24..24, applied when it plays (not on channel 10) */
    uint8_t rsv;
    uint16_t nev;                       /* its notes in ev[] */
} seq_trk_t;

typedef struct {
    uint32_t magic, format;
    uint16_t tempo;                     /* BPM 30..300 */
    uint8_t bars;                       /* 1..SEQ_MAXBARS */
    uint8_t quant;                      /* index into seq_quant_ticks() */
    uint8_t rsv[24];
    seq_trk_t trk[SEQ_NTRK];
    uint16_t nev, rsv2;                 /* notes in all tracks */
    seq_ev_t ev[SEQ_MAXEV];             /* by track, then by tick */
} song_t;

#define SONG_HEAD_BYTES ((uint32_t)offsetof(song_t, ev))
static inline uint32_t song_bytes(const song_t *s) { return SONG_HEAD_BYTES + (uint32_t)s->nev * sizeof(seq_ev_t); }

enum { TP_CHAN, TP_PROG, TP_VOL, TP_PAN, TP_REV, TP_VEL, TP_MUTE, TP_SOLO, TP_TRANSPOSE, TP_N };
enum { SP_TEMPO, SP_BARS, SP_QUANT, SP_N };
#define SEQ_NQUANT 7
int seq_quant_ticks(int q);             /* 1 (off), 48, 24, 16, 12, 8, 6 */
const char *seq_quant_name(int q);      /* "OFF", "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" */

/* commands (main loop -> render) */
enum {
    SC_NOTE_ON,     /* a: track, b: note, c: velocity: played now, recorded when recording */
    SC_NOTE_OFF,    /* a: track, b: note */
    SC_MIDI,        /* a: status, b, c: data: straight to the engine (and recorded when it is a note on the
                     * recording track's channel) */
    SC_PLAY,        /* from the top (with the count-in when recording and it is on) */
    SC_STOP,
    SC_REC,         /* a: 1 arm / 0 disarm real-time recording */
    SC_STEP,        /* a: 1 enter / 0 leave step recording (stopped only) */
    SC_STEP_REST,   /* step recording: the cursor on by one step */
    SC_STEP_BACK,   /* step recording: back one step, clearing it */
    SC_SELECT,      /* a: the track that records */
    SC_TRK,         /* a: track, b: TP_*, w: value */
    SC_SONG,        /* a: SP_*, w: value */
    SC_ERASE,       /* a: track: all its notes */
    SC_UNDO,        /* swap the song with the copy taken before the last recording / erase */
    SC_APPLY,       /* every track's settings to the engine (after a load) */
    SC_PANIC,       /* everything quiet */
    SC_GMRESET,     /* the engine to GM defaults, then SC_APPLY */
};
int seq_post(uint32_t op, uint32_t a, uint32_t b, uint32_t c, int32_t w);   /* 0, or -1 (full) */
int seq_idle(void);                     /* 1 = every command posted has been carried out */

typedef struct {
    uint8_t midi_out;                   /* the notes the sequencer and the keys play go to MIDI out too */
    uint8_t clock_out;                  /* MIDI clock, start, stop while playing */
    uint8_t metro;                      /* 0 off, 1 while recording, 2 always */
    uint8_t metro_vol;                  /* 0..10 */
    uint8_t countin;                    /* 1: a bar of clicks before real-time recording from the top */
} seq_cfg_t;
extern seq_cfg_t seq_cfg;               /* the main loop writes it; the render reads it */

extern song_t seq_song;                 /* the song (the main loop: read only, or while seq_lock) */
extern volatile uint8_t seq_lock;
/* state (UI) */
extern volatile uint8_t seq_playing, seq_rec, seq_step, seq_counting, seq_full, seq_sel;
extern volatile uint16_t seq_tick;      /* the tick playing */
extern volatile uint16_t seq_step_tick; /* the step cursor */
extern volatile uint32_t seq_changes;   /* + 1 for every change to the song */
extern volatile uint8_t seq_undo_ok;

void seq_init(void);                    /* boot, before the audio starts: the default song */
void song_defaults(song_t *s);
int song_valid(const song_t *s, uint32_t len);   /* a loaded object is a song: 1 */
uint32_t seq_song_ticks(void);
/* the render (audio ISR): n stereo frames, 24-bit in int32; gain Q12 */
void seq_render(int32_t *out_lr, uint32_t n, uint32_t gain_q12);
/* MIDI out from the render: the app sends it (USB) */
void seq_midi_out(uint32_t st, uint32_t d1, uint32_t d2);
