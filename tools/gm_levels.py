#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""How loud each of FM1-GM's instruments is next to FluidR3 GM played by FluidSynth, and the level
corrections (dB louder) that even them out: assets/soundfont/fm1gm_levels.json, which tools/sf2_reduce.py
adds to each zone's attenuation.

  tools/gm_levels.py FluidR3_GM.sf2 [--sf2 assets/soundfont/fm1gm.sf2] [--update]

Each program plays three notes (an octave either side of the key its sample was taken at, and that
key), each drum its note, at velocity 100, dry (no reverb or chorus), in FluidSynth from FluidR3 and
in the firmware's own engine (firmware/src/dsp/gm.c, built for the host) from the reduced SoundFont.
A note's level is its power over its first 1.5 s; an instrument's, its notes' mean power. What a
correction evens out is the gap to FluidR3, less the middle gap of all (so the mix as a whole stays
as loud as it was). --update adds the gaps left to the corrections in the json and writes the same
change into the SoundFont's zones, so a new run of sf2_reduce.py is not needed for it.
Needs fluidsynth, a C compiler and numpy.
"""
import argparse
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sf2  # noqa: E402
import sf2_reduce  # noqa: E402

SRC = Path(__file__).resolve().parents[1]
LEVELS = SRC / "assets" / "soundfont" / "fm1gm_levels.json"
SR, HOLD, GAP, WIN = 44100, 2.0, 1.0, 1.5
CB_PER_DB = 25                  # players apply 40 % of initialAttenuation (cB): a dB is 25 of it
RENDER_C = r"""
#include <stdio.h>
#include <stdint.h>
#include "gm.h"
void seq_midi_out(uint32_t st, uint32_t d1, uint32_t d2) { (void)st; (void)d1; (void)d2; }
static void run(FILE *f, double sec)
{
    int n = (int)(sec * 44100 + 0.5), i;
    float l[32], r[32];
    while (n > 0) {
        int k = n > 32 ? 32 : n;
        for (i = 0; i < k; i++)
            l[i] = r[i] = 0;
        gm_render(l, r, (uint32_t)k);
        for (i = 0; i < k; i++) {
            float s[2] = {l[i], r[i]};
            fwrite(s, 4, 2, f);
        }
        n -= k;
    }
}
int main(int argc, char **argv)
{
    int c, p, n;
    FILE *f = fopen(argv[1], "wb");
    (void)argc;
    gm_init();
    for (c = 0; c < 16; c++) {
        gm_midi(0xB0u | c, 91, 0);
        gm_midi(0xB0u | c, 93, 0);
    }
    while (scanf("%d %d %d", &c, &p, &n) == 3) {
        gm_midi(0xC0u | c, p, 0);
        gm_midi(0x90u | c, n, 100);
        run(f, %HOLD%);
        gm_midi(0x80u | c, n, 0);
        run(f, %GAP%);
    }
    fclose(f);
    return 0;
}
"""


def notes():
    """(channel, program, key, what) for every note played"""
    out = []
    for p in range(128):
        k = sf2_reduce.CENTRE.get(p, sf2_reduce.FAMILY[p >> 3][0])
        out += [(0, p, n, str(p)) for n in (k - 12, k, k + 12)]
    out += [(9, 0, n, f"drum {n}") for n in range(sf2_reduce.DRUM_LO, sf2_reduce.DRUM_HI + 1)]
    return out


def midi(path, items):
    def vlq(n):
        b = [n & 0x7F]
        n >>= 7
        while n:
            b.append(0x80 | (n & 0x7F))
            n >>= 7
        return bytes(reversed(b))
    tps = 960                                   # 480 a quarter at 120 bpm
    ev = bytearray()
    for c in (0, 9):
        ev += vlq(0) + bytes([0xB0 | c, 91, 0]) + vlq(0) + bytes([0xB0 | c, 93, 0])
    for c, p, n, _ in items:
        ev += vlq(0) + bytes([0xC0 | c, p]) + vlq(0) + bytes([0x90 | c, n, 100])
        ev += vlq(int(HOLD * tps)) + bytes([0x80 | c, n, 0]) + vlq(int(GAP * tps)) + bytes([0xB0 | c, 123, 0])
    ev += vlq(0) + bytes([0xFF, 0x2F, 0])
    Path(path).write_bytes(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + b"MTrk" + struct.pack(">I", len(ev)) + ev)


def powers(raw, items):
    a = np.fromfile(raw, dtype=np.float32).reshape(-1, 2)
    step = int((HOLD + GAP) * SR)
    return [float(np.mean(a[i * step:i * step + int(WIN * SR)] ** 2)) + 1e-20 for i in range(len(items))]


def measure(fluid_sf2, our_sf2, tmp):
    items = notes()
    midi(tmp / "levels.mid", items)
    subprocess.run(["fluidsynth", "-ni", "-R", "0", "-C", "0", "-g", "0.5", "-r", str(SR), "-O", "float", "-T", "raw",
                    "-F", str(tmp / "fluid.raw"), str(fluid_sf2), str(tmp / "levels.mid")], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([sys.executable, str(SRC / "tools" / "gen_wavetable.py"), str(tmp / "gm_wavetable.h"), str(our_sf2)],
                   check=True, stdout=subprocess.DEVNULL)
    (tmp / "render.c").write_text(RENDER_C.replace("%HOLD%", str(HOLD)).replace("%GAP%", str(GAP)))
    fw = SRC / "firmware" / "src"
    subprocess.run(["cc", "-O2", "-std=c99", "-DOM_HOST", f"-I{fw}", f"-I{fw / 'dsp'}", f"-I{tmp}", "-o",
                    str(tmp / "render"), str(tmp / "render.c"), str(fw / "dsp" / "gm.c"), "-lm"], check=True)
    with open(tmp / "notes.txt", "w") as f:
        f.write("".join(f"{c} {p} {n}\n" for c, p, n, _ in items))
    with open(tmp / "notes.txt") as f:
        subprocess.run([str(tmp / "render"), str(tmp / "ours.raw")], stdin=f, check=True)
    pf, po = powers(tmp / "fluid.raw", items), powers(tmp / "ours.raw", items)
    gap = {}
    for (_, _, _, what), a, b in zip(items, pf, po):
        s = gap.setdefault(what, [0.0, 0.0])
        s[0] += a
        s[1] += b
    gap = {w: 10 * np.log10(b / a) for w, (a, b) in gap.items()}
    mid = float(np.median([g for w, g in gap.items() if not w.startswith("drum")]))
    return {w: g - mid for w, g in gap.items()}, mid


def apply(path, change):
    """the corrections' change (dB louder, by program or "drum n") written into the SoundFont's zones"""
    sf = sf2.read(path)
    G = sf2.G
    zone_of = {}
    for p in sf.presets:
        for i in {z.gens[G["instrument"]] for z in p.zones if G["instrument"] in z.gens}:
            for z in sf.instruments[i].zones:
                what = str(p.program) if p.bank == 0 else f"drum {z.gens[G['keyRange']][0]}"
                zone_of.setdefault(what, []).append(z)
    for what, d in change.items():
        for z in zone_of.get(what, []):
            a = z.gens.get(G["initialAttenuation"], 0) - int(round(d * CB_PER_DB))
            z.gens[G["initialAttenuation"]] = a
            if not a:
                del z.gens[G["initialAttenuation"]]
    sf2.write(path, sf)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("fluid", help="FluidR3_GM.sf2")
    ap.add_argument("--sf2", default=str(SRC / "assets" / "soundfont" / "fm1gm.sf2"))
    ap.add_argument("--update", action="store_true")
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as t:
        gap, mid = measure(a.fluid, a.sf2, Path(t))
    names = {str(p.program): p.name for p in sf2.read(a.fluid).presets if p.bank == 0}
    print(f"FM1-GM's instruments next to FluidR3's (dB, less the middle gap of {mid:+.1f} dB):")
    for w, g in sorted(gap.items(), key=lambda wg: wg[1]):
        print(f"  {w:8s} {names.get(w, ''):20s} {g:+5.1f}")
    rms = float(np.sqrt(np.mean([g * g for g in gap.values()])))
    print(f"spread: {rms:.1f} dB RMS, {max(abs(g) for g in gap.values()):.1f} dB at most")
    if a.update:
        levels = json.loads(LEVELS.read_text()) if LEVELS.exists() else {}
        change = {}
        for w, g in gap.items():                # within the room sf2_reduce.py leaves (LEVEL_ROOM)
            new = round(max(-sf2_reduce.LEVEL_ROOM, min(sf2_reduce.LEVEL_ROOM, levels.get(w, 0.0) - g)), 1)
            change[w] = new - levels.get(w, 0.0)
            levels[w] = new
        LEVELS.write_text(json.dumps(dict(sorted(levels.items(), key=lambda kv: (kv[0].startswith("drum"),
                                                                                  int(kv[0].split()[-1])))),
                                     indent=1) + "\n")
        apply(a.sf2, change)
        print(f"{LEVELS.name} and {Path(a.sf2).name} updated")


if __name__ == "__main__":
    main()
