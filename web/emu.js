// SPDX-License-Identifier: GPL-3.0-only
// The FM-1 emulator: web/fm1gm.wasm (the firmware's app, engine and sequencer, host/gm_web.c) run
// against the panel on the page. The device's time follows the audio: whenever the audio worklet has
// played a block, the device runs on until about 70 ms of sound is queued again. Without sound (before
// the first click) it follows the clock. The flash (songs, settings) is kept in IndexedDB.

export const BUTTONS = ["FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"];
export const ENCODERS = ["SELECT", "ALGORITHM", "PRESETS", "KNOB 1", "KNOB 2", "KNOB 3", "KNOB 4"];
export const NKEYS = 27;                       // F3 .. G5
const RATE = 44100, AHEAD = 3072;              // frames queued ahead of the speaker
const TAP = 40;                                // ms: the shortest press the device sees

// ------------------------------------------------------------ the flash ---
const DB = "fm1gm-emulator", STORE = "flash";
function idb(mode, fn) {
    return new Promise((res, rej) => {
        let req;
        try { req = indexedDB.open(DB, 1); } catch (e) { rej(e); return; }
        req.onupgradeneeded = () => req.result.createObjectStore(STORE);
        req.onerror = () => rej(req.error);
        req.onsuccess = () => {
            const tx = req.result.transaction(STORE, mode), r = fn(tx.objectStore(STORE));
            tx.oncomplete = () => res(r && r.result);
            tx.onerror = () => rej(tx.error);
        };
    });
}
const flashLoad = () => idb("readonly", s => s.get("nor")).catch(() => null);
const flashSave = bytes => idb("readwrite", s => s.put(bytes, "nor")).catch(() => null);
const flashErase = () => idb("readwrite", s => s.delete("nor")).catch(() => null);

export class Emulator {
    constructor() {
        this.x = null;
        this.buttons = 0;
        this.keys = 0;
        this.ctx = null;
        this.node = null;
        this.sent = 0;
        this.played = 0;
        this.lastWall = performance.now();
        this.writes = 0;
        this.saveTimer = 0;
        this.listeners = new Set();
        this.fbGen = -1;
        this.persistent = true;
        this.ms = 0;                                 // the device's time
        this.downAt = new Map();                     // control -> when it went down
        this.ups = [];                               // quick taps: [due, mask, isKey]
        this.sxq = [];                               // SysEx waiting for the device (one at a time)
    }

    async load(url = "fm1gm.wasm") {
        const bytes = await (await fetch(url)).arrayBuffer();
        const {instance} = await WebAssembly.instantiate(bytes, {});
        this.x = instance.exports;
        const saved = await flashLoad();
        const fresh = !(saved instanceof Uint8Array) || saved.length !== this.x.web_nor_size();
        if (!fresh) this.nor().set(saved);
        this.x.web_boot(fresh ? 1 : 0);
        this.writes = this.x.web_store_writes();
        this.restored = !fresh;
        setInterval(() => this.clockTick(), 10);
    }

    nor() { return new Uint8Array(this.x.memory.buffer, this.x.web_nor(), this.x.web_nor_size()); }

    // ---------------------------------------------------------- audio ---
    async startAudio() {
        if (this.ctx) { await this.ctx.resume(); return; }
        const ctx = new AudioContext({sampleRate: RATE, latencyHint: "interactive"});
        await ctx.audioWorklet.addModule(new URL("worklet.js", import.meta.url));
        this.node = new AudioWorkletNode(ctx, "fm1-out", {outputChannelCount: [2]});
        this.node.connect(ctx.destination);
        this.node.port.onmessage = e => { this.played = e.data; this.audioTick(); };
        this.ctx = ctx;
        this.x.web_audio_take(1 << 20);              // what was rendered with nobody listening
        await ctx.resume();
        this.audioTick();
    }
    get audioOn() { return !!this.ctx && this.ctx.state === "running"; }

    audioTick() {
        const need = AHEAD - (this.sent - this.played);
        if (need <= 0) return;
        this.run(Math.ceil(need / (RATE / 1000)));
        for (;;) {
            const n = this.x.web_audio_take(4096);
            if (!n) break;
            const blk = new Float32Array(this.x.memory.buffer, this.x.web_audio_buf(), n * 2).slice();
            this.node.port.postMessage(blk, [blk.buffer]);
            this.sent += n;
        }
    }

    clockTick() {                                    // no sound: the device follows the clock
        const t = performance.now(), dt = Math.min(100, Math.round(t - this.lastWall));
        if (this.audioOn) { this.lastWall = t; return; }
        if (dt < 1) return;
        this.lastWall += dt;
        this.run(dt);
        this.x.web_audio_take(1 << 20);
    }

    run(ms) {
        const x = this.x;
        while (ms > 0) {                             // a tap is held for at least TAP ms of the device's time
            const due = this.ups.length ? Math.min(...this.ups.map(u => u[0])) : Infinity;
            const n = Math.max(0, Math.min(ms, due - this.ms));
            x.web_set_buttons(this.buttons);
            x.web_set_keys(this.keys);
            this.sxPump();
            if (n) x.web_run(n);
            this.ms += n;
            ms -= n;
            for (const u of this.ups.filter(u => u[0] <= this.ms)) {
                if (u[2]) this.keys = (this.keys & ~u[1]) >>> 0; else this.buttons &= ~u[1];
            }
            this.ups = this.ups.filter(u => u[0] > this.ms);
        }
        x.web_set_buttons(this.buttons);
        x.web_set_keys(this.keys);
        for (let p; (p = x.web_midi_out());) for (const f of this.listeners) f("midiout", p);
        this.sxPump();
        const sn = x.web_sx_out_len();
        if (sn) {
            const out = new Uint8Array(x.memory.buffer, x.web_sx_out(), sn).slice();
            x.web_sx_out_clear();
            for (let i = 0; i < sn;) {                // F0 .. F7 each
                const j = out.indexOf(0xF7, i);
                if (j < 0) break;
                for (const f of this.listeners) f("sysex", out.subarray(i, j + 1));
                i = j + 1;
            }
        }
        const w = x.web_store_writes();
        if (w !== this.writes) {
            this.writes = w;
            clearTimeout(this.saveTimer);
            if (this.persistent) this.saveTimer = setTimeout(() => flashSave(this.nor().slice()), 400);
        }
    }

    // --------------------------------------------------------- the panel ---
    press(b, down) { this.control("b" + b, 1 << BUTTONS.indexOf(b), false, down); }
    key(k, down) { this.control("k" + k, 1 << k, true, down); }
    control(id, bit, isKey, down) {
        const held = isKey ? this.keys : this.buttons;
        if (down) {
            this.ups = this.ups.filter(u => !(u[2] === isKey && u[1] === bit));
            this.downAt.set(id, this.ms);
            if (isKey) this.keys = (held | bit) >>> 0; else this.buttons = held | bit;
            return;
        }
        if (!(held & bit)) return;
        const at = this.downAt.get(id) ?? -1e9;
        if (this.ms - at < TAP) { this.ups.push([at + TAP, bit, isKey]); return; }
        if (isKey) this.keys = (held & ~bit) >>> 0; else this.buttons = held & ~bit;
    }
    // a SysEx message (F0 .. F7) for the device, as from its USB port (the song editor's, link.c)
    sysex(msg) {
        if (msg[0] !== 0xF0 || msg.at(-1) !== 0xF7) return;
        this.sxq.push(msg.slice(1, -1));
        if (!this.audioOn) this.run(2);              // answer now, even with the page in the background
    }
    sxPump() {
        const x = this.x;
        while (this.sxq.length && !x.web_sx_busy()) {
            const m = this.sxq.shift();
            if (m.length > 640) continue;
            new Uint8Array(x.memory.buffer, x.web_sx_in_buf(), m.length).set(m);
            x.web_sx_in(m.length);
        }
    }
    turn(e, n) { this.x.web_turn(ENCODERS.indexOf(e), n); }
    master(v) { this.x.web_set_master(Math.round(v)); }
    midi(st, d1 = 0, d2 = 0) { this.x.web_midi_in(st, d1, d2); }
    lights() {
        const x = this.x;
        return {buttons: x.web_lit_buttons(), keys: x.web_lit_keys() >>> 0, glow: !!x.web_glow(), red: !!x.web_play_red()};
    }

    // the screen into a 240x240 ImageData, when it changed; -> true if it did
    paint(img) {
        const g = this.x.web_fb_gen();
        if (g === this.fbGen) return false;
        this.fbGen = g;
        const fb = new Uint16Array(this.x.memory.buffer, this.x.web_fb(), 240 * 240), d = img.data;
        for (let i = 0, o = 0; i < fb.length; i++, o += 4) {
            const v = fb[i], p = ((v >> 8) | (v << 8)) & 0xFFFF;
            d[o] = ((p >> 11) & 31) * 255 / 31;
            d[o + 1] = ((p >> 5) & 63) * 255 / 63;
            d[o + 2] = (p & 31) * 255 / 31;
            d[o + 3] = 255;
        }
        return true;
    }

    reboot() {                                       // power off and on: everything from the flash
        this.buttons = this.keys = 0;
        this.ups = [];
        this.x.web_boot(0);
        this.fbGen = -1;
    }
    async erase() {                                  // a new FM-1: the flash erased, songs and settings gone
        clearTimeout(this.saveTimer);
        await flashErase();
        this.buttons = this.keys = 0;
        this.ups = [];
        this.x.web_boot(1);
        this.writes = this.x.web_store_writes();
        this.fbGen = -1;
    }
    on(f) { this.listeners.add(f); }
}
