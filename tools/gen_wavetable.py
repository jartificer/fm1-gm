#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""FM1-GM's instrument tables (a C header) from assets/soundfont/fm1gm.sf2 and fm1gm_sustain.json.

  tools/gen_wavetable.py OUT.h [SF2]

Each sample (an instrument's attack, or a drum) is peak-normalised and stored as 4-bit IMA ADPCM
(two points a byte, low nibble first). Its sustain (tools/sf2_reduce.py fitted it, the JSON next to
the SoundFont) becomes a waveform table, band-limited at 7 levels (64, 32 .. 1 harmonics, the engine
picks by pitch), or filtered noise; with where the sample crossfades into it, and its pitch, phase
and level. Each zone (a program, or one drum note) becomes a gmw_zone_t (firmware/src/dsp/gm_wave.h):
its sample, root key, tuning, gain and the SoundFont generators the engine follows (volume envelope,
filter and its modulation envelope, drum exclusive class, pan). Standard library only.
"""
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gm_shapes  # noqa: E402
import sf2  # noqa: E402

SRC = Path(__file__).resolve().parents[1]
DEFAULT_SF2 = SRC / "assets" / "soundfont" / "fm1gm.sf2"
G = sf2.G
DRUM_LO, DRUM_HI = 35, 81
TAB_LEVELS = 7                    # levels: 64 >> level harmonics
TAB_N = [256, 256, 256, 128, 64, 32, 32]    # each level's points a cycle (at least 4 a cycle of its top harmonic)
TAB_OFF = [sum(n + 1 for n in TAB_N[:L]) for L in range(TAB_LEVELS + 1)]
SH_NONE, SH_NOISE = 255, 254

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
        88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
        658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
        16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_step(x, pred, idx):
    """one point: the nibble, and the decoder's state after it (exactly as gm.c decodes)"""
    step = STEP[idx]
    d = x - pred
    n = 0
    if d < 0:
        n, d = 8, -d
    if d >= step:
        n |= 4
        d -= step
    if d >= step >> 1:
        n |= 2
        d -= step >> 1
    if d >= step >> 2:
        n |= 1
    pred = adpcm_dec(n, pred, idx)
    idx = min(max(idx + INDEX[n & 7], 0), 88)
    return n, pred, idx


def adpcm_dec(n, pred, idx):
    step = STEP[idx]
    dq = step >> 3
    if n & 4:
        dq += step
    if n & 2:
        dq += step >> 1
    if n & 1:
        dq += step >> 2
    pred = pred - dq if n & 8 else pred + dq
    return min(max(pred, -32768), 32767)


def encode(pts):
    """nibbles, and the step index to start from"""
    def run(idx0, n):
        pred, idx, err = 0, idx0, 0
        for x in pts[:n]:
            _, pred, idx = adpcm_step(x, pred, idx)
            err += (x - pred) ** 2
        return err
    idx0 = min(range(0, 89, 4), key=lambda i: run(i, 48)) if pts else 0
    out, pred, idx = [], 0, idx0
    for x in pts:
        n, pred, idx = adpcm_step(x, pred, idx)
        out.append(n)
    return out, idx0


def table(name):
    """the shape's levels, int16, TAB_N[level] + 1 points each (the last repeats the first), one
    scale for all"""
    lv = [[gm_shapes.value(name, i / TAB_N[L], gm_shapes.H_MAX >> L) for i in range(TAB_N[L])] for L in range(TAB_LEVELS)]
    k = 32000.0 / max(max(abs(v) for v in t) for t in lv)
    return [[int(round(v * k)) for v in t + t[:1]] for t in lv], k


def cf(v, digits=7):
    """a C float literal"""
    t = f"{v:.{digits}g}"
    return (t if "." in t or "e" in t else t + ".0") + "f"


def zone_list(sf, preset):
    return sf2.zones(sf, preset)


def tc(v):                         # timecents, as stored (int16)
    return max(-12000, min(8000, int(v)))


def sustain_path(sf2_path):
    p = Path(sf2_path)
    return p.with_name(p.stem + "_sustain.json")


def main():
    out = Path(sys.argv[1])
    src = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_SF2
    sf = sf2.read(src)
    presets = {(p.bank, p.program): p for p in sf.presets}
    data = bytearray()
    smp_rows, smp_of = [], {}

    sus = json.loads(sustain_path(src).read_text())["samples"]
    shapes, tabs = {}, []

    def sample(i):
        if i in smp_of:
            return smp_of[i]
        s = sf.samples[i]
        pts = [int.from_bytes(s.pcm[j:j + 2], "little", signed=True) for j in range(0, len(s.pcm), 2)]
        peak = max(1, max(abs(x) for x in pts)) if pts else 1
        k = 32000.0 / peak
        pts = [int(round(x * k)) for x in pts]
        nib, idx0 = encode(pts)
        off = len(data)
        for j in range(0, len(nib), 2):
            data.append(nib[j] | ((nib[j + 1] if j + 1 < len(nib) else 0) << 4))
        u = sus.get(s.name)
        row = dict(off=off, len=len(nib), idx0=idx0, rate=s.rate, name=s.name, shape=SH_NONE, xs=0, xe=0,
                   ph0=0.0, cps=0.0, og=0.0, hp=0.0, lp=0.0)
        if u:
            # the sustain's level, on the decoder's scale: the sample's own float (pcm / 32768) is
            # x * k, x the reducer's; a table's float is the shape's value * its scale / 32768
            row.update(xs=u["xs"], xe=min(u["xe"], len(nib) - 2))
            if u["shape"] == "noise":
                row.update(shape=SH_NOISE, og=u["gain"] * 32767 / 32768 * k, hp=u["hp"], lp=u["lp"])
            else:
                if u["shape"] not in shapes:
                    t, ks = table(u["shape"])
                    shapes[u["shape"]] = (len(tabs), ks)
                    tabs.append((u["shape"], t))
                ti, ks = shapes[u["shape"]]
                row.update(shape=ti, ph0=u["ph0"], cps=u["cps"], og=u["gain"] * 32767 * k / ks)
        smp_rows.append(row)
        smp_of[i] = (len(smp_rows) - 1, peak / 32000.0)
        return smp_of[i]

    zones = []

    def zone(z, drum):
        g = z["gens"]
        s = sf.samples[z["sample"]]
        si, scale = sample(z["sample"])
        root = g.get(G["overridingRootKey"], -1)
        root = root if root >= 0 else s.root
        tune = g.get(G["coarseTune"], 0) * 100 + g.get(G["fineTune"], 0) + s.corr
        # SoundFont players (EMU's, FluidSynth) apply 40 % of initialAttenuation; so does this
        gain = scale * 10 ** (-0.4 * g.get(G["initialAttenuation"], 0) / 200)
        pan = g.get(G["pan"], 0) if drum else 0
        zones.append(dict(
            smp=si, lo=z["key"][0], hi=z["key"][1], root=root, excl=g.get(G["exclusiveClass"], 0) if drum else 0,
            pan=max(-64, min(63, round(pan * 64 / 500))), tune=tune, kt=g.get(G["scaleTuning"], 100),
            gain=gain, att=tc(g.get(G["attackVolEnv"], -12000)), hold=tc(g.get(G["holdVolEnv"], -12000)),
            dec=tc(g.get(G["decayVolEnv"], -12000)), sus=max(0, min(1440, g.get(G["sustainVolEnv"], 0))),
            rel=tc(min(g.get(G["releaseVolEnv"], -12000), 2400)),          # at most 4 s to fall 100 dB
            kdec=g.get(G["keynumToVolEnvDecay"], 0), khold=g.get(G["keynumToVolEnvHold"], 0),
            fc=max(1500, min(13500, g.get(G["initialFilterFc"], 13500))), q=max(0, min(960, g.get(G["initialFilterQ"], 0))),
            fenv=max(-12000, min(12000, g.get(G["modEnvToFilterFc"], 0))), fatt=tc(g.get(G["attackModEnv"], -12000)),
            fdec=tc(g.get(G["decayModEnv"], -12000)), fsus=max(0, min(1000, g.get(G["sustainModEnv"], 0)))))

    prog_first = []
    for prog in range(128):
        prog_first.append(len(zones))
        p = presets.get((0, prog))
        for z in sorted(zone_list(sf, p) if p else [], key=lambda z: z["key"]):
            zone(z, False)
    prog_first.append(len(zones))
    drum_zone = []
    kit = presets.get((128, 0))
    kz = zone_list(sf, kit) if kit else []
    for note in range(DRUM_LO, DRUM_HI + 1):
        here = [z for z in kz if z["key"][0] <= note <= z["key"][1]]
        if here:
            drum_zone.append(len(zones))
            zone(dict(here[0], key=(note, note)), True)
        else:
            drum_zone.append(-1)

    w = [f"/* Generated by tools/gen_wavetable.py from {src.name} and {sustain_path(src).name}: do not edit. */",
         f"/* {len(smp_rows)} samples, {len(data)} bytes of 4-bit ADPCM; {len(tabs)} waveforms; {len(zones)} zones. */",
         "#pragma once", "/* gm.c includes gm_wave.h (the types) first */", "",
         f"#define GMW_DATA_BYTES {len(data)}u",
         "static const uint8_t GMW_DATA[] = {"]
    for i in range(0, len(data), 24):
        w.append("    " + ",".join(str(b) for b in data[i:i + 24]) + ",")
    w += ["};", "", "/* the sustain waveforms: [waveform][GMW_LV_OFF[level] + point], level L holding 64 >> L",
          " * harmonics in GMW_LV_N[L] points (and the first again) */",
          f"static const uint16_t GMW_LV_OFF[GMW_LEVELS] = {{{', '.join(map(str, TAB_OFF[:-1]))}}};",
          f"static const uint16_t GMW_LV_N[GMW_LEVELS] = {{{', '.join(map(str, TAB_N))}}};",
          f"static const int16_t GMW_TAB[{max(len(tabs), 1)}][{TAB_OFF[-1]}] = {{"]
    for name, t in tabs or [("none", [[0] * (n + 1) for n in TAB_N])]:
        w.append(f"    {{   /* {name} */")
        for lv in t:
            w.append("        " + ",".join(str(v) for v in lv) + ",")
        w.append("    },")
    w += ["};", "", f"static const gmw_smp_t GMW_SMP[{len(smp_rows)}] = {{",
          "    /* off, len, step at 0, rate; sustain: waveform (255 none, 254 noise), crossfade from, to;"
          " phase at 0, cycles a point, level, noise high-pass, low-pass */"]
    for r in smp_rows:
        w.append(f"    {{{r['off']}u, {r['len']}u, {r['idx0']}, {cf(r['rate'])}, {r['shape']}, {r['xs']}u, {r['xe']}u, "
                 f"{cf(r['ph0'])}, {cf(r['cps'], 8)}, {cf(r['og'], 6)}, {cf(r['hp'], 6)}, {cf(r['lp'], 6)}}},   /* {r['name']} */")
    w += ["};", "", f"static const gmw_zone_t GMW_ZONE[{len(zones)}] = {{"]
    keys = ["smp", "lo", "hi", "root", "excl", "pan", "tune", "kt", "att", "hold", "dec", "sus", "rel", "kdec",
            "khold", "fc", "q", "fenv", "fatt", "fdec", "fsus"]
    w.append("    /* " + ", ".join(keys) + ", gain */")
    for z in zones:
        w.append("    {" + ", ".join(str(int(z[k])) for k in keys) + f", {cf(z['gain'], 6)}}},")
    w += ["};", "", "/* the zones of program p: GMW_PROG[p] .. GMW_PROG[p + 1] - 1, by key */",
          "static const uint16_t GMW_PROG[129] = {"]
    for i in range(0, 129, 16):
        w.append("    " + ", ".join(str(v) for v in prog_first[i:i + 16]) + ",")
    w += ["};", "", f"/* the zone of each drum note {DRUM_LO}..{DRUM_HI}, -1: none */",
          f"static const int16_t GMW_DRUM[{DRUM_HI - DRUM_LO + 1}] = {{"]
    for i in range(0, len(drum_zone), 16):
        w.append("    " + ", ".join(str(v) for v in drum_zone[i:i + 16]) + ",")
    w.append("};")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(w) + "\n")
    print(f"wavetable: {len(smp_rows)} samples, {len(data)} B ADPCM, {len(tabs)} waveforms "
          f"({len(tabs) * TAB_OFF[-1] * 2} B), {len(zones)} zones -> {out}")


if __name__ == "__main__":
    main()
