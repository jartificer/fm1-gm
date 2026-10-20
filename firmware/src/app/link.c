/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM: the songs over USB-MIDI SysEx, for the web song editor (web/editor.html, web/fm1link.js).
 * Part of the unity build after ui.c; the web emulator (host/gm_web.c) and the simulator include it too.
 * The includer supplies:
 *   link_rx(&p, &n)   the SysEx received and not yet taken (the bytes between F0 and F7): 1, else 0
 *   link_rx_done()    taken
 *   link_tx(p, n)     send one whole message, F0 .. F7
 *
 *   host -> FM-1:  F0 7D 46 47 cmd args .. F7        FM-1 -> host:  F0 7D 46 47 cmd|40 status args .. F7
 * 7D is MIDI's ID for non-commercial use, 46 47 "FG". Numbers go 7 bits a byte, the low bits first; a
 * song's bytes (seq.c's stream, as in flash) are packed 8 bits into 7, the low bits first (as ota.c's).
 * Slot 64 is the song in memory.
 *
 *   01 HELLO                    41 0   protocol, song format, slots, sectors (2), sectors free (2),
 *                                      the slot in use, unsaved changes (1), version (ASCII)
 *   02 LIST first               42 0   first, n, then n x (used, bars (2), tempo (2), notes (2), bytes (3),
 *                                      sectors)
 *   03 READ slot offset (3)     43 st  slot, offset (3), the song's bytes (3), up to LK_CHUNK of them, packed
 *   04 WRITE slot bytes (3)     44 st  then its bytes in order, each DATA at most LK_CHUNK of them:
 *   05 DATA offset (3) packed   45 st  the bytes in so far (3); when they are all in, once the sequencer
 *                               47 st  slot   has stopped: the song is loaded (and saved), or why not
 *   06 DELETE slot              46 st  slot
 * status: 0 done, 1 a bad request (or an empty slot), 2 not a song, 3 no room, 4 a flash error, 5 busy.
 *
 * A READ from offset 0 takes a copy of the song (a slot's bytes, or the song in memory streamed) and the
 * reads after it come from that copy. A WRITE's song comes into the same buffer; then seq_import makes it
 * the song in memory (ARP undoes that) and, to a slot, it is saved there and that slot is the one in use.
 * The host waits for each answer before it sends again (usb.c keeps one SysEx at a time). */
#define LK_PROTO 1u
#define LK_CHUNK 512u                    /* -> 586 bytes packed: a DATA fits usb.c's 640-byte frame */
#define LK_BUF 49152u                    /* the most a song's stream can take (8192 notes) is ~44 KiB */
#define LK_SONG SEQ_NSONGS               /* the song in memory */

enum { LK_IDLE, LK_COPY, LK_RECV, LK_LOAD };
static uint8_t lk_buf[LK_BUF] OM_POOL;
static uint8_t lk_out[16u + (LK_CHUNK * 8u + 6u) / 7u + 2u];
static struct {
    uint8_t mode, slot;
    uint32_t total, have, t;
} lk;

static uint32_t lk_num(const uint8_t *p, int k)
{
    uint32_t v = 0;
    while (k--)
        v = v << 7 | (p[k] & 127u);
    return v;
}

static uint8_t *lk_put(uint8_t *o, uint32_t v, int k)
{
    while (k--) {
        *o++ = (uint8_t)(v & 127u);
        v >>= 7;
    }
    return o;
}

static uint8_t *lk_head(uint32_t cmd, uint32_t st)
{
    lk_out[0] = 0xF0;
    lk_out[1] = 0x7D;
    lk_out[2] = 0x46;
    lk_out[3] = 0x47;
    lk_out[4] = (uint8_t)(cmd | 0x40u);
    lk_out[5] = (uint8_t)st;
    return lk_out + 6;
}

static void lk_send(uint8_t *end)
{
    *end++ = 0xF7;
    link_tx(lk_out, (uint32_t)(end - lk_out));
}

static int lk_get(void) { return lk.have < lk.total ? lk_buf[lk.have++] : -1; }

static uint32_t lk_copy(uint32_t slot)          /* the song's bytes -> lk_buf: a status */
{
    uint32_t n = 0;
    int c;
    if (slot == LK_SONG) {
        n = song_bytes(&seq_song);
        if (n > LK_BUF)
            return 1;
        song_stream(&seq_song, lk_buf, 0, n);
    } else {
        if (slot > LK_SONG || !slot_info[slot].used || plat_song_open((int)slot) != 0)
            return 1;
        while ((c = plat_song_getc()) >= 0) {
            if (n >= LK_BUF)
                return 4;
            lk_buf[n++] = (uint8_t)c;
        }
        if (c != -1)
            return 4;
    }
    lk.mode = LK_COPY;
    lk.slot = (uint8_t)slot;
    lk.total = n;
    return 0;
}

static void lk_load(void)                       /* the song that came in, once the sequencer has stopped */
{
    uint32_t st = 0;
    int rc;
    if (seq_playing || !seq_idle()) {
        if (plat_ms() - lk.t > 3000u) {
            lk.mode = LK_IDLE;
            lk_put(lk_head(7, 5), lk.slot, 1);
            lk_send(lk_out + 7);
        }
        return;
    }
    seq_lock = 1;
    lk.have = 0;
    if (seq_import(lk_get) != 0) {
        st = 2;
    } else if (lk.slot < LK_SONG) {
        rc = song_save_slot(lk.slot);
        st = rc == 0 ? 0u : rc == -10 ? 3u : 4u;
    }
    seq_lock = 0;
    if (st != 2u)
        ui_song_replaced(st == 0u && lk.slot < LK_SONG ? lk.slot : -1);
    lk.mode = LK_IDLE;
    lk_put(lk_head(7, st), lk.slot, 1);
    lk_send(lk_out + 7);
}

static uint32_t lk_pack(uint8_t *o, const uint8_t *in, uint32_t n)
{
    uint32_t acc = 0, nb = 0, k = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        for (nb += 8u; nb >= 7u; nb -= 7u, acc >>= 7)
            o[k++] = (uint8_t)(acc & 127u);
    }
    if (nb)
        o[k++] = (uint8_t)acc;
    return k;
}

static uint32_t lk_unpack(uint8_t *o, const uint8_t *in, uint32_t n, uint32_t max)
{
    uint32_t acc = 0, nb = 0, k = 0;
    while (n--) {
        acc |= (uint32_t)(*in++ & 127u) << nb;
        if ((nb += 7u) >= 8u) {
            if (k < max)
                o[k++] = (uint8_t)acc;
            acc >>= 8;
            nb -= 8u;
        }
    }
    return k;
}

static void link_service(void)                  /* main loop */
{
    const uint8_t *p;
    uint32_t n, cmd, a, b, st = 0, k;
    uint8_t *o;
    if (lk.mode == LK_LOAD)
        lk_load();
    if (!link_rx(&p, &n))
        return;
    if (n < 4u || p[0] != 0x7D || p[1] != 0x46 || p[2] != 0x47)
        return;                                 /* not ours (ota.c's, on the device) */
    cmd = p[3];
    p += 4;
    n -= 4u;
    a = n ? p[0] : 0u;
    o = lk_head(cmd, 0);
    switch (cmd) {
    case 1: {
        static const char V[] = OM_VERSION;
        *o++ = LK_PROTO;
        *o++ = SONG_FORMAT;
        *o++ = SEQ_NSONGS;
        o = lk_put(o, plat_song_sectors(), 2);
        o = lk_put(o, plat_song_free(), 2);
        *o++ = set.song;
        *o++ = (uint8_t)ui_song_dirty();
        for (k = 0; k + 1u < sizeof V && k < 16u; k++)
            *o++ = (uint8_t)(V[k] & 127);
        break;
    }
    case 2:
        if (a >= SEQ_NSONGS) {
            st = 1;
            break;
        }
        b = SEQ_NSONGS - a < 16u ? SEQ_NSONGS - a : 16u;
        *o++ = (uint8_t)a;
        *o++ = (uint8_t)b;
        for (k = a; k < a + b; k++) {
            const slot_info_t *i = &slot_info[k];
            *o++ = i->used;
            o = lk_put(o, i->bars, 2);
            o = lk_put(o, i->tempo, 2);
            o = lk_put(o, i->nev, 2);
            o = lk_put(o, i->bytes, 3);
            *o++ = (uint8_t)(i->nsec & 127u);
        }
        break;
    case 3:
        b = n >= 4u ? lk_num(p + 1, 3) : 1u;    /* the offset */
        if (lk.mode == LK_RECV || lk.mode == LK_LOAD)
            st = 5;
        else if (n < 4u || (b && (lk.mode != LK_COPY || lk.slot != a || b > lk.total)))
            st = 1;
        else if (!b)
            st = lk_copy(a);
        *o++ = (uint8_t)a;
        o = lk_put(o, b, 3);
        o = lk_put(o, st ? 0u : lk.total, 3);
        if (!st) {
            k = lk.total - b < LK_CHUNK ? lk.total - b : LK_CHUNK;
            o += lk_pack(o, lk_buf + b, k);
        }
        break;
    case 4:
        b = n >= 4u ? lk_num(p + 1, 3) : 0u;    /* the song's bytes */
        if (lk.mode == LK_LOAD) {
            st = 5;
        } else if (n < 4u || a > LK_SONG || !b || b > LK_BUF) {
            st = 1;
        } else {
            lk.mode = LK_RECV;
            lk.slot = (uint8_t)a;
            lk.total = b;
            lk.have = 0;
        }
        break;
    case 5:
        b = n >= 3u ? lk_num(p, 3) : 0u;        /* where these bytes go */
        if ((lk.mode != LK_RECV && lk.mode != LK_LOAD) || n < 3u || b > lk.have) {
            st = 1;
        } else if (lk.mode == LK_RECV && b == lk.have) {   /* else a resend of what is in: the answer again */
            lk.have += lk_unpack(lk_buf + b, p + 3, n - 3u, lk.total - b);
            if (lk.have == lk.total) {
                lk.mode = LK_LOAD;
                lk.t = plat_ms();
                ui_stop();
            }
        }
        o = lk_put(o, lk.mode == LK_RECV || lk.mode == LK_LOAD ? lk.have : 0u, 3);
        break;
    case 6:
        if (a >= SEQ_NSONGS || !n) {
            st = 1;
        } else {
            st = song_delete_slot((int)a) ? 4u : 0u;
            ui_song_deleted((int)a);
            if (lk.mode == LK_COPY && lk.slot == a)
                lk.mode = LK_IDLE;
        }
        *o++ = (uint8_t)a;
        break;
    default:
        st = 1;
    }
    link_rx_done();
    lk_out[5] = (uint8_t)st;
    lk_send(o);
}
