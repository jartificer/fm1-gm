/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * X0X / OMNI: object map changed. FM1-GM: the songs in a store of their own, at the end. */
/* Persistent storage on the SPI NOR.
 *
 * Every object has an A/B sector pair. A save goes to the copy that is not
 * the current one: erase the sector, program the payload (from offset 256),
 * then the 32-byte header at offset 0 last. The header is the commit record;
 * on load the valid copy with the highest seq wins, so a torn write leaves
 * the previous copy in charge.
 *
 * Flash access goes through three hooks (also used by the host test):
 *   st_read(off, dst, n)   st_erase(off)   st_prog(off, src, n)
 */
#define ST_MAGIC 0x30583058u                   /* "X0X0": never mistaken for a Felucca object */
#define ST_SECTOR 4096u
#define ST_PAYLOAD_OFF 256u
#define ST_PAYLOAD_MAX (ST_SECTOR - ST_PAYLOAD_OFF)

/* flash map, FM1-GM (plat.h OBJ_*): the settings at 0xFC000 (Felucca's FL_GLOB), the panel calibration
 * at 0xDC000 (as OMNI), each one sector a copy. Every other sector of Felucca's data area
 * (0x97000..0xDFFFF) belongs to the songs (the song store, below). Nothing above 0xDFFFF but the
 * settings: 0xE0000.. is the update loader's staging area, 0xEA000.. the stock firmware's user
 * patches (kept). The magic is X0X's, so an object is told apart by its own magic. */
typedef struct {
    uint32_t magic;
    uint16_t type, slot;
    uint32_t seq, len, crc, rsv[2];
    uint32_t hcrc;
} st_hdr_t;

static int st_read(uint32_t off, void *dst, uint32_t n);
static int st_erase(uint32_t off);
static int st_prog(uint32_t off, const void *src, uint32_t n);

static uint32_t st_crc_add(uint32_t c, const void *p, uint32_t n)   /* zlib CRC-32, 4 bits per step */
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    const uint8_t *b = p;
    c = ~c;
    while (n--) {
        c ^= *b++;
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return ~c;
}
static uint32_t st_crc32(const void *p, uint32_t n) { return st_crc_add(0, p, n); }

static uint32_t st_max(uint32_t obj) { (void)obj; return ST_PAYLOAD_MAX; }

static uint32_t st_sector(uint32_t obj, uint32_t copy)  /* flash offset of copy A (0) / B (1) */
{
    return (obj == OBJ_PANEL ? 0xDC000u : 0xFC000u) + copy * ST_SECTOR;
}

static uint8_t st_buf[ST_SECTOR] __attribute__((aligned(4)));   /* an object's payload, or a song sector's */

static int st_head(uint32_t obj, uint32_t copy, st_hdr_t *h)   /* commit record valid: 0 */
{
    if (st_read(st_sector(obj, copy), h, sizeof *h))
        return -1;
    if (h->magic != ST_MAGIC || h->type != obj || h->len > st_max(obj) ||
        h->hcrc != st_crc32(h, sizeof *h - 4u))
        return -1;
    return 0;
}

static int st_body(uint32_t obj, uint32_t copy, const st_hdr_t *h)   /* payload -> st_buf, CRC ok: 0 */
{
    if (st_read(st_sector(obj, copy) + ST_PAYLOAD_OFF, st_buf, h->len) || st_crc32(st_buf, h->len) != h->crc)
        return -1;
    return 0;
}

/* the current copy: the valid one with the highest seq (A on a tie), -1 when
 * neither is valid. Headers first, so only the winner's payload is read (it
 * is left in st_buf); *h gets its header. */
static int st_current(uint32_t obj, st_hdr_t *h)
{
    st_hdr_t a, b;
    int va = st_head(obj, 0, &a) == 0, vb = st_head(obj, 1, &b) == 0;
    if (vb && (!va || b.seq > a.seq)) {
        if (st_body(obj, 1, &b) == 0) {
            *h = b;
            return 1;
        }
        vb = 0;
    }
    if (va && st_body(obj, 0, &a) == 0) {
        *h = a;
        return 0;
    }
    if (vb && st_body(obj, 1, &b) == 0) {
        *h = b;
        return 1;
    }
    return -1;
}

/* load object into dst (up to max bytes); returns the length, or -1 */
static int st_load(uint32_t obj, void *dst, uint32_t max)
{
    uint32_t i;
    st_hdr_t h;
    if (st_current(obj, &h) < 0)
        return -1;
    if (h.len > max)
        h.len = max;
    for (i = 0; i < h.len; i++)
        ((uint8_t *)dst)[i] = st_buf[i];
    return (int)h.len;
}

static int st_save(uint32_t obj, const void *src, uint32_t len)
{
    uint32_t seq, base, off;
    int cur, rc;
    st_hdr_t h;
    if (obj >= OBJ_NOBJ || len > st_max(obj))
        return -1;
    cur = st_current(obj, &h);
    seq = cur < 0 ? 0u : h.seq;
    base = st_sector(obj, cur == 0 ? 1u : 0u);       /* write the other copy */
    for (off = 0; off < len; off++)
        st_buf[off] = ((const uint8_t *)src)[off];    /* the driver wants RAM sources */
    if ((rc = st_erase(base)) != 0)
        return rc;
    for (off = 0; off < len; off += 256u) {
        uint32_t n = len - off > 256u ? 256u : len - off;
        if ((rc = st_prog(base + ST_PAYLOAD_OFF + off, st_buf + off, n)) != 0)
            return rc;
    }
    h.magic = ST_MAGIC;
    h.type = (uint16_t)obj;
    h.slot = (uint16_t)(cur == 0 ? 1 : 0);
    h.seq = seq + 1u;
    h.len = len;
    h.crc = st_crc32(st_buf, len);
    h.rsv[0] = h.rsv[1] = 0xFFFFFFFFu;
    h.hcrc = st_crc32(&h, sizeof h - 4u);
    if ((rc = st_prog(base, &h, sizeof h)) != 0)       /* the commit record, last */
        return rc;
    {   /* read back: a write-protected or failing part must not report SAVED */
        st_hdr_t chk;
        uint32_t c = cur == 0 ? 1u : 0u;
        if (st_head(obj, c, &chk) || chk.seq != h.seq || st_body(obj, c, &chk))
            return -7;
    }
    return 0;
}

/* ---------------------------------------------------------------- the song store --- *
 * FM1-GM. The songs share every sector of the data area that is not the panel's: 0x97000..0xDBFFF
 * and 0xDE000..0xDFFFF, SS_NSEC sectors. A song is saved as a new version, in as many free sectors
 * as it takes (its parts), each with a 32-byte header at offset 0 (programmed last: the sector's
 * commit record) and up to SS_PAY bytes after it. Each save takes the next global sequence number.
 * At boot (ss_scan) a slot's song is its newest version with every part present and every CRC
 * right; the sectors of older versions, of torn saves and of nothing are free. So a save never
 * touches the version it replaces, and a torn one leaves it in charge. Delete erases a slot's
 * older versions first, then the current one. Free sectors are taken from a rotating cursor, which
 * spreads the erases. Nothing here knows a song's format: the app streams its bytes in and out. */
#define SS_MAGIC 0x53534D46u                   /* "FMSS" */
#define SS_HDR 32u
#define SS_PAY (ST_SECTOR - SS_HDR)
#define SS_LO_N 69u                            /* 0x97000..0xDBFFF; then two at 0xDE000 */
#define SS_NSEC (SS_LO_N + 2u)
#define SS_FREE 0xFFu
#define SS_NSLOTS PLAT_NSONGS

typedef struct {
    uint32_t magic, gseq;
    uint8_t slot, part, nparts, bars;
    uint16_t len, tempo;                       /* this part's bytes; the song's tempo */
    uint16_t nev, rsv;                         /* the song's notes */
    uint32_t total;                            /* the song's bytes, all parts */
    uint32_t crc;                              /* of this part's bytes */
    uint32_t hcrc;
} ss_hdr_t;

typedef struct {
    uint8_t used, bars;
    uint16_t tempo, nev, nsec;
    uint32_t bytes;
} ss_info_t;

static uint8_t ss_slot[SS_NSEC], ss_part[SS_NSEC];   /* each sector: the slot its valid header names */
static uint32_t ss_gseq[SS_NSEC];
static uint32_t ss_cur[SS_NSLOTS];             /* each slot's version (its gseq), 0: none */
static ss_info_t ss_info[SS_NSLOTS];
static uint32_t ss_top, ss_next;               /* the highest gseq seen; the free-sector cursor */

static uint32_t ss_addr(uint32_t i) { return i < SS_LO_N ? 0x97000u + i * ST_SECTOR : 0xDE000u + (i - SS_LO_N) * ST_SECTOR; }
static int ss_live(uint32_t i) { return ss_slot[i] != SS_FREE && ss_gseq[i] == ss_cur[ss_slot[i]]; }

static int ss_head(uint32_t i, ss_hdr_t *h)    /* the header is valid: 0 */
{
    if (st_read(ss_addr(i), h, sizeof *h))
        return -1;
    if (h->magic != SS_MAGIC || h->hcrc != st_crc32(h, sizeof *h - 4u) || h->slot >= SS_NSLOTS || !h->nparts ||
        h->part >= h->nparts || h->nparts > SS_NSEC || h->len > SS_PAY || h->gseq == 0)
        return -1;
    return 0;
}

static int ss_body(uint32_t i, const ss_hdr_t *h)   /* the payload -> st_buf, its CRC right: 0 */
{
    return st_read(ss_addr(i) + SS_HDR, st_buf, h->len) || st_crc32(st_buf, h->len) != h->crc ? -1 : 0;
}

void ss_scan(void)
{
    uint32_t i, j, s;
    ss_hdr_t h;
    ss_top = 0;
    for (s = 0; s < SS_NSLOTS; s++) {
        ss_cur[s] = 0;
        ss_info[s].used = 0;
    }
    for (i = 0; i < SS_NSEC; i++) {
        ss_slot[i] = SS_FREE;
        if (ss_head(i, &h))
            continue;
        if (h.gseq > ss_top)                   /* rotten or not: its number is never used again */
            ss_top = h.gseq;
        if (ss_body(i, &h))
            continue;
        ss_slot[i] = h.slot;
        ss_part[i] = h.part;
        ss_gseq[i] = h.gseq;
    }
    for (i = 0; i < SS_NSEC; i++) {            /* each slot: its newest complete version */
        uint32_t n = 0, np, sl = ss_slot[i];
        if (sl == SS_FREE || ss_gseq[i] <= ss_cur[sl] || ss_part[i] != 0)
            continue;
        if (ss_head(i, &h))
            continue;
        np = h.nparts;
        for (j = 0; j < SS_NSEC; j++)          /* parts 0..np-1, each once (a gseq is one save) */
            if (ss_slot[j] == sl && ss_gseq[j] == ss_gseq[i] && ss_part[j] < np)
                n++;
        if (n != np)
            continue;
        ss_cur[sl] = ss_gseq[i];
        ss_info[sl].used = 1;
        ss_info[sl].bars = h.bars;
        ss_info[sl].tempo = h.tempo;
        ss_info[sl].nev = h.nev;
        ss_info[sl].nsec = (uint16_t)np;
        ss_info[sl].bytes = h.total;
    }
}

uint32_t ss_free_sectors(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < SS_NSEC; i++)
        n += !ss_live(i);
    return n;
}

/* save slot's song: total bytes, which fill(dst, from, n) writes, n bytes from offset from. 0 saved
 * and verified; -10 the store is full; else a flash error. The song before stays until it is done. */
int ss_save(uint32_t slot, uint32_t total, void (*fill)(uint8_t *dst, uint32_t from, uint32_t n),
            uint32_t bars, uint32_t tempo, uint32_t nev)
{
    static uint8_t taken[SS_NSEC];
    uint32_t np = total ? (total + SS_PAY - 1u) / SS_PAY : 1u, p, i, g, k;
    int rc;
    if (slot >= SS_NSLOTS)
        return -1;
    if (np > ss_free_sectors())
        return -10;
    g = ++ss_top;
    for (i = 0; i < SS_NSEC; i++)
        taken[i] = 0;
    for (p = 0; p < np; p++) {
        uint32_t len = total - p * SS_PAY > SS_PAY ? SS_PAY : total - p * SS_PAY, base, off;
        ss_hdr_t h, chk;
        for (k = 0; k < SS_NSEC; k++) {         /* the next free sector from the cursor */
            i = (ss_next + k) % SS_NSEC;
            if (!ss_live(i) && !taken[i])
                break;
        }
        taken[i] = 1;
        ss_next = (i + 1u) % SS_NSEC;
        base = ss_addr(i);
        ss_slot[i] = SS_FREE;                   /* whatever it held is gone */
        if ((rc = st_erase(base)) != 0)
            return rc;
        if (len)
            fill(st_buf, p * SS_PAY, len);
        for (off = 0; off < len;) {             /* page by page: a program never crosses a 256-byte page */
            uint32_t a = base + SS_HDR + off, n = 256u - (a & 255u);
            if (n > len - off)
                n = len - off;
            if ((rc = st_prog(a, st_buf + off, n)) != 0)
                return rc;
            off += n;
        }
        h.magic = SS_MAGIC;
        h.gseq = g;
        h.slot = (uint8_t)slot;
        h.part = (uint8_t)p;
        h.nparts = (uint8_t)np;
        h.bars = (uint8_t)bars;
        h.len = (uint16_t)len;
        h.tempo = (uint16_t)tempo;
        h.nev = (uint16_t)nev;
        h.rsv = 0xFFFFu;
        h.total = total;
        h.crc = st_crc32(st_buf, len);
        h.hcrc = st_crc32(&h, sizeof h - 4u);
        if ((rc = st_prog(base, &h, sizeof h)) != 0)   /* the commit record, last */
            return rc;
        if (ss_head(i, &chk) || chk.gseq != g || ss_body(i, &chk))   /* read back */
            return -7;
        ss_slot[i] = (uint8_t)slot;
        ss_part[i] = (uint8_t)p;
        ss_gseq[i] = g;
    }
    ss_cur[slot] = g;                           /* every part is in: the new version is the song */
    ss_info[slot].used = 1;
    ss_info[slot].bars = (uint8_t)bars;
    ss_info[slot].tempo = (uint16_t)tempo;
    ss_info[slot].nev = (uint16_t)nev;
    ss_info[slot].nsec = (uint16_t)np;
    ss_info[slot].bytes = total;
    return 0;
}

int ss_delete(uint32_t slot)                   /* 0, or a flash error */
{
    uint32_t i, pass;
    int rc;
    if (slot >= SS_NSLOTS)
        return -1;
    for (pass = 0; pass < 2u; pass++)           /* the older versions, then the song */
        for (i = 0; i < SS_NSEC; i++)
            if (ss_slot[i] == slot && (pass == 1u || ss_gseq[i] != ss_cur[slot])) {
                if ((rc = st_erase(ss_addr(i))) != 0)
                    return rc;
                ss_slot[i] = SS_FREE;
                if (pass == 0u)
                    continue;
                ss_cur[slot] = 0;               /* a part gone: the song is gone */
                ss_info[slot].used = 0;
            }
    ss_cur[slot] = 0;
    ss_info[slot].used = 0;
    return 0;
}

/* reading a song: ss_open, then ss_getc to its end */
static struct {
    uint32_t slot, part, np, pos, len;
    int bad;
} ss_rd;

static int ss_find(uint32_t slot, uint32_t part)   /* the sector of the slot's song's part, or -1 */
{
    uint32_t i;
    for (i = 0; i < SS_NSEC; i++)
        if (ss_slot[i] == slot && ss_part[i] == part && ss_cur[slot] && ss_gseq[i] == ss_cur[slot])
            return (int)i;
    return -1;
}

static int ss_fetch(void)                      /* the next part -> st_buf: 0 */
{
    ss_hdr_t h;
    int i = ss_find(ss_rd.slot, ss_rd.part);
    if (i < 0 || ss_head((uint32_t)i, &h) || h.gseq != ss_cur[ss_rd.slot] || ss_body((uint32_t)i, &h))
        return -1;
    ss_rd.np = h.nparts;
    ss_rd.len = h.len;
    ss_rd.pos = 0;
    return 0;
}

int ss_open(uint32_t slot)                     /* 0: the song is there, read it with ss_getc */
{
    if (slot >= SS_NSLOTS || !ss_cur[slot])
        return -1;
    ss_rd.slot = slot;
    ss_rd.part = 0;
    ss_rd.bad = ss_fetch();
    return ss_rd.bad;
}

int ss_getc(void)                              /* the next byte; -1 at the end, -2 unreadable */
{
    while (ss_rd.pos >= ss_rd.len) {
        if (ss_rd.bad)
            return -2;
        if (ss_rd.part + 1u >= ss_rd.np)
            return -1;
        ss_rd.part++;
        if ((ss_rd.bad = ss_fetch()) != 0)
            return -2;
    }
    return st_buf[ss_rd.pos++];
}
