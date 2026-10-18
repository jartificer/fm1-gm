#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Make FM1-GM's small General MIDI SoundFont (assets/soundfont/fm1gm.sf2) from FluidR3_GM.sf2.

  tools/sf2_reduce.py FluidR3_GM.sf2 assets/soundfont/fm1gm.sf2

FluidR3 GM (Frank Wen, MIT License) is 141 MiB; the FM-1 has about 440 KiB of flash left after the
firmware. For each GM program this keeps one sample, the one Fluid plays at the instrument's middle
key (one velocity layer, mono), for the whole keyboard; at the lowest rate its spectrum needs; and
only its attack and a loop after it. The loops are made to be seamless:

- long (0.12-0.25 s, by family), so what little a loop still changes is slow, not a buzz;
- a whole number of periods exactly: the sample is resampled by a hair so that they come out to a
  whole number of points, and the loop replays in step with itself;
- level: the sample's fade across the loop is taken out (the volume envelope fades it instead);
- the seam crossfaded, over the loop's last 7-30 % (searched: a long crossfade over sides that do not
  match is an echo), with what comes just before its start; the length
  and start searched for where the two match best (a recorded vibrato or chorus wants whole cycles
  of its own), and the crossfade's law keeping the level steady for however alike they are (linear
  for a periodic sound, equal power for noise).
- tuned: its pitch, measured across the loop, sits on its note (to the semitone the zone's tuning
  means).

A few programs have an attack and a loop of their own (TIMING); one mixes its layers into its sample
(LAYERED). Where the recording ends in a short loop of its own before an attack and a loop fit, the
whole recording is kept and its own loop holds.

Sounds that die away within about half a second keep their whole sample, with no loop; so does an
inharmonic bell. The drum kit (bank 128, program 0) keeps notes 35..81, each trimmed (cymbals and
other long drums looped the same way, and faded). FluidR3's vibrato and tremolo are dropped.

The result is an ordinary SoundFont (16-bit, the reduced rates): play it in any SoundFont player.
tools/gen_wavetable.py turns it into the firmware's tables (4-bit ADPCM, about a quarter of its
size). Needs numpy and scipy (this tool only; the firmware build does not).
"""
import argparse
import sys
from fractions import Fraction
from pathlib import Path

import numpy as np
from scipy.signal import resample as fft_resample
from scipy.signal import resample_poly, welch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sf2  # noqa: E402

G = sf2.G
GN_MOD = __import__("re").compile(r"^(modLfo|vibLfo|modEnv)To")
VEL = 100                       # the velocity layer kept (the FM-1's keys play at a fixed velocity, 100 by default)
DRUM_LO, DRUM_HI = 35, 81

# per GM family (program >> 3): the key whose sample is kept, the attack kept (s), the loop (s)
FAMILY = [
    (60, 0.10, 0.20),           # piano
    (72, 0.10, 0.15),           # chromatic percussion
    (60, 0.05, 0.15),           # organ
    (52, 0.10, 0.15),           # guitar
    (40, 0.10, 0.20),           # bass
    (60, 0.10, 0.20),           # strings
    (60, 0.10, 0.20),           # ensemble
    (60, 0.08, 0.15),           # brass
    (60, 0.07, 0.15),           # reed
    (72, 0.07, 0.15),           # pipe
    (60, 0.05, 0.12),           # synth lead
    (60, 0.10, 0.25),           # synth pad
    (60, 0.10, 0.20),           # synth effects
    (60, 0.10, 0.15),           # ethnic
    (60, 0.12, 0.15),           # percussive
    (60, 0.15, 0.25),           # sound effects
]
CENTRE = {43: 40, 58: 40, 70: 48, 72: 84, 73: 72, 47: 48}   # Contrabass, Tuba, Bassoon, Piccolo, Flute, Timpani
# programs whose recording wants an attack and a loop (s) of its own: Harmonica and Alto Sax start a
# little flat and slide up to pitch (a loop there replays the slide), Slap Bass 1 changes its tone for
# half a second after the slap, Contrabass bows on with a slow swell, and Alto Sax's vibrato wants a
# whole cycle; Fifth Sawtooth's loop holds one beat of its detuned layers (LAYERED)
TIMING = {22: (0.17, 0.15), 36: (0.45, 0.20), 43: (0.15, 0.40), 65: (0.28, 0.22), 86: (0.05, 0.80)}
LAYERED = {86}                  # programs whose layers (on the centre key) are mixed into their sample
STATIC_LOOP = 0.04              # the loop when the recording is over before a loop of its own fits (s)
UNPITCHED = {119, 120, 121, 122, 124, 125, 126, 127}   # GM programs whose loops are noise, not periods
NOISE_LOOP = 0.25               # an unpitched loop (s): long, so it does not sound like a repeat
ONE_SHOT = 0.6                  # a sample without a loop that has died away within this (s) stays a one-shot
ONE_SHOT_KEEP = 0.5             # and keeps at most this (s)
BELL = 0.6                      # an inharmonic instrument (a glockenspiel) rings out for up to this (s)
XF_SHARE = 0.3                  # the seam's crossfade, a share of the loop, at most
XF_SHARES = (0.3, 0.15, 0.07)   # the shares tried
XF_MIN = 0.006                  # and at least this (s)
XF_REF = 0.015                  # the crossfade (s) that least shows a mismatch, both as an echo and as a jump
SEARCH = 0.03                   # the loop start is searched over this (s) after the attack
RATE_MIN, RATE_MAX = 11025, 16000
DRUM_RATE_MAX = 28000           # hi-hats and cymbals have half their energy above 11 kHz
DRUM_MIN, DRUM_MAX = 0.06, 0.24  # a drum keeps until it is 36 dB down, within these (s)
DRUM_LONG = 0.6                 # a drum that rings longer than this is looped after DRUM_ATT
DRUM_ATT, DRUM_LOOP = 0.10, 0.20
DRUM_LONG_RATE = 20000          # a looped drum's rate, at most (its loop is noise: it needs length more than top end)


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


# ------------------------------------------------------------------ pitch ---

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


# ------------------------------------------------------------------ loops ---

def level_slope(x, a, b, hop):
    """the slope (log amplitude a point) of x's level over [a, b), from RMS in windows of hop"""
    n = (b - a) // hop
    if n < 3:
        return 0.0
    r = np.sqrt(np.mean(x[a:a + n * hop].reshape(n, hop) ** 2, axis=1) + 1e-12)
    t = a + hop * (np.arange(n) + 0.5)
    return float(np.polyfit(t, np.log(r), 1)[0])


def match_all(x, n, f, lo, hi):
    """for each loop start s in [lo, hi): how alike the loop's last f points (before s + n) are to the
    f points before s, the crossfade's two halves (correlation, by running sums)"""
    m = len(x) - n
    pr = np.concatenate([[0.0], np.cumsum(x[:m] * x[n:])])
    e = np.concatenate([[0.0], np.cumsum(x * x)])
    s = np.arange(lo, hi)
    dot = pr[s] - pr[s - f]
    ea, eb = e[s + n] - e[s + n - f], e[s] - e[s - f]
    return dot / (np.sqrt(ea * eb) + 1e-12)


def make_loop(x, nr, f0, s0, loop_t, exact=False):
    """x (at rate nr) cut to a seamless loop after point s0 (the attack's end): returns the sample
    (attack + loop), its rate (a hair from nr when the loop was made period-exact), the loop (start,
    end) and how alike the crossfade's two halves were. The crossfade's other half is what comes
    just before the loop, after the attack.

    The loop's length (a whole number of periods, within a quarter of loop_t: a recorded vibrato or
    chorus wants whole cycles of its own; exact: f0 is the loop's own pitch, one period a loop) and start (within SEARCH) are those whose two halves match
    best; then the sample is resampled by a hair so that the periods are whole points."""
    rate = nr
    hop = max(int(nr * 0.01), 16)
    n0 = loop_t * nr
    if f0:
        # whole periods of the pitch where the loop starts, and of its pitch across the loop (they differ
        # by a few cents where it slides or has a vibrato, enough to put a long loop's seam out of step)
        if exact:
            ps = {nr / f0}
        else:
            ps = {1.0 / refine_cps(x[s0:s0 + min(int(t * nr), len(x) - s0)], f0 / nr, 8)
                  for t in (0.1, max(0.1, 1.3 * loop_t))}
        lens = []
        for p in ps:
            m0 = max(1, int(0.75 * n0 / p))
            lens += [(m * p, m) for m in range(m0, max(m0, int(1.25 * n0 / p)) + 1)]
    else:
        lens = [(float(n), 0) for n in range(int(0.75 * n0), int(1.25 * n0) + 1, max(1, int(nr * 0.001)))]
    need = int(s0 + 1.25 * n0 * (1 + XF_SHARE) + SEARCH * nr + 4)
    if len(x) < need:
        x = np.pad(x, (0, need - len(x)))
    # the crossfade too: over two sides that do not match, a long one is heard as an echo (the loop's end
    # and what came before its start, both at once), a short one as a jump; the cost of a mismatch (1 - rho)
    # is least at XF_REF, and a longer crossfade only pays where the two sides are alike
    ref = XF_REF * nr
    best = None
    for nf, m in lens:
        n = int(round(nf))
        for share in XF_SHARES:
            f = max(int(n * share), int(XF_MIN * nr))
            if f > n * XF_SHARE:
                continue
            lo = s0 + int(n * XF_SHARE)
            rho = match_all(x, n, f, lo, lo + int(SEARCH * nr) + 1)
            i = int(np.argmax(rho))
            cost = (1 - rho[i]) * (f / ref + ref / f) / 2 + 0.05 * abs(nf / n0 - 1) - 0.002 * f / n
            if best is None or cost < best[0]:
                best = (cost, nf, lo + i, f / n)
    _, nf, s, share = best
    if f0:                                                  # the periods to whole points
        r = round(nf) / nf
        if abs(r - 1) > 1e-7:
            x = fft_resample(x, int(round(len(x) * r)))
            rate, s = nr * r, int(round(s * r))
    n = int(round(nf * (rate / nr)))
    f = int(round(n * share))
    # the fade across the loop, taken out from the crossfade's other half on
    k = level_slope(x, s, s + n, hop)
    k = max(min(k, np.log(4) / n), -np.log(4) / n)          # at most 12 dB across the loop
    a = x[s + n - f:s + n] * np.exp(-k * (np.arange(s + n - f, s + n) - s))
    b = x[s - f:s] * np.exp(-k * (np.arange(s - f, s) - s))
    rho = float(np.dot(a, b) / (np.sqrt(np.dot(a, a) * np.dot(b, b)) + 1e-12))
    y = x[:s + n].copy()
    y[s:] *= np.exp(-k * np.arange(n))
    t = (np.arange(f) + 0.5) / f
    g = 1.0 / np.sqrt((1 - t) ** 2 + t ** 2 + 2 * max(rho, 0.0) * t * (1 - t))
    y[s + n - f:] = (1 - t) * g * a + t * g * b
    return y, rate, (s, s + n), rho


def unroll(x, ls, le, need):
    """a looped sample played on to need points"""
    if len(x) >= need or not 0 <= ls < le <= len(x):
        return x
    reps = int(np.ceil((need - le) / (le - ls))) + 1
    return np.concatenate([x[:le]] + [x[ls:le]] * reps)[:need]


def envelope_fade(decay, g, extra, att):
    """a sample's own fade (decay, timecents per 100 dB), now that a loop holds its level, goes
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


def layered(sf, z, zs, key, f0, rate):
    """z's sample (a waveform's few cycles, about f0 Hz) mixed with the other zones that sound on key,
    each at its pitch relative to z, at z's rate and level; and the loop (s) that holds the mix.
    FluidR3's Fifth Sawtooth is a saw and a saw a fourth below, each a stereo pair 5 cents apart, so
    that they beat slowly: each layer's pitch is moved by a hair so that they all repeat together
    after one beat of the pair (about 0.8 s at 440 Hz), and the loop is that"""
    here = [o for o in zs if o["key"][0] <= key <= o["key"][1]]
    zsm = sf.samples[z["sample"]]

    def native(o):                                          # the waveform's pitch (Hz): whole cycles in its loop
        sm = sf.samples[o["sample"]]
        xo = unroll(pcm_float(sm), sm.loop[0], sm.loop[1], sm.rate)
        nominal = f0 * 2 ** (100 * (root_of(sf, o) - root_of(sf, z)) / 1200)
        n = sm.loop[1] - sm.loop[0]
        return round(n * refine_cps(xo[:sm.rate // 10], nominal / sm.rate, 8)) * sm.rate / n, sm

    def sounds(o, fo):                                      # its pitch as played, as a share of z's
        g = o["gens"]
        t = g.get(G["coarseTune"], 0) * 100 + g.get(G["fineTune"], 0) + sf.samples[o["sample"]].corr
        return fo * 2 ** ((t + 100 * (key - root_of(sf, o))) / 1200)
    nat = [native(o) for o in here]
    fz = nat[here.index(z)][0]
    rel = [sounds(o, fo) / sounds(z, fz) for o, (fo, _) in zip(here, nat)]
    beat = min((abs(np.log2(r1 / r2)) for r1 in rel for r2 in rel if abs(np.log2(r1 / r2)) > 1e-4), default=0.0)
    cycles = int(round(1 / (2 ** beat - 1))) if beat else int(round(0.5 * fz))   # of z, in the loop
    n = int(np.ceil((0.8 + 1.6 * cycles / fz) * rate))
    y = np.zeros(n)
    for r, (fo, sm) in zip(rel, nat):
        want = round(r * cycles) / cycles * fz / rate       # cycles a point of the mix
        step = want / (fo / sm.rate)                        # points of o a point of the mix
        xo = unroll(pcm_float(sm), sm.loop[0], sm.loop[1], int(n * step) + 2)
        y += np.interp(np.arange(n) * step, np.arange(len(xo)), xo)
    x = unroll(pcm_float(zsm), zsm.loop[0], zsm.loop[1], n)
    return y * np.sqrt(np.mean(x ** 2) / (np.mean(y ** 2) + 1e-12)), cycles / fz, fz


def process(sf, z, zs, root, att, loop_t, pitched, drum, layer_key=None):
    """the sample kept (floats), its rate, its loop (or None), generators it adds, the loop's match,
    the loop's pitch (Hz)"""
    x, rate, (ls0, le0), mixed = region_sample(sf, z, zs)
    g = z["gens"]
    # the sample's own pitch: its root key, less the correction a player adds; or, when the zone's
    # tuning corrects the sample (FluidR3's trumpet is an E played as a C), that
    f0 = 440.0 * 2 ** ((root - 69) / 12 - sf.samples[z["sample"]].corr / 1200) if pitched else None
    looped = bool(g.get(G["sampleModes"], 0) & 1 and 0 <= ls0 < le0 <= len(x))
    extra = {} if drum and not mixed else {G["pan"]: 0}     # mono now: no pan (melodic: the channel pans)
    ring = ring_time(x, rate) if not looped else 1e9
    want = band_rate(x, rate, f0, DRUM_RATE_MAX if drum else RATE_MAX)
    if drum:
        att, loop_t, f0 = DRUM_ATT, DRUM_LOOP, None
        long = ring > DRUM_LONG
        if long:
            want = min(want, DRUM_LONG_RATE)
    else:
        long = ring > ONE_SHOT
    if not long:                                            # dies away by itself: a one-shot
        keep = min(max(ring, DRUM_MIN) * 1.15, DRUM_MAX * 1.15 if drum else ONE_SHOT_KEEP)
        xr, nr, _ = resample(x[:int(keep * rate) + 64], rate, want)
        return cut(xr, int(keep * nr), nr), nr, None, extra, None, None
    if not f0 and not drum:
        loop_t = max(loop_t, NOISE_LOOP)
    if layer_key is not None:
        x, loop_t, fz = layered(sf, z, zs, layer_key, f0, rate)
    elif looped and le0 - ls0 < 0.5 * loop_t * rate and ls0 < (att + 0.75 * loop_t) * rate:
        # the recording is over (into a short loop of its own) before a loop of ours fits after the
        # attack: the whole recording is the attack, and its own loop holds, as FluidR3 plays it (a loop
        # of ours would replay the recording's end and its loop by turns, a flutter)
        att, loop_t = ls0 / rate, STATIC_LOOP
    need = int((att + loop_t * (1 + XF_SHARE) + SEARCH + 0.05) * rate)
    if looped and layer_key is None:
        x = unroll(x, ls0, le0, need)
    xr, nr, _ = resample(x[:need + 64], rate, want)
    if f0 and layer_key is None:
        seg = xr[int(att * nr):int(att * nr) + int(0.1 * nr)]
        tune = g.get(G["coarseTune"], 0) * 100 + g.get(G["fineTune"], 0)
        best = None
        for f in {f0, f0 * 2 ** (-tune / 1200)}:
            cps = refine_cps(seg, f / nr, 8)
            resid = harmonics(seg, cps, max(1, min(32, int(0.45 / cps))))[1]
            if best is None or resid < best[0]:
                best = (resid, cps * nr)
        resid, f0 = best
        if 1 - resid < 0.3:                                 # a bell: no period to loop, so it rings out
            keep = min(ring, BELL)
            xr, nr, _ = resample(x[:int(keep * rate) + 64], rate, want)
            return cut(xr, int(keep * nr), nr), nr, None, extra, None, None
    if layer_key is not None:                               # the mix repeats once a loop, exactly
        y, nr2, loop, rho = make_loop(xr, nr, 1 / loop_t, int(round(att * nr)), loop_t, exact=True)
        fl = fz
    else:
        y, nr2, loop, rho = make_loop(xr, nr, f0, int(round(att * nr)), loop_t)
        fl = None
        if f0:                                              # the sustain's pitch, across the whole loop
            ls, le = loop
            fl = refine_cps(y[ls:le], f0 / nr2, 8) * nr2
    if not looped:
        envelope_fade(decay_tc(x, rate, att), g, extra, att)
    elif ls0 > (att + 0.1) * rate:                          # looped late: it settles before its loop holds
        envelope_settle(x, rate, att, ls0, g, extra)
    return y, nr2, loop, extra, rho, fl


def out_gens(z, root, loop, extra, key):
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
    if loop:
        keep[G["sampleModes"]] = 1
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
    samples, cache, instruments, out_presets = [], {}, [], []
    stats = []
    tuned = {}

    def add_sample(z, zs, root, att, loop_t, pitched, drum, name, layer_key=None):
        key = (z["sample"], root, att, loop_t, drum, layer_key, tuple(sorted(
            (k, v) for k, v in z["gens"].items() if k in sf2.NOT_ADDITIVE or k in (G["coarseTune"], G["fineTune"]))))
        if key not in cache:
            y, rate, loop, extra, rho, fl = process(sf, z, zs, root, att, loop_t, pitched, drum, layer_key)
            sm = sf.samples[z["sample"]]
            # the rate is whole Hz in a SoundFont: what a period-exact loop's hair of resampling leaves
            # over goes into the pitch correction
            corr = sm.corr + 1200 * np.log2(rate / round(rate))
            if fl:
                # the loop in tune at its root key, to the nearest semitone the zone's tuning means: some
                # of FluidR3's samples are a few cents off (its 440 Hz saw is a period of 101 points, 13
                # cents flat), or settle a little off the pitch their attack starts at
                g = z["gens"]
                d = 1200 * np.log2(fl / (440.0 * 2 ** ((root - 69) / 12))) + sm.corr + \
                    g.get(G["coarseTune"], 0) * 100 + g.get(G["fineTune"], 0)
                d -= 100 * round(d / 100)
                corr -= d
                tuned[len(samples)] = d
            if loop:                                        # 8 points after the loop end (SoundFont 7.10)
                y = np.concatenate([y[:loop[1]], y[loop[0]:loop[0] + 8]])
            samples.append(sf2.Sample(name[:19], to_pcm(y), int(round(rate)), root, int(round(corr)),
                                      loop if loop else (max(len(y) - 8, 0), max(len(y) - 1, 0))))
            cache[key] = (len(samples) - 1, loop, extra, rho)
        return cache[key]

    for prog in range(128):
        ck, att, loop_t = FAMILY[prog >> 3]
        ck = CENTRE.get(prog, ck)
        att, loop_t = TIMING.get(prog, (att, loop_t))
        p = presets[(0, prog)]
        zs = sf2.zones(sf, p)
        for z in zs:
            z["stype"] = sf.samples[z["sample"]].stype
        z = centre_zone(sf, zs, ck)
        root = root_of(sf, z)
        si, loop, extra, rho = add_sample(z, zs, root, att, loop_t, prog not in UNPITCHED, False,
                                          f"{prog:03d} {p.name}"[:15] + f" {root}", ck if prog in LAYERED else None)
        gz = out_gens(z, root, loop, extra, (0, 127))
        gz[G["sampleID"]] = si
        instruments.append(sf2.Instrument(p.name, [sf2.Zone(gz)]))
        out_presets.append(sf2.Preset(p.name, prog, 0, [sf2.Zone({G["instrument"]: len(instruments) - 1})]))
        stats.append((prog, p.name, len(samples[si].pcm) // 2, loop, rho, samples[si].rate, tuned.get(si)))

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
        si, loop, extra, rho = add_sample(z, here, root, 0.0, 0.0, False, True, f"Drum {note}")
        gz = out_gens(z, root, loop, extra, (note, note))
        gz[G["sampleID"]] = si
        zones.append(sf2.Zone(gz))
        stats.append((note, f"drum {note}", len(samples[si].pcm) // 2, loop, rho, samples[si].rate, None))
    instruments.append(sf2.Instrument("Standard Kit", zones))
    out_presets.append(sf2.Preset("Standard", 0, 128, [sf2.Zone({G["instrument"]: len(instruments) - 1})]))
    if verbose:
        for prog, name, n, loop, rho, rate, cents in stats:
            d = f"loop {(loop[1] - loop[0]) / rate:.3f} s from {loop[0] / rate:.3f} s, match {rho:.2f}" if loop else "one-shot"
            if cents is not None:
                d += f", tuned {-cents:+.0f} c"
            print(f"  {prog:3d} {name:22s} {n:6d} pts  {d}")
    info = {
        "isng": "E-mu 10K1",
        "INAM": "FM1-GM mini GM",
        "IENG": "Frank Wen (Fluid R3 GM); reduced for FM1-GM",
        "ICOP": "Fluid R3 GM: Copyright (c) 2000-2002, 2008 Frank Wen, 2008 Toby Smithe. MIT License.",
        "ICMT": "Reduced from Fluid R3 GM by FM1-GM's tools/sf2_reduce.py: one mono sample per program, lower "
                "rates, the attack and a long seamless loop. MIT License (assets/soundfont/LICENSE).",
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
