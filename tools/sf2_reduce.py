#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Make FM1-GM's General MIDI instruments (assets/soundfont/fm1gm.sf2 and fm1gm_sustain.json) from FluidR3_GM.sf2.

  tools/sf2_reduce.py FluidR3_GM.sf2 assets/soundfont/fm1gm.sf2

FluidR3 GM (Frank Wen, MIT License) is 141 MiB; the FM-1 has about 440 KiB of flash left after the
firmware. FM1-GM plays each instrument as a hybrid: the attack of one Fluid sample, then a simple
waveform (the shapes of tools/gm_shapes.py, or noise) for as long as the note sounds, no loops.

For each GM program this takes one sample, the one Fluid plays at the instrument's middle key (one
velocity layer, mono), at the lowest rate its spectrum needs, and keeps only its attack plus a short
window after it. Over that window it fits the waveform: the shape whose harmonics are nearest the
sample's, with the phase and level that line it up with the sample, so the firmware crossfades from
the one into the other without a seam. A sample that fades by itself gives its fade to the volume
envelope. The drum kit (bank 128, program 0) keeps notes 35..81, each a trimmed one-shot (cymbals
and other long ones go on as filtered noise).

The SoundFont holds the attacks (16-bit, one-shots: in an ordinary SoundFont player they stop after
their attack); fm1gm_sustain.json holds the waveforms, per sample. tools/gen_wavetable.py turns the two
into the firmware's tables. Needs numpy and scipy (this tool only; the firmware build does not).
"""
import argparse
import json
import sys
from fractions import Fraction
from pathlib import Path

import numpy as np
from scipy.signal import resample_poly, welch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gm_shapes  # noqa: E402
import sf2  # noqa: E402

G = sf2.G
GN_MOD = __import__("re").compile(r"^(modLfo|vibLfo|modEnv)To")
VEL = 100                       # the velocity layer kept (the FM-1's keys play at a fixed velocity, 100 by default)
DRUM_LO, DRUM_HI = 35, 81
OUT_SR = 44100                  # the firmware's output rate (gm.h GM_SR): its noise runs at it

# per GM family (program >> 3): the key whose sample is kept, the attack kept (s)
FAMILY = [
    (60, 0.20),                 # piano
    (72, 0.15),                 # chromatic percussion
    (60, 0.06),                 # organ
    (52, 0.15),                 # guitar
    (40, 0.12),                 # bass
    (60, 0.20),                 # strings
    (60, 0.20),                 # ensemble
    (60, 0.10),                 # brass
    (60, 0.08),                 # reed
    (72, 0.08),                 # pipe
    (60, 0.05),                 # synth lead
    (60, 0.20),                 # synth pad
    (60, 0.15),                 # synth effects
    (60, 0.12),                 # ethnic
    (60, 0.20),                 # percussive
    (60, 0.25),                 # sound effects
]
CENTRE = {43: 40, 58: 40, 70: 48, 72: 84, 73: 72, 47: 48}   # Contrabass, Tuba, Bassoon, Piccolo, Flute, Timpani
UNPITCHED = {119, 120, 121, 122, 124, 125, 126, 127}   # GM programs whose sustain is noise, not a waveform
XF_MIN, XF_MAX, XF_PERIODS = 0.03, 0.10, 4  # the crossfade window: at least this long and this many periods
ONE_SHOT = 0.6                  # a sample without a loop that has died away within this (s) stays a one-shot
RATE_MIN, RATE_MAX = 11025, 22050
DRUM_RATE_MAX = 32000           # hi-hats and cymbals have half their energy above 11 kHz
DRUM_MIN, DRUM_MAX = 0.06, 0.30  # a drum keeps until it is 36 dB down, within these (s)
DRUM_LONG = 0.6                 # a drum that rings longer than this goes on as noise after DRUM_ATT
DRUM_ATT = 0.15
BELL = 1.0                      # an inharmonic instrument (a glockenspiel) rings out for up to this (s)
HARM = 32                       # harmonics fitted


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



def ring_time(x, rate):
    """seconds until x is 36 dB below its peak (10 ms windows)"""
    hop = max(rate // 100, 1)
    n = len(x) // hop
    if n < 2:
        return 0.0
    db = 10 * np.log10(np.mean(x[:n * hop].reshape(n, hop) ** 2, axis=1) + 1e-12)
    pk = int(np.argmax(db))
    below = np.nonzero(db[pk:] < db[pk] - 36)[0]
    return (pk + (below[0] if len(below) else n - pk)) * hop / rate


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



def to_pcm(y):
    return (np.clip(np.round(y * 32767.0), -32768, 32767).astype("<i2")).tobytes()


# ------------------------------------------------------------------ the sustain fit ---

def harmonics(seg, cps, n):
    """seg as a sum of harmonics of cps (cycles a point) by least squares: the complex amplitudes
    c[h - 1] = a e^(i p) of a sin(2 pi h cps j + p), and the share of seg's energy they leave out"""
    j = np.arange(len(seg))
    h = np.arange(1, n + 1)
    ph = 2 * np.pi * np.outer(j, h) * cps
    m = np.hstack([np.sin(ph), np.cos(ph)])
    s = seg - seg.mean()
    coef, *_ = np.linalg.lstsq(m, s, rcond=None)
    res = s - m @ coef
    return coef[:n] + 1j * coef[n:], float(np.dot(res, res) / (np.dot(s, s) + 1e-18))


def refine_cps(seg, cps, n):
    """the pitch (cycles a point) near cps that the first harmonics fit best"""
    best = None
    for d in np.linspace(-0.03, 0.03, 121):
        c = cps * (1 + d)
        r = harmonics(seg, c, min(n, 8))[1]
        if best is None or r < best[0]:
            best = (r, c)
    return best[1]


def shape_coefs(name, n):
    s = np.zeros(n, complex)
    for h, a, p in gm_shapes.SHAPES[name]:
        if h <= n:
            s[h - 1] += a * np.exp(1j * p)
    return s


PHI = np.arange(512) / 512


def fit_shape(c):
    """the shape nearest c (its harmonics' levels first, their phases second), and the phase phi
    (turns) and gain that line it up: c ~ gain * S_h e^(2 pi i h phi)"""
    n = len(c)
    e = np.sum(np.abs(c) ** 2)
    mc = np.abs(c) / np.sqrt(e)
    rot = np.exp(-2j * np.pi * np.outer(PHI, np.arange(1, n + 1)))
    best = None
    for name in gm_shapes.NAMES if n > 1 else ["sine"]:    # one harmonic measured: no more to go on
        s = shape_coefs(name, n)
        es = np.sum(np.abs(s) ** 2)
        dm = float(np.sum((mc - np.abs(s) / np.sqrt(es)) ** 2))
        d = np.real(rot @ (np.conj(s) * c))
        k = int(np.argmax(d))
        rc = 1 - d[k] ** 2 / (es * e) if d[k] > 0 else 1.0
        score = dm + 0.3 * rc
        if best is None or score < best[0]:
            best = (score, name, float(PHI[k]), float(d[k] / es), dm, rc)
    return best[1:]


def noise_fit(seg, rate):
    """filtered noise like seg: the high- and low-pass corners (Hz) and the gain on the firmware's
    noise (uniform -1..1, then one-pole low-pass and high-pass at OUT_SR)"""
    f, p = welch(seg, rate, nperseg=min(1024, len(seg)))
    c = np.cumsum(p)
    hp = float(max(f[np.searchsorted(c, 0.10 * c[-1])], 30.0))
    lp = float(max(f[min(np.searchsorted(c, 0.85 * c[-1]), len(f) - 1)], hp * 1.5))
    from scipy.signal import lfilter
    w = np.random.default_rng(1).uniform(-1, 1, 1 << 16)
    al, ah = 1 - np.exp(-2 * np.pi * lp / OUT_SR), 1 - np.exp(-2 * np.pi * hp / OUT_SR)
    lo = lfilter([al], [1, al - 1], w)
    y = lo - lfilter([ah], [1, ah - 1], lo)
    rms = np.sqrt(np.mean(y[4096:] ** 2))
    return hp, lp, float(np.sqrt(np.mean(seg ** 2)) / rms)


def unroll(x, ls, le, need):
    """a looped sample played on to need points"""
    if len(x) >= need or not 0 <= ls < le <= len(x):
        return x
    reps = int(np.ceil((need - le) / (le - ls))) + 1
    return np.concatenate([x[:le]] + [x[ls:le]] * reps)[:need]


def envelope_fade(decay, g, extra, att):
    """a sample's own fade (decay, timecents per 100 dB), now that a waveform holds its level, goes
    into the volume envelope: from the end of the attack on, added to the envelope's own decay when
    that falls to silence (and is not held off for seconds: FluidR3's drums hold for longer than their
    samples last), else instead of it"""
    if decay is None:
        return
    rate = 100.0 / 2 ** (decay / 1200)                      # dB / s
    if g.get(G["sustainVolEnv"], 0) >= 960 and 2 ** (g.get(G["holdVolEnv"], -12000) / 1200) < 2.0:
        rate += 100.0 / 2 ** (g.get(G["decayVolEnv"], -12000) / 1200)
    extra[G["decayVolEnv"]] = int(round(1200 * np.log2(100.0 / rate)))
    extra[G["sustainVolEnv"]] = 1000
    extra[G["holdVolEnv"]] = int(round(1200 * np.log2(max(att, 0.001))))


def envelope_settle(x, rate, att, ls, g, extra):
    """a looped sample that falls from its attack to a lower level where its loop holds: the fall
    (from att to ls) goes into the volume envelope, as a decay to a sustain level"""
    w = int(0.02 * rate)
    drop = 10 * np.log10((np.mean(x[int(att * rate):int(att * rate) + w] ** 2) + 1e-12) /
                         (np.mean(x[ls - w:ls] ** 2) + 1e-12))
    decay = decay_tc(x[:ls], rate, att)
    if decay is None or drop < 1.0:
        return
    if g.get(G["sustainVolEnv"], 0) >= 960:                 # the envelope fades it out anyway
        envelope_fade(decay, g, extra, att)
        return
    extra[G["decayVolEnv"]] = decay
    extra[G["sustainVolEnv"]] = int(min(1440, g.get(G["sustainVolEnv"], 0) + round(drop * 10)))
    extra[G["holdVolEnv"]] = int(round(1200 * np.log2(max(att, 0.001))))


def cut(y, n, nr):
    """y cut to n points, the cut faded"""
    y = y[:n].copy()
    nf = int(len(y) * 0.3) if n < len(y) else min(len(y), int(0.004 * nr))
    if nf:
        y[-nf:] *= np.linspace(1.0, 0.0, nf) ** 2
    return y


def process(sf, z, zs, root, att, pitched, drum):
    """the sample kept (floats), its rate, generators it adds, and its sustain (or None)"""
    x, rate, (ls0, le0), mixed = region_sample(sf, z, zs)
    g = z["gens"]
    # the sample's own pitch: its root key, less the correction a player adds (the zone's tuning
    # moves where it plays, not what it holds)
    f0 = 440.0 * 2 ** ((root - 69) / 12 - sf.samples[z["sample"]].corr / 1200) if pitched else None
    looped = bool(g.get(G["sampleModes"], 0) & 1 and 0 <= ls0 < le0 <= len(x))
    extra = {} if drum and not mixed else {G["pan"]: 0}     # mono now: no pan (melodic: the channel pans)
    ring = ring_time(x, rate) if not looped else 1e9
    want = band_rate(x, rate, f0, DRUM_RATE_MAX if drum else RATE_MAX)
    if drum:
        long = ring > DRUM_LONG
        att, pitched, f0 = DRUM_ATT, False, None
    else:
        long = ring > ONE_SHOT
    if not long:                                            # dies away by itself: a one-shot
        keep = min(max(ring, DRUM_MIN), DRUM_MAX if drum else ONE_SHOT) * 1.15
        xr, nr, _ = resample(x[:int(keep * rate) + 64], rate, want)
        return cut(xr, int(keep * nr), nr), nr, extra, None
    xf = min(max(XF_MIN, XF_PERIODS / f0 if f0 else 0.0), XF_MAX)
    need = int((att + xf + 0.02) * rate)
    if looped:
        x = unroll(x, ls0, le0, need)
    xr, nr, _ = resample(x[:need + 64], rate, want)
    xs, xe = int(round(att * nr)), int(round((att + xf) * nr))
    xe = min(xe, len(xr) - 2)
    seg = xr[xs:xe]
    if drum:                                                # a tom, a bell: its strongest partial as the pitch
        f, p = welch(seg, nr, nperseg=len(seg), nfft=1 << 15)
        f0 = float(f[np.argmax(p[1:]) + 1])
    sus = None
    if f0:
        # the zone's tuning either corrects the sample's pitch (FluidR3's trumpet is an E played as
        # a C) or transposes the instrument (its drawbar organ sounds two octaves down): whichever
        # the sample's harmonics bear out
        tune = g.get(G["coarseTune"], 0) * 100 + g.get(G["fineTune"], 0)
        best = None
        for f in {f0, f0 * 2 ** (-tune / 1200)}:
            cps = refine_cps(seg, f / nr, HARM)
            n = max(1, min(HARM, int(0.45 / cps)))
            c, resid = harmonics(seg, cps, n)
            if best is None or resid < best[0]:
                best = (resid, cps, n, c)
        resid, cps, n, c = best
        if not drum and 1 - resid < 0.3:                    # a bell: no pitch to hold, so it rings out
            keep = min(ring, BELL)
            xr, nr, _ = resample(x[:int(keep * rate) + 64], rate, want)
            return cut(xr, int(keep * nr), nr), nr, extra, None
        if 1 - resid >= (0.6 if drum else 0.3):             # else not a pitch it can hold: noise
            name, phi, _, dm, rc = fit_shape(c)
            # the level: the harmonics', and half the rest (an inharmonic sound's partials die away
            # during the crossfade; meeting them halfway keeps its loudness and its pitch)
            e = np.sum(np.abs(c) ** 2) / 2 * (1 + 0.5 * resid / max(1 - resid, 1e-3))
            gain = float(np.sqrt(e / (np.sum(np.abs(shape_coefs(name, n)) ** 2) / 2)))
            sus = dict(shape=name, ph0=float((phi - xs * cps) % 1.0), cps=float(cps), gain=gain,
                       fit=dict(harmonic=round(1 - resid, 3), levels=round(dm, 3), phases=round(rc, 3)))
    if sus is None:
        hp, lp, gain = noise_fit(seg, nr)
        sus = dict(shape="noise", hp=round(hp, 1), lp=round(lp, 1), gain=gain)
    sus.update(xs=xs, xe=xe)
    if not looped:
        envelope_fade(decay_tc(x, rate, att), g, extra, att)
    elif ls0 > (att + 0.1) * rate:                          # looped late: it settles before its loop holds
        envelope_settle(x, rate, att, ls0, g, extra)
    return xr[:xe + 2], nr, extra, sus


def out_gens(z, root, extra, key):
    keep = {k: v for k, v in z["gens"].items() if k not in sf2.NOT_ADDITIVE and k not in sf2.RANGE and
            k != G["instrument"] and sf2.DEFAULTS.get(k, 0) != v and k not in DROP}
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
    return keep


# FluidR3's vibrato and tremolo: gone (a note's pitch and level stay put; the mod wheel still adds vibrato)
DROP = {G[n] for n in ("vibLfoToPitch", "modLfoToPitch", "modLfoToVolume", "modLfoToFilterFc", "delayVibLFO",
                       "freqVibLFO", "delayModLFO", "freqModLFO")}


def centre_zone(sf, zs, key):
    """the zone that sounds for key (or the nearest key the program has)"""
    regions = pick(zs)
    r = min(regions, key=lambda r: 0 if r[0] <= key <= r[1] else min(abs(key - r[0]), abs(key - r[1])))
    return r[2]


def reduce(src, verbose=False):
    sf = sf2.read(src)
    presets = {(p.bank, p.program): p for p in sf.presets}
    samples, cache, instruments, out_presets, sustain = [], {}, [], [], {}
    stats = []

    def add_sample(z, zs, root, att, pitched, drum, name):
        key = (z["sample"], root, att, drum, tuple(sorted((k, v) for k, v in z["gens"].items()
                                                          if k in sf2.NOT_ADDITIVE or k in (G["coarseTune"], G["fineTune"]))))
        if key not in cache:
            y, rate, extra, sus = process(sf, z, zs, root, att, pitched, drum)
            sm = sf.samples[z["sample"]]
            name = name[:19]
            samples.append(sf2.Sample(name, to_pcm(y), rate, root, sm.corr, (max(len(y) - 8, 0), max(len(y) - 1, 0))))
            if sus:
                sustain[name] = sus
            cache[key] = (len(samples) - 1, extra, sus)
        return cache[key]

    for prog in range(128):
        ck, att = FAMILY[prog >> 3]
        ck = CENTRE.get(prog, ck)
        p = presets[(0, prog)]
        zs = sf2.zones(sf, p)
        for z in zs:
            z["stype"] = sf.samples[z["sample"]].stype
        z = centre_zone(sf, zs, ck)
        root = root_of(sf, z)
        si, extra, sus = add_sample(z, zs, root, att, prog not in UNPITCHED, False, f"{prog:03d} {p.name}"[:15] + f" {root}")
        gz = out_gens(z, root, extra, (0, 127))
        gz[G["sampleID"]] = si
        instruments.append(sf2.Instrument(p.name, [sf2.Zone(gz)]))
        out_presets.append(sf2.Preset(p.name, prog, 0, [sf2.Zone({G["instrument"]: len(instruments) - 1})]))
        stats.append((prog, p.name, len(samples[si].pcm) // 2, sus))

    kit = presets[(128, 0)]
    zs = sf2.zones(sf, kit)
    for z in zs:
        z["stype"] = sf.samples[z["sample"]].stype
    zones = []
    for note in range(DRUM_LO, DRUM_HI + 1):
        here = [z for z in zs if z["key"][0] <= note <= z["key"][1]]
        if not here:
            continue
        z = pick([dict(z, key=(note, note)) for z in here])[0][2]
        root = root_of(sf, z)
        si, extra, sus = add_sample(z, here, root, 0.0, False, True, f"Drum {note}")
        gz = out_gens(z, root, extra, (note, note))
        gz[G["sampleID"]] = si
        zones.append(sf2.Zone(gz))
        stats.append((note, f"drum {note}", len(samples[si].pcm) // 2, sus))
    instruments.append(sf2.Instrument("Standard Kit", zones))
    out_presets.append(sf2.Preset("Standard", 0, 128, [sf2.Zone({G["instrument"]: len(instruments) - 1})]))
    if verbose:
        for prog, name, n, sus in stats:
            d = ""
            if sus and sus["shape"] == "noise":
                d = f"noise {sus['hp']:.0f}..{sus['lp']:.0f} Hz"
            elif sus:
                f = sus["fit"]
                d = f"{sus['shape']:10s} harm {f['harmonic']:.2f} lv {f['levels']:.2f} ph {f['phases']:.2f}"
            print(f"  {prog:3d} {name:22s} {n:6d} pts  {d}")
    info = {
        "isng": "E-mu 10K1",
        "INAM": "FM1-GM mini GM",
        "IENG": "Frank Wen (Fluid R3 GM); reduced for FM1-GM",
        "ICOP": "Fluid R3 GM: Copyright (c) 2000-2002, 2008 Frank Wen, 2008 Toby Smithe. MIT License.",
        "ICMT": "Reduced from Fluid R3 GM by FM1-GM's tools/sf2_reduce.py: one mono attack sample per program "
                "(its sustain, a simple waveform, is in fm1gm_sustain.json). MIT License (assets/soundfont/LICENSE).",
        "ISFT": "FM1-GM sf2_reduce.py",
    }
    return sf2.SoundFont(info, out_presets, instruments, samples), sustain


def sustain_path(sf2_path):
    p = Path(sf2_path)
    return p.with_name(p.stem + "_sustain.json")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src", help="FluidR3_GM.sf2")
    ap.add_argument("out", help="the reduced SoundFont (its sustains go next to it, NAME_sustain.json)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    sf, sustain = reduce(a.src, a.verbose)
    sf2.write(a.out, sf)
    with open(sustain_path(a.out), "w") as f:
        json.dump({"about": "FM1-GM: each sample's sustain (tools/sf2_reduce.py). xs..xe: the crossfade from "
                            "the sample into it (points); cps: cycles a point; ph0: its phase at point 0 (turns); "
                            "gain: on the sample's own scale; noise: one-pole high-pass and low-pass (Hz)",
                   "samples": sustain}, f, indent=1, sort_keys=True)
        f.write("\n")
    n = sum(len(s.pcm) // 2 for s in sf.samples)
    print(f"{a.out}: {len(sf.samples)} samples, {n} points ({n * 2 // 1024} KiB as 16-bit, "
          f"about {n // 2 // 1024} KiB as 4-bit ADPCM); {len(sustain)} sustains")


if __name__ == "__main__":
    main()
