// SPDX-License-Identifier: GPL-3.0-only
// The emulator's audio out: stereo blocks from the page (emu.js), played in order. Reports how many
// frames it has played so the page can keep about 70 ms queued.
class FM1Out extends AudioWorkletProcessor {
    constructor() {
        super();
        this.q = [];
        this.off = 0;
        this.played = 0;
        this.n = 0;
        this.port.onmessage = e => {
            if (e.data === "flush") { this.q = []; this.off = 0; return; }
            this.q.push(e.data);
        };
    }
    process(inputs, outputs) {
        const [l, r] = outputs[0];
        for (let i = 0; i < l.length; i++) {
            const b = this.q[0];
            if (!b) { l[i] = 0; if (r) r[i] = 0; continue; }
            l[i] = b[this.off];
            if (r) r[i] = b[this.off + 1];
            this.off += 2;
            this.played++;
            if (this.off >= b.length) { this.q.shift(); this.off = 0; }
        }
        if (++this.n % 4 === 0) this.port.postMessage(this.played);
        return true;
    }
}
registerProcessor("fm1-out", FM1Out);
