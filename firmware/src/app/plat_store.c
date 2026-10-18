/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM: plat.h's storage over storage.c, while flash_ok (the flash answered at boot). Included by
 * plat_fm1.c and by the host simulator, after storage.c. */
int plat_store_load(uint32_t obj, void *dst, uint32_t max) { return flash_ok ? st_load(obj, dst, max) : -1; }
int plat_store_save(uint32_t obj, const void *src, uint32_t len) { return flash_ok ? st_save(obj, src, len) : -9; }
void plat_song_scan(void)
{
    if (flash_ok)
        ss_scan();
}
void plat_song_info(int slot, plat_song_info_t *i)
{
    const ss_info_t *s = &ss_info[(uint32_t)slot % SS_NSLOTS];
    i->used = s->used;
    i->bars = s->bars;
    i->tempo = s->tempo;
    i->nev = s->nev;
    i->nsec = s->nsec;
    i->bytes = s->bytes;
}
uint32_t plat_song_free(void) { return flash_ok ? ss_free_sectors() : 0; }
uint32_t plat_song_sectors(void) { return SS_NSEC; }
int plat_song_save(int slot, uint32_t bytes, void (*fill)(uint8_t *, uint32_t, uint32_t), const plat_song_info_t *i)
{
    return flash_ok ? ss_save((uint32_t)slot, bytes, fill, i->bars, i->tempo, i->nev) : -9;
}
int plat_song_open(int slot) { return flash_ok ? ss_open((uint32_t)slot) : -1; }
int plat_song_getc(void) { return ss_getc(); }
int plat_song_delete(int slot) { return flash_ok ? ss_delete((uint32_t)slot) : -9; }
