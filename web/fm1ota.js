// SPDX-License-Identifier: GPL-3.0-only
// Install a Felucca package (.fwsc) on an FM-1 over USB-MIDI, from the browser (Web MIDI with SysEx).
// The same update as tools/fm1_install.py, line for line: step 1, the running firmware reads parts of
// the package and starts the update loader; step 2, the loader reads the whole package and writes it.
// In both steps the device asks (SysEx read requests on the logical image) and we answer. Then the
// FM-1 restarts and the installed identity is checked.
//
// The updater talks to a backend: { inputNames(), outputNames(), open(inName, outName) -> link },
// link: { send(bytes) -> bool, read(ms) -> Promise<bytes | null>, drain(), lost, close() }.
// webMidiBackend(access) is the browser's; tests/web_install_test.mjs has a simulated FM-1.

export const HS_QUERY = Uint8Array.of(0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7);
export const UPGRADE = Uint8Array.of(0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7);
export const FINISH_CHECK = 0xE0000000, FINISH_WRITE = 0xF0000000;
export const MAXDATA = 512;
export const BLOCKS = 20, BLK = 0x30, KEEP = 0x2F;
export const LOADER_MARK = new TextEncoder().encode("FELUCCA-LOADER-1");
export const PORT_RE = /fm-?1|felucca|ota|composite|sinco|usb-midi/i;   // never probe other gear
export const OPEN_RE = /^FM1_(OFFICIAL_OPEN|COMMUNITY_\d+)$/;
export const STOCK = "FM-1_015";
export const STOCK_SHA256 = "db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a";

// milliseconds; the tests shorten them
export const DELAY = {open: 300, start: 2000, reply: 10, loader: 3000, reboot: 3000, retry: 1000,
    poll: 1000, hs: 1000, idle_check: 8000, idle_write: 180000, wait_loader: 30000, wait_reboot: 40000};

export class InstallError extends Error {
    constructor(code, msg) {
        super(msg);
        this.code = code;
    }
}

const sleep = ms => new Promise(r => setTimeout(r, ms));
const now = () => performance.now();
const sum = a => a.reduce((s, b) => s + b, 0);
const hex = a => Array.from(a, b => b.toString(16).padStart(2, "0")).join("");
const eq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const le = (a, off, n) => { let v = 0; for (let i = n - 1; i >= 0; i--) v = v * 256 + a[off + i]; return v; };
const leBytes = (v, n) => Uint8Array.from({length: n}, (_, i) => Math.floor(v / 2 ** (8 * i)) & 0xFF);
const cat = (...parts) => {
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
};

// ------------------------------------------------------------ wire format ---

export function pack7(data) {
    const out = [];
    let acc = 0, nb = 0;
    for (const b of data) {
        acc |= b << nb;
        nb += 8;
        while (nb >= 7) { out.push(acc & 0x7F); acc >>= 7; nb -= 7; }
    }
    if (nb) out.push(acc & 0x7F);
    return Uint8Array.from(out);
}

export function unpack7(s) {
    const out = [];
    let acc = 0, nb = 0;
    for (const b of s) {
        acc |= (b & 0x7F) << nb;
        nb += 7;
        while (nb >= 8) { out.push(acc & 0xFF); acc >>= 8; nb -= 8; }
    }
    return Uint8Array.from(out);
}

const sysex = body => cat([0xF0], pack7(body), [0xF7]);

export function response(addr, data, fl = 0) {
    const n = data.length;
    const body = cat([0x00, 0x59, 0x30, (n + 8) & 0xFF, ((n + 8) >> 8) & 0xFF, 0, fl], leBytes(addr, 4),
        [n & 0xFF, (n >> 8) & 0xFF, 0], data, [0]);
    body[body.length - 1] = ~sum(body.subarray(6, body.length - 1)) & 0xFF;
    return sysex(body);
}

const framed = p => p.length >= 2 && p[0] === 0xF0 && p[p.length - 1] === 0xF7;

export function parseRequest(pkt) {   // -> [flags, addr, len] or null
    if (!framed(pkt)) return null;
    const u = unpack7(pkt.subarray(1, -1));
    if (u.length !== 15 || u[0] !== 0 || u[1] !== 0x59 || u[2] !== 0x30 || (~sum(u.subarray(6, 14)) & 0xFF) !== u[14])
        return null;
    return [u[6], le(u, 7, 4), le(u, 11, 3)];
}

export class Identity {
    constructor(text, model, version) { Object.assign(this, {text, model, version}); }
    get open() { return OPEN_RE.test(this.text); }
    get loader() { return this.model.toLowerCase().startsWith("ota-"); }
}

export function parseIdentity(pkt) {
    if (!framed(pkt)) return null;
    const d = unpack7(pkt.subarray(1, -1));
    if (d.length !== 34 || d[0] !== 0 || d[1] !== 0x59 || d[2] !== 0x11) return null;
    const txt = String.fromCharCode(...d.subarray(6, 33)).replace(/\0+$/, "");
    if (OPEN_RE.test(txt)) return new Identity(txt, "FM-1", 0);
    const m = /^([^_]+)_(\d+)$/.exec(txt);
    return m ? new Identity(txt, m[1], parseInt(m[2], 10)) : null;
}

// --------------------------------------------------------------- package ---

// the package identity ("FM-1_906"): one marker byte after each of the first 20 blocks
export function productOf(raw) {
    if (raw.length < BLOCKS * BLK) throw new InstallError("badpkg", "not an FM-1 package (too short)");
    let s = "";
    for (let i = 0; i < BLOCKS; i++) {
        const m = raw[i * BLK + KEEP];
        if (m !== 0x7D) s += String.fromCharCode((m - i - 1) & 0xFF);
    }
    return s;
}

// the image the device reads during an update: the package without the 20 marker bytes
export function logicalImage(raw) {
    const parts = [];
    for (let i = 0; i < BLOCKS; i++) parts.push(raw.subarray(i * BLK, i * BLK + KEEP));
    parts.push(raw.subarray(BLOCKS * BLK));
    return cat(...parts);
}

export const modelOf = text => text.split("_")[0].replace(/^ota-/, "");

function contains(hay, needle) {
    outer: for (let i = 0; i + needle.length <= hay.length; i++) {
        for (let j = 0; j < needle.length; j++) if (hay[i + j] !== needle[j]) continue outer;
        return true;
    }
    return false;
}

// -> {product, image, digest (hex), loader (has the Felucca loader marker)}; force: install a
// package without the marker
export async function loadPackage(raw, force = false) {
    raw = raw instanceof Uint8Array ? raw : new Uint8Array(raw);
    const product = productOf(raw);
    if (!/^[^_]+_\d+$/.test(product))
        throw new InstallError("badpkg", `not an FM-1 package (identity ${JSON.stringify(product)})`);
    const digest = hex(new Uint8Array(await crypto.subtle.digest("SHA-256", raw)));
    const loader = contains(raw, LOADER_MARK);
    if (!loader && !force && digest !== STOCK_SHA256)
        throw new InstallError("badpkg", "no Felucca update loader in this package; only Felucca's own packages " +
            "and M-VAVE's FM-1 V15 are installed");
    return {product, image: logicalImage(raw), digest, loader, size: raw.length};
}

// --------------------------------------------------------------- Web MIDI ---

class WebMidiLink {
    constructor(access, input, output) {
        Object.assign(this, {access, input, output, q: [], waiters: [], name: input.name, id: input.id});
        this.onmsg = e => {
            const d = e.data;
            if (d[0] !== 0xF0) return;
            const w = this.waiters.shift();
            if (w) w(new Uint8Array(d)); else this.q.push(new Uint8Array(d));
        };
        input.addEventListener("midimessage", this.onmsg);
    }
    get lost() {
        const i = this.access.inputs.get(this.id);
        return !i || i.state !== "connected";
    }
    send(pkt) {
        try { this.output.send(pkt); return true; } catch { return false; }
    }
    read(ms) {
        if (this.q.length) return Promise.resolve(this.q.shift());
        return new Promise(res => {
            let done = false;
            const w = p => { if (!done) { done = true; clearTimeout(t); res(p); } };
            const t = setTimeout(() => {
                if (done) return;
                done = true;
                this.waiters.splice(this.waiters.indexOf(w), 1);
                res(null);
            }, Math.max(ms, 0));
            this.waiters.push(w);
        });
    }
    drain() { this.q.length = 0; }
    close() {   // the ports stay open: the page may be playing from the same port
        this.input.removeEventListener("midimessage", this.onmsg);
    }
}

const uniq = a => [...new Set(a)];

export function webMidiBackend(access) {
    return {
        inputNames: () => uniq([...access.inputs.values()].filter(p => p.state === "connected").map(p => p.name)),
        outputNames: () => uniq([...access.outputs.values()].filter(p => p.state === "connected").map(p => p.name)),
        async open(inName, outName) {
            const input = [...access.inputs.values()].find(p => p.name === inName && p.state === "connected");
            const output = [...access.outputs.values()].find(p => p.name === outName && p.state === "connected");
            if (!input || !output) throw new Error("no such port");
            await input.open();
            await output.open();
            const link = new WebMidiLink(access, input, output);
            await sleep(DELAY.open);
            return link;
        },
    };
}

export function pairOutput(name, outs) {
    if (outs.includes(name)) return name;
    const strip = s => s.replace(/\s+\d+$/, "");   // Windows numbers each port
    const same = outs.filter(o => strip(o) === strip(name));
    if (same.length === 1) return same[0];
    return outs.length === 1 ? outs[0] : null;
}

// --------------------------------------------------------------- updater ---

// an M-UPGRADE command: F0 pack7(00 59 cmd len16 00 payload ~sum) F7
export function frame(cmd, payload = new Uint8Array()) {
    const n = payload.length;
    return sysex(cat([0, 0x59, cmd, n & 0xFF, n >> 8, 0], payload, [~sum(payload) & 0xFF]));
}

async function query(link, cmd, payload) {   // send a command, -> the payload of its reply, or null
    link.drain();
    if (!link.send(frame(cmd, payload))) return null;
    const end = now() + DELAY.hs * 1.5;
    for (let left; (left = end - now()) > 0;) {
        const p = await link.read(left);
        if (!p) break;
        if (!framed(p)) continue;
        const u = unpack7(p.subarray(1, -1));
        if (u.length >= 7 && u[0] === 0 && u[1] === 0x59 && u[2] === cmd && (sum(u.subarray(6)) & 0xFF) === 0xFF)
            return u.slice(6, 6 + le(u, 3, 2));
    }
    return null;
}

export async function handshake(link, tries = 3) {
    link.drain();
    for (let k = 0; k < tries; k++) {
        if (!link.send(HS_QUERY)) return null;
        const end = now() + DELAY.hs;
        for (let left; (left = end - now()) > 0;) {
            const p = await link.read(left);
            if (!p) break;
            const id = parseIdentity(p);
            if (id) return id;
        }
    }
    return null;
}

export class Updater {
    constructor(backend, port = null) { Object.assign(this, {backend, port}); }

    candidates(onlyPort) {
        const names = this.backend.inputNames();
        if (this.port) {
            const mine = names.filter(n => n.toLowerCase().includes(this.port.toLowerCase()));
            return onlyPort ? mine : mine.concat(names.filter(n => !mine.includes(n) && PORT_RE.test(n)));
        }
        return names.filter(n => PORT_RE.test(n));
    }

    // the first matching port that answers the handshake (and want(identity)) -> {link, id, name}
    async find(want = null, onlyPort = true) {
        const outs = this.backend.outputNames();
        for (const name of this.candidates(onlyPort)) {
            const out = pairOutput(name, outs);
            if (!out) continue;
            let link;
            try { link = await this.backend.open(name, out); } catch { continue; }
            const id = await handshake(link, 2);
            if (id && (!want || want(id))) return {link, id, name};
            link.close();
        }
        return null;
    }

    async waitFor(want, ms) {
        const end = now() + ms;
        while (now() < end) {
            const dev = await this.find(want, false);
            if (dev) return dev;
            await sleep(DELAY.retry);
        }
        return null;
    }

    // answer read requests until the device asks for `finish` -> [served, finished, lost]
    async serve(link, image, finish, idle, progress) {
        let served = 0, last = now();
        for (;;) {
            const pkt = await link.read(DELAY.poll);
            if (!pkt) {
                if (link.lost) return [served, false, true];
                if (now() - last > idle) return [served, false, false];
                continue;
            }
            const r = parseRequest(pkt);
            if (!r) continue;
            const [fl, addr, n] = r;
            last = now();
            if (addr === FINISH_CHECK || addr === FINISH_WRITE) {
                link.send(response(addr, new TextEncoder().encode("success\0")));
                if (addr === finish) return [served, true, false];
                continue;
            }
            if (n > MAXDATA || addr + n > image.length)
                throw new InstallError("badreq", `the device asked for 0x${addr.toString(16)}+${n}, outside the package`);
            await sleep(DELAY.reply);
            if (!link.send(response(addr, image.subarray(addr, addr + n), fl))) return [served, false, true];
            served++;
            progress(served, addr + n);
        }
    }

    async check(dev, image, step) {   // step 1: the running firmware checks the package, starts the loader
        step("start", dev.id.text);
        dev.link.send(UPGRADE);
        await sleep(DELAY.start);
        const [served, finished, lost] = await this.serve(dev.link, image, FINISH_CHECK, DELAY.idle_check,
            k => step("check", k));
        dev.link.close();
        if (!finished)
            throw new InstallError(lost ? "lost" : "stopped", `the FM-1 ${lost ? "was disconnected" : "stopped answering"} ` +
                `after ${served} requests; nothing was written`);
        step("loader");
        await sleep(DELAY.loader);
        const ota = await this.waitFor(i => i.loader, DELAY.wait_loader);
        if (!ota)
            throw new InstallError("noloader", "the update loader did not appear. Replug the USB cable and run the " +
                "install again: the FM-1 stays in update mode until it is done");
        return ota;
    }

    async write(ota, image, step) {   // step 2: the loader reads and writes the whole package, restarts
        step("write", 0);
        ota.link.send(UPGRADE);
        await sleep(DELAY.start);
        const [served, finished, lost] = await this.serve(ota.link, image, FINISH_WRITE, DELAY.idle_write,
            (k, end) => step("write", Math.min(99, Math.floor(end * 100 / image.length))));
        ota.link.close();
        if (!finished)
            throw new InstallError(lost ? "lost" : "stopped", `the loader ${lost ? "was disconnected" : "stopped answering"} ` +
                `after ${served} requests. Replug the USB cable and run the install again to finish`);
        step("write", 100);
    }

    async verify(product, step) {
        step("reboot");
        await sleep(DELAY.reboot);
        const back = await this.waitFor(i => !i.loader, DELAY.wait_reboot);
        if (!back) throw new InstallError("noreturn", "the FM-1 did not come back: power-cycle it");
        back.link.close();
        if (back.id.text !== product)
            throw new InstallError("mismatch", `written, but the FM-1 reports ${back.id.text} (expected ${product})`);
        step("done", back.id.text);
        return back.id.text;
    }

    async arm(dev, image, product, digest) {   // M-VAVE's Open firmware: allow stock V15 (and nothing else)
        if (product !== STOCK || digest !== STOCK_SHA256) {
            dev.link.close();
            throw new InstallError("open", `the FM-1 runs M-VAVE's ${dev.id.text} firmware: put the stock firmware ` +
                "back first (M-VAVE's FM-1 V15, FM-1.fwsc), then install this");
        }
        const cap = await query(dev.link, 49);
        if (!cap || cap.length !== 47 || String.fromCharCode(...cap.subarray(0, 4)) !== "FMC1" || cap[6] !== 0) {
            dev.link.close();
            throw new InstallError("stopped", `${dev.id.text} did not accept the update (state ` +
                `${cap ? hex(cap) : "no reply"}); nothing was written`);
        }
        const dg = Uint8Array.from(digest.match(/../g), h => parseInt(h, 16));
        const ack = await query(dev.link, 50, cat([1, cap[5]], leBytes(image.length, 4), dg));
        if (!ack || !eq(ack, [0])) {
            dev.link.close();
            throw new InstallError("stopped", `${dev.id.text} refused the package (${ack ? hex(ack) : "no reply"}); ` +
                "nothing was written");
        }
    }

    // running firmware -> loader -> new firmware; dev from find(). A device already in update mode
    // only needs the write.
    async install(dev, pkg, step = () => {}) {
        const {image, product, digest} = pkg;
        if (modelOf(dev.id.text) !== modelOf(product) && !dev.id.open) {
            dev.link.close();
            throw new InstallError("model", `the device is ${dev.id.text}, the package is for ${product}`);
        }
        if (dev.id.open) await this.arm(dev, image, product, digest);
        const ota = dev.id.loader ? dev : await this.check(dev, image, step);
        await this.write(ota, image, step);
        return this.verify(product, step);
    }
}
