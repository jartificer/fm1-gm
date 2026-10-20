/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM in the browser: the whole app (UI, settings, sequencer, engine, the song store) as
 * WebAssembly, against a simulated FM-1 that web/emu.js drives. The same sources as the firmware
 * and host/gm_host.c, built freestanding (no libc) by host/build_web.sh into web/fm1gm.wasm.
 *
 * The page sets the panel's state (buttons, keys, encoders, MASTER), queues MIDI in, and calls
 * web_run(ms) to run the device; it reads the screen (web_fb, RGB565 as the panel takes it), the
 * lights, MIDI out and the audio (stereo float, 44.1 kHz, web_audio_*). The flash (web_nor) is
 * 1 MiB the page keeps between visits. */
#define OM_HOST 1
#define OM_WEB 1
#include <stddef.h>
#include <stdint.h>
#define WEB(name) __attribute__((export_name(#name)))

/* ------------------------------------------------------------ libc --- */
void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    if (p < q)
        while (n--)
            *p++ = *q++;
    else
        while (n--)
            p[n] = q[n];
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}
#define printf(...) 0                    /* ui_fit_check's report: not used here */

/* ------------------------------------------------------------ screen --- */
static uint16_t fb[240 * 240];          /* RGB565, byte-swapped as the panel takes it (gfx.c) */
static uint32_t fb_gen;                 /* bumped on every draw: the page repaints when it changes */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    uint16_t sc = (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8));
    fb_gen++;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = sc;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    fb_gen++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u)
                fb[(y + j) * 240u + x + i] = px[j * w + i];
}
#include "../firmware/src/gfx.c"
static unsigned str_len(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

/* ---------------------------------------------------------- platform --- */
#include "../firmware/src/app/plat.h"

static uint32_t now_ms, held_btn, held_keys, master = 2048;
static int32_t enc_acc[NE];
static uint32_t lit_btn, lit_keys;
static int glow_on = 1, play_red;

uint32_t plat_ms(void) { return now_ms; }
uint32_t plat_buttons(void) { return held_btn; }
uint32_t plat_keys(void) { return held_keys; }
int32_t plat_enc(int role)
{
    int32_t v = enc_acc[role];
    enc_acc[role] = 0;
    return v;
}
uint32_t plat_master(void) { return master; }
int plat_battery(void) { return 4; }   /* the emulator runs on USB power */
void plat_leds(uint32_t b, uint32_t k)
{
    lit_btn = b;
    lit_keys = k;
}
void plat_glow(int on) { glow_on = on; }
void plat_play_red(int red) { play_red = red; }

#define MQ 256u                          /* USB-MIDI packets, both ways */
static uint32_t min_q[MQ], mi_w, mi_r, mout_q[MQ], mo_w, mo_r;
int plat_midi_in(uint32_t *pkt)
{
    if (mi_r == mi_w)
        return 0;
    *pkt = min_q[mi_r++ % MQ];
    return 1;
}
void plat_midi_out(uint32_t pkt)
{
    if (mo_w - mo_r < MQ)
        mout_q[mo_w++ % MQ] = pkt;
}
void seq_midi_out(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint32_t cin = st >= 0xF0u ? 0xFu : st >> 4;
    plat_midi_out(cin | st << 8 | d1 << 16 | d2 << 24);
}

/* flash: a simulated NOR (erase -> 0xFF, programming clears bits) under the firmware's storage.c */
static uint8_t nor[0x100000];
static uint32_t store_writes;           /* the page saves the flash when this changes */
static const uint8_t flash_ok = 1;
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off)
{
    memset(nor + off, 0xFF, 4096);
    store_writes++;
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *p = src;
    uint32_t i;
    for (i = 0; i < n; i++)
        nor[off + i] &= p[i];
    store_writes++;
    return 0;
}
#include "../firmware/src/storage.c"
#include "../firmware/src/app/plat_store.c"

static uint32_t cpu_pct;
uint32_t plat_cpu_pct(void) { return cpu_pct; }
uint32_t plat_xruns(void) { return 0; }

/* -------------------------------------------------------------- app --- */
#include "../firmware/src/app/app.h"
#include "../firmware/src/app/settings.c"
#include "../firmware/src/app/ui.c"

/* SysEx for the songs (link.c, the web song editor): one message in at a time, as usb.c keeps it;
 * the answers wait in sx_out until the page takes them */
static uint8_t sx_in[640], sx_out[16384];
static uint32_t sx_in_n, sx_out_n;
static int link_rx(const uint8_t **p, uint32_t *n)
{
    if (!sx_in_n)
        return 0;
    *p = sx_in;
    *n = sx_in_n;
    return 1;
}
static void link_rx_done(void) { sx_in_n = 0; }
static int link_tx(const uint8_t *p, uint32_t n)
{
    if (sx_out_n + n > sizeof sx_out)
        return -1;
    memcpy(sx_out + sx_out_n, p, n);
    sx_out_n += n;
    return 0;
}
#include "../firmware/src/app/link.c"

/* ------------------------------------------------------------ audio --- */
/* rendered frames wait here until the page takes them (web_audio_take) */
#define AQ 16384u                        /* frames, a power of two */
static float aq[AQ * 2];
static uint32_t aq_w, aq_r;
static float aout[AQ * 2];

/* --------------------------------------------------------- the device --- */
static uint32_t audio_due_x256;         /* the next block, in 1/256 ms */
static uint32_t ui_due;

WEB(web_boot) void web_boot(int fresh)
{
    if (fresh)                           /* a new device: the flash erased (else the page loaded it) */
        memset(nor, 0xFF, sizeof nor);
    now_ms = 0;
    audio_due_x256 = 0;
    ui_due = 0;
    aq_w = aq_r = 0;
    memset(&set, 0, sizeof set);
    seq_init();
    settings_load();
    song_boot_load();
    lcd_fill(0, 0, 240, 240, 0);
    ui_init();
}

WEB(web_run) void web_run(uint32_t ms)
{
    static int32_t blk[512];
    uint32_t i, k;
    for (i = 0; i < ms; i++) {
        now_ms++;
        while (audio_due_x256 <= now_ms * 256u) {   /* 256-frame blocks, as the device renders them */
            seq_render(blk, 256, master);
            for (k = 0; k < 256; k++) {
                uint32_t w = (aq_w + k) & (AQ - 1u);
                aq[2 * w] = (float)blk[2 * k] / 8388608.0f;
                aq[2 * w + 1] = (float)blk[2 * k + 1] / 8388608.0f;
            }
            aq_w += 256;
            if (aq_w - aq_r > AQ)            /* nobody listening: keep the newest */
                aq_r = aq_w - AQ;
            audio_due_x256 += 256u * 256u * 1000u / 44100u;
        }
        link_service();
        sx_in_n = 0;                         /* not the songs': dropped */
        if (now_ms >= ui_due) {
            ui_due = now_ms + 16;
            ui_frame();
        } else {
            ui_input_only();
        }
    }
}

/* the frames rendered and not yet taken, at most max, copied to web_audio_buf (L R L R ...) */
WEB(web_audio_take) uint32_t web_audio_take(uint32_t max)
{
    uint32_t n = aq_w - aq_r, i;
    if (n > max)
        n = max;
    if (n > AQ)
        n = AQ;
    for (i = 0; i < n; i++) {
        uint32_t r = (aq_r + i) & (AQ - 1u);
        aout[2 * i] = aq[2 * r];
        aout[2 * i + 1] = aq[2 * r + 1];
    }
    aq_r += n;
    return n;
}
WEB(web_audio_buf) float *web_audio_buf(void) { return aout; }

WEB(web_fb) uint16_t *web_fb(void) { return fb; }
WEB(web_fb_gen) uint32_t web_fb_gen(void) { return fb_gen; }
WEB(web_nor) uint8_t *web_nor(void) { return nor; }
WEB(web_nor_size) uint32_t web_nor_size(void) { return sizeof nor; }
WEB(web_store_writes) uint32_t web_store_writes(void) { return store_writes; }

WEB(web_set_buttons) void web_set_buttons(uint32_t b) { held_btn = b; }
WEB(web_set_keys) void web_set_keys(uint32_t k) { held_keys = k; }
WEB(web_turn) void web_turn(int e, int32_t n)
{
    if (e >= 0 && e < NE)
        enc_acc[e] += n;
}
WEB(web_set_master) void web_set_master(uint32_t m) { master = m > 4096u ? 4096u : m; }
WEB(web_lit_buttons) uint32_t web_lit_buttons(void) { return lit_btn; }
WEB(web_lit_keys) uint32_t web_lit_keys(void) { return lit_keys; }
WEB(web_glow) int web_glow(void) { return glow_on; }
WEB(web_play_red) int web_play_red(void) { return play_red; }

/* a channel or realtime message from the page's MIDI in (USB-MIDI packet, cable 0) */
WEB(web_midi_in) void web_midi_in(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint32_t cin = st >= 0xF0u ? 0xFu : st >> 4;
    if (mi_w - mi_r < MQ)
        min_q[mi_w++ % MQ] = cin | st << 8 | d1 << 16 | d2 << 24;
}
/* the next MIDI out packet, 0 = none */
WEB(web_midi_out) uint32_t web_midi_out(void) { return mo_r == mo_w ? 0u : mout_q[mo_r++ % MQ]; }

WEB(web_cpu) uint32_t web_cpu(void) { return cpu_pct; }
WEB(web_set_cpu) void web_set_cpu(uint32_t p) { cpu_pct = p; }
WEB(web_view) int web_view(void) { return ui.view; }
WEB(web_playing) int web_playing(void) { return seq_playing; }

/* a SysEx message from the page: the bytes between F0 and F7 written to web_sx_in_buf first. 0: one is
 * still waiting (send it again later) */
WEB(web_sx_in_buf) uint8_t *web_sx_in_buf(void) { return sx_in; }
WEB(web_sx_in) int web_sx_in(uint32_t n)
{
    if (sx_in_n || !n || n > sizeof sx_in)
        return 0;
    sx_in_n = n;
    return 1;
}
WEB(web_sx_busy) int web_sx_busy(void) { return sx_in_n != 0; }
/* the SysEx messages sent (F0 .. F7 each, one after another), web_sx_out_len bytes; web_sx_out_clear
 * once taken */
WEB(web_sx_out) uint8_t *web_sx_out(void) { return sx_out; }
WEB(web_sx_out_len) uint32_t web_sx_out_len(void) { return sx_out_n; }
WEB(web_sx_out_clear) void web_sx_out_clear(void) { sx_out_n = 0; }
