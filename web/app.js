// SPDX-License-Identifier: GPL-3.0-only
// The page: the FM-1's panel wired to the emulator (emu.js), MIDI in from a keyboard, and the
// installer (fm1ota.js) for a real FM-1.
import {Emulator, BUTTONS, NKEYS} from "./emu.js";
import * as OTA from "./fm1ota.js";

const $ = id => document.getElementById(id);
const emu = new Emulator();
window.fm1 = emu;                                  // for the console

// ------------------------------------------------------------ the panel ---
// the button order follows the panel's matrix (OCT- and OCT+ first); the real layout may differ
const ORDER = ["OCT-", "OCT+", "FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC"];
const caps = {};
for (const name of ORDER) {
    const w = document.createElement("div");
    w.className = "btn";
    w.innerHTML = `<button class="cap" type="button" aria-label="${name}"><i></i></button><div class="lbl">${name}</div>`;
    const c = w.firstChild;
    hold(c, down => emu.press(name, down));
    caps[name] = c;
    $("btns").append(w);
}

// a control held while the pointer (or a key) is down on it
function hold(el, fn) {
    let id = null;
    el.addEventListener("pointerdown", e => {
        if (id !== null) return;
        id = e.pointerId;
        el.setPointerCapture(id);
        el.classList.add("down");
        fn(true);
        e.preventDefault();
    });
    const up = e => {
        if (e.pointerId !== id) return;
        id = null;
        el.classList.remove("down");
        fn(false);
    };
    el.addEventListener("pointerup", up);
    el.addEventListener("pointercancel", up);
    el.addEventListener("keydown", e => {
        if ((e.key === " " || e.key === "Enter") && !e.repeat) { el.classList.add("down"); fn(true); e.preventDefault(); e.stopPropagation(); }
    });
    el.addEventListener("keyup", e => {
        if (e.key === " " || e.key === "Enter") { el.classList.remove("down"); fn(false); e.preventDefault(); e.stopPropagation(); }
    });
}

// the keyboard: 27 keys, F3 .. G5
const NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const keyEls = [];
{
    const whites = [];
    for (let k = 0; k < NKEYS; k++) {
        const n = (5 + k) % 12, black = NAMES[n].includes("#");
        if (!black) whites.push(k);
    }
    const W = 100 / whites.length;
    let wi = 0;
    for (let k = 0; k < NKEYS; k++) {
        const n = (5 + k) % 12, oct = 3 + Math.floor((5 + k) / 12), black = NAMES[n].includes("#");
        const b = document.createElement("button");
        b.type = "button";
        b.className = "key " + (black ? "b" : "w");
        b.setAttribute("aria-label", NAMES[n] + oct);
        if (black) { b.style.left = `calc(${wi * W}% - ${W * 0.32}%)`; b.style.width = `${W * 0.64}%`; }
        else { b.style.left = `${wi * W}%`; b.style.width = `${W}%`; wi++; }
        b.innerHTML = `<em></em>${!black && n === 0 ? `<span>C${oct}</span>` : ""}`;
        hold(b, down => emu.key(k, down));
        keyEls.push(b);
        $("kb").append(b);
    }
}

// the encoders: drag (a detent every 10 px), the wheel, or the arrow keys
const angles = new Map();
for (const d of document.querySelectorAll(".dial")) {
    const name = d.dataset.enc, rot = d.querySelector(".rot");
    angles.set(d, 0);
    const turn = n => {
        if (!n) return;
        emu.turn(name, n);
        const a = angles.get(d) + n * 18;
        angles.set(d, a);
        rot.style.transform = `rotate(${a}deg)`;
    };
    let y0 = null, id = null;
    d.addEventListener("pointerdown", e => { id = e.pointerId; y0 = e.clientY; d.setPointerCapture(id); e.preventDefault(); d.focus(); });
    d.addEventListener("pointermove", e => {
        if (e.pointerId !== id) return;
        const steps = Math.trunc((y0 - e.clientY) / (10 * scale));
        if (steps) { turn(steps); y0 -= steps * 10 * scale; }
    });
    const end = e => { if (e.pointerId === id) id = null; };
    d.addEventListener("pointerup", end);
    d.addEventListener("pointercancel", end);
    let acc = 0;
    d.addEventListener("wheel", e => {
        e.preventDefault();
        acc += e.deltaMode ? e.deltaY * 40 : e.deltaY;
        const steps = Math.trunc(-acc / 40);
        if (steps) { turn(steps); acc += steps * 40; }
    }, {passive: false});
    d.addEventListener("keydown", e => {
        const n = {ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1, PageUp: 5, PageDown: -5}[e.key];
        if (n) { turn(n); e.preventDefault(); e.stopPropagation(); }
    });
}
$("master").addEventListener("input", e => emu.master(+e.target.value));

// the computer's keyboard plays the keys
const KEYMAP = ["KeyZ", "KeyS", "KeyX", "KeyD", "KeyC", "KeyF", "KeyV", "KeyB", "KeyH", "KeyN", "KeyJ", "KeyM",
    "Comma", "KeyL", "Period", "Semicolon", "Slash", "Digit2", "KeyW", "KeyE", "Digit4", "KeyR", "Digit5", "KeyT",
    "KeyY", "Digit7", "KeyU"];
const typing = e => /^(INPUT|SELECT|TEXTAREA)$/.test(e.target.tagName) || e.ctrlKey || e.metaKey || e.altKey;
addEventListener("keydown", e => {
    if (typing(e) || e.repeat) return;
    const k = KEYMAP.indexOf(e.code);
    if (k >= 0) { emu.key(k, true); keyEls[k].classList.add("down"); e.preventDefault(); }
    else if (e.code === "Space" && e.target === document.body) { emu.press("PLAY", true); caps.PLAY.classList.add("down"); e.preventDefault(); }
});
addEventListener("keyup", e => {
    const k = KEYMAP.indexOf(e.code);
    if (k >= 0) { emu.key(k, false); keyEls[k].classList.remove("down"); }
    else if (e.code === "Space") { emu.press("PLAY", false); caps.PLAY.classList.remove("down"); }
});
addEventListener("blur", () => {
    for (let k = 0; k < NKEYS; k++) { emu.key(k, false); keyEls[k].classList.remove("down"); }
    for (const b of BUTTONS) { emu.press(b, false); caps[b].classList.remove("down"); }
});

// the device scales to the page's width
let scale = 1;
const stage = $("stage"), device = $("device");
new ResizeObserver(() => {
    scale = Math.min(1.1, stage.clientWidth / device.offsetWidth);
    device.style.transform = `scale(${scale})`;
    stage.style.height = `${device.offsetHeight * scale}px`;
}).observe(stage);

// the screen and the lights, every frame
const ctx2d = $("screen").getContext("2d"), img = ctx2d.createImageData(240, 240);
function frame() {
    if (emu.x) {
        if (emu.paint(img)) ctx2d.putImageData(img, 0, 0);
        const L = emu.lights();
        for (const [i, b] of BUTTONS.entries()) {
            const c = caps[b], lit = !!(L.buttons >> i & 1);
            c.classList.toggle("lit", lit);
            c.classList.toggle("glow", !lit && L.glow);
            if (b === "PLAY") { c.classList.toggle("green", !L.red); c.classList.toggle("red", L.red); }
        }
        for (let k = 0; k < NKEYS; k++) keyEls[k].classList.toggle("lit", !!(L.keys >> k & 1));
    }
    requestAnimationFrame(frame);
}

// ------------------------------------------------------------- start up ---
async function boot() {
    try {
        await emu.load();
        $("st-fw").textContent = "running";
        $("st-flash").textContent = emu.restored ? "kept from last time" : "new";
        requestAnimationFrame(frame);
    } catch (e) {
        $("st-fw").textContent = "failed to load";
        $("overlay").innerHTML = `<p style="color:#eee;max-width:40ch;text-align:center">The emulator did not load (${e.message}). It needs a browser with WebAssembly, and the page served over http(s), not opened as a file.</p>`;
    }
}
$("start").addEventListener("click", async () => {
    $("overlay").hidden = true;
    try {
        await emu.startAudio();
        $("st-audio").textContent = `${emu.ctx.sampleRate / 1000} kHz`;
    } catch (e) {
        $("st-audio").textContent = "unavailable";
    }
});
$("reboot").addEventListener("click", () => emu.reboot());
let eraseArmed = 0;
$("erase").addEventListener("click", async () => {
    if (Date.now() - eraseArmed > 4000) {
        eraseArmed = Date.now();
        $("erase-note").textContent = "Every saved song and setting goes. Click again to erase.";
        setTimeout(() => { if (Date.now() - eraseArmed >= 4000) $("erase-note").textContent = ""; }, 4100);
        return;
    }
    eraseArmed = 0;
    await emu.erase();
    $("erase-note").textContent = "Erased: a new FM-1.";
    $("st-flash").textContent = "new";
});
boot();

// ----------------------------------------------------------------- MIDI ---
let access = null, kbIn = null, busy = false, device_ = null, pkg = null, pkgName = "";
const log = s => { const l = $("log"); l.textContent += s + "\n"; l.scrollTop = l.scrollHeight; };

function fillInputs() {
    const sel = $("midikb"), keep = sel.value;
    sel.innerHTML = `<option value="">None</option>`;
    for (const p of access.inputs.values()) {
        if (p.state !== "connected") continue;
        const o = document.createElement("option");
        o.value = p.id;
        o.textContent = p.name;
        sel.append(o);
    }
    sel.value = [...sel.options].some(o => o.value === keep) ? keep : "";
}
const forward = e => {
    const [st, d1 = 0, d2 = 0] = e.data;
    if (st >= 0x80 && st < 0xF0 || st === 0xFA || st === 0xFC) emu.midi(st, d1, d2);
};
$("midikb").addEventListener("change", e => {
    if (kbIn) kbIn.removeEventListener("midimessage", forward);
    kbIn = e.target.value ? access.inputs.get(e.target.value) : null;
    if (kbIn) { kbIn.addEventListener("midimessage", forward); kbIn.open(); }
});

$("connect").addEventListener("click", async () => {
    if (!navigator.requestMIDIAccess) {
        $("midi-note").textContent = "This browser has no Web MIDI. Use Chrome, Edge or Opera on a computer, or tools/fm1_install.py.";
        return;
    }
    try {
        access = await navigator.requestMIDIAccess({sysex: true});
    } catch (e) {
        $("midi-note").textContent = `MIDI was refused (${e.name}). The installer needs MIDI with SysEx: allow it for this site in the browser's settings and try again.`;
        return;
    }
    $("connect").disabled = true;
    $("connect").textContent = "MIDI connected";
    $("detect").disabled = false;
    fillInputs();
    let t = 0;
    access.addEventListener("statechange", () => {
        fillInputs();
        clearTimeout(t);
        if (!busy) t = setTimeout(detect, 800);
    });
    detect();
});
$("detect").addEventListener("click", () => detect());

async function detect() {
    if (!access || busy) return;
    busy = true;
    $("dev").textContent = "looking…";
    try {
        const up = new OTA.Updater(OTA.webMidiBackend(access));
        const d = await up.find();
        if (device_) device_.link.close();
        device_ = d;
        if (!d) {
            const names = OTA.webMidiBackend(access).inputNames();
            $("dev").innerHTML = `<span class="pill bad">not found</span> MIDI inputs: ${names.length ? names.map(esc).join(", ") : "none"}`;
        } else {
            const mode = d.id.loader ? `<span class="pill wait">update mode</span>` : `<span class="pill ok">running</span>`;
            $("dev").innerHTML = `${mode} ${esc(d.id.text)} on ${esc(d.name)}`;
        }
    } finally {
        busy = false;
        ready();
    }
}
const esc = s => s.replace(/[&<>"]/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;"}[c]));

function ready() { $("go").disabled = !(device_ && pkg) || busy; }

async function usePackage(bytes, name) {
    try {
        pkg = await OTA.loadPackage(bytes);
        pkgName = name;
        $("pkg").textContent = `${pkg.product} · ${name} · ${(pkg.size / 1024).toFixed(1)} KB`;
        $("pkg-sha").textContent = pkg.digest;
    } catch (e) {
        pkg = null;
        $("pkg").innerHTML = `<span class="pill bad">refused</span> ${esc(name)}: ${esc(e.message)}`;
        $("pkg-sha").textContent = "…";
    }
    ready();
}
fetch("fm1gm.fwsc").then(r => r.ok ? r.arrayBuffer() : Promise.reject(new Error(r.status)))
    .then(b => usePackage(new Uint8Array(b), "this page's build"))
    .catch(() => { $("pkg").textContent = "none bundled: choose a .fwsc"; });
fetch("build.txt").then(r => r.ok ? r.text() : "").then(t => { $("build").textContent = t.trim(); }).catch(() => {});
$("file").addEventListener("change", async e => {
    const f = e.target.files[0];
    if (f) usePackage(new Uint8Array(await f.arrayBuffer()), f.name);
});

$("go").addEventListener("click", () => {
    const where = device_.id.loader ? "finish the update on the FM-1 (it is in update mode)" : `replace ${esc(device_.id.text)} on the FM-1`;
    $("confirm-text").innerHTML = `Install <b>${esc(pkg.product)}</b> (${esc(pkgName)}) and ${where}? Keep the USB cable in until it says done.`;
    $("confirm").hidden = false;
    $("go").disabled = true;
});
$("no").addEventListener("click", () => { $("confirm").hidden = true; ready(); });
$("yes").addEventListener("click", async () => {
    $("confirm").hidden = true;
    busy = true;
    ready();
    $("progress").hidden = false;
    $("log").textContent = "";
    const bar = $("bar");
    const step = (k, a) => {
        if (k === "check") { bar.style.width = "3%"; return; }
        if (k === "write") { bar.style.width = `${5 + a * 0.9}%`; if (a % 10 === 0 && a !== lastPct) { lastPct = a; log(`writing ${a}%`); } return; }
        log({start: `starting the update on ${a}`, loader: "switching to update mode", reboot: "restarting", done: `done: the FM-1 runs ${a}`}[k]);
        if (k === "done") bar.style.width = "100%";
    };
    let lastPct = -1;
    const up = new OTA.Updater(OTA.webMidiBackend(access));
    const dev = device_;
    device_ = null;
    try {
        await up.install(dev, pkg, step);
        $("dev").innerHTML = `<span class="pill ok">running</span> ${esc(pkg.product)}`;
    } catch (e) {
        log(`error: ${e.message}`);
        bar.style.background = "var(--bad)";
    } finally {
        busy = false;
        setTimeout(detect, 500);
    }
});
