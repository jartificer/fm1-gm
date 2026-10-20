// SPDX-License-Identifier: GPL-3.0-only
// The song editor's two halves against the firmware: web/fm1song.js (the song format and MIDI export)
// and web/fm1link.js (the songs over SysEx) talking to firmware/src/app/link.c in the emulator,
// web/fm1gm.wasm (sh host/build_web.sh). No browser, no hardware. From the repo root:
//   node tests/web_song_test.mjs
import {readFileSync} from "node:fs";
import * as S from "../web/fm1song.js";
import {SongLink, MEMORY, LinkError} from "../web/fm1link.js";
import {Preview} from "../web/synth.js";

let failed = 0;
const ok = (cond, what) => {
    console.log(`${what.padEnd(64)} ${cond ? "ok" : "FAIL"}`);
    failed += !cond;
};
const eq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

// ------------------------------------------------------------- the device ---
const {instance} = await WebAssembly.instantiate(readFileSync(new URL("../web/fm1gm.wasm", import.meta.url)), {});
const x = instance.exports;
x.web_boot(1);
x.web_run(500);

// the USB port: what the host sends waits its turn (the device keeps one message); the device runs a
// millisecond at a time while the host waits for an answer
class WasmTransport {
    constructor() { this.inq = []; this.outq = []; }
    send(p) { this.inq.push(p.slice(1, -1)); return true; }
    pump() {
        if (this.inq.length && !x.web_sx_busy()) {
            const m = this.inq.shift();
            new Uint8Array(x.memory.buffer, x.web_sx_in_buf(), m.length).set(m);
            x.web_sx_in(m.length);
        }
        x.web_run(1);
        const n = x.web_sx_out_len();
        if (!n) return;
        const out = new Uint8Array(x.memory.buffer, x.web_sx_out(), n).slice();
        x.web_sx_out_clear();
        for (let i = 0; i < n;) { const j = out.indexOf(0xF7, i); this.outq.push(out.subarray(i, j + 1)); i = j + 1; }
    }
    async read(ms) {
        for (let t = 0; t < ms && !this.outq.length; t++) this.pump();
        return this.outq.shift() || null;
    }
    drain() { this.outq.length = 0; }
    close() {}
}
const link = new SongLink(new WasmTransport(), "wasm");

// ------------------------------------------------------------- songs ---
function demo(nPerTrack = 8, seed = 1) {
    const s = S.newSong();
    s.tempo = 97;
    s.bars = 8;
    s.quant = 2;
    let r = seed;
    const rnd = k => (r = (r * 1103515245 + 12345) >>> 0) % k;
    s.tracks.forEach((t, i) => {
        t.vel = 1 + rnd(127);
        t.transpose = rnd(49) - 24;
        t.pan = rnd(128);
        t.mute = i === 3;
        for (let k = 0; k < nPerTrack; k++)
            t.notes.push({tick: rnd(s.bars * S.BAR), note: 20 + rnd(90), len: [12, 24, 48, 300, 20000][rnd(5)]});
    });
    return s;
}
const norm = s => ({...s, tracks: s.tracks.map(t => ({...t, notes: [...t.notes].sort((a, b) => a.tick - b.tick || a.note - b.note)}))});

{
    const s = demo(), b = S.songBytes(s);
    ok(JSON.stringify(norm(S.parseSong(b))) === JSON.stringify(norm(s)), "format: a song survives bytes -> song -> bytes");
    ok(b[0] === S.FORMAT && b[1] === 97 && b[3] === 8, "format: the header is the firmware's");
    let threw = false;
    try { S.parseSong(b.subarray(0, b.length - 1)); } catch (e) { threw = e instanceof S.SongError; }
    ok(threw, "format: a cut-off song is refused");
    const bad = demo();
    bad.tempo = 400;
    ok(S.problems(bad).length === 1, "format: a tempo of 400 is a problem");
    const e = S.songBytes(S.newSong());
    ok(e.length === 5 + 16 * 8 + 16, "format: a new song is 149 bytes");
}

// --------------------------------------------------------------- MIDI ---
{
    const s = demo(4), m = S.songToMidi(s, {name: "test"});
    const str = (o, n) => String.fromCharCode(...m.subarray(o, o + n));
    ok(str(0, 4) === "MThd" && m[9] === 1 && m[13] === 48, "midi: type 1, 48 ticks a quarter");
    const ntrk = m[10] << 8 | m[11];
    ok(ntrk === 17, "midi: a tempo track and 16 tracks with notes");
    // walk the tracks: count the note-ons and check every track ends
    let o = 14, ons = 0, ends = 0;
    for (let k = 0; k < ntrk; k++) {
        const len = m[o + 4] << 24 | m[o + 5] << 16 | m[o + 6] << 8 | m[o + 7], end = o + 8 + len;
        let i = o + 8, run = 0;
        const vlq = () => { let v = 0, c; do { c = m[i++]; v = v << 7 | (c & 127); } while (c & 128); return v; };
        while (i < end) {
            vlq();
            let st = m[i];
            if (st === 0xFF) { const t = m[i + 1]; i += 2; const l = vlq(); i += l; if (t === 0x2F) ends++; continue; }
            if (st === 0xF0) { i++; i += vlq(); continue; }
            if (st & 0x80) { run = st; i++; } else st = run;
            if ((st & 0xF0) === 0x90 && m[i + 1]) ons++;
            i += (st & 0xF0) === 0xC0 || (st & 0xF0) === 0xD0 ? 1 : 2;
        }
        o = end;
    }
    const inSong = s.tracks.reduce((n, t) => n + t.notes.filter(v => v.tick < s.bars * S.BAR).length, 0);
    ok(o === m.length && ends === ntrk, "midi: every track is whole and ends");
    ok(ons === inSong, `midi: a note-on for each note in the song (${ons})`);
    const solo = demo(4);
    solo.tracks[5].solo = true;
    ok((S.songToMidi(solo, {skipMuted: true})[11]) === 2, "midi: with a solo and skipMuted, the soloed track only");
}

// ------------------------------------------------------- over SysEx ---
{
    const h = await link.hello();
    ok(h.proto === 1 && h.format === S.FORMAT && h.slots === 64, `hello: protocol 1, format 2, 64 slots (${h.version})`);
    ok(h.sectors === 71 && h.free === 71, "hello: 71 sectors, all free");

    const s = demo(), b = S.songBytes(s);
    await link.write(5, b);
    const l = await link.list();
    ok(l[5].used && l[5].notes === S.countNotes(s) && l[5].tempo === 97 && l[5].bars === 8, "write: slot 6 holds it");
    ok(l.filter(v => v.used).length === 1, "write: no other slot changed");
    ok(eq(await link.read(5), b), "read: slot 6 gives the same bytes back");
    ok(eq(await link.read(MEMORY), b), "read: the song in memory is it too");
    const h2 = await link.hello();
    ok(h2.slot === 5 && !h2.dirty, "write: slot 6 is the song in use, saved");

    // the most notes a song holds: many chunks
    const big = S.newSong();
    big.bars = 128;
    let n = 0;
    for (const t of big.tracks) for (let k = 0; k < 512; k++, n++) t.notes.push({tick: k * 48 + (n % 7), note: 30 + n % 80, len: 1 + (n * 37) % 3000});
    const bb = S.songBytes(big);
    let parts = 0;
    await link.write(MEMORY, bb, () => parts++);
    ok(parts === Math.ceil(bb.length / 512), `write: ${S.MAXEV} notes, ${bb.length} bytes, in ${parts} parts`);
    ok(eq(await link.read(MEMORY), bb), "read: the song in memory is the big one");
    const h3 = await link.hello();
    ok(h3.slot === 5 && h3.dirty, "write to memory: not saved, slot 6 still in use");

    let err = null;
    const notSong = bb.slice();
    notSong[0] = 9;
    try { await link.write(7, notSong); } catch (e) { err = e; }
    ok(err instanceof LinkError && err.status === 2, "write: not a song is refused");
    ok(eq(await link.read(MEMORY), bb), "write: and the song in memory is left as it was");
    ok(!(await link.list())[7].used, "write: and nothing saved");

    err = null;
    try { await link.read(9); } catch (e) { err = e; }
    ok(err instanceof LinkError && err.status === 1, "read: an empty slot says so");

    await link.remove(5);
    ok(!(await link.list())[5].used, "delete: slot 6 is empty");
    ok((await link.hello()).free === 71, "delete: every sector free again");
}

// ------------------------------------------------------------ preview ---
{
    // a device of its own, run without a speaker: the song's notes through the firmware's engine
    const {instance: pi} = await WebAssembly.instantiate(readFileSync(new URL("../web/fm1gm.wasm", import.meta.url)), {});
    const s = S.newSong();
    s.tempo = 120;
    s.bars = 1;
    s.tracks[0].notes.push({tick: 0, note: 60, len: 48}, {tick: 96, note: 64, len: 48});
    s.tracks[9].notes.push({tick: 48, note: 38, len: 12});
    const p = new Preview(() => s);
    p.x = pi.exports;
    p.x.web_boot(1);
    p.x.web_run(300);
    const run = ms => {                        // -> the RMS of what it played
        p.x.web_audio_take(1 << 20);
        let sum = 0, n = 0;
        for (let i = 0; i < ms; i++) {
            p.step();
            p.x.web_run(1);
            p.ms++;
            const k = p.x.web_audio_take(4096), a = new Float32Array(p.x.memory.buffer, p.x.web_audio_buf(), k * 2);
            for (const v of a) { sum += v * v; n++; }
        }
        return Math.sqrt(sum / Math.max(n, 1));
    };
    ok(run(200) < 1e-4, "preview: silent before Play");
    p.allOff();
    p.apply();
    p.pos = -1e-9;
    p.next = 0;
    p.playing = true;
    const notes = [];
    const gm = p.x.web_gm;
    p.midi = (st, d1, d2 = 0) => { if ((st & 0xF0) === 0x90) notes.push([p.ms, st & 15, d1]); gm(st, d1, d2); };
    const loud = run(2100);
    ok(loud > 1e-3, `preview: the song sounds (RMS ${loud.toFixed(4)})`);
    ok(JSON.stringify(notes.slice(0, 3).map(n => n.slice(1))) === "[[0,60],[9,38],[0,64]]", "preview: its notes in order, drums on channel 10");
    ok(notes.length === 4 && Math.abs(notes[3][0] - notes[0][0] - 2000) <= 1, "preview: it loops each bar (2 s at 120 BPM)");
    s.tracks[0].transpose = 12;
    p.changed();
    notes.length = 0;
    run(2000);
    ok(notes.some(n => n[2] === 72), "preview: an edit is heard as it plays (transpose)");
    s.tracks[0].mute = true;
    notes.length = 0;
    run(2000);
    ok(notes.length === 1 && notes.every(n => n[1] === 9), "preview: a muted track goes quiet");
    p.stop();
    run(1500);
    ok(run(300) < 1e-4, "preview: Stop silences it");
}

console.log(failed ? `${failed} FAILED` : "all ok");
process.exit(failed ? 1 : 0);
