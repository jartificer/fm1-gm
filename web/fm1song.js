// SPDX-License-Identifier: GPL-3.0-only
// FM1-GM songs in the browser: the firmware's own song format (seq.c's song_stream / song_parse, the
// bytes the FM-1 keeps in flash, and the .fm1song files the editor reads and writes), and a Standard
// MIDI File of a song.
//
//   format (2), tempo (2 bytes, low first), bars, quantize;
//   16 tracks: channel, program, volume, pan, reverb, velocity, mute | solo << 1, transpose (signed);
//   16 tracks: notes (a variable-length number), then each note: ticks since the note before (the
//   first: since 0), the note | 0x80 when its length is the note before's, else the note then its length.
// Variable-length numbers are MIDI's: 7 bits a byte, the high first, bit 7 set on all but the last.

export const FORMAT = 2;
export const NTRK = 16, PPQN = 48, BAR = PPQN * 4, MAXBARS = 128, MAXEV = 8192, NSONGS = 64;
export const DRUM_CH = 9, DRUM_LO = 35, DRUM_HI = 81;
export const QUANT_TICKS = [1, 48, 24, 16, 12, 8, 6];
export const QUANT_NAMES = ["Off", "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"];
const DEF_PROG = [0, 33, 48, 4, 24, 16, 61, 73, 81, 0, 89, 11, 52, 27, 65, 38];

export const GM_NAMES = [
    "Acoustic Grand", "Bright Piano", "Electric Grand", "Honky-tonk", "E.Piano 1", "E.Piano 2", "Harpsichord", "Clavi",
    "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Perc. Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
    "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar", "Muted Guitar", "Overdrive Gtr", "Distortion Gtr", "Gtr Harmonics",
    "Acoustic Bass", "Finger Bass", "Pick Bass", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
    "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato", "Harp", "Timpani",
    "Strings 1", "Strings 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Square Lead", "Saw Lead", "Calliope Lead", "Chiff Lead", "Charang Lead", "Voice Lead", "Fifths Lead", "Bass + Lead",
    "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad", "Bowed Pad", "Metallic Pad", "Halo Pad", "Sweep Pad",
    "Rain", "Soundtrack", "Crystal", "Atmosphere", "Brightness", "Goblins", "Echoes", "Sci-fi",
    "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
    "Gtr Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone", "Helicopter", "Applause", "Gunshot",
];
export const GM_FAMILIES = ["Piano", "Mallets", "Organ", "Guitar", "Bass", "Strings", "Ensemble", "Brass",
    "Reed", "Pipe", "Lead", "Pad", "Synth FX", "Ethnic", "Perc", "SFX"];
export const DRUM_NAMES = [
    "Kick 2", "Kick 1", "Side Stick", "Snare 1", "Hand Clap", "Snare 2", "Low Floor Tom", "Closed Hat",
    "High Floor Tom", "Pedal Hat", "Low Tom", "Open Hat", "Low-Mid Tom", "Hi-Mid Tom", "Crash 1", "High Tom",
    "Ride 1", "China", "Ride Bell", "Tambourine", "Splash", "Cowbell", "Crash 2", "Vibraslap", "Ride 2",
    "Hi Bongo", "Low Bongo", "Mute Conga", "Open Conga", "Low Conga", "Hi Timbale", "Low Timbale",
    "Hi Agogo", "Low Agogo", "Cabasa", "Maracas", "Short Whistle", "Long Whistle", "Short Guiro",
    "Long Guiro", "Claves", "Hi Woodblock", "Low Woodblock", "Mute Cuica", "Open Cuica", "Mute Triangle",
    "Open Triangle",
];
const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
export const noteName = n => NOTE_NAMES[n % 12] + (Math.floor(n / 12) - 1);   // 60 = C4
export const drumName = n => n >= DRUM_LO && n <= DRUM_HI ? DRUM_NAMES[n - DRUM_LO] : null;

export class SongError extends Error {}

export function newTrack(i) {
    return {chan: i, prog: DEF_PROG[i], vol: 100, pan: 64, rev: 40, vel: 100, mute: false, solo: false, transpose: 0, notes: []};
}
// a new song, as the FM-1's (song_defaults)
export function newSong() {
    return {tempo: 120, bars: 4, quant: 4, tracks: Array.from({length: NTRK}, (_, i) => newTrack(i))};
}

export const countNotes = song => song.tracks.reduce((n, t) => n + t.notes.length, 0);
export const songTicks = song => song.bars * BAR;
const byTick = (a, b) => a.tick - b.tick || a.note - b.note;

// the firmware's checks (song_parse), as a list of what is wrong; [] = the FM-1 takes it
export function problems(song) {
    const p = [], int = (v, lo, hi) => Number.isInteger(v) && v >= lo && v <= hi;
    if (!int(song.tempo, 30, 300)) p.push("tempo must be 30..300");
    if (!int(song.bars, 1, MAXBARS)) p.push(`bars must be 1..${MAXBARS}`);
    if (!int(song.quant, 0, QUANT_TICKS.length - 1)) p.push("bad quantize");
    if (song.tracks.length !== NTRK) p.push(`a song has ${NTRK} tracks`);
    song.tracks.forEach((t, i) => {
        const ok = int(t.chan, 0, 15) && int(t.prog, 0, 127) && int(t.vol, 0, 127) && int(t.pan, 0, 127) &&
            int(t.rev, 0, 127) && int(t.vel, 1, 127) && int(t.transpose, -24, 24);
        if (!ok) p.push(`track ${i + 1}: a setting is out of range`);
        for (const n of t.notes)
            if (!int(n.tick, 0, MAXBARS * BAR - 1) || !int(n.note, 0, 127) || !int(n.len, 1, 0xFFFF)) {
                p.push(`track ${i + 1}: a note is out of range`);
                break;
            }
    });
    if (countNotes(song) > MAXEV) p.push(`${countNotes(song)} notes: the FM-1 holds ${MAXEV} a song`);
    return p;
}

// ------------------------------------------------------------ the stream ---
export function songBytes(song) {
    const bad = problems(song);
    if (bad.length) throw new SongError(bad[0]);
    const out = [];
    const vlq = v => {
        const b = [];
        do { b.push(v & 127); v = Math.floor(v / 128); } while (v);
        for (let k = b.length - 1; k >= 0; k--) out.push(b[k] | (k ? 128 : 0));
    };
    out.push(FORMAT, song.tempo & 255, song.tempo >> 8, song.bars, song.quant);
    for (const t of song.tracks)
        out.push(t.chan, t.prog, t.vol, t.pan, t.rev, t.vel, (t.mute ? 1 : 0) | (t.solo ? 2 : 0), t.transpose & 255);
    for (const t of song.tracks) {
        const notes = [...t.notes].sort(byTick);
        vlq(notes.length);
        let prev = 0, plen = 0;
        for (const n of notes) {
            vlq(n.tick - prev);
            if (n.len === plen) out.push(n.note | 128);
            else { out.push(n.note); vlq(n.len); }
            prev = n.tick;
            plen = n.len;
        }
    }
    return Uint8Array.from(out);
}

export function parseSong(bytes) {
    let i = 0;
    const fail = why => { throw new SongError(why); };
    const get8 = () => i < bytes.length ? bytes[i++] : fail("the file ends too soon");
    const vlq = () => {
        let v = 0;
        for (let k = 0; k < 3; k++) {            // nothing in a song needs more than 21 bits
            const c = get8();
            v = v << 7 | (c & 127);
            if (!(c & 128)) return v;
        }
        return fail("a number is too long");
    };
    if (!bytes.length) fail("the file is empty");
    if (bytes[0] !== FORMAT) fail(bytes[0] < FORMAT ? "an older song format" : "not an FM1-GM song (or a newer format)");
    i = 1;
    const song = {tempo: get8() | get8() << 8, bars: get8(), quant: get8(), tracks: []};
    for (let t = 0; t < NTRK; t++) {
        const [chan, prog, vol, pan, rev, vel, f, tr] = [get8(), get8(), get8(), get8(), get8(), get8(), get8(), get8()];
        if (f > 3) fail(`track ${t + 1}: bad flags`);
        song.tracks.push({chan, prog, vol, pan, rev, vel, mute: !!(f & 1), solo: !!(f & 2), transpose: tr << 24 >> 24, notes: []});
    }
    let total = 0;
    for (const t of song.tracks) {
        const ne = vlq();
        if ((total += ne) > MAXEV) fail(`more than ${MAXEV} notes`);
        let tick = 0, plen = 0;
        for (let j = 0; j < ne; j++) {
            tick += vlq();
            const nb = get8(), len = nb & 128 ? plen : vlq();
            if (tick >= MAXBARS * BAR || len < 1 || len > 0xFFFF) fail("a note is out of range");
            t.notes.push({tick, note: nb & 127, len});
            plen = len;
        }
    }
    if (i !== bytes.length) fail("bytes left over after the song");
    const bad = problems(song);
    if (bad.length) fail(bad[0]);
    return song;
}

// --------------------------------------------------------------- MIDI ---
// A Standard MIDI File (type 1, 48 ticks a quarter note) of one pass through the song, as the FM-1
// plays it: a tempo and GM-on track, then a track for each track with notes, on its channel, with its
// program, volume, pan and reverb (from the lowest track on a channel, as the FM-1 sets them), its
// transpose applied, and every note at the track's velocity. Notes past the song's end are left out
// (the FM-1 does not play them); muted tracks are left out when skipMuted (and only soloed ones, when
// any is).
export function songToMidi(song, {name = "FM1-GM song", skipMuted = false} = {}) {
    const end = songTicks(song), solo = song.tracks.some(t => t.solo);
    const chunk = (id, data) => [...[...id].map(c => c.charCodeAt(0)), ...u32(data.length), ...data];
    const u32 = v => [v >>> 24 & 255, v >> 16 & 255, v >> 8 & 255, v & 255];
    const vlq = v => {
        const b = [v & 127];
        while ((v = Math.floor(v / 128))) b.unshift((v & 127) | 128);
        return b;
    };
    const text = (type, s) => { const b = [...new TextEncoder().encode(s)]; return [0xFF, type, ...vlq(b.length), ...b]; };
    const track = evs => {                      // [tick, order, bytes]: in time, offs before ons at a tick
        evs.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
        const d = [];
        let at = 0;
        for (const [t, , m] of evs) { d.push(...vlq(t - at), ...m); at = t; }
        d.push(...vlq(Math.max(0, end - at)), 0xFF, 0x2F, 0);
        return chunk("MTrk", d);
    };
    const us = Math.round(60e6 / song.tempo);
    const tracks = [track([
        [0, 0, text(3, name)],
        [0, 1, [0xF0, 5, 0x7E, 0x7F, 0x09, 0x01, 0xF7]],             // GM System On
        [0, 2, [0xFF, 0x51, 3, us >> 16 & 255, us >> 8 & 255, us & 255]],
        [0, 3, [0xFF, 0x58, 4, 4, 2, 24, 8]],                          // 4/4
    ])];
    const owner = new Map();                    // channel -> the lowest track on it
    song.tracks.forEach((t, i) => { if (!owner.has(t.chan)) owner.set(t.chan, i); });
    song.tracks.forEach((t, i) => {
        const plays = solo ? t.solo : !t.mute;
        const notes = t.notes.filter(n => n.tick < end);
        if (!notes.length || (skipMuted && !plays)) return;
        const c = t.chan, evs = [[0, 0, text(3, `${i + 1} ${c === DRUM_CH ? "Drums" : GM_NAMES[t.prog]}`)]];
        if (owner.get(c) === i) {
            if (c !== DRUM_CH) evs.push([0, 1, [0xC0 | c, t.prog]]);
            evs.push([0, 2, [0xB0 | c, 7, t.vol]], [0, 3, [0xB0 | c, 10, t.pan]], [0, 4, [0xB0 | c, 91, t.rev]]);
        }
        for (const n of notes) {
            const k = Math.max(0, Math.min(127, n.note + (c === DRUM_CH ? 0 : t.transpose)));
            evs.push([n.tick, 10, [0x90 | c, k, t.vel]], [n.tick + n.len, 5, [0x80 | c, k, 0]]);
        }
        tracks.push(track(evs));
    });
    return Uint8Array.from([...chunk("MThd", [0, 1, 0, tracks.length, 0, PPQN]), ...tracks.flat()]);
}
