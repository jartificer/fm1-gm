// SPDX-License-Identifier: GPL-3.0-only
// The song editor: one FM1-GM song (fm1song.js) with its 16 tracks and a piano roll, kept in this
// browser between visits; .fm1song files in and out, MIDI out; and the FM-1's 64 slots over USB (or the
// emulator's, in another tab) through fm1link.js.
import * as S from "./fm1song.js";
import {MEMORY, findDevice, findEmulator} from "./fm1link.js";

const $ = id => document.getElementById(id);
const esc = s => String(s).replace(/[&<>"]/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;"}[c]));
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

// ------------------------------------------------------------ the song ---
let song = S.newSong(), name = "untitled", sel = 0;   // sel: the track in the roll
let picked = new Set();                                // the selected notes (objects of song.tracks[sel])
const undos = [], redos = [];
const DRAFT = "fm1gm-editor-draft";

const snapshot = () => JSON.stringify({song, name});
function restore(s) {
    const o = JSON.parse(s);
    song = o.song;
    name = o.name;
    picked = new Set();
}
// every change: call before making it
function edit() {
    undos.push(snapshot());
    if (undos.length > 200) undos.shift();
    redos.length = 0;
}
function changed() {
    try { localStorage.setItem(DRAFT, snapshot()); } catch { /* private window: no draft */ }
    render();
}
function load(s, n) {
    edit();
    song = s;
    name = n;
    picked = new Set();
    sel = 0;
    changed();
    centerRoll();
}
try {
    const d = localStorage.getItem(DRAFT);
    if (d) restore(d);
    if (!Array.isArray(song?.tracks) || song.tracks.length !== S.NTRK) throw new Error("not a song");
} catch { song = S.newSong(); name = "untitled"; }

const status = (msg, kind = "") => { const e = $("status"); e.textContent = msg; e.className = "msg " + kind; };

// ------------------------------------------------------- song settings ---
for (const [i, q] of S.QUANT_NAMES.entries()) $("quant").add(new Option(q, i));
const SNAPS = [["1/4", 48], ["1/8", 24], ["1/8T", 16], ["1/16", 12], ["1/16T", 8], ["1/32", 6], ["Off", 1]];
for (const [n, t] of SNAPS) $("snap").add(new Option(n, t));
$("snap").value = 12;
const LENS = [["Snap", 0], ["1/32", 6], ["1/16", 12], ["1/8", 24], ["1/4", 48], ["1/2", 96], ["1 bar", 192], ["Last used", -1]];
for (const [n, t] of LENS) $("len").add(new Option(n, t));
$("len").value = -1;
let lastLen = 12;

function numField(id, get, set, lo, hi) {
    $(id).addEventListener("change", e => {
        const v = clamp(Math.round(+e.target.value || 0), lo, hi);
        if (v === get()) { e.target.value = v; return; }
        edit();
        set(v);
        changed();
    });
}
numField("tempo", () => song.tempo, v => song.tempo = v, 30, 300);
numField("bars", () => song.bars, v => song.bars = v, 1, S.MAXBARS);
$("quant").addEventListener("change", e => { edit(); song.quant = +e.target.value; changed(); });
$("name").addEventListener("change", e => { edit(); name = e.target.value.trim() || "untitled"; changed(); });

// ------------------------------------------------------------- tracks ---
const progOptions = (() => {
    let h = "";
    for (let f = 0; f < 16; f++) {
        h += `<optgroup label="${S.GM_FAMILIES[f]}">`;
        for (let p = f * 8; p < f * 8 + 8; p++) h += `<option value="${p}">${p + 1} ${esc(S.GM_NAMES[p])}</option>`;
        h += "</optgroup>";
    }
    return h;
})();
const chanOptions = Array.from({length: 16}, (_, c) => `<option value="${c}">${c + 1}${c === S.DRUM_CH ? " drums" : ""}</option>`).join("");
const tbody = $("tracks").tBodies[0];
const FIELDS = [["vol", 0, 127], ["pan", 0, 127], ["rev", 0, 127], ["vel", 1, 127], ["transpose", -24, 24]];
for (let i = 0; i < S.NTRK; i++) {
    const tr = tbody.insertRow();
    tr.dataset.t = i;
    tr.innerHTML = `<td class="n">${i + 1}</td><td><select data-k="chan" aria-label="Track ${i + 1} channel">${chanOptions}</select></td>` +
        `<td><select data-k="prog" aria-label="Track ${i + 1} program">${progOptions}</select></td>` +
        FIELDS.map(([k, lo, hi]) => `<td><input type="number" data-k="${k}" min="${lo}" max="${hi}" aria-label="Track ${i + 1} ${k}"></td>`).join("") +
        `<td style="white-space:nowrap"><button type="button" class="tog m" data-k="mute" aria-label="Mute track ${i + 1}">M</button> ` +
        `<button type="button" class="tog s" data-k="solo" aria-label="Solo track ${i + 1}">S</button></td><td class="cnt"></td>`;
}
tbody.addEventListener("click", e => {
    const tr = e.target.closest("tr"), t = +tr.dataset.t, k = e.target.dataset.k;
    if (k === "mute" || k === "solo") {
        edit();
        song.tracks[t][k] = !song.tracks[t][k];
        changed();
    }
    if (t !== sel) { sel = t; picked = new Set(); render(); centerRoll(); }
});
tbody.addEventListener("change", e => {
    const t = +e.target.closest("tr").dataset.t, k = e.target.dataset.k, f = FIELDS.find(f => f[0] === k);
    let v = Math.round(+e.target.value || 0);
    if (f) v = clamp(v, f[1], f[2]);
    edit();
    song.tracks[t][k] = v;
    changed();
});

// -------------------------------------------------------------- render ---
function render() {
    $("name").value = name;
    $("tempo").value = song.tempo;
    $("bars").value = song.bars;
    $("quant").value = song.quant;
    const n = S.countNotes(song), bad = S.problems(song);
    let bytes = "";
    try { bytes = `<span><b>${S.songBytes(song).length}</b> bytes</span>`; } catch { /* the problem says why */ }
    $("facts").innerHTML = `<span><b>${n}</b> / ${S.MAXEV} notes</span>${bytes}` +
        (bad.length ? `<span class="bad">${esc(bad[0])}</span>` : "");
    [...tbody.rows].forEach((tr, i) => {
        const t = song.tracks[i];
        tr.classList.toggle("sel", i === sel);
        for (const el of tr.querySelectorAll("[data-k]")) {
            const k = el.dataset.k;
            if (el.tagName === "BUTTON") el.classList.toggle("on", !!t[k]);
            else if (document.activeElement !== el) el.value = t[k];
        }
        tr.querySelector("[data-k=prog]").disabled = t.chan === S.DRUM_CH;
        tr.querySelector("[data-k=transpose]").disabled = t.chan === S.DRUM_CH;
        tr.cells[tr.cells.length - 1].textContent = t.notes.length;
    });
    const t = song.tracks[sel];
    $("roll-title").textContent = `Track ${sel + 1}: ${t.chan === S.DRUM_CH ? "Drums" : S.GM_NAMES[t.prog]} (channel ${t.chan + 1})`;
    $("undo").disabled = !undos.length;
    $("redo").disabled = !redos.length;
    $("save").disabled = $("midi").disabled = !!bad.length;
    devButtons();
    sizeRoll();
    draw();
}

// ------------------------------------------------------------ the roll ---
const roll = $("roll"), cv = $("cv"), ctx = cv.getContext("2d"), spacer = $("spacer");
const KW = 64, RULER = 22, RH = 12;            // keyboard width, ruler height, row height (px)
let ppt = 160 / S.BAR;                           // pixels a tick
const contentW = () => KW + Math.max(song.bars, ...song.tracks[sel].notes.map(n => Math.ceil((n.tick + n.len) / S.BAR))) * S.BAR * ppt + 40;
const contentH = () => RULER + 128 * RH;
const css = v => getComputedStyle(document.documentElement).getPropertyValue(v).trim();

function sizeRoll() {
    const w = roll.clientWidth, h = roll.clientHeight, dpr = window.devicePixelRatio || 1;
    if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) {
        cv.width = Math.round(w * dpr);
        cv.height = Math.round(h * dpr);
        cv.style.width = w + "px";
        cv.style.height = h + "px";
    }
    spacer.style.width = contentW() + "px";
    spacer.style.height = Math.max(0, contentH() - h) + "px";
}
const tickX = tick => KW + tick * ppt - roll.scrollLeft;
const noteY = note => RULER + (127 - note) * RH - roll.scrollTop;

function draw() {
    const dpr = window.devicePixelRatio || 1, w = cv.width / dpr, h = cv.height / dpr;
    const c = ctx, t = song.tracks[sel], drums = t.chan === S.DRUM_CH, end = S.songTicks(song);
    const col = {bg: css("--roll-bg"), alt: css("--roll-alt"), line: css("--roll-line"), bar: css("--roll-bar"),
        note: css("--note"), sel: css("--note-sel"), out: css("--note-out"), kw: css("--key-w"), kb: css("--key-b"),
        ink: css("--ink"), ink2: css("--ink-2"), sheet: css("--sheet")};
    c.setTransform(dpr, 0, 0, dpr, 0, 0);
    c.fillStyle = col.bg;
    c.fillRect(0, 0, w, h);
    const lo = clamp(Math.floor(127 - (roll.scrollTop + h - RULER) / RH), 0, 127);
    const hi = clamp(Math.ceil(127 - roll.scrollTop / RH), 0, 127);
    // rows: black keys darker; for the drums, the kit's notes lighter
    for (let n = lo; n <= hi; n++) {
        const y = noteY(n), black = [1, 3, 6, 8, 10].includes(n % 12);
        if (drums ? !S.drumName(n) : black) { c.fillStyle = col.alt; c.fillRect(KW, y, w, RH); }
        if (n % 12 === 0 && !drums) { c.fillStyle = col.line; c.fillRect(KW, y + RH - 1, w, 1); }
    }
    // the grid: steps, beats, bars; past the end shaded
    const step = +$("snap").value, t0 = Math.max(0, Math.floor((roll.scrollLeft) / ppt)), t1 = (roll.scrollLeft + w) / ppt;
    const every = step * ppt >= 6 ? step : S.PPQN;
    for (let k = Math.floor(t0 / every) * every; k <= t1; k += every) {
        const x = Math.round(tickX(k)) + .5;
        c.fillStyle = k % S.BAR === 0 ? col.bar : k % S.PPQN === 0 ? col.line : col.alt;
        c.fillRect(x, RULER, 1, h);
    }
    if (tickX(end) < w) { c.fillStyle = col.ink + "22"; c.fillRect(Math.max(KW, tickX(end)), RULER, w, h); }
    // the notes
    for (const n of t.notes) {
        const x = tickX(n.tick), y = noteY(n.note), nw = Math.max(3, n.len * ppt - 1);
        if (x > w || x + nw < KW || y > h || y + RH < RULER) continue;
        c.fillStyle = picked.has(n) ? col.sel : n.tick >= end ? col.out : col.note;
        c.fillRect(x + 1, y + 1, nw - 1, RH - 2);
        c.fillStyle = "#0004";
        c.fillRect(x + nw - 2, y + 1, 2, RH - 2);
    }
    // the ruler
    c.fillStyle = col.sheet;
    c.fillRect(0, 0, w, RULER);
    c.fillStyle = col.line;
    c.fillRect(0, RULER - 1, w, 1);
    c.font = `500 11px ${css("--f-mono")}`;
    c.textBaseline = "middle";
    const barPx = S.BAR * ppt, bstep = barPx < 30 ? Math.ceil(30 / barPx) : 1;
    for (let b = Math.floor(t0 / S.BAR); b * S.BAR <= t1; b++) {
        if (b % bstep) continue;
        const x = tickX(b * S.BAR);
        c.fillStyle = b < song.bars ? col.ink : col.ink2;
        c.fillText(String(b + 1), x + 4, RULER / 2);
        c.fillRect(x, RULER - 7, 1, 6);
    }
    // the keyboard
    c.fillStyle = col.sheet;
    c.fillRect(0, RULER, KW, h);
    c.font = `500 10px ${css("--f-mono")}`;
    for (let n = lo; n <= hi; n++) {
        const y = noteY(n), black = [1, 3, 6, 8, 10].includes(n % 12);
        if (y + RH < RULER) continue;
        c.fillStyle = col.kw;
        c.fillRect(0, y, KW - 1, RH - (n % 12 === 4 || n % 12 === 11 ? 1 : 0));
        if (black) { c.fillStyle = col.kb; c.fillRect(0, y + 1, KW * .62, RH - 2); }
        const label = drums ? S.drumName(n) : n % 12 === 0 ? S.noteName(n) : "";
        if (label) {
            c.fillStyle = black ? col.kw : col.ink;
            c.fillText(drums ? label.slice(0, 10) : label, 3, y + RH / 2);
        }
    }
    c.fillStyle = col.sheet;
    c.fillRect(0, 0, KW, RULER);
}
roll.addEventListener("scroll", draw);
new ResizeObserver(() => { sizeRoll(); draw(); }).observe(roll);
matchMedia("(prefers-color-scheme: dark)").addEventListener("change", draw);

function centerRoll() {
    const t = song.tracks[sel], ns = t.notes.map(n => n.note);
    const mid = ns.length ? (Math.min(...ns) + Math.max(...ns)) / 2 : t.chan === S.DRUM_CH ? 44 : 60;
    sizeRoll();
    roll.scrollTop = RULER + (127 - mid) * RH - roll.clientHeight / 2;
    draw();
}
function zoom(f) {
    const mid = (roll.scrollLeft + (roll.clientWidth - KW) / 2) / ppt;
    ppt = clamp(ppt * f, 8 / S.BAR, 1600 / S.BAR);
    sizeRoll();
    roll.scrollLeft = mid * ppt - (roll.clientWidth - KW) / 2;
    draw();
}
$("zoomin").addEventListener("click", () => zoom(1.5));
$("zoomout").addEventListener("click", () => zoom(1 / 1.5));

// pointer: add, move, resize, delete
const at = e => {
    const r = cv.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top;
    return {x, y, tick: (x - KW + roll.scrollLeft) / ppt, note: 127 - Math.floor((y - RULER + roll.scrollTop) / RH)};
};
const hit = p => {
    const ns = song.tracks[sel].notes;
    for (let i = ns.length - 1; i >= 0; i--) {
        const n = ns[i];
        if (n.note === p.note && p.tick >= n.tick && p.tick < n.tick + Math.max(n.len, 3 / ppt)) return n;
    }
    return null;
};
const snapT = t => { const s = +$("snap").value; return Math.max(0, Math.round(t / s) * s); };
const newLen = () => { const l = +$("len").value; return l > 0 ? l : l === 0 ? +$("snap").value : lastLen; };
let drag = null;

cv.addEventListener("pointerdown", e => {
    const p = at(e);
    if (p.x < KW || p.y < RULER || p.note < 0 || p.note > 127) return;
    roll.focus({preventScroll: true});
    if (e.button === 2) return;
    let n = hit(p);
    const t = song.tracks[sel];
    if (n) {
        if (e.shiftKey) { picked.has(n) ? picked.delete(n) : picked.add(n); draw(); return; }
        if (!picked.has(n)) picked = new Set([n]);
        const edge = tickX(n.tick + n.len) - p.x < 7;
        drag = {mode: edge ? "len" : "move", p0: p, start: [...picked].map(m => ({m, tick: m.tick, note: m.note, len: m.len})), undo: snapshot(), moved: false};
    } else {
        if (S.countNotes(song) >= S.MAXEV) { status(`The FM-1 holds ${S.MAXEV} notes a song.`, "bad"); return; }
        if (e.shiftKey) { picked = new Set(); draw(); return; }
        edit();
        const tick = Math.floor(p.tick / +$("snap").value) * +$("snap").value;
        n = {tick: clamp(tick, 0, S.MAXBARS * S.BAR - 1), note: p.note, len: newLen()};
        t.notes.push(n);
        picked = new Set([n]);
        drag = {mode: "len", p0: p, start: [{m: n, tick: n.tick, note: n.note, len: n.len}], undo: null, moved: true, fresh: true};
    }
    cv.setPointerCapture(e.pointerId);
    draw();
});
cv.addEventListener("pointermove", e => {
    const p = at(e);
    if (!drag) {
        const n = p.x >= KW && p.y >= RULER ? hit(p) : null;
        cv.style.cursor = n ? (tickX(n.tick + n.len) - p.x < 7 ? "ew-resize" : "grab") : "default";
        return;
    }
    const dt = p.tick - drag.p0.tick, dn = p.note - drag.p0.note;
    if (!drag.moved && Math.abs(dt * ppt) < 3 && !dn) return;
    if (!drag.moved) { if (drag.undo) { undos.push(drag.undo); redos.length = 0; } drag.moved = true; }
    for (const s of drag.start) {
        if (drag.mode === "move") {
            s.m.tick = clamp(snapT(s.tick + dt), 0, S.MAXBARS * S.BAR - 1);
            s.m.note = clamp(s.note + dn, 0, 127);
        } else {
            const endT = Math.max(snapT(s.tick + s.len + dt), s.tick + 1);
            s.m.len = clamp(endT - s.tick, 1, 0xFFFF);
        }
    }
    draw();
});
const finish = () => {
    if (!drag) return;
    if (drag.moved) {
        if (drag.mode === "len" && drag.start.length === 1) lastLen = drag.start[0].m.len;
        dedupe();
        changed();
    }
    drag = null;
};
cv.addEventListener("pointerup", finish);
cv.addEventListener("pointercancel", finish);
function remove(notes) {
    if (!notes.length) return;
    edit();
    const t = song.tracks[sel], gone = new Set(notes);
    t.notes = t.notes.filter(n => !gone.has(n));
    picked = new Set([...picked].filter(n => !gone.has(n)));
    changed();
}
cv.addEventListener("contextmenu", e => { e.preventDefault(); const n = hit(at(e)); if (n) remove([n]); });
cv.addEventListener("dblclick", e => { const n = hit(at(e)); if (n) remove([n]); });
// the same note at the same tick twice: the FM-1 keeps one (the later)
function dedupe() {
    const t = song.tracks[sel], seen = new Map();
    for (const n of t.notes) seen.set(n.tick * 128 + n.note, n);
    if (seen.size !== t.notes.length) t.notes = [...seen.values()];
}
roll.addEventListener("keydown", e => {
    const t = song.tracks[sel], ps = [...picked];
    if ((e.key === "Delete" || e.key === "Backspace") && ps.length) { remove(ps); e.preventDefault(); return; }
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "a") { picked = new Set(t.notes); draw(); e.preventDefault(); return; }
    const mv = {ArrowUp: [0, 1], ArrowDown: [0, -1], ArrowLeft: [-1, 0], ArrowRight: [1, 0]}[e.key];
    if (mv && ps.length) {
        e.preventDefault();
        const st = +$("snap").value * (e.shiftKey ? 4 : 1), sn = e.shiftKey ? 12 : 1;
        edit();
        for (const n of ps) {
            n.tick = clamp(n.tick + mv[0] * st, 0, S.MAXBARS * S.BAR - 1);
            n.note = clamp(n.note + mv[1] * sn, 0, 127);
        }
        dedupe();
        changed();
    }
    if (e.key === "Escape") { picked = new Set(); draw(); }
});
$("clear").addEventListener("click", () => arm($("clear"), "Click again to clear", () => remove([...song.tracks[sel].notes])));
$("snap").addEventListener("change", draw);

// --------------------------------------------------------- undo, files ---
function undo() {
    if (!undos.length) return;
    redos.push(snapshot());
    restore(undos.pop());
    changed();
}
function redo() {
    if (!redos.length) return;
    undos.push(snapshot());
    restore(redos.pop());
    changed();
}
$("undo").addEventListener("click", undo);
$("redo").addEventListener("click", redo);
document.addEventListener("keydown", e => {
    if (!(e.ctrlKey || e.metaKey) || e.target.matches("input, select")) return;
    const k = e.key.toLowerCase();
    if (k === "z" && !e.shiftKey) { undo(); e.preventDefault(); }
    if ((k === "z" && e.shiftKey) || k === "y") { redo(); e.preventDefault(); }
});

// a button that needs a second click within 4 s
function arm(btn, ask, fn) {
    if (btn.dataset.armed && Date.now() - +btn.dataset.armed < 4000) {
        delete btn.dataset.armed;
        btn.classList.remove("armed");
        btn.textContent = btn.dataset.text;
        fn();
        return;
    }
    btn.dataset.text = btn.textContent;
    btn.dataset.armed = Date.now();
    btn.classList.add("armed");
    btn.textContent = ask;
    setTimeout(() => {
        if (btn.dataset.armed && Date.now() - +btn.dataset.armed >= 4000) {
            delete btn.dataset.armed;
            btn.classList.remove("armed");
            btn.textContent = btn.dataset.text;
        }
    }, 4100);
}

$("new").addEventListener("click", () => arm($("new"), "Click again: a new song", () => { load(S.newSong(), "untitled"); status("A new song (Undo brings the last one back)."); }));
$("open").addEventListener("click", () => $("file").click());
$("file").addEventListener("change", async e => {
    const f = e.target.files[0];
    e.target.value = "";
    if (!f) return;
    try {
        const s = S.parseSong(new Uint8Array(await f.arrayBuffer()));
        load(s, f.name.replace(/\.fm1song$/i, ""));
        status(`Opened ${f.name}.`, "ok");
    } catch (err) {
        status(`${f.name} is not an FM1-GM song: ${err.message}.`, "bad");
    }
});
const fileName = ext => (name.replace(/[\\/:*?"<>|]+/g, "-").trim() || "untitled") + ext;
function download(bytes, file, type) {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([bytes], {type}));
    a.download = file;
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
}
$("save").addEventListener("click", () => {
    try { download(S.songBytes(song), fileName(".fm1song"), "application/octet-stream"); status(`Saved ${fileName(".fm1song")}.`, "ok"); }
    catch (err) { status(err.message, "bad"); }
});
$("midi").addEventListener("click", () => {
    download(S.songToMidi(song, {name, skipMuted: $("skipmuted").checked}), fileName(".mid"), "audio/midi");
    status(`Exported ${fileName(".mid")} (one pass through the song).`, "ok");
});

// ------------------------------------------------------------- the FM-1 ---
let link = null, info = null, slots = [], slotSel = null, busy = false;

async function connect(how) {
    if (busy) return;
    busy = true;
    $("dev").className = "msg";
    $("dev").textContent = "looking…";
    try {
        if (link) { link.close(); link = null; }
        let r;
        if (how === "usb") {
            if (!navigator.requestMIDIAccess) throw new Error("This browser has no Web MIDI. Use Chrome, Edge or Opera on a computer.");
            let access;
            try { access = await navigator.requestMIDIAccess({sysex: true}); }
            catch (e) { throw new Error(`MIDI was refused (${e.name}). The editor needs MIDI with SysEx: allow it for this site and try again.`); }
            r = await findDevice(access);
            if (!r.link) throw new Error(r.found ? `Found ${r.found}, which does not answer the song editor: install the latest FM1-GM from the emulator page.` : "No FM-1 found. Plug it into one of the computer's own USB ports and try again.");
        } else {
            r = await findEmulator();
            if (!r) throw new Error("No emulator answered. Open the emulator page in another tab of this browser, then try again.");
        }
        link = r.link;
        await refresh(r.info);
    } catch (e) {
        $("dev").className = "msg bad";
        $("dev").textContent = e.message;
        $("devbox").hidden = true;
    } finally {
        busy = false;
        devButtons();
    }
}
$("connect").addEventListener("click", () => connect("usb"));
$("emu").addEventListener("click", () => connect("emu"));
$("refresh").addEventListener("click", () => guarded(() => refresh()));

async function refresh(hello) {
    info = hello || await link.hello();
    slots = await link.list();
    $("dev").className = "msg ok";
    $("dev").innerHTML = `<span class="pill ok">connected</span> FM1-GM ${esc(info.version)} on ${esc(link.name)}`;
    $("devbox").hidden = false;
    const used = info.sectors - info.free;
    $("space-text").textContent = `${used} of ${info.sectors} sectors used (${Math.round(info.free * 4064 / 1024)} KB free)`;
    $("space").style.width = `${100 * used / info.sectors}%`;
    const box = $("slots");
    box.innerHTML = "";
    const row = (i, label, detail, empty) => {
        const d = document.createElement("div");
        d.className = "slot" + (empty ? " empty" : "") + (slotSel === i ? " sel" : "");
        d.setAttribute("role", "option");
        d.tabIndex = 0;
        d.dataset.i = i;
        d.innerHTML = `<span class="n">${i === MEMORY ? "▶" : String(i + 1).padStart(2, "0")}</span><span class="t">${label}</span><span class="d">${detail}</span>`;
        box.append(d);
    };
    row(MEMORY, `Song in memory${info.dirty ? " <span class='cur' style='color:var(--amber)'>unsaved</span>" : ""}`, `slot ${info.slot + 1}`, false);
    slots.forEach((s, i) => row(i, s.used ? `${s.bars} bars · ${s.tempo} BPM${i === info.slot ? "<span class='cur'>in use</span>" : ""}` : "empty",
        s.used ? `${s.notes} notes · ${(s.bytes / 1024).toFixed(1)} KB` : "", !s.used));
    devButtons();
}
$("slots").addEventListener("click", e => {
    const d = e.target.closest(".slot");
    if (!d) return;
    slotSel = +d.dataset.i;
    for (const el of $("slots").children) el.classList.toggle("sel", el === d);
    devButtons();
});
$("slots").addEventListener("keydown", e => {
    if (e.key === "Enter" || e.key === " ") { e.target.click(); e.preventDefault(); }
});
function devButtons() {
    const ok = !!link && !busy && slotSel !== null, used = slotSel === MEMORY || (slotSel !== null && slots[slotSel]?.used);
    $("dev-open").disabled = !ok || !used;
    $("dev-write").disabled = !ok || !!S.problems(song).length;
    if (!$("dev-write").dataset.armed) $("dev-write").textContent = slotSel === MEMORY ? "Send to memory (not saved)" : "Write editor song here";
    $("dev-del").disabled = !ok || slotSel === MEMORY || !used;
    $("connect").disabled = $("emu").disabled = busy;
}
async function guarded(fn) {
    if (busy || !link) return;
    busy = true;
    devButtons();
    $("progress").hidden = false;
    $("bar").style.width = "0";
    try {
        await fn();
    } catch (e) {
        status(e.message, "bad");
        try { await refresh(); } catch { /* gone: say so below */ }
    } finally {
        busy = false;
        $("progress").hidden = true;
        devButtons();
    }
}
const prog = f => { $("bar").style.width = `${Math.round(f * 100)}%`; };
const slotName = i => i === MEMORY ? "the song in memory" : `slot ${i + 1}`;

$("dev-open").addEventListener("click", () => guarded(async () => {
    const i = slotSel, bytes = await link.read(i, prog);
    load(S.parseSong(bytes), i === MEMORY ? "fm1gm-memory" : `fm1gm-slot-${String(i + 1).padStart(2, "0")}`);
    status(`Opened ${slotName(i)} from the FM-1 (Undo brings back what was here).`, "ok");
}));
$("dev-write").addEventListener("click", () => {
    const i = slotSel, over = i !== MEMORY && slots[i]?.used;
    const go = () => guarded(async () => {
        await link.write(i, S.songBytes(song), prog);
        await refresh();
        status(i === MEMORY ? "Sent: the FM-1 is playing it, not saved." : `Saved to slot ${i + 1}; the FM-1 switched to it.`, "ok");
    });
    if (over || (info && info.dirty)) arm($("dev-write"), over ? `Click again: replace slot ${i + 1}` : "The FM-1's song is not saved: click again", go);
    else go();
});
$("dev-del").addEventListener("click", () => {
    const i = slotSel;
    arm($("dev-del"), `Click again: delete slot ${i + 1}`, () => guarded(async () => {
        await link.remove(i);
        await refresh();
        status(`Deleted slot ${i + 1} on the FM-1.`, "ok");
    }));
});

render();
centerRoll();
