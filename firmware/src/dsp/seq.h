/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM sequencer: 16 tracks of recorded notes, each on one of the 16 MIDI channels, played
 * through the GM engine (gm.h) and, optionally, MIDI out.
 *
 *   A song is a loop of 1..128 bars of 4/4 at 48 ticks a quarter note, at 30..300 BPM. Each track
 *   has its channel, GM program, volume, pan, reverb send, transpose, velocity, mute and solo, and
 *   its notes (tick, note, length): every note of a track plays at the track's velocity, as the
 *   panel's keys do. Up to SEQ_MAXEV notes per song, shared by the tracks.
 *
 *   Real-time recording (overdub, quantized on input) while the loop plays, with an optional
 *   one-bar count-in; step recording while stopped; one level of undo; erase a track. Bar editing:
 *   loop one bar; clear, copy, paste or delete it; delete one step's notes, or change their length.
 *
 *   In flash a song is a byte stream (song_stream / song_parse), as compact as a MIDI file without
 *   the parts the FM-1 has no use for (velocity, controllers, bend): the song's and the tracks'
 *   settings, then each track's notes as (delta ticks, note, length), the deltas and lengths as
 *   MIDI's variable-length numbers, the length left out when it is the note before's (the note's
 *   top bit says so, like MIDI's running status). About 2 to 4 bytes a note.
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
#define SEQ_MAXEV 8192
#define SEQ_NSONGS 64
#define SONG_FORMAT 2u                  /* the stream's first byte */
#define SEQ_CLIP 1024                   /* notes in the bar clipboard */

typedef struct {
    uint16_t tick;                      /* 0 .. bars * SEQ_BAR - 1 */
    uint16_t len;                       /* ticks, >= 1 */
    uint8_t note, rsv;
} seq_ev_t;

typedef struct {
    uint8_t chan;                       /* 0..15 (9 = the drums) */
    uint8_t prog;                       /* GM program 0..127 (ignored on channel 10) */
    uint8_t vol, pan, rev;              /* CC7, CC10, CC91 */
    uint8_t vel;                        /* the velocity its notes play at (and the panel's keys) */
    uint8_t mute, solo;
    int8_t transpose;                   /* semitones, -24..24, applied when it plays (the drums too) */
    uint8_t rsv;
    uint16_t nev;                       /* its notes in ev[] */
} seq_trk_t;

typedef struct {
    uint16_t tempo;                     /* BPM 30..300 */
    uint8_t bars;                       /* 1..SEQ_MAXBARS */
    uint8_t quant;                      /* index into seq_quant_ticks() */
    uint8_t rsv[4];
    seq_trk_t trk[SEQ_NTRK];
    uint16_t nev, rsv2;                 /* notes in all tracks */
    seq_ev_t ev[SEQ_MAXEV];             /* by track, then by tick */
} song_t;

#define SONG_HEAD_BYTES ((uint32_t)offsetof(song_t, ev))
static inline uint32_t song_ram_bytes(const song_t *s) { return SONG_HEAD_BYTES + (uint32_t)s->nev * sizeof(seq_ev_t); }

enum { TP_CHAN, TP_PROG, TP_VOL, TP_PAN, TP_REV, TP_VEL, TP_MUTE, TP_SOLO, TP_TRANSPOSE, TP_N };
enum { SP_TEMPO, SP_BARS, SP_QUANT, SP_N };
#define SEQ_NQUANT 7
int seq_quant_ticks(int q);             /* 1 (off), 48, 24, 16, 12, 8, 6 */
const char *seq_quant_name(int q);      /* "OFF", "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" */

/* commands (main loop -> render) */
enum {
    SC_NOTE_ON,     /* a: track, b: note, c: velocity: played now, recorded when recording (without it) */
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
    /* bar editing: w is the bar (0..) unless said */
    SC_LOOP,        /* a: 1 play (and record) bar w only, over and over; 0 the whole song */
    SC_CURSOR,      /* w: the step cursor's tick (also where SC_STEP_DELETE works, stopped or not) */
    SC_BAR_CLEAR,   /* a: track: its notes in bar w */
    SC_BAR_COPY,    /* a: track: its notes in bar w to the clipboard */
    SC_BAR_PASTE,   /* a: track: bar w's notes replaced by the clipboard's */
    SC_BAR_DELETE,  /* bar w out of the song, every track; the bars after it move up one */
    SC_STEP_DELETE, /* a: track: its notes in the step at the step cursor (a quantize step long); c 1: only
                     * note b; c 2: only note b, and the undo copy kept (the next of a chord of keys) */
    SC_STEP_LEN,    /* a: track, b: note (128: every note): its notes in the step at the step cursor, w steps
                     * longer (w < 0: shorter), a step at least; one undo for the changes at a step in a row */
    SC_STEP_MOVE,   /* a: track | 16 (by time) | 32 (go on: the notes the last move moved, if it was the
                     * last edit), b: base note, w: notes (bit i: base + i; 0: every note), c: an int8 d:
                     * the step's notes d semitones up, or d steps later in their bar (the cursor follows);
                     * none move if one can't (seq_step_can_move, kind 1 time | 2 go on); one undo as for
                     * SC_STEP_LEN */
    SC_INIT,        /* a new song in place of this one: no notes, every track and setting at its default
                     * (stops first; undo brings the song back) */
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
extern volatile uint8_t seq_loop;       /* bar seq_loop_bar only (SC_LOOP) */
extern volatile uint8_t seq_loop_bar;
extern volatile uint16_t seq_clip_n;    /* notes in the clipboard (SC_BAR_COPY) */

void seq_init(void);                    /* boot, before the audio starts: the default song */
void song_defaults(song_t *s);
/* bytes from .. from + n - 1 of song s's stream into dst (dst 0: none); returns the stream's length */
uint32_t song_stream(const song_t *s, uint8_t *dst, uint32_t from, uint32_t n);
/* song s from a stream, get() its next byte (-1 at the end, < -1 an error): 0, or -1 (s is undefined) */
int song_parse(song_t *s, int (*get)(void));
static inline uint32_t song_bytes(const song_t *s) { return song_stream(s, 0, 0, 0); }
/* the song from a stream (as song_parse) in place of the song in memory, the one before kept as the undo
 * copy (SC_UNDO brings it back); stopped and with seq_lock set. 0, or -1: not a song (nothing changed) */
int seq_import(int (*get)(void));
uint32_t seq_song_ticks(void);
int seq_step_can_move(int t, int base, uint32_t mask, int kind, int32_t d);   /* SC_STEP_MOVE would move notes */
/* the render (audio ISR): n stereo frames, 24-bit in int32; gain Q12 */
void seq_render(int32_t *out_lr, uint32_t n, uint32_t gain_q12);
/* MIDI out from the render: the app sends it (USB) */
void seq_midi_out(uint32_t st, uint32_t d1, uint32_t d2);
