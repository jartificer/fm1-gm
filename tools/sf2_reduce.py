#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Make FM1-GM's small General MIDI SoundFont (assets/soundfont/fm1gm.sf2) from FluidR3_GM.sf2.

  tools/sf2_reduce.py FluidR3_GM.sf2 assets/soundfont/fm1gm.sf2

FluidR3 GM (Frank Wen, MIT License) is 141 MiB. The FM-1 has about 440 KiB of flash left after the
firmware, so for each GM program this keeps one velocity layer, one sample per key region and only a
few regions (spread over the keyboard), mono; it lowers each sample's rate to what its spectrum
needs; and it keeps only each sample's attack and a short loop after it (the loop's level is
evened out and its seam crossfaded). A long sample without a loop gets a loop and a volume-envelope
decay that follows its own fade. The drum kit (bank 128, program 0) keeps notes 35..81, each
trimmed (or looped and faded, for cymbals).

The result is an ordinary SoundFont (16-bit, the reduced rates): play it in any SoundFont player.
tools/gen_wavetable.py turns it into the firmware's tables (4-bit ADPCM, about a quarter of its
size). Needs numpy and scipy (this tool only; the firmware build does not).
"""
import argparse
import sys
from fractions import Fraction
from pathlib import Path

import numpy as np
from scipy.signal import resample_poly, welch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sf2  # noqa: E402

G = sf2.G
GN_MOD = __import__("re").compile(r"^(modLfo|vibLfo|modEnv)To")
VEL = 100                       # the velocity layer kept (the FM-1's keys play at a fixed velocity, 100 by default)
DRUM_LO, DRUM_HI = 35, 81

# per GM family (program >> 3): regions kept, attack kept (s), loop length (s)
FAMILY = [
    (3, 0.14, 0.05),            # piano
    (2, 0.12, 0.05),            # chromatic percussion
    (2, 0.05, 0.06),            # organ
    (2, 0.11, 0.05),            # guitar
    (2, 0.10, 0.05),            # bass
    (2, 0.07, 0.08),            # strings
    (2, 0.07, 0.08),            # ensemble
    (2, 0.08, 0.05),            # brass
    (2, 0.07, 0.05),            # reed
    (2, 0.07, 0.05),            # pipe
    (2, 0.05, 0.05),            # synth lead
    (2, 0.06, 0.09),            # synth pad
    (2, 0.08, 0.08),            # synth effects
    (2, 0.10, 0.05),            # ethnic
    (1, 0.16, 0.05),            # percussive
    (1, 0.20, 0.10),            # sound effects
]
UNPITCHED = {119, 120, 121, 122, 124, 125, 126, 127}   # GM programs whose loops are noise, not periods
RATE_MIN, RATE_MAX = 11025, 22050
DRUM_RATE_MAX = 32000           # hi-hats and cymbals have half their energy above 11 kHz
DRUM_MIN, DRUM_MAX, DRUM_LOOP = 0.06, 0.24, 0.05   # a drum keeps until it is 36 dB down, within these (s)


def pcm_float(sm):
    return np.frombuffer(sm.pcm, dtype="<i2").astype(np.float64) / 32768.0


def region_sample(sf, z, zs):
    """the zone's sample as floats; a stereo pair (the linked sample in a zone of the same keys) mixed
    to mono"""
    sm = sf.samples[z["sample"]]
    x = pcm_float(sm)
    mixed = False
    if sm.stype in (2, 4):
        for o in zs:
            if o is not z and o["sample"] == sm.link and o["key"][0] <= z["key"][1] and z["key"][0] <= o["key"][1]:
                y = pcm_float(sf.samples[o["sample"]])
                if len(y) == len(x) and sf.samples[o["sample"]].loop == sm.loop:
                    m = 0.5 * (x + y)           # keep the pair's power (the sides are partly uncorrelated)
                    k = np.sqrt((np.mean(x ** 2) + np.mean(y ** 2)) / 2 / (np.mean(m ** 2) + 1e-12))
                    x = m * min(max(k, 1.0), 1.41)
                    mixed = True
                break
    g = z["gens"]
    s0 = g.get(G["startAddrsOffset"], 0) + 32768 * g.get(G["startAddrsCoarseOffset"], 0)
    e0 = g.get(G["endAddrsOffset"], 0) + 32768 * g.get(G["endAddrsCoarseOffset"], 0)
    ls = sm.loop[0] + g.get(G["startloopAddrsOffset"], 0) + 32768 * g.get(G["startloopAddrsCoarseOffset"], 0)
    le = sm.loop[1] + g.get(G["endloopAddrsOffset"], 0) + 32768 * g.get(G["endloopAddrsCoarseOffset"], 0)
    x = x[max(0, s0):len(x) + min(0, e0)]
    return x, sm.rate, (ls - max(0, s0), le - max(0, s0)), mixed


def pick(zs, vel=VEL):
    """the zone that sounds for each key, as regions [(lo, hi, zone)]: of the zones on a key, the
    velocity layer with vel (or the nearest), left or mono before right, and of layers the first
    one the SoundFont lists (its main sound), unless it only starts after a long delay or attack"""
    def vd(z):
        lo, hi = z["vel"]
        return 0 if lo <= vel <= hi else min(abs(vel - lo), abs(vel - hi))

    def late(z):
        g = z["gens"]
        t = 2 ** (g.get(G["delayVolEnv"], -12000) / 1200) + 2 ** (g.get(G["attackVolEnv"], -12000) / 1200)
        return t > 0.6
    regions = []
    for k in range(128):
        here = [(i, z) for i, z in enumerate(zs) if z["key"][0] <= k <= z["key"][1]]
        if not here:
            continue
        best = min(vd(z) for _, z in here)
        here = [(i, z) for i, z in here if vd(z) == best]
        if any(z["stype"] != 2 for _, z in here):
            here = [(i, z) for i, z in here if z["stype"] != 2]
        i, z = min(here, key=lambda iz: (late(iz[1]), iz[0]))
        if regions and regions[-1][2] is z and regions[-1][1] == k - 1:
            regions[-1][1] = k
        else:
            regions.append([k, k, z])
    return [tuple(r) for r in regions]


def choose(regions, k):
    """k regions spread over the playing range (keys 24..108 of what the program covers)"""
    lo = max(min(r[0] for r in regions), 24)
    hi = min(max(r[1] for r in regions), 108)
    if lo > hi:
        lo, hi = min(r[0] for r in regions), max(r[1] for r in regions)
    out = []
    for i in range(k):
        t = lo + (i + 0.5) * (hi - lo) / k
        r = min(regions, key=lambda r: 0 if r[0] <= t <= r[1] else min(abs(t - r[0]), abs(t - r[1])))
        if r not in out:
            out.append(r)
    return out


def root_of(sf, z):
    r = z["gens"].get(G["overridingRootKey"], -1)
    return r if r >= 0 else sf.samples[z["sample"]].root


def band_rate(x, rate, f0, cap):
    """the lowest rate that keeps 99.97 % of the energy of the first half second (and 5 harmonics)"""
    seg = x[:rate // 2]
    if len(seg) < 256:
        seg = np.pad(seg, (0, 256 - len(seg)))
    f, p = welch(seg, rate, nperseg=min(2048, len(seg)))
    c = np.cumsum(p)
    if c[-1] <= 0:
        return RATE_MIN
    fb = f[min(np.searchsorted(c, 0.9997 * c[-1]), len(f) - 1)]
    want = max(2.2 * fb, RATE_MIN, 10 * f0 if f0 else 0)
    return min(want, cap, rate)


def resample(x, rate, want):
    """x at a rate of rate * p / q >= want, p / q a simple fraction"""
    if want >= rate:
        return x, rate, Fraction(1)
    best = None
    for q in range(1, 9):
        for p in range(1, q + 1):
            fr = Fraction(p, q)
            if rate * fr >= want and (best is None or fr < best):
                best = fr
    if best == 1:
        return x, rate, best
    return resample_poly(x, best.numerator, best.denominator), int(round(rate * best)), best


def period(x, s, p0, span):
    """the pitch period near p0 (samples, fractional), by autocorrelation around s"""
    lo, hi = int(p0 * 0.96), int(np.ceil(p0 * 1.04)) + 1
    seg = x[s:s + span + hi]
    if lo < 2 or len(seg) < span + hi:
        return p0
    a = seg[:span]
    best, bp = -2.0, p0
    for lag in range(lo, hi + 1):
        b = seg[lag:lag + span]
        c = float(np.dot(a, b) / (np.sqrt(np.dot(a, a) * np.dot(b, b)) + 1e-12))
        if c > best:
            best, bp = c, lag
    return float(bp)


def make_loop(x, rate, f0, s_lo, s_hi, target, l_max=None):
    """a loop (start, end) with its start in [s_lo, s_hi] and a length near target (a whole number of
    periods when f0); the seam crossfaded and the level evened out across the loop; returns the
    sample cut at the loop end"""
    s_lo = max(int(s_lo), 1)
    s_hi = max(int(s_hi), s_lo + 1)
    w = max(int(rate * 0.006), 16)
    if f0:
        p = period(x, s_lo, rate / f0, max(w, int(rate / f0 * 2)))
        m = max(1, int(round(target * rate / p)))
        if l_max:
            m = max(1, min(m, int(l_max / p)))
        lens = sorted({int(np.floor(m * p)) + d for d in (-1, 0, 1, 2)})
    else:
        lens = [int(target * rate)]
        if l_max:
            lens = [min(lens[0], int(l_max))]
    best = None
    for ln in lens:
        if ln < 8:
            continue
        hi = min(s_hi, len(x) - ln - w)
        if hi <= s_lo:
            continue
        a = x[s_lo:hi + w]
        b = x[s_lo + ln:hi + ln + w]
        e = np.concatenate([[0.0], np.cumsum((a - b) ** 2)])
        en = np.concatenate([[0.0], np.cumsum(a ** 2 + b ** 2)])
        n = hi - s_lo
        cost = (e[w:w + n] - e[:n]) / (en[w:w + n] - en[:n] + 1e-9)
        i = int(np.argmin(cost))
        if best is None or cost[i] < best[0]:
            best = (cost[i], s_lo + i, ln)
    if best is None:                                        # too short to loop: keep it whole
        return x, None
    _, ls, ln = best
    le = ls + ln
    y = x[:le + w].copy()
    # even out the level across the loop: an exponential gain from 1 at the start to whatever brings
    # the loop's second half up (or down) to its first half's level, at most 12 dB
    h = ln // 2
    r1 = np.sqrt(np.mean(y[ls:ls + h] ** 2) + 1e-12)
    r2 = np.sqrt(np.mean(y[ls + h:le] ** 2) + 1e-12)
    k = np.clip(np.log(r1 / r2), -np.log(4.0), np.log(4.0)) / max(h, 1)
    ramp = np.exp(k * (np.arange(len(y) - ls)))
    y[ls:] *= ramp
    # crossfade the end of the loop into what comes before its start (equal power)
    f = min(ln // 2, ls)
    if f > 0:
        t = np.linspace(0.0, 1.0, f, endpoint=False) + 0.5 / f
        a, b = np.cos(t * np.pi / 2), np.sin(t * np.pi / 2)
        y[le - f:le] = y[le - f:le] * a + y[ls - f:ls] * b
    return y[:le], (ls, le)


def drum_length(x, rate):
    """seconds until x is 36 dB below its peak (10 ms windows), within DRUM_MIN..DRUM_MAX"""
    hop = max(rate // 100, 1)
    n = len(x) // hop
    if n < 2:
        return DRUM_MIN
    db = 10 * np.log10(np.mean(x[:n * hop].reshape(n, hop) ** 2, axis=1) + 1e-12)
    pk = int(np.argmax(db))
    below = np.nonzero(db[pk:] < db[pk] - 36)[0]
    t = (pk + (below[0] if len(below) else n - pk)) * hop / rate
    return float(min(max(t, DRUM_MIN), DRUM_MAX))


def decay_tc(x, rate, t0):
    """a volume-envelope decay (timecents to fall 100 dB) that follows x's own fade after t0"""
    hop = max(rate // 100, 1)
    n = (len(x) - int(t0 * rate)) // hop
    if n < 8:
        return None
    seg = x[int(t0 * rate):int(t0 * rate) + n * hop].reshape(n, hop)
    db = 10 * np.log10(np.mean(seg ** 2, axis=1) + 1e-12)
    top = db.max()
    keep = db > top - 50
    t = np.arange(n) * hop / rate
    if keep.sum() < 4:
        return None
    slope = np.polyfit(t[keep], db[keep], 1)[0]             # dB / s
    if slope > -3.0:                                        # steady: no fade to follow
        return None
    t100 = min(max(100.0 / -slope, 0.05), 20.0)
    return int(round(1200 * np.log2(t100)))


def fade_with_envelope(decay, g, extra):
    """a sample's own fade (decay, timecents per 100 dB), now that a loop holds its level, goes into
    the volume envelope: added to the envelope's decay when that falls to silence (and is not held off
    for seconds: FluidR3's drums hold for longer than their samples last), else instead of it (no
    hold: the fade starts at once)"""
    if decay is None:
        return
    rate = 100.0 / 2 ** (decay / 1200)                      # dB / s
    if g.get(G["sustainVolEnv"], 0) >= 960 and 2 ** (g.get(G["holdVolEnv"], -12000) / 1200) < 2.0:
        rate += 100.0 / 2 ** (g.get(G["decayVolEnv"], -12000) / 1200)
    extra[G["decayVolEnv"]] = int(round(1200 * np.log2(100.0 / rate)))
    extra[G["sustainVolEnv"]] = 1000
    extra[G["holdVolEnv"]] = -12000


def to_pcm(y):
    return (np.clip(np.round(y * 32767.0), -32768, 32767).astype("<i2")).tobytes()


def process(sf, z, zs, root, att, loop_t, pitched, drum):
    x, rate, (ls0, le0), mixed = region_sample(sf, z, zs)
    g = z["gens"]
    # the sample's own pitch: its root key, less the correction a player adds
    f0 = 440.0 * 2 ** ((root - 69) / 12 - sf.samples[z["sample"]].corr / 1200) if pitched else None
    looped = g.get(G["sampleModes"], 0) & 1 and 0 <= ls0 < le0 <= len(x)
    extra = {}
    if drum:
        keep_s = drum_length(x, rate)
        att, loop_t = keep_s * 0.6, DRUM_LOOP
        long = keep_s >= DRUM_MAX
    else:
        keep_s = att + loop_t * 1.6
        long = True
    decay = None if looped or not long else decay_tc(x, rate, att)
    want = band_rate(x, rate, f0, DRUM_RATE_MAX if drum else RATE_MAX)
    need = int(max(le0 + 16 if looped else 0, (keep_s * 1.15 + loop_t * 2 + 0.05) * rate))
    xr, nr, fr = resample(x[:need], rate, want)
    sc = float(fr)
    ls, le = int(round(ls0 * sc)), int(round(le0 * sc))
    loop = None
    if looped:
        if le <= keep_s * nr:                               # short enough: as it is
            y, loop = xr[:le], (ls, le)
        elif ls <= att * nr:                                # a shorter loop inside the original one
            y, loop = make_loop(xr, nr, f0, ls, min(ls + 0.03 * nr, le - 16), loop_t, l_max=le - ls)
        else:                                               # a new loop right after the attack
            y, loop = make_loop(xr, nr, f0, att * nr, att * nr + 0.03 * nr, loop_t)
            fade_with_envelope(decay_tc(x[:ls0], rate, att), g, extra)    # the fade it had before its loop
    elif len(xr) <= keep_s * nr * 1.15 or decay is None:    # short, or it dies away: cut it, fade the cut
        y = xr[:int(keep_s * nr * 1.15)].copy()
        if len(y) < len(xr):
            nf = int(len(y) * 0.3)
            y[-nf:] *= np.linspace(1.0, 0.0, nf) ** 2
        else:
            nf = min(len(y), int(0.004 * nr))
            y[-nf:] *= np.linspace(1.0, 0.0, nf)
    else:                                                   # long: loop it and fade it with the envelope
        y, loop = make_loop(xr, nr, f0, att * nr, att * nr + 0.03 * nr, loop_t)
        if loop:
            fade_with_envelope(decay, g, extra)
    if loop:                                                # 8 points after the loop end (SoundFont 7.10)
        y = np.concatenate([y[:loop[1]], y[loop[0]:loop[0] + 8]])
    if mixed or not drum:                                   # mono now: no pan (melodic: the channel pans)
        extra[G["pan"]] = 0
    return y, nr, loop, extra


def out_gens(z, root, loop, extra, key):
    keep = {k: v for k, v in z["gens"].items() if k not in sf2.NOT_ADDITIVE and k not in sf2.RANGE and
            k != G["instrument"] and sf2.DEFAULTS.get(k, 0) != v}
    for k in (G["exclusiveClass"],):
        if k in z["gens"]:
            keep[k] = z["gens"][k]
    keep.update(extra)
    keep = {k: v for k, v in keep.items() if not (k == G["pan"] and v == 0)}
    for k, v in keep.items():                               # summed preset + instrument values can overflow
        lim = 12000 if GN_MOD.match(sf2.GN.get(k, "")) else 32767
        keep[k] = max(-lim, min(lim, v))
    keep[G["keyRange"]] = key
    keep[G["overridingRootKey"]] = root
    if loop:
        keep[G["sampleModes"]] = 1
    return keep


def reduce(src, verbose=False):
    sf = sf2.read(src)
    presets = {(p.bank, p.program): p for p in sf.presets}
    samples, cache, instruments, out_presets = [], {}, [], []
    stats = []

    def add_sample(z, zs, root, att, loop_t, pitched, drum, name):
        key = (z["sample"], root, att, loop_t, drum, tuple(sorted((k, v) for k, v in z["gens"].items()
                                                                   if k in sf2.NOT_ADDITIVE or k in (G["coarseTune"], G["fineTune"]))))
        if key not in cache:
            y, rate, loop, extra = process(sf, z, zs, root, att, loop_t, pitched, drum)
            sm = sf.samples[z["sample"]]
            samples.append(sf2.Sample(name[:19], to_pcm(y), rate, root, sm.corr,
                                      loop if loop else (max(len(y) - 8, 0), max(len(y) - 1, 0))))
            cache[key] = (len(samples) - 1, loop, extra)
        return cache[key]

    for prog in range(128):
        kmax, att, loop_t = FAMILY[prog >> 3]
        p = presets[(0, prog)]
        zs = sf2.zones(sf, p)
        for z in zs:
            z["stype"] = sf.samples[z["sample"]].stype
        regs = sorted((r[2] for r in choose(pick(zs), kmax)), key=lambda z: root_of(sf, z))
        roots = [root_of(sf, z) for z in regs]
        zones = []
        before = len(samples)
        for i, z in enumerate(regs):
            lo = 0 if i == 0 else (roots[i - 1] + roots[i]) // 2 + 1
            hi = 127 if i == len(regs) - 1 else (roots[i] + roots[i + 1]) // 2
            if lo > hi:
                continue
            si, loop, extra = add_sample(z, zs, roots[i], att, loop_t, prog not in UNPITCHED, False,
                                         f"{prog:03d} {p.name}"[:15] + f" {roots[i]}")
            gz = out_gens(z, roots[i], loop, extra, (lo, hi))
            gz[G["sampleID"]] = si
            zones.append(sf2.Zone(gz))
        instruments.append(sf2.Instrument(p.name, zones))
        out_presets.append(sf2.Preset(p.name, prog, 0, [sf2.Zone({G["instrument"]: len(instruments) - 1})]))
        stats.append((prog, p.name, sum(len(s.pcm) // 2 for s in samples[before:]), len(zones)))

    kit = presets[(128, 0)]
    zs = sf2.zones(sf, kit)
    for z in zs:
        z["stype"] = sf.samples[z["sample"]].stype
    zones = []
    before = len(samples)
    for note in range(DRUM_LO, DRUM_HI + 1):
        here = [z for z in zs if z["key"][0] <= note <= z["key"][1]]
        if not here:
            continue
        z = pick([dict(z, key=(note, note)) for z in here])[0][2]
        root = root_of(sf, z)
        si, loop, extra = add_sample(z, here, root, 0.0, DRUM_LOOP, False, True, f"Drum {note}")
        gz = out_gens(z, root, loop, extra, (note, note))
        gz[G["sampleID"]] = si
        zones.append(sf2.Zone(gz))
    instruments.append(sf2.Instrument("Standard Kit", zones))
    out_presets.append(sf2.Preset("Standard", 0, 128, [sf2.Zone({G["instrument"]: len(instruments) - 1})]))
    stats.append((128, "Standard", sum(len(s.pcm) // 2 for s in samples[before:]), len(zones)))
    if verbose:
        for prog, name, n, nz in stats:
            print(f"  {prog:3d} {name:22s} {nz:2d} zones {n:7d} new sample points")
    info = {
        "isng": "E-mu 10K1",
        "INAM": "FM1-GM mini GM",
        "IENG": "Frank Wen (Fluid R3 GM); reduced for FM1-GM",
        "ICOP": "Fluid R3 GM: Copyright (c) 2000-2002, 2008 Frank Wen, 2008 Toby Smithe. MIT License.",
        "ICMT": "Reduced from Fluid R3 GM by FM1-GM's tools/sf2_reduce.py: one layer, a few mono samples per "
                "program, lower rates, short loops. MIT License (assets/soundfont/LICENSE).",
        "ISFT": "FM1-GM sf2_reduce.py",
    }
    return sf2.SoundFont(info, out_presets, instruments, samples)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src", help="FluidR3_GM.sf2")
    ap.add_argument("out", help="the reduced SoundFont")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    sf = reduce(a.src, a.verbose)
    sf2.write(a.out, sf)
    n = sum(len(s.pcm) // 2 for s in sf.samples)
    print(f"{a.out}: {len(sf.samples)} samples, {n} points ({n * 2 // 1024} KiB as 16-bit, "
          f"about {n // 2 // 1024} KiB as 4-bit ADPCM)")


if __name__ == "__main__":
    main()
