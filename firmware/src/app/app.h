/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM app: the settings (one flash object), the songs' slots, and the app's entry points.
 * Included by the unity build after gfx.c (drawing helpers in scope) and by the host simulator. */
#pragma once
#include <stdint.h>
#include "plat.h"
#include "../dsp/seq.h"

#ifndef OM_VERSION                       /* a release build passes its own (tools/build.py --release) */
#define OM_VERSION "DEV"
#endif
#define SET_MAGIC 0x54534D47u            /* "GMST" */
#define SET_FORMAT 1u

typedef struct {
    uint32_t magic, format;
    uint8_t song;                        /* the slot in use (0..SEQ_NSONGS - 1) */
    uint8_t midi_in;                     /* 0 TRACK: every channel plays the selected track; 1 MULTI: as sent */
    uint8_t midi_out;                    /* seq_cfg's */
    uint8_t clock_out;
    uint8_t metro, metro_vol, countin;
    uint8_t poly;                        /* melodic voices, 8..GM_NVOICE */
    uint8_t leds;                        /* key and button lights: 0 off, 1 on */
    uint8_t theme;                       /* the screen's colours (ui.c THEME) */
    uint8_t rsv[22];
} settings_t;

extern settings_t set;

#define NTHEMES 5

void settings_defaults(void);
int settings_load(void);                 /* 0 = loaded; else the defaults are in place */
int settings_save(void);                 /* 0 = saved and verified */
void settings_apply(void);               /* to the sequencer and the engine */

/* the songs in flash: what each slot holds (read at boot, kept up to date by saves) */
typedef plat_song_info_t slot_info_t;
extern slot_info_t slot_info[SEQ_NSONGS];
void slots_scan(void);
/* with seq_lock set (the render leaves the song alone): */
int song_load_slot(int slot);            /* 0 loaded, 1 empty slot (a new song), -1 unreadable (a new song) */
int song_save_slot(int slot);            /* 0 = saved and verified, -10 = no room */
int song_delete_slot(int slot);          /* 0 = deleted */
void song_boot_load(void);               /* boot, before the audio starts: the slot in use */

void ui_init(void);
void ui_frame(void);
void ui_input_only(void);
void ui_say(const char *a, const char *b);
