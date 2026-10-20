// SPDX-License-Identifier: GPL-3.0-only
// The song editor's preview: the firmware's own sound engine (web/fm1gm.wasm: gm.c and the FM-1's
// samples) playing the editor's song as the FM-1's sequencer would: each track on its channel with its
// program, volume, pan and reverb (from the lowest track on a channel), its transpose and its velocity,
// mute and solo, looping the song. It reads the song as it plays, so edits are heard at once. A device of
// its own (not the emulator's): its flash starts empty and is not kept. As in emu.js, its time follows
// the audio: the device runs on, a millisecond at a time, until about 90 ms of sound is queued.
import * as S from "./fm1song.js";

const RATE = 44100, AHEAD = 4096;

export class Preview {
    constructor(getSong) {
        this.getSong = getSong;
        this.x = null;
        this.ctx = null;
        this.sent = 0;
        this.played = 0;
        this.ms = 0;                               // the device's time
        this.playing = false;
        this.pos = 0;                              // the song's position, in ticks (fractional)
        this.next = 0;                             // the next whole tick to play
        this.on = [];                              // sounding: {off (device ms), chan, key}
        this.index = null;                         // tick -> [[track, note]], rebuilt after an edit
        this.loading = null;
        this.starting = null;
        this.hist = [];                            // [device ms, position] while playing, the last 400 ms
    }

    load(url = new URL("fm1gm.wasm", import.meta.url)) {
        this.loading ||= (async () => {
            const {instance} = await WebAssembly.instantiate(await (await fetch(url)).arrayBuffer(), {});
            this.x = instance.exports;
            this.x.web_boot(1);
            this.x.web_run(300);                   // past the boot, sequencer stopped
            this.x.web_audio_take(1 << 20);
        })();
        return this.loading;
    }

    // from a click or a key: browsers let the audio start only then, and Firefox (mobile above all) only
    // while still inside that event, so the context is made and resumed before anything is awaited
    async start() {
        if (!this.ctx) this.ctx = new AudioContext({sampleRate: RATE, latencyHint: "interactive"});
        const resumed = this.ctx.state !== "running" ? this.ctx.resume() : null;
        await (this.starting ||= this.open());
        await resumed?.catch(() => {});            // refused outside a tap: running() waits for the next one
    }
    // until the audio runs and the speaker is taking it: on a phone the first tap's pointerdown may not
    // start it (its pointerup will), and a note sent before the output is up can be lost
    running(ms = 3000) {
        const ready = () => this.ctx.state === "running" && this.played > 0;
        if (ready()) return Promise.resolve(true);
        return new Promise(done => {
            const t0 = performance.now(), wait = () => {
                if (ready()) done(true);
                else if (performance.now() - t0 > ms) done(false);
                else setTimeout(wait, 10);
            };
            wait();
        });
    }
    async open() {
        const ctx = this.ctx;
        await Promise.all([this.load(), ctx.audioWorklet.addModule(new URL("worklet.js", import.meta.url))]);
        const node = new AudioWorkletNode(ctx, "fm1-out", {outputChannelCount: [2]});
        node.connect(ctx.destination);
        node.port.onmessage = e => { this.played = e.data; this.tick(); };
        this.node = node;
        this.x.web_audio_take(1 << 20);
        this.tick();
    }

    tick() {
        const need = AHEAD - (this.sent - this.played);
        if (need <= 0) return;
        for (let ms = Math.ceil(need / (RATE / 1000)); ms > 0; ms--) {
            this.step();
            this.x.web_run(1);
            this.ms++;
        }
        for (let n; (n = this.x.web_audio_take(4096));) {
            const blk = new Float32Array(this.x.memory.buffer, this.x.web_audio_buf(), n * 2).slice();
            this.node.port.postMessage(blk, [blk.buffer]);
            this.sent += n;
        }
    }

    midi(st, d1, d2 = 0) { this.x.web_gm(st, d1, d2); }

    // every track's settings to its channel (the lowest track on a channel wins, as on the FM-1)
    apply() {
        if (!this.x) return;
        const s = this.getSong(), done = new Set();
        for (const t of s.tracks) {
            if (done.has(t.chan)) continue;
            done.add(t.chan);
            if (t.chan !== S.DRUM_CH) this.midi(0xC0 | t.chan, t.prog);
            this.midi(0xB0 | t.chan, 7, t.vol);
            this.midi(0xB0 | t.chan, 10, t.pan);
            this.midi(0xB0 | t.chan, 91, t.rev);
        }
    }
    changed() {                                    // the song was edited
        this.index = null;
        if (this.playing) this.apply();
    }

    noteOn(t, note, ticks) {
        const s = this.getSong(), k = s.tracks[t], chan = k.chan;
        const key = chan === S.DRUM_CH ? note : Math.max(0, Math.min(127, note + k.transpose));
        const i = this.on.findIndex(v => v.chan === chan && v.key === key);
        if (i >= 0) { this.midi(0x80 | chan, key); this.on.splice(i, 1); }
        this.midi(0x90 | chan, key, k.vel);
        this.on.push({off: this.ms + ticks * 60000 / (s.tempo * S.PPQN), chan, key});
    }

    step() {                                       // one millisecond of the song
        if (this.playing) {
            const s = this.getSong(), end = S.songTicks(s);
            if (!this.index) {
                this.index = new Map();
                s.tracks.forEach((t, i) => {
                    for (const n of t.notes) {
                        if (!this.index.has(n.tick)) this.index.set(n.tick, []);
                        this.index.get(n.tick).push([i, n]);
                    }
                });
            }
            const solo = s.tracks.some(t => t.solo);
            this.pos += s.tempo * S.PPQN / 60000;
            while (this.next <= this.pos) {
                if (this.next >= end) {            // the loop: back to the top
                    const k = Math.floor(this.next / end) * end;
                    this.next -= k;
                    this.pos -= k;
                }
                for (const [i, n] of this.index.get(this.next) || [])
                    if (solo ? s.tracks[i].solo : !s.tracks[i].mute) this.noteOn(i, n.note, n.len);
                this.next++;
            }
            this.hist.push([this.ms, this.pos]);
            if (this.hist.length > 400) this.hist.shift();
        }
        if (this.on.length) {
            const keep = [];
            for (const v of this.on) if (v.off <= this.ms) this.midi(0x80 | v.chan, v.key); else keep.push(v);
            this.on = keep;
        }
    }

    async play(from = 0) {
        await this.start();
        await this.running();
        this.allOff();
        this.apply();
        this.pos = from - 1e-9;
        this.next = Math.ceil(from);
        this.index = null;
        this.hist = [[this.ms, from]];
        this.playing = true;
    }
    stop() {
        this.playing = false;
        this.allOff();
    }
    allOff() {
        if (!this.x) return;
        for (const v of this.on) this.midi(0x80 | v.chan, v.key);
        this.on = [];
        for (let c = 0; c < 16; c++) this.midi(0xB0 | c, 123, 0);
    }
    // one note of track t, now (a click in the piano roll)
    async note(t, note) {
        await this.start();
        if (!await this.running()) return;
        if (!this.playing) this.apply();
        this.noteOn(t, note, S.PPQN);
    }
    // where the song is, in ticks, as heard (the device runs ahead of the speaker by what is queued)
    get heard() {
        const at = this.ms - Math.max(0, this.sent - this.played) / (RATE / 1000);
        let p = this.hist[0][1];
        for (const [ms, pos] of this.hist) { if (ms > at) break; p = pos; }
        return Math.max(0, p);
    }
}
