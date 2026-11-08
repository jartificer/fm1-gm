/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Host test of firmware/src/ota.c: a fake M-UPGRADE serves a real .fwsc over
 * the SysEx protocol (pack7, cmd 0x30 reads, 0xE0000000 -> "success") and a
 * simulated NOR plays the flash.
 *   cc -o ota_test tests/ota_test.c && ./ota_test FILE.fwsc [OFFICIAL.fwsc ...]
 * With M-VAVE's official packages (V15, V16: FM-1.fwsc) as further arguments it also checks the
 * OTA_STOCK pins against them and the return to stock over each boot area.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FELUCCA_ID "FM-1_900"
#define FELUCCA_OTA_DRYRUN 0
#define OTA_STOCK_CONST                          /* writable: the tests change a pin */

static uint8_t nor[0x100000];
static uint8_t *logical;
static size_t logical_len;
static uint8_t rx[1024];                 /* one pending host frame (7-bit bytes) */
static uint32_t rx_len, rx_full;
static uint32_t now_ms, requests, ident_replies, mute_after = 0xFFFFFFFFu, bad_erase;
static int committed;
static uint8_t committed_parm[112];
static int32_t last_code;
static uint8_t *corrupt_at;              /* flip a served byte (loader body) */
static uint8_t dev_head[0x4000];         /* the device's boot area, copied into nor by reset_dev */

static uint32_t pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 8;
        while (nb >= 7) { out[o++] = acc & 0x7F; acc >>= 7; nb -= 7; }
    }
    if (nb) out[o++] = acc & 0x7F;
    return o;
}

static uint32_t unpack7h(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 7;
        if (nb >= 8) { out[o++] = (uint8_t)acc; acc >>= 8; nb -= 8; }
    }
    return o;
}

static void host_reply(const uint8_t *msg, uint32_t n)       /* queue msg as a device-bound frame */
{
    rx_len = pack7(msg, n, rx);
    rx_full = 1;
}

/* ---- hooks ---- */
static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    uint8_t u[64], m[600];
    uint32_t d, addr, len, i, s = 0;
    if (p[0] != 0xF0 || p[n - 1] != 0xF7) { printf("bad wire framing\n"); exit(1); }
    d = unpack7h(p + 1, n - 2, u);
    if (u[2] == 0x11) { ident_replies++; return 0; }
    if (d != 15 || u[2] != 0x30) { printf("unexpected device msg cmd %02x len %u\n", u[2], d); exit(1); }
    addr = u[7] | u[8] << 8 | u[9] << 16 | (uint32_t)u[10] << 24;
    len = u[11] | u[12] << 8 | u[13] << 16;
    if (++requests > mute_after)
        return 0;                                         /* host gone */
    memcpy(m, "\x00\x59\x30", 3);
    m[3] = (uint8_t)(len + 8); m[4] = (uint8_t)((len + 8) >> 8); m[5] = 0;
    m[6] = 0;
    memcpy(m + 7, &addr, 4);
    m[11] = (uint8_t)len; m[12] = (uint8_t)(len >> 8); m[13] = 0;
    if (addr == 0xE0000000u) {
        memset(m + 14, 0, len);
        memcpy(m + 14, "success", 8);
    } else {
        if (addr + len > logical_len) { printf("read past the package %#x\n", addr); exit(1); }
        memcpy(m + 14, logical + addr, len);
        if (corrupt_at && corrupt_at >= logical + addr && corrupt_at < logical + addr + len)
            m[14 + (corrupt_at - (logical + addr))] ^= 0x01;
    }
    for (i = 6; i < 14 + len; i++) s += m[i];
    m[14 + len] = (uint8_t)~s;
    host_reply(m, 15 + len);
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n)
{
    if (!rx_full) return 0;
    *p = rx; *n = rx_len;
    return 1;
}
static void ota_frame_done(void) { rx_full = 0; }
static uint32_t ota_now_ms(void) { return now_ms; }
static void ota_idle(void) { now_ms += 1; }
static int ota_erase(uint32_t off)
{
    if (off < 0xE0000u || off >= 0xE5000u || (off & 0xFFFu)) { bad_erase++; return -8; }
    memset(nor + off, 0xFF, 4096);
    return 0;
}
static int ota_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    if (off < 0xE0000u || off + n > 0xE5000u) { bad_erase++; return -8; }
    for (i = 0; i < n; i++) nor[off + i] &= s[i];
    return 0;
}
static int ota_fread(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static void ota_show(uint32_t step, int32_t code) { if (step == 9) last_code = code; }
static void ota_commit(const uint8_t *parm) { committed = 1; memcpy(committed_parm, parm, 112); }

#include "../firmware/src/ota.c"

static uint32_t ota_body_off(void)                  /* ota.bin data + 0x20 in the logical image */
{
    uint8_t h[0x400];
    uint32_t i, n;
    memcpy(h, logical, sizeof h);
    ota_jl_enc(h, 0x40);
    n = ota_rd16(h + 8);
    for (i = 0; i < n; i++) {
        uint8_t *e = h + 0x40 + i * 0x50u;
        ota_jl_enc(e, 0x50);
        if (ota_rd16(e) == 100) return ota_rd32(e + 8) + 0x20u;
    }
    return 0;
}

static int check(const char *what, int ok)
{
    printf("%-52s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

static int area_erased(void)
{
    uint32_t i;
    for (i = 0xE0000; i < 0xE5000; i++) if (nor[i] != 0xFF) return 0;
    return 1;
}

static void reset_dev(void)
{
    memset(nor, 0xFF, sizeof nor);
    memcpy(nor, dev_head, 0x4000);                        /* the device's boot area */
    rx_full = 0; requests = 0; ident_replies = 0; committed = 0; last_code = 0;
    mute_after = 0xFFFFFFFFu; corrupt_at = NULL; bad_erase = 0;
}

static size_t load(const char *path)            /* -> logical, logical_len; 0 on error */
{
    FILE *f = fopen(path, "rb");
    uint8_t *raw;
    size_t n, i;
    if (!f) { perror(path); return 0; }
    raw = malloc(0x200000);
    n = fread(raw, 1, 0x200000, f);
    fclose(f);
    free(logical);
    logical = malloc(n);
    for (i = 0; i < 20; i++) memcpy(logical + i * 47, raw + i * 48, 47);   /* drop the marker bytes */
    memcpy(logical + 20 * 47, raw + 20 * 48, n - 20 * 48);
    logical_len = n - 20;
    free(raw);
    return n;
}

static int run(void)                            /* one session -> its code, 0 = committed */
{
    reset_dev();
    return ota_session();
}

/* M-VAVE's official package: its pin, and the return to stock over its own boot area, another one
 * (FM1-GM's package head stands in for V15's, V16's or m-bootloader's) and with a changed boot area */
static int official(const char *path, const uint8_t *other_head)
{
    uint8_t hdr[0x400], own[0x4000];
    uint32_t fl_off, fl_len, fl_crc = 0, i, k, sha[8], key0;
    uint16_t crc0;
    int bad = 0, rc;
    char what[96];
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    if (!load(path)) return 1;
    memcpy(hdr, logical, sizeof hdr);                  /* the UFW header and entries, as ota_ufw reads them */
    ota_jl_enc(hdr, 0x40);
    fl_off = fl_len = 0;
    for (i = 0; i < ota_rd16(hdr + 8) && i < 11u; i++) {
        uint8_t *e = hdr + 0x40 + i * 0x50u;
        ota_jl_enc(e, 0x50);
        if (ota_rd16(e) == 0) { fl_off = ota_rd32(e + 8); fl_len = ota_rd32(e + 12); fl_crc = ota_rd16(e + 4); }
    }
    if (!fl_off || fl_off + fl_len > logical_len) { printf("%s: not a package\n", name); return 1; }
    for (k = 0; k < sizeof OTA_STOCK / sizeof OTA_STOCK[0]; k++)
        if (OTA_STOCK[k].fl_len == fl_len) break;
    snprintf(what, sizeof what, "%s: flash.bin size %#x is pinned", name, fl_len);
    if (check(what, k < sizeof OTA_STOCK / sizeof OTA_STOCK[0])) return 1;
    snprintf(what, sizeof what, "%s: flash.bin data CRC %04x == pin", name, fl_crc);
    bad += check(what, fl_crc == OTA_STOCK[k].fl_crc && ota_crc16(logical + fl_off, fl_len, 0) == fl_crc);
    ota_sha256_init(sha);
    for (i = 0; i < OTA_HEAD_LEN; i += 64u) ota_sha256_block(sha, logical + fl_off + i);
    snprintf(what, sizeof what, "%s: boot area SHA-256 == pin", name);
    bad += check(what, ota_sha256_is(sha, OTA_HEAD_LEN, OTA_STOCK[k].head_sha256));
    memcpy(own, logical + fl_off, sizeof own);

    memcpy(dev_head, own, sizeof dev_head);
    rc = run();
    snprintf(what, sizeof what, "%s: over its own boot area -> staged", name);
    bad += check(what, rc == 0 && committed && bad_erase == 0);
    bad += check("  staged body == its official loader", !memcmp(nor + 0xE0020, logical + ota_body_off(), 64));

    memcpy(dev_head, other_head, sizeof dev_head);
    rc = run();
    snprintf(what, sizeof what, "%s: over another boot area -> staged", name);
    bad += check(what, rc == 0 && committed && bad_erase == 0);

    memcpy(dev_head, other_head, sizeof dev_head);
    reset_dev();
    corrupt_at = logical + fl_off + 0x1000;            /* one byte of the SPL, as served */
    rc = ota_session();
    snprintf(what, sizeof what, "%s: boot area changed -> -5, area clean", name);
    bad += check(what, rc == -5 && !committed && area_erased());

    memcpy(dev_head, own, sizeof dev_head);
    reset_dev();
    corrupt_at = logical + fl_off + 0x1000;            /* changed, and no longer the device's own either */
    rc = ota_session();
    bad += check("  same, over its own boot area -> -5", rc == -5 && !committed && area_erased());

    crc0 = OTA_STOCK[k].fl_crc;
    OTA_STOCK[k].fl_crc ^= 0x0101u;                    /* an image that is not a reviewed one */
    rc = run();
    OTA_STOCK[k].fl_crc = crc0;
    bad += check("  flash.bin not pinned -> -5", rc == -5 && !committed && area_erased());

    key0 = OTA_STOCK[k].key;
    OTA_STOCK[k].key = 0x1234u;                        /* a chip with another key */
    memcpy(dev_head, own, sizeof dev_head);
    rc = run();
    OTA_STOCK[k].key = (uint16_t)key0;
    bad += check("  another chip key -> -16", rc == -16 && !committed && area_erased());
    {
        uint32_t key = 0;
        memcpy(dev_head, own, sizeof dev_head);
        reset_dev();
        snprintf(what, sizeof what, "  its boot area's chip key is %04X", 0x980Fu);
        bad += check(what, ota_chip_key(&key) == 0 && key == 0x980Fu);
    }
    return bad;
}

int main(int argc, char **argv)
{
    int bad = 0, rc, a;
    static uint8_t pkg_head[0x4000];
    if (argc < 2) { printf("usage: ota_test FILE.fwsc [OFFICIAL.fwsc ...]\n"); return 2; }
    if (!load(argv[1])) return 2;
    memcpy(dev_head, logical + 0x400, 0x4000);         /* device flash head = package flash.bin head */
    memcpy(pkg_head, dev_head, sizeof pkg_head);

    /* handshake outside a session */
    reset_dev();
    {
        uint8_t m[7] = {0, 0x59, 0x11, 0, 0, 0, 0xFF};
        host_reply(m, 7);
        ota_service();
        bad += check("handshake answered", ident_replies == 1);
        bad += check("identity frame is " FELUCCA_ID,
                     ota_msg[2] == 0x11 && !memcmp(ota_msg + 6, FELUCCA_ID, sizeof FELUCCA_ID - 1));
    }

    /* full session */
    reset_dev();
    rc = ota_session();
    printf("  %u requests\n", requests);
    bad += check("session commits", rc == 0 && committed);
    bad += check("headers + loader + confirm, no head reads", requests >= 10 && requests <= 30);
    {
        uint8_t *h = nor + 0xE0000, *p = committed_parm;
        bad += check("staged body == the package's loader", !memcmp(nor + 0xE0020, logical + ota_body_off(), 64));
        bad += check("LOADER.BIN head CRC",
                     (h[0] | h[1] << 8) == ota_crc16(h + 2, 30, 0) && !memcmp(h + 16, "LOADER.BIN", 10));
        bad += check("record at 0xE4F00 == RAM record", !memcmp(nor + 0xE4F00, p, 112));
        bad += check("record CRC / type / magic / ota_addr",
                     (p[0] | p[1] << 8) == ota_crc16(p + 2, 78, 0) && (p[2] | p[3] << 8) == 0x5A0D &&
                     (p[6] | p[7] << 8) == 0x5441 && ota_rd32(p + 72) == 0xE0000);
        bad += check("nothing written outside 0xE0000..0xE4FFF", bad_erase == 0);
    }
    /* boot cleanup erases it */
    bad += check("boot cleanup erases the area", ota_boot_cleanup() == 1 && area_erased());
    bad += check("second boot: nothing to do", ota_boot_cleanup() == 0);

    /* host disappears in the middle */
    reset_dev();
    mute_after = 4;
    rc = ota_session();
    bad += check("host gone -> no commit, area clean", rc < 0 && !committed && area_erased());

    /* host never confirms */
    reset_dev();
    mute_after = 0xFFFFFFFFu;
    {
        uint32_t total;
        rc = ota_session();
        total = requests;                                  /* requests of a full run incl. confirm */
        reset_dev();
        mute_after = total - 1;                            /* answer everything but the confirmation */
        rc = ota_session();
        bad += check("no 'success' -> -13, no commit, area clean", rc == -13 && !committed && area_erased());
    }

    /* M-VAVE's official packages, when given */
    for (a = 2; a < argc; a++)
        bad += official(argv[a], pkg_head);

    printf("%s\n", bad ? "OTA TEST FAILED" : "ota test passed");
    return bad != 0;
}
