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
static void slot_note(int slot, uint32_t bars, uint32_t tempo, uint32_t nev)
{
    slot_info_t *i = &slot_info[slot];
    i->used = 1;
    i->bars = (uint8_t)bars;
    i->tempo = (uint16_t)tempo;
    i->nev = (uint16_t)nev;
}

void slots_scan(void)
{
    static uint8_t hb[SONG_HEAD_BYTES] __attribute__((aligned(4)));   /* a song's head: magic .. the tracks */
    int k, n;
    for (k = 0; k < SEQ_NSONGS; k++) {
        uint32_t magic, format;
        uint16_t tempo, nev;
        slot_info[k].used = 0;
        n = plat_store_load(OBJ_SONG0 + (uint32_t)k, hb, SONG_HEAD_BYTES);
        if (n != (int)SONG_HEAD_BYTES)
            continue;
        memcpy(&magic, hb + offsetof(song_t, magic), 4);
        memcpy(&format, hb + offsetof(song_t, format), 4);
        memcpy(&tempo, hb + offsetof(song_t, tempo), 2);
        memcpy(&nev, hb + offsetof(song_t, nev), 2);
        if (magic == SONG_MAGIC && format == SONG_FORMAT)
            slot_note(k, hb[offsetof(song_t, bars)], tempo, nev);
    }
}

int song_load_slot(int slot)
{
    int n = plat_store_load(OBJ_SONG0 + (uint32_t)slot, &seq_song, sizeof seq_song);
    seq_undo_ok = 0;                             /* the copy belongs to the song before */
    seq_full = 0;
    if (n < 0) {
        song_defaults(&seq_song);
        return 1;
    }
    if (!song_valid(&seq_song, (uint32_t)n)) {
        song_defaults(&seq_song);
        return -1;
    }
    return 0;
}

int song_save_slot(int slot)
{
    int rc = plat_store_save(OBJ_SONG0 + (uint32_t)slot, &seq_song, song_bytes(&seq_song));
    if (rc == 0)
        slot_note(slot, seq_song.bars, seq_song.tempo, seq_song.nev);
    return rc;
}

void song_boot_load(void)
{
    slots_scan();
    song_load_slot(set.song);
    settings_apply();
    seq_post(SC_APPLY, 0, 0, 0, 0);
}
