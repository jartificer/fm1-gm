// SPDX-License-Identifier: GPL-3.0-only
// The FM1-GM songs over SysEx (firmware/src/app/link.c): list, read, write and delete the songs on an
// FM-1 running FM1-GM, over Web MIDI, or on the emulator (emu.js) in another tab of this site.
//
//   host -> FM-1:  F0 7D 46 47 cmd args .. F7        FM-1 -> host:  F0 7D 46 47 cmd|40 status args .. F7
// Numbers go 7 bits a byte, the low bits first; a song's bytes are packed 8 bits into 7 (fm1ota.js's
// pack7). The host waits for each answer before it sends again: the FM-1 keeps one message at a time.
import {pack7, unpack7, PORT_RE, pairOutput, webMidiBackend, handshake} from "./fm1ota.js";

export const PROTO = 1, MEMORY = 64, CHUNK = 512;
const HEAD = [0x7D, 0x46, 0x47];
const CMD = {hello: 1, list: 2, read: 3, write: 4, data: 5, del: 6, done: 7};
export const STATUS = ["done", "bad request", "not a song the FM-1 takes", "no room left on the FM-1",
    "a flash error", "busy (the sequencer did not stop)"];

export class LinkError extends Error {
    constructor(msg, status = -1) { super(msg); this.status = status; }
}

const num = (d, i, k) => { let v = 0; for (let j = k - 1; j >= 0; j--) v = v * 128 + (d[i + j] & 127); return v; };
const put = (v, k) => Array.from({length: k}, (_, j) => Math.floor(v / 128 ** j) & 127);
const sleep = ms => new Promise(r => setTimeout(r, ms));

// a transport: send(F0 .. F7), read(ms) -> the next SysEx received (F0 .. F7) or null, drain(), close()
export class SongLink {
    constructor(t, name) {
        this.t = t;
        this.name = name;
        this.box = [];                         // answers that came while waiting for another
    }
    close() { this.t.close(); }

    send(cmd, args = []) {
        if (!this.t.send(Uint8Array.from([0xF0, ...HEAD, cmd, ...args, 0xF7])))
            throw new LinkError("the FM-1 is gone (unplugged?)");
    }
    // the next answer to cmd: {st, d} (d: what follows the status, without F7), or null in ms
    async wait(cmd, ms) {
        const end = performance.now() + ms;
        for (;;) {
            const i = this.box.findIndex(m => m.cmd === cmd);
            if (i >= 0) return this.box.splice(i, 1)[0];
            const left = end - performance.now();
            if (left <= 0) return null;
            const p = await this.t.read(left);
            if (!p) return null;
            if (p.length < 7 || p[1] !== HEAD[0] || p[2] !== HEAD[1] || p[3] !== HEAD[2] || p.at(-1) !== 0xF7) continue;
            this.box.push({cmd: p[4] & 0x3F, st: p[5], d: p.subarray(6, -1)});
            if (this.box.length > 64) this.box.shift();
        }
    }
    async ask(cmd, args = [], {ms = 1500, tries = 3, accept = () => true} = {}) {
        for (let k = 0; k < tries; k++) {
            this.send(cmd, args);
            for (;;) {
                const r = await this.wait(cmd, ms);
                if (!r) break;
                if (accept(r)) return r;
            }
        }
        throw new LinkError("the FM-1 does not answer");
    }

    async hello(ms = 800, tries = 2) {
        this.box.length = 0;
        const {d} = await this.ask(CMD.hello, [], {ms, tries});
        return {
            proto: d[0], format: d[1], slots: d[2], sectors: num(d, 3, 2), free: num(d, 5, 2), slot: d[7],
            dirty: !!d[8], version: String.fromCharCode(...d.subarray(9)),
        };
    }

    async list() {                             // every slot: {used, bars, tempo, notes, bytes, sectors}
        const out = [];
        while (out.length < MEMORY) {
            const first = out.length, {st, d} = await this.ask(CMD.list, [first], {accept: r => r.d[0] === first});
            if (st) throw new LinkError(`list: ${STATUS[st] || st}`, st);
            for (let k = 0, i = 2; k < d[1]; k++, i += 11)
                out.push({used: !!d[i], bars: num(d, i + 1, 2), tempo: num(d, i + 3, 2), notes: num(d, i + 5, 2),
                    bytes: num(d, i + 7, 3), sectors: d[i + 10]});
        }
        return out;
    }

    // a slot's song (MEMORY: the song in memory), its bytes as in flash
    async read(slot, progress = () => {}) {
        let total = null, off = 0, buf = null;
        do {
            const o = off, r = await this.ask(CMD.read, [slot, ...put(o, 3)],
                {accept: r => r.d[0] === slot && num(r.d, 1, 3) === o});
            if (r.st) throw new LinkError(r.st === 1 && !o ? "that slot is empty" : `read: ${STATUS[r.st] || r.st}`, r.st);
            if (total === null) { total = num(r.d, 4, 3); buf = new Uint8Array(total); }
            const part = unpack7(r.d.subarray(7)).subarray(0, Math.min(CHUNK, total - off));
            if (!part.length && off < total) throw new LinkError("read: no data");
            buf.set(part, off);
            off += part.length;
            progress(off / total);
        } while (off < total);
        return buf;
    }

    // a song's bytes to a slot (saved there, and the FM-1's song in memory; MEMORY: only that)
    async write(slot, bytes, progress = () => {}) {
        this.box = this.box.filter(m => m.cmd !== CMD.done);
        const w = await this.ask(CMD.write, [slot, ...put(bytes.length, 3)]);
        if (w.st) throw new LinkError(`write: ${STATUS[w.st] || w.st}`, w.st);
        for (let off = 0; off < bytes.length;) {
            const o = off, n = Math.min(CHUNK, bytes.length - off);
            const r = await this.ask(CMD.data, [...put(o, 3), ...pack7(bytes.subarray(o, o + n))],
                {accept: r => r.st || num(r.d, 0, 3) >= o + n || num(r.d, 0, 3) < o});
            if (r.st) throw new LinkError(`write: ${STATUS[r.st] || r.st}`, r.st);
            off = num(r.d, 0, 3);
            progress(off / bytes.length);
        }
        // the FM-1 stops, loads (and saves) it, then answers; keep asking HELLO meanwhile (it also keeps
        // the emulator running in a background tab)
        const end = performance.now() + 10000;
        while (performance.now() < end) {
            this.send(CMD.hello);
            const r = await this.wait(CMD.done, 700);
            if (r) {
                if (r.st) throw new LinkError(STATUS[r.st] || `error ${r.st}`, r.st);
                return;
            }
        }
        throw new LinkError("the FM-1 did not say whether it took the song");
    }

    async remove(slot) {
        const r = await this.ask(CMD.del, [slot], {accept: r => r.d[0] === slot});
        if (r.st) throw new LinkError(`delete: ${STATUS[r.st] || r.st}`, r.st);
    }
}

// ------------------------------------------------------------- finding it ---
// Web MIDI: the FM-1's ports (by name, as the installer: other gear is never probed), each asked HELLO.
// -> {link, info} or {found: what answered instead, or null}
export async function findDevice(access) {
    const be = webMidiBackend(access), outs = be.outputNames();
    let other = null;
    for (const name of be.inputNames().filter(n => PORT_RE.test(n))) {
        const out = pairOutput(name, outs);
        if (!out) continue;
        let t;
        try { t = await be.open(name, out); } catch { continue; }
        const link = new SongLink(t, name);
        try {
            const info = await link.hello();
            return {link, info};
        } catch {
            const id = await handshake(t, 1).catch(() => null);
            other = id ? id.text : other || name;
            t.close();
        }
    }
    return {found: other};
}

// the emulator in another tab of this site (app.js answers on this channel)
export const CHANNEL = "fm1gm-link";
export class ChannelTransport {
    constructor() {
        this.bc = new BroadcastChannel(CHANNEL);
        this.q = [];
        this.waiters = [];
        this.bc.onmessage = e => {
            if (!(e.data && e.data.from === "emu" && e.data.sx instanceof Uint8Array)) return;
            const w = this.waiters.shift();
            if (w) w(e.data.sx); else this.q.push(e.data.sx);
        };
    }
    send(p) { this.bc.postMessage({from: "editor", sx: p}); return true; }
    read(ms) {
        if (this.q.length) return Promise.resolve(this.q.shift());
        return new Promise(res => {
            let done = false;
            const w = p => { if (!done) { done = true; clearTimeout(t); res(p); } };
            const t = setTimeout(() => {
                done = true;
                this.waiters.splice(this.waiters.indexOf(w), 1);
                res(null);
            }, Math.max(ms, 0));
            this.waiters.push(w);
        });
    }
    drain() { this.q.length = 0; }
    close() { this.bc.close(); }
}

export async function findEmulator() {
    const link = new SongLink(new ChannelTransport(), "the emulator");
    try {
        return {link, info: await link.hello(600, 2)};
    } catch {
        link.close();
        return null;
    }
}

export {sleep};
