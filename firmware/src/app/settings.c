/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM settings and song slots. Part of the unity build, after app.h; the host simulator includes it
 * too. */
settings_t set;
slot_info_t slot_info[SEQ_NSONGS];

void settings_defaults(void)
{
    memset(&set, 0, sizeof set);
    set.magic = SET_MAGIC;
    set.format = SET_FORMAT;
    set.midi_in = 0;
    set.midi_out = 0;
    set.clock_out = 0;
    set.metro = 1;
    set.metro_vol = 6;
    set.countin = 1;
    set.poly = GM_NVOICE_DEF;
    set.leds = 1;
}

static int settings_valid(void)
{
    return set.magic == SET_MAGIC && set.format == SET_FORMAT && set.song < SEQ_NSONGS && set.midi_in <= 1 &&
           set.midi_out <= 1 && set.clock_out <= 1 && set.metro <= 2 && set.metro_vol <= 10 && set.countin <= 1 &&
           set.poly >= 8 && set.poly <= GM_NVOICE && set.leds <= 1;
}

int settings_load(void)
{
    int n;
    memset(&set, 0, sizeof set);
    n = plat_store_load(OBJ_SET, &set, sizeof set);
    if (n >= 8 && settings_valid())
        return 0;
    settings_defaults();
    return -1;
}

int settings_save(void) { return plat_store_save(OBJ_SET, &set, sizeof set); }

void settings_apply(void)
{
    seq_cfg.midi_out = set.midi_out;
    seq_cfg.clock_out = set.clock_out;
    seq_cfg.metro = set.metro;
    seq_cfg.metro_vol = set.metro_vol;
    seq_cfg.countin = set.countin;
    gm_voice_max = set.poly;
}

/* -------------------------------------------------------------- slots --- */
/* the songs: plat.h's song store; their bytes are seq.c's stream (song_stream / song_parse) */
void slots_scan(void)
{
    int k;
    plat_song_scan();
    for (k = 0; k < SEQ_NSONGS; k++)
        plat_song_info(k, &slot_info[k]);
}

int song_load_slot(int slot)
{
    int rc;
    seq_undo_ok = 0;                             /* the copy belongs to the song before */
    seq_full = 0;
    if (plat_song_open(slot) != 0) {
        song_defaults(&seq_song);
        return 1;
    }
    rc = song_parse(&seq_song, plat_song_getc);
    if (rc != 0) {
        song_defaults(&seq_song);
        return -1;
    }
    return 0;
}

static void song_fill(uint8_t *dst, uint32_t from, uint32_t n) { song_stream(&seq_song, dst, from, n); }

int song_save_slot(int slot)
{
    plat_song_info_t i;
    int rc;
    i.used = 1;
    i.bars = seq_song.bars;
    i.tempo = seq_song.tempo;
    i.nev = seq_song.nev;
    rc = plat_song_save(slot, song_bytes(&seq_song), song_fill, &i);
    plat_song_info(slot, &slot_info[slot]);
    return rc;
}

int song_delete_slot(int slot)
{
    int rc = plat_song_delete(slot);
    plat_song_info(slot, &slot_info[slot]);
    return rc;
}

void song_boot_load(void)
{
    slots_scan();
    song_load_slot(set.song);
    settings_apply();
    seq_post(SC_APPLY, 0, 0, 0, 0);
}
