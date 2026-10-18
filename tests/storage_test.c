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
#define OBJ_PROJECT0 OBJ_SONG0                 /* FM1-GM: the songs (two sectors a copy), settings, panel */
#define OBJ_SETTINGS OBJ_SET

static int check(const char *what, int ok)
{
    printf("%-46s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
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
    bad += check("other objects untouched", st_load(OBJ_PANEL, got, sizeof got) < 0);
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
    {   /* a song as big as a song gets: both sectors of a copy, torn halfway, and the slots apart */
        static uint8_t big[PLAT_SONG_MAX], big2[PLAT_SONG_MAX], got2[PLAT_SONG_MAX];
        uint32_t i;
        for (i = 0; i < sizeof big; i++) {
            big[i] = (uint8_t)(i * 7u + 3u);
            big2[i] = (uint8_t)(i * 13u + 1u);
        }
        memset(nor, 0xFF, sizeof nor);
        bad += check("full song saves", st_save(OBJ_SONG0 + 5, big, sizeof big) == 0);
        n = st_load(OBJ_SONG0 + 5, got2, sizeof got2);
        bad += check("full song loads", n == (int)sizeof big && !memcmp(got2, big, sizeof big));
        bad += check("a song over the limit is refused", st_save(OBJ_SONG0 + 5, big, sizeof big + 1) < 0);
        fail_after = 20;                             /* into the copy's second sector, then torn */
        st_save(OBJ_SONG0 + 5, big2, sizeof big2);
        fail_after = -1;
        n = st_load(OBJ_SONG0 + 5, got2, sizeof got2);
        bad += check("torn full song keeps the old one", n == (int)sizeof big && !memcmp(got2, big, sizeof big));
        bad += check("full song saves over", st_save(OBJ_SONG0 + 5, big2, sizeof big2) == 0);
        bad += check("... twice (both copies erased first)", st_save(OBJ_SONG0 + 5, big, 100) == 0 &&
                                                             st_save(OBJ_SONG0 + 5, big2, sizeof big2) == 0);
        n = st_load(OBJ_SONG0 + 5, got2, sizeof got2);
        bad += check("... and loads", n == (int)sizeof big2 && !memcmp(got2, big2, sizeof big2));
        bad += check("the slot next to it is empty", st_load(OBJ_SONG0 + 4, got2, sizeof got2) < 0 &&
                                                     st_load(OBJ_SONG0 + 6, got2, sizeof got2) < 0);
    }
    {   /* FM1-GM map: every object inside Felucca's data areas, below the loader's staging area (0xE0000)
         * but the settings, and no overlaps */
        uint32_t o, p, ok = 1;
        for (o = 0; o < OBJ_NOBJ; o++) {
            uint32_t a0 = st_sector(o, 0), a1 = st_sector(o, 1) + st_nsec(o) * 4096;
            int in_data = (a0 >= 0x97000 && a1 <= 0xE0000) || (a0 >= 0xFC000 && a1 <= 0xFF000);
            if (!in_data || a1 - a0 != 2u * st_nsec(o) * 4096u || !FL_STORE_OK(a0, a1 - a0))
                ok = 0;
            for (p = 0; p < o; p++)
                if (a0 < st_sector(p, 1) + st_nsec(p) * 4096 && st_sector(p, 0) < a1)
                    ok = 0;
        }
        bad += check("objects in the data areas, no overlaps", (int)ok);
    }
    printf("%s\n", bad ? "STORAGE TEST FAILED" : "storage test passed");
    return bad != 0;
}
