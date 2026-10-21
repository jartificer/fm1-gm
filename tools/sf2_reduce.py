#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Make FM1-GM's small General MIDI SoundFont (assets/soundfont/fm1gm.sf2) from FluidR3_GM.sf2.

  tools/sf2_reduce.py FluidR3_GM.sf2 assets/soundfont/fm1gm.sf2

FluidR3 GM (Frank Wen, MIT License) is 141 MiB; the FM-1 has about 440 KiB of flash left after the
firmware. For each GM program this keeps one sample, the one Fluid plays at the instrument's middle
key (one velocity layer, mono), for the whole keyboard; at one rate for all (RATE); and
only its attack and a loop after it. The loops are made to be seamless:

- long (0.12-0.25 s, by family), so what little a loop still changes is slow, not a buzz;
- a whole number of periods exactly: the sample is resampled by a hair so that they come out to a
  whole number of points, and the loop replays in step with itself;
- level: the sample's fade across the loop is taken out, in octave bands (the volume envelope fades it
  instead), and the loop may start a little later where the level is smoother;
- the seam crossfaded, over the loop's last 7-30 % (searched: a long crossfade over sides that do not
  match is an echo), with what comes just before its start; the length
  and start searched for where the two match best (a recorded vibrato or chorus wants whole cycles
  of its own), and the crossfade's law keeping the level steady for however alike they are (linear
  for a periodic sound, equal power for noise).
- tuned: its pitch, measured across the loop, sits on its note (to the semitone the zone's tuning
  means).

The standard instruments (STANDARD) take their loop (0.25 s or more) from wherever in the recording's
first seconds it is steadiest, spliced on after the attack (splice_loop): a piano's tone changes for a
second or two after the strike, and a loop of that change is heard as an echo at every pass. The rest
keep the short loop just after the attack, to save flash.

A few programs have an attack and a loop of their own (TIMING); one mixes its layers into its sample
(LAYERED). Where the recording ends in a short loop of its own before an attack and a loop fit, the
whole recording is kept and its own loop holds.

Sounds that die away within about half a second keep their whole sample, with no loop; so does an
inharmonic bell. The drum kit (bank 128, program 0) keeps notes 35..81, each trimmed (cymbals and
other long drums looped the same way, and faded). FluidR3's vibrato and tremolo are dropped.

The result is an ordinary SoundFont (16-bit, at RATE): play it in any SoundFont player.
tools/gen_wavetable.py turns it into the firmware's tables (4-bit ADPCM, about a quarter of its
size). Needs numpy and scipy (this tool only; the firmware build does not).
"""
import argparse
import sys
from fractions import Fraction
from pathlib import Path

import numpy as np
from scipy.signal import resample as fft_resample
from scipy.signal import resample_poly

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
BAND_LO = 100.0                 # the loop's fade is taken out in octave bands above this (Hz), and in one below
BAND_FLAT = np.log(16.0)        # by at most 24 dB across the loop
LATE = 0.15                    # a loop may start up to this (s) after its attack, where its level is smoother
LATE_COST = 0.5                 # dB of roughness that starting LATE later must save
XF_REF = 0.015                  # the crossfade (s) that least shows a mismatch, both as an echo and as a jump
DEBUG = False
SPLICE_LOOP = 0.25              # a loop this long (s) at least: a shorter one repeats fast enough to buzz
SPLICE_MAX = 4.0                # a loop may be taken from as late as this (s) in the recording
SPLICE_XF = 0.06                # and spliced on after the attack with a crossfade this long (s)
SPLICE_GRID = 0.02              # the places tried, this far apart (s)
STEADY_W = 0.08                 # the level is held steady over this (s)
STEADY_MIN = 0.3                # dB of wobble that is held steady
WOBBLE_COST = 3.0               # the score a dB of level wobble in the loop costs
SPLICE_TRY = 4                  # the steadiest of them made into loops and compared
MEM_COST = 14.0                 # the score a second of sample costs (dB of seam step): the flash goes to
MEM_COST_EXOTIC = 40.0          # the standard instruments; the rest keep their short loop after the attack
STANDARD = (set(range(0, 8)) | set(range(16, 24)) | set(range(24, 46)) | {48, 49, 52, 53} |
            set(range(56, 62)) | set(range(64, 76)))
SEARCH = 0.03                   # the loop start is searched over this (s) after the attack
RATE = 14700                    # every sample's rate (a third of 44.1 kHz): the same top end and noise for all
DRUM_MIN, DRUM_MAX = 0.06, 0.24  # a drum keeps until it is 36 dB down, within these (s)
DRUM_LONG = 0.6                 # a drum that rings longer than this is looped after DRUM_ATT
DRUM_ATT, DRUM_LOOP = 0.10, 0.20


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


def resample(x, rate):
    """x at RATE"""
    fr = Fraction(RATE, rate)
    if fr == 1:
        return x, rate, fr
    return resample_poly(x, fr.numerator, fr.denominator), int(round(rate * fr)), fr



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


def flatten_bands(x, a, s, b, rate, hop):
    """x[a:b] with the fade across [s, b) taken out of each octave band (from the loop start s on, each
    band at the level it starts the loop with): a piano's top end fades faster than its body, and a loop
    that only holds its overall level brightens again at every seam, as if struck anew"""
    pad = 4 * hop
    lo, hi = max(0, a - pad), min(len(x), b + pad)
    seg = x[lo:hi]
    X = np.fft.rfft(seg)
    fr = np.fft.rfftfreq(len(seg), 1.0 / rate)
    edges = [0.0] + [BAND_LO * 2 ** i for i in range(16) if BAND_LO * 2 ** i < rate / 2] + [rate]
    t = np.arange(lo, hi) - s
    out = np.zeros(len(seg))
    lf = np.log2(np.maximum(fr, 1.0))
    for i in range(len(edges) - 1):
        # raised-cosine shoulders an octave wide, at the band edges: the bands sum to x again
        def up(e):
            return np.ones_like(lf) if e <= 0 else np.clip(0.5 - 0.5 * np.cos(np.pi * np.clip(lf - np.log2(e) + 0.5, 0, 1)), 0, 1)
        m = up(edges[i]) - (up(edges[i + 1]) if edges[i + 1] < rate else 0)
        band = np.fft.irfft(X * m, len(seg))
        k = level_slope(band, s - lo, b - lo, hop)
        k = max(min(k, BAND_FLAT / (b - s)), -BAND_FLAT / (b - s))
        out += band * np.exp(-k * t)
    return out[a - lo:b - lo]


def settled(x, s0, n, hop, rate):
    """where after s0 (within LATE) n points of x have the smoothest level (dB in windows of hop, about
    a straight line): a choir's or a pad's attack can swell and dip for longer than its family's, and a
    loop over a dip comes back as a fresh onset every pass"""
    m = (len(x) - s0 - n) // hop
    late = min(int(LATE * rate / hop), m)
    if late <= 0:
        return s0
    k = n // hop
    db = 10 * np.log10(np.mean(x[s0:s0 + (late + k) * hop].reshape(late + k, hop) ** 2, axis=1) + 1e-12)
    t = np.arange(k)

    def rough(i):
        e = db[i:i + k]
        return float(np.std(e - np.polyval(np.polyfit(t, e, 1), t)))
    r = [rough(i) + LATE_COST * i / late for i in range(late + 1)]
    i = int(np.argmin(r))
    return s0 + i * hop if rough(0) - r[i] > 0.5 else s0


def band_step(u, v, rate):
    """how far (dB) the octave bands that matter (within 30 dB of the strongest) step from u to v"""
    def levels(seg):
        sp = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2
        fr = np.fft.rfftfreq(len(seg), 1.0 / rate)
        e = [0.0] + [BAND_LO * 2 ** i for i in range(16) if BAND_LO * 2 ** i < rate / 2] + [rate]
        return np.array([sp[(fr >= e[i]) & (fr < e[i + 1])].sum() + 1e-12 for i in range(len(e) - 1)])
    lu, lv = levels(u), levels(v)
    keep = lu + lv > (lu + lv).max() * 1e-3
    return float(np.max(np.abs(10 * np.log10(lv[keep] / lu[keep]))))


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


def make_loop(x, nr, f0, s0, loop_t, exact=False, bands=True):
    """make_loop1 after the attack, or later where the level is smoother (settled), whichever steps
    least at the seam"""
    y, rate, loop, rho = best = make_loop1(x, nr, f0, s0, loop_t, exact, bands)
    if bands:
        later = settled(x, s0, int(loop_t * nr * (1 + XF_SHARE)), max(int(nr * 0.01), 16), nr)
        if later != s0:
            alt = make_loop1(x, nr, f0, later, loop_t, exact, bands)
            if seam_step(*alt[:3]) < seam_step(y, rate, loop) - 1.0:
                best = alt
    return best


def splice_loop(x, nr, f0, att, loop_t, mem=MEM_COST):
    """the attack, then a loop from wherever in the recording it is steadiest and seams best (often well
    after the attack: a piano's or a guitar's tone changes for a second or two, and a loop of that
    change comes back to its start at every pass, heard as a retrigger or an echo), spliced on with a
    crossfade and its level matched to the attack's end (the volume envelope does the fading)"""
    a = int(round(att * nr))
    w = max(int(0.025 * nr), 64)

    def env_rough(c, n):
        hop = max(int(nr * 0.01), 16)
        k = n // hop
        if k < 4:
            return 0.0
        db = 10 * np.log10(np.mean(x[c:c + k * hop].reshape(k, hop) ** 2, axis=1) + 1e-12)
        t = np.arange(k)
        return float(np.std(db - np.polyval(np.polyfit(t, db, 1), t)))

    def drift(c, n):                                        # what flattening would have to take out
        q = max(n // 4, 64)
        return band_step(x[c:c + q], x[c + n - q:c + n], nr)
    results = []
    ks = (1, 1.5, 2) if mem <= MEM_COST else (1,)          # 1.5: two cycles of a 5-6 Hz vibrato
    for lt in (k * max(loop_t, SPLICE_LOOP) for k in ks):
        n = int(lt * nr)
        pre = int(XF_SHARE * n)
        hi = min(len(x) - int(1.4 * n) - int(SEARCH * nr) - 8, int(SPLICE_MAX * nr))
        cands = list(range(a + pre, max(a + pre, hi), max(int(SPLICE_GRID * nr), 1)))
        ranked = sorted(cands, key=lambda c: drift(c, n) + env_rough(c, n))[:SPLICE_TRY]
        for c in sorted(set(ranked) | {a + pre}):
            y, rate, (ls, le), rho = make_loop1(x, nr, f0, c - pre, lt)
            n2, a2 = le - ls, int(round(a * rate / nr))
            raw = drift(int(round(ls * nr / rate)), int(round(n2 * nr / rate)))
            tx = int(SPLICE_XF * rate)
            if ls - a2 <= tx + int(XF_SHARE * n2) + int(SEARCH * rate):     # no splice: as made
                out, loop, junction = y, (ls, le), 0.0
            else:
                p = max(int(rate / f0), 1) if f0 else int(0.01 * rate)
                b = y[ls - tx:ls]
                best = None
                for a3 in range(max(1, a2 - p), a2 + p + 1):          # in step with the loop's lead-in
                    u = y[a3:a3 + tx]
                    r = float(np.dot(u, b) / (np.sqrt(np.dot(u, u) * np.dot(b, b)) + 1e-12))
                    if best is None or r > best[0]:
                        best = (r, a3)
                r, a3 = best
                gl = np.sqrt(np.mean(y[a3 - w:a3] ** 2) / (np.mean(y[ls:ls + w] ** 2) + 1e-12))
                t = (np.arange(tx) + 0.5) / tx
                gg = 1.0 / np.sqrt((1 - t) ** 2 + t ** 2 + 2 * max(r, 0.0) * t * (1 - t))
                trans = (1 - t) * gg * y[a3:a3 + tx] + t * gg * gl * b
                out = np.concatenate([y[:a3], trans, gl * y[ls:le]])
                loop = (a3 + tx, a3 + tx + n2)
                junction = band_step(out[a3 - w:a3], out[loop[0]:loop[0] + w], rate)
            score = (seam_step(out, rate, loop) + 10 * (1 - rho) + 0.5 * raw + 0.15 * junction +
                     mem * len(out) / rate)
            if wobble(out, rate, loop) > STEADY_MIN:
                out = steady(out, rate, loop, f0 * rate / nr if f0 else None, tx)
            score += WOBBLE_COST * wobble(out, rate, loop)
            results.append((score + 100 * (n2 < 0.95 * SPLICE_LOOP * rate), out, rate, loop, rho, ls / rate))
    _, out, rate, loop, rho, src = min(results, key=lambda r: r[0])
    if DEBUG:
        print(f"      splice: loop {(loop[1]-loop[0])/rate:.3f} s from {src:.2f} s of the recording")
    return out, rate, loop, rho


def wobble(y, rate, loop):
    """how much (dB, a deviation) the loop's level moves as it plays: a loop of a slow beat or swell is
    heard as a tremolo at the loop's rate"""
    hop = max(int(0.025 * rate), 16)
    u = np.tile(y[loop[0]:loop[1]], 2)
    k = len(u) // hop
    return float(np.std(10 * np.log10(np.mean(u[:k * hop].reshape(k, hop) ** 2, axis=1) + 1e-12)))


def steady(y, rate, loop, f0, tx):
    """the loop's slow swells and beats (a recording's chorus or rotary speaker, an ensemble's beating)
    taken out of its level: what is left of them replays at the loop's rate, a tremolo. The level is
    measured over whole periods, about STEADY_W long, round the loop as it wraps, and eased in over the
    tx points before the loop"""
    ls, le = loop
    n = le - ls
    p = rate / f0 if f0 else 0.01 * rate
    w = min(max(int(round(max(1, round(STEADY_W * rate / p)) * p)), 16), n)
    seg = y[ls:le]
    c = np.concatenate([seg[n - w // 2:], seg, seg[:w - w // 2]]) ** 2
    cs = np.concatenate([[0.0], np.cumsum(c)])
    e = np.sqrt((cs[w:w + n] - cs[:n]) / w + 1e-12)
    g = np.clip(np.sqrt(np.mean(seg ** 2)) / e, 0.5, 2.0)
    out = y.copy()
    out[ls:le] = seg * g
    t = max(min(tx, ls), 1)
    out[ls - t:ls] *= np.linspace(1.0, g[0], t, endpoint=False)
    return out


def seam_step(y, rate, loop):
    w = max(int(0.025 * rate), 64)
    return band_step(y[loop[1] - w:loop[1]], y[loop[0]:loop[0] + w], rate)


def make_loop1(x, nr, f0, s0, loop_t, exact=False, bands=True):
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
    # the fade across the loop, band by band, taken out from the crossfade's other half on
    k = level_slope(x, s, s + n, hop)
    k = max(min(k, np.log(4) / n), -np.log(4) / n)          # at most 12 dB across the loop

    def seamed(p):                                          # x with p as its loop, the seam crossfaded
        a, b = p[n:n + f], p[:f]
        rho = float(np.dot(a, b) / (np.sqrt(np.dot(a, a) * np.dot(b, b)) + 1e-12))
        y = x[:s + n].copy()
        y[s:] = p[f:f + n]
        t = (np.arange(f) + 0.5) / f
        g = 1.0 / np.sqrt((1 - t) ** 2 + t ** 2 + 2 * max(rho, 0.0) * t * (1 - t))
        y[s + n - f:] = (1 - t) * g * a + t * g * b
        return y, rho
    tries = [seamed(x[s - f:s + n] * np.exp(-k * (np.arange(s - f, s + n) - s)))]
    if bands:
        tries.append(seamed(flatten_bands(x, s - f, s, s + n, rate, hop)))
    # the one whose bands step least where the loop's end runs into its start (a vibrato's band levels
    # swing without fading: flattening them band by band can make the seam worse)
    y, rho = min(tries, key=lambda yr: seam_step(yr[0], rate, (s, s + n)))
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


def process(sf, z, zs, root, att, loop_t, pitched, drum, layer_key=None, bands=True, mem=MEM_COST):
    """the sample kept (floats), its rate, its loop (or None), generators it adds, the loop's match,
    the loop's pitch (Hz)"""
    x, rate, (ls0, le0), _ = region_sample(sf, z, zs)
    g = z["gens"]
    # the sample's own pitch: its root key, less the correction a player adds; or, when the zone's
    # tuning corrects the sample (FluidR3's trumpet is an E played as a C), that
    f0 = 440.0 * 2 ** ((root - 69) / 12 - sf.samples[z["sample"]].corr / 1200) if pitched else None
    looped = bool(g.get(G["sampleModes"], 0) & 1 and 0 <= ls0 < le0 <= len(x))
    # mono now: no pan (the channel pans). FluidR3's drums are stereo pairs, each side panned hard
    # (-500 / +500); where the pair does not mix, the one side kept would sound from one ear only
    extra = {G["pan"]: 0}
    ring = ring_time(x, rate) if not looped else 1e9
    if drum:
        att, loop_t, f0 = DRUM_ATT, DRUM_LOOP, None
        long = ring > DRUM_LONG
    else:
        long = ring > ONE_SHOT
    if not long:                                            # dies away by itself: a one-shot
        keep = min(max(ring, DRUM_MIN) * 1.15, DRUM_MAX * 1.15 if drum else ONE_SHOT_KEEP)
        xr, nr, _ = resample(x[:int(keep * rate) + 64], rate)
        return cut(xr, int(keep * nr), nr), nr, None, extra, None, None
    if not f0 and not drum:
        loop_t = max(loop_t, NOISE_LOOP)
    if layer_key is not None:
        x, loop_t, fz = layered(sf, z, zs, layer_key, f0, rate)
    elif looped and le0 - ls0 < 0.5 * loop_t * rate and ls0 < (att + 0.75 * loop_t) * rate:
        # the recording is over (into a short loop of its own) before a loop of ours fits after the
        # attack: the whole recording is the attack, and its own loop holds, as FluidR3 plays it (a loop
        # of ours would replay the recording's end and its loop by turns, a flutter)
        att, loop_t, bands = ls0 / rate, STATIC_LOOP, False
    need = int((att + loop_t * (1 + XF_SHARE) + SEARCH + LATE + 0.05) * rate)
    splice = bands and layer_key is None and not drum and mem <= MEM_COST
    if splice:                                              # the loop may come from anywhere in it
        need = max(need, int((SPLICE_MAX + 2.8 * loop_t + SEARCH + 0.05) * rate))
    if looped and layer_key is None:
        x = unroll(x, ls0, le0, need)
    xr, nr, _ = resample(x[:need + 64], rate)
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
            xr, nr, _ = resample(x[:int(keep * rate) + 64], rate)
            return cut(xr, int(keep * nr), nr), nr, None, extra, None, None
    if layer_key is not None:                               # the mix repeats once a loop, exactly
        y, nr2, loop, rho = make_loop(xr, nr, 1 / loop_t, int(round(att * nr)), loop_t, exact=True, bands=False)
        fl = fz
    else:
        if splice:
            y, nr2, loop, rho = splice_loop(xr, nr, f0, att, loop_t, mem)
        else:
            y, nr2, loop, rho = make_loop(xr, nr, f0, int(round(att * nr)), loop_t, bands=bands)
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

    def add_sample(z, zs, root, att, loop_t, pitched, drum, name, layer_key=None, bands=True, mem=MEM_COST):
        key = (z["sample"], root, att, loop_t, drum, layer_key, bands, tuple(sorted(
            (k, v) for k, v in z["gens"].items() if k in sf2.NOT_ADDITIVE or k in (G["coarseTune"], G["fineTune"]))))
        if key not in cache:
            y, rate, loop, extra, rho, fl = process(sf, z, zs, root, att, loop_t, pitched, drum, layer_key, bands, mem)
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
                                          f"{prog:03d} {p.name}"[:15] + f" {root}", ck if prog in LAYERED else None,
                                          prog not in TIMING,   # those with their own timing: as they were tuned by ear
                                          MEM_COST if prog in STANDARD or prog in TIMING else MEM_COST_EXOTIC)
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
