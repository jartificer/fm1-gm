/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Host test of firmware/src/storage.c against a simulated NOR flash:
 * erase -> 0xFF, program can only clear bits, page writes must not wrap. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint8_t nor[0x100000];
static int fail_after = -1;            /* torn-write injection: stop after N programs */

static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off) { memset(nor + off, 0xFF, 4096); return 0; }
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    if (fail_after == 0)
        return -9;
    if (fail_after > 0)
        fail_after--;
    if ((off & 0xFFu) + n > 256u) {
        printf("page wrap at %#x\n", off);
        exit(1);
    }
    for (i = 0; i < n; i++)
        nor[off + i] &= s[i];
    return 0;
}
/* the regions the app's flash driver may write (firmware/hal/fm1_flash.h, which is device code) */
#define FL_DATA_LO 0x00097000u
#define FL_DATA_HI 0x000E0000u
#define FL_GLOB_LO 0x000FC000u
#define FL_GLOB_HI 0x000FF000u
#define FL_IN(off, n, lo, hi) ((uint32_t)(off) >= (lo) && (uint32_t)(off) <= (hi) && \
                               (uint32_t)(n) <= (hi) - (uint32_t)(off))
#define FL_STORE_OK(off, n) (FL_IN(off, n, FL_DATA_LO, FL_DATA_HI) || FL_IN(off, n, FL_GLOB_LO, FL_GLOB_HI))
#include "../firmware/src/app/plat.h"     /* FM1-GM: the object map */
#include "../firmware/src/storage.c"
#define OBJ_PROJECT0 OBJ_PANEL                 /* FM1-GM: the objects are the settings and the panel */
#define OBJ_SETTINGS OBJ_SET

static int check(const char *what, int ok)
{
    printf("%-46s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

/* the song store: a song is bytes, here a pattern from a seed */
static uint32_t fill_seed;
static void fill(uint8_t *dst, uint32_t from, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        dst[i] = (uint8_t)((from + i) * 7u + fill_seed * 13u + ((from + i) >> 9));
}
static int song_is(uint32_t slot, uint32_t seed, uint32_t len)   /* slot holds that song: 1 */
{
    uint32_t i;
    uint8_t want;
    if (ss_open(slot))
        return 0;
    for (i = 0; i < len; i++) {
        int c = ss_getc();
        fill_seed = seed;
        fill(&want, i, 1);
        if (c != want)
            return 0;
    }
    return ss_getc() == -1;
}
static int save(uint32_t slot, uint32_t seed, uint32_t len)
{
    fill_seed = seed;
    return ss_save(slot, len, fill, 4, 120, len / 3);
}

static int song_store_tests(void)
{
    int bad = 0, rc;
    uint32_t i, n;
    memset(nor, 0xFF, sizeof nor);
    ss_scan();
    bad += check("songs: an empty store", ss_free_sectors() == SS_NSEC && ss_open(0) < 0);
    bad += check("a small song saves", save(3, 1, 500) == 0 && song_is(3, 1, 500));
    bad += check("... in one sector", ss_info[3].nsec == 1 && ss_info[3].bytes == 500 && ss_free_sectors() == SS_NSEC - 1);
    bad += check("a song of three sectors", save(7, 2, 3 * SS_PAY - 5) == 0 && song_is(7, 2, 3 * SS_PAY - 5));
    bad += check("an empty song (0 bytes)", save(9, 3, 0) == 0 && song_is(9, 3, 0) && ss_info[9].used);
    ss_scan();                                   /* a reboot */
    bad += check("after a reboot: all three", song_is(3, 1, 500) && song_is(7, 2, 3 * SS_PAY - 5) && song_is(9, 3, 0) &&
                                              ss_info[7].nsec == 3 && ss_info[7].tempo == 120 && !ss_info[4].used);
    bad += check("a resave replaces it", save(7, 4, SS_PAY + 1) == 0 && song_is(7, 4, SS_PAY + 1));
    ss_scan();
    bad += check("... and frees the old sectors", song_is(7, 4, SS_PAY + 1) && ss_free_sectors() == SS_NSEC - 4);
    for (n = 1; n < 40; n++) {                   /* torn after n programs: the song before stays */
        fail_after = (int)n;
        save(7, 5, 2 * SS_PAY + 100);
        fail_after = -1;
        ss_scan();
        if (!song_is(7, 4, SS_PAY + 1) && !song_is(7, 5, 2 * SS_PAY + 100)) {
            printf("  torn after %u programs: neither song\n", n);
            bad++;
            break;
        }
        if (song_is(7, 5, 2 * SS_PAY + 100) && n < 17u + 17u + 2u) {   /* parts of 17 programs (16 payload pages, the header) and 2 */
            printf("  torn after %u programs: the new song, too early\n", n);
            bad++;
            break;
        }
        if (save(7, 4, SS_PAY + 1) != 0)
            bad++;
    }
    bad += check("torn saves keep the song before", n == 40 && song_is(3, 1, 500));
    for (i = 0; i < SS_NSEC; i++)                /* bit rot in song 3 */
        if (ss_slot[i] == 3)
            nor[ss_addr(i) + SS_HDR + 3] ^= 0x01;
    ss_scan();
    bad += check("a rotten song is gone, the others stay", !ss_info[3].used && song_is(7, 4, SS_PAY + 1));
    bad += check("delete", save(3, 6, 100) == 0 && save(3, 7, 200) == 0 && ss_delete(3) == 0 && ss_open(3) < 0);
    ss_scan();
    bad += check("... stays deleted (no older copy comes back)", !ss_info[3].used && ss_open(3) < 0);
    memset(nor, 0xFF, sizeof nor);
    ss_scan();
    for (i = 0; i < SS_NSLOTS && (rc = save(i, i, 2000)) == 0; i++)
        ;
    bad += check("64 small songs fit", i == SS_NSLOTS && ss_free_sectors() == SS_NSEC - SS_NSLOTS);
    bad += check("one too big for what is free is refused", save(0, 9, (SS_NSEC - SS_NSLOTS) * SS_PAY + 1) == -10 &&
                                                          song_is(0, 0, 2000));
    bad += check("one that just fits", save(0, 9, (SS_NSEC - SS_NSLOTS) * SS_PAY) == 0 &&
                                       song_is(0, 9, (SS_NSEC - SS_NSLOTS) * SS_PAY));
    for (i = 1; i < SS_NSLOTS; i++)
        ss_delete(i);
    bad += check("one song in (almost) the whole store", save(0, 10, (SS_NSEC - 8) * SS_PAY) == 0);
    ss_scan();
    bad += check("... loads", song_is(0, 10, (SS_NSEC - 8) * SS_PAY));
    {   /* the erases spread: many saves of one small song wear every sector about the same */
        static uint32_t erases[0x100];
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        memset(nor, 0xFF, sizeof nor);
        ss_scan();
        for (i = 0; i < 20u * SS_NSEC; i++) {
            uint32_t k;
            save(1, i, 300);
            for (k = 0; k < SS_NSEC; k++)
                if (ss_slot[k] == 1 && ss_gseq[k] == ss_cur[1])
                    erases[k]++;
        }
        for (i = 0; i < SS_NSEC; i++) {
            lo = erases[i] < lo ? erases[i] : lo;
            hi = erases[i] > hi ? erases[i] : hi;
        }
        bad += check("wear is spread over the sectors", lo >= 19 && hi <= 21);
    }
    return bad;
}

int main(void)
{
    char a[600], b[600], got[600];
    int bad = 0, n;
    memset(nor, 0xFF, sizeof nor);
    memset(a, 'A', sizeof a);
    memset(b, 'B', sizeof b);
    bad += check("empty flash loads nothing", st_load(OBJ_PROJECT0, got, sizeof got) < 0);
    bad += check("save A", st_save(OBJ_PROJECT0, a, sizeof a) == 0);
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("load returns A", n == (int)sizeof a && !memcmp(got, a, sizeof a));
    bad += check("save B", st_save(OBJ_PROJECT0, b, sizeof b) == 0);
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("load returns B (newer seq)", n == (int)sizeof b && !memcmp(got, b, sizeof b));
    fail_after = 1;                                  /* payload page 1 written, the rest torn */
    st_save(OBJ_PROJECT0, a, sizeof a);
    fail_after = -1;
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("torn save keeps B", n == (int)sizeof b && !memcmp(got, b, sizeof b));
    fail_after = 3;                                  /* all payload pages, header torn */
    st_save(OBJ_PROJECT0, a, sizeof a);
    fail_after = -1;
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("header not written keeps B", n == (int)sizeof b && !memcmp(got, b, sizeof b));
    bad += check("save A again", st_save(OBJ_PROJECT0, a, sizeof a) == 0);
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("load returns A", n == (int)sizeof a && !memcmp(got, a, sizeof a));
    nor[st_sector(OBJ_PROJECT0, 0) + ST_PAYLOAD_OFF + 10] ^= 0x01;   /* bit rot in one copy */
    nor[st_sector(OBJ_PROJECT0, 1) + ST_PAYLOAD_OFF + 10] ^= 0x01;
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("both copies corrupt -> nothing", n < 0);
    bad += check("other objects untouched", st_load(OBJ_SET, got, sizeof got) < 0);
    bad += check("settings save/load",
                 st_save(OBJ_SETTINGS, "hello", 5) == 0 && st_load(OBJ_SETTINGS, got, 5) == 5 && !memcmp(got, "hello", 5));
    bad += check("CRC-32 is zlib's (check value 0xCBF43926)", st_crc32("123456789", 9) == 0xCBF43926u);
    memset(nor, 0xFF, sizeof nor);
    st_save(OBJ_PANEL, a, sizeof a);
    st_save(OBJ_PANEL, b, sizeof b);              /* B is newer, in the other copy */
    {
        st_hdr_t h;
        int cur = st_current(OBJ_PANEL, &h);
        nor[st_sector(OBJ_PANEL, (uint32_t)cur) + ST_PAYLOAD_OFF + 300] ^= 0x10;   /* rot in the newer copy */
    }
    n = st_load(OBJ_PANEL, got, sizeof got);
    bad += check("newer copy rotten -> the older one loads", n == (int)sizeof a && !memcmp(got, a, sizeof a));
    bad += check("save over the rotten copy", st_save(OBJ_PANEL, b, sizeof b) == 0);
    n = st_load(OBJ_PANEL, got, sizeof got);
    bad += check("... loads the new data", n == (int)sizeof b && !memcmp(got, b, sizeof b));
    nor[st_sector(OBJ_PANEL, 0) + 8] ^= 0x01;    /* both headers broken */
    nor[st_sector(OBJ_PANEL, 1) + 8] ^= 0x01;
    bad += check("both headers broken -> nothing", st_load(OBJ_PANEL, got, sizeof got) < 0);
    bad += song_store_tests();
    {   /* FM1-GM map: the objects and the song sectors inside Felucca's data areas, below the loader's
         * staging area (0xE0000) but the settings, and no overlaps */
        static uint8_t owner[0x100];             /* by sector */
        uint32_t o, c, i, ok = 1;
        memset(owner, 0, sizeof owner);
        for (o = 0; o < OBJ_NOBJ; o++)
            for (c = 0; c < 2; c++) {
                uint32_t a0 = st_sector(o, c);
                if (!FL_STORE_OK(a0, 4096) || owner[a0 >> 12]++)
                    ok = 0;
            }
        for (i = 0; i < SS_NSEC; i++) {
            uint32_t a0 = ss_addr(i);
            if (!FL_STORE_OK(a0, 4096) || a0 < 0x97000 || a0 >= 0xE0000 || owner[a0 >> 12]++)
                ok = 0;
        }
        for (i = 0x97; i < 0xE0; i++)            /* and every sector of the data area is used */
            if (owner[i] != 1)
                ok = 0;
        bad += check("objects, songs in the data areas, no overlaps", (int)ok);
    }
    printf("%s\n", bad ? "STORAGE TEST FAILED" : "storage test passed");
    return bad != 0;
}
