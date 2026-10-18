// SPDX-License-Identifier: GPL-3.0-only
// web/fm1ota.js (the web page's installer) against a simulated FM-1, as tests/install_test.py tests
// tools/fm1_install.py: identity on the handshake, then "device asks, host answers" reads of the
// logical image. No browser, no hardware. Run from the repo root:  node tests/web_install_test.mjs
// Also reads the page's package, web/fm1gm.fwsc, when there is one.
import {readFileSync, existsSync} from "node:fs";
import {createHash} from "node:crypto";
import * as I from "../web/fm1ota.js";

Object.assign(I.DELAY, {open: 0, start: 10, reply: 0, loader: 10, reboot: 10, retry: 20, poll: 50, hs: 100,
    idle_check: 400, idle_write: 400, wait_loader: 2000, wait_reboot: 2000});
let failed = 0;
const ok = (cond, what) => {
    console.log(`${what.padEnd(64)} ${cond ? "ok" : "FAIL"}`);
    failed += !cond;
};
const enc = s => new TextEncoder().encode(s);
const eq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const cat = (...p) => Uint8Array.from(p.flatMap(x => [...x]));
const le = (a, o, n) => { let v = 0; for (let i = n - 1; i >= 0; i--) v = v * 256 + a[o + i]; return v; };
const lb = (v, n) => Array.from({length: n}, (_, i) => Math.floor(v / 2 ** (8 * i)) & 0xFF);
const sum = a => a.reduce((s, b) => s + b, 0);
const sx = body => cat([0xF0], I.pack7(Uint8Array.from(body)), [0xF7]);

// ---------------------------------------------------------- simulated FM-1 ---

class FakeLink {
    constructor(dev, gen) { Object.assign(this, {dev, gen, q: [], waiters: [], closed: false}); }
    get lost() { return this.gen !== this.dev.gen || !this.dev.connected; }
    send(pkt) {
        if (this.lost) return false;
        this.dev.sent.push(pkt);
        this.dev.rx(pkt);
        return true;
    }
    put(p) { const w = this.waiters.shift(); if (w) w(p); else this.q.push(p); }
    read(ms) {
        if (this.q.length) return Promise.resolve(this.q.shift());
        return new Promise(res => {
            const w = p => { clearTimeout(t); res(p); };
            const t = setTimeout(() => { this.waiters.splice(this.waiters.indexOf(w), 1); res(null); }, ms);
            this.waiters.push(w);
        });
    }
    drain() { this.q.length = 0; }
    close() { this.closed = true; }
}

class FakeFM1 {
    constructor(image, o = {}) {
        Object.assign(this, {image, unplugAfter: o.unplugAfter, afterWrite: o.afterWrite ?? "FM-1_900",
            stallAfter: o.stallAfter, badAddr: o.badAddr, refuseArm: o.refuseArm, stuck: o.stuck});
        Object.assign(this, {armed: false, arms: [], served: 0, bad: 0, upgrades: 0, sent: [], links: [], gen: 0});
        this.boot(o.identity ?? "FM-1_015", o.name ?? "FM-1");
    }
    boot(identity, name) {
        this.gen++;
        Object.assign(this, {identity, name, connected: !(this.stuck && identity.startsWith("ota-")), waiting: null, queue: []});
    }
    inputNames() { return this.connected ? [this.name] : []; }
    outputNames() { return this.inputNames(); }
    async open(inName) {
        if (inName !== this.name || !this.connected) throw new Error("no such port");
        const l = new FakeLink(this, this.gen);
        this.links.push(l);
        return l;
    }
    tx(p) { for (const l of this.links) if (l.gen === this.gen && !l.closed) l.put(p); }
    rx(d) {
        const u = I.unpack7(d.subarray(1, -1));
        if (this.identity.startsWith("FM1_") && u.length >= 7 && u[0] === 0 && u[1] === 0x59 && (u[2] === 49 || u[2] === 50))
            return this.openCmd(u[2], u.subarray(6, -1));
        if (eq(d, I.HS_QUERY)) {
            const t = enc(this.identity);
            this.tx(sx([0, 0x59, 0x11, 0, 0, 0, ...t, ...new Array(28 - t.length).fill(0)]));
        } else if (eq(d, I.UPGRADE)) {
            if (this.identity.startsWith("FM1_") && !this.armed) return;
            this.upgrades++;
            const first = this.badAddr ?? 0;
            this.queue = this.identity.startsWith("ota-")
                ? [...Array.from({length: 6}, (_, k) => [first + k * 512, 512]), [I.FINISH_WRITE, 8]]
                : [[0, 64], [0x40, 160], [0x1000, 512], [I.FINISH_CHECK, 8]];
            this.next();
        } else if (this.waiting) {
            const [addr, n] = this.waiting;
            const want = addr >= I.FINISH_CHECK ? enc("success\0") : this.image.subarray(addr, addr + n);
            if (!eq(u.subarray(14, 14 + want.length), want) || u.length !== 15 + want.length) this.bad++;
            this.waiting = null;
            this.served++;
            if (this.unplugAfter && this.served >= this.unplugAfter) { this.connected = false; return; }
            if (this.stallAfter && this.served >= this.stallAfter) return;
            if (addr === I.FINISH_CHECK) setTimeout(() => this.boot("ota-FM-1_900", "Felucca Update"), 50);
            else if (addr === I.FINISH_WRITE) setTimeout(() => this.boot(this.afterWrite, "Felucca"), 50);
            else this.next();
        }
    }
    openCmd(cmd, data) {
        let out;
        if (cmd === 49) out = [...enc("FMC1"), 1, 7, 0, 1, ...lb(512, 2), ...lb(1 << 20, 4), ...new Array(33).fill(0)];
        else {
            this.arms.push(Uint8Array.from(data));
            this.armed = data.length === 38 && data[0] === 1 && data[1] === 7 && le(data, 2, 4) === this.image.length && !this.refuseArm;
            out = [this.armed ? 0 : 1];
        }
        this.tx(sx([0, 0x59, cmd, out.length, 0, 0, ...out, ~sum(out) & 0xFF]));
    }
    next() {
        if (!this.queue.length) return;
        const [addr, n] = this.waiting = this.queue.shift();
        const u = [0, 0x59, 0x30, 0, 0, 0, 0, ...lb(addr, 4), n & 0xFF, n >> 8, 0];
        u.push(~sum(u.slice(6, 14)) & 0xFF);
        this.tx(sx(u));
    }
}

function makePackage(product = "FM-1_900", marker = true, size = 0x2000) {
    const raw = Uint8Array.from({length: size + I.BLOCKS}, (_, i) => (i * 7) & 0xFF);
    for (let i = 0; i < I.BLOCKS; i++)
        raw[i * I.BLK + I.KEEP] = i < product.length ? (product.charCodeAt(i) + i + 1) & 0xFF : 0x7D;
    raw.set(marker ? I.LOADER_MARK : new Uint8Array(16), 0x1800);
    return raw;
}

async function attempt(dev, pkg) {
    const up = new I.Updater(dev), steps = [];
    try {
        const d = await up.find();
        if (!d) return {code: "notfound", steps};
        const done = await up.install(d, pkg, (k, a) => steps.push([k, a]));
        return {code: 0, done, steps};
    } catch (e) {
        if (!(e instanceof I.InstallError)) throw e;
        return {code: e.code, msg: e.message, steps};
    }
}

// ------------------------------------------------------------------- tests ---

{
    const data = Uint8Array.from({length: 768}, (_, i) => i & 0xFF);
    ok(eq(I.unpack7(I.pack7(data)).subarray(0, data.length), data), "pack7 / unpack7 round trip");
    const u = I.unpack7(I.response(0x12345, Uint8Array.of(1, 2, 3), 5).subarray(1, -1));
    ok(u[2] === 0x30 && le(u, 7, 4) === 0x12345 && eq(u.subarray(14, 17), [1, 2, 3]) && (~sum(u.subarray(6, 17)) & 0xFF) === u[17],
        "response: header, address, data, checksum");
    const raw = makePackage("FM-1_906");
    ok(I.productOf(raw) === "FM-1_906" && I.logicalImage(raw).length === raw.length - 20, "productOf / logicalImage (synthetic)");
    ok(I.productOf(makePackage("FM-1_9001000")) === "FM-1_9001000", "productOf: a 12-character identity");
}

{
    const pkg = await I.loadPackage(makePackage());
    let dev = new FakeFM1(pkg.image), r = await attempt(dev, pkg);
    ok(r.code === 0 && dev.bad === 0 && dev.upgrades === 2 && dev.served === 11 && r.done === "FM-1_900"
        && r.steps.some(([k, a]) => k === "write" && a === 100), `install: running -> loader -> Felucca (${dev.served} reads)`);
    dev = new FakeFM1(pkg.image, {identity: "ota-FM-1_900", name: "Felucca Update"});
    r = await attempt(dev, pkg);
    ok(r.code === 0 && dev.bad === 0 && dev.upgrades === 1, "install: device already in update mode -> finishes the write");
    r = await attempt(new FakeFM1(pkg.image, {name: "IAC Driver Bus 1"}), pkg);
    ok(r.code === "notfound", "no FM-1 port -> not found, nothing probed");
    r = await attempt(new FakeFM1(pkg.image, {unplugAfter: 2}), pkg);
    ok(r.code === "lost" && /nothing was written/.test(r.msg), "unplugged in step 1 -> 'lost', nothing written");
    r = await attempt(new FakeFM1(pkg.image, {identity: "ota-FM-1_900", name: "Felucca Update", unplugAfter: 3}), pkg);
    ok(r.code === "lost" && /install again/.test(r.msg), "unplugged during the write -> 'lost'");
    r = await attempt(new FakeFM1(pkg.image, {identity: "ota-FM-1_900", name: "Felucca Update", stallAfter: 3}), pkg);
    ok(r.code === "stopped" && /stopped answering/.test(r.msg), "loader stops answering -> 'stopped' after the idle time");
    r = await attempt(new FakeFM1(pkg.image, {identity: "ota-FM-1_900", name: "Felucca Update", badAddr: pkg.image.length}), pkg);
    ok(r.code === "badreq", "read past the package -> 'badreq'");
    r = await attempt(new FakeFM1(pkg.image, {afterWrite: "FM-1_015"}), pkg);
    ok(r.code === "mismatch" && /reports FM-1_015/.test(r.msg), "another identity after the restart -> 'mismatch'");
    dev = new FakeFM1(pkg.image, {identity: "XY-9_001", name: "usb-midi"});
    r = await attempt(dev, pkg);
    ok(r.code === "model" && dev.upgrades === 0, "another model -> 'model', nothing sent");
    r = await attempt(new FakeFM1(pkg.image, {stuck: true}), pkg);
    ok(r.code === "noloader", "no loader after step 1 -> 'noloader'");
}

{
    let code = null;
    try { await I.loadPackage(makePackage("FM-1_900", false)); } catch (e) { code = e.code; }
    ok(code === "badpkg", "package without the loader marker -> refused");
    ok((await I.loadPackage(makePackage("FM-1_900", false), true)).product === "FM-1_900", "... accepted with force");
    code = null;
    try { await I.loadPackage(new Uint8Array(100)); } catch (e) { code = e.code; }
    ok(code === "badpkg", "not a package -> refused");
}

{
    const stockRaw = makePackage("FM-1_015", false);
    const real = I.STOCK_SHA256, digest = createHash("sha256").update(stockRaw).digest("hex");
    const stock = {product: "FM-1_015", image: I.logicalImage(stockRaw), digest};
    let dev = new FakeFM1(stock.image, {identity: "FM1_OFFICIAL_OPEN", name: "FM1 Community", afterWrite: "FM-1_015"});
    // arm() compares with STOCK_SHA256, a constant: stand in the synthetic V15 by its own digest
    const up = new I.Updater(dev);
    up.arm = async function (d, image, product, dg) {
        return I.Updater.prototype.arm.call(this, d, image, product, dg === digest ? real : dg);
    };
    const d = await up.find();
    let done = null;
    try { done = await up.install(d, {...stock, digest}); } catch (e) { done = e.message; }
    ok(done === "FM-1_015" && dev.arms.length === 1 && dev.bad === 0 && le(dev.arms[0], 2, 4) === stock.image.length,
        "Open -> stock V15: armed (length, SHA-256), then installed");
    const ours = await I.loadPackage(makePackage());
    dev = new FakeFM1(ours.image, {identity: "FM1_OFFICIAL_OPEN", name: "FM1 Community"});
    const r = await attempt(dev, ours);
    ok(r.code === "open" && dev.arms.length === 0 && dev.upgrades === 0, "Open -> one of ours: refused, nothing sent (stock first)");
}

if (existsSync("web/fm1gm.fwsc")) {
    const pkg = await I.loadPackage(readFileSync("web/fm1gm.fwsc"));
    ok(pkg.product.startsWith("FM-1_6") && pkg.loader, `web/fm1gm.fwsc: ${pkg.product}, with the Felucca loader`);
    const dev = new FakeFM1(pkg.image, {afterWrite: pkg.product}), r = await attempt(dev, pkg);
    ok(r.code === 0 && dev.bad === 0, "web/fm1gm.fwsc: installs on the simulated FM-1");
} else console.log("web/fm1gm.fwsc: skipped (not there)");

console.log(failed ? `WEB INSTALL TESTS FAILED (${failed})` : "web install tests passed");
process.exit(failed ? 1 : 0);
