# SPDX-License-Identifier: GPL-3.0-only
"""FM1-GM's sustain waveforms: the simple shapes a note settles into after its sampled attack.

Each shape is a sum of harmonics, value(t) = sum of a * sin(2 pi h t + p) over (h, a, p), t in turns:
sine, triangle, saw, square, pulses, the same softened (their harmonics rolled off above the 3rd, 6th
or 12th), and two-operator FM (ratio 1, 2, 3; a few indices). tools/sf2_reduce.py picks, for each
instrument, the shape whose spectrum is nearest its sample's; tools/gen_wavetable.py turns the
shapes in use into band-limited single-cycle tables. Standard library only.
"""
import math

H_MAX = 64                      # harmonics in the fullest table


def _bessel_j(k, x):
    """J_k(x), integer k, by its series"""
    if k < 0:
        return (-1) ** k * _bessel_j(-k, x)
    s, term, m = 0.0, (x / 2) ** k / math.factorial(k), 0
    while abs(term) > 1e-12 or m < 4:
        s += term
        m += 1
        term *= -(x / 2) ** 2 / (m * (m + k))
    return s


def _soft(hs, hc):
    return [(h, a / math.sqrt(1 + (h / hc) ** 2), p) for h, a, p in hs]


def _shapes():
    base = {
        "sine": [(1, 1.0, 0.0)],
        "triangle": [(h, 8 / math.pi ** 2 * (-1) ** ((h - 1) // 2) / h ** 2, 0.0) for h in range(1, H_MAX + 1, 2)],
        "saw": [(h, 2 / (math.pi * h) * (-1) ** (h + 1), 0.0) for h in range(1, H_MAX + 1)],
        "square": [(h, 4 / (math.pi * h), 0.0) for h in range(1, H_MAX + 1, 2)],
    }
    for name, d in (("pulse25", 0.25), ("pulse12", 0.125)):
        base[name] = [(h, 2 / (math.pi * h) * math.sin(math.pi * h * d), math.pi / 2) for h in range(1, H_MAX + 1)]
    out = dict(base)
    for name in ("saw", "square", "pulse25", "pulse12", "triangle"):
        for hc in (3, 6, 12):
            if name == "triangle" and hc > 3:
                continue
            out[f"{name}~{hc}"] = _soft(base[name], hc)
    for r in (1, 2, 3):                                  # carrier 1, modulator r: harmonics 1 + k r
        for idx in (0.5, 1.0, 2.0, 3.5):
            hs = {}
            for k in range(-40, 41):
                h = 1 + k * r
                a = _bessel_j(k, idx)
                if h == 0 or abs(a) < 1e-5:
                    continue
                if h < 0:                                # sin(-x) = -sin(x)
                    h, a = -h, -a
                if h <= H_MAX:
                    hs[h] = hs.get(h, 0.0) + a
            out[f"fm{r}:{idx:g}"] = [(h, a, 0.0) for h, a in sorted(hs.items())]
    return out


SHAPES = _shapes()
NAMES = sorted(SHAPES)


def value(name, t, hmax=H_MAX):
    return sum(a * math.sin(2 * math.pi * h * t + p) for h, a, p in SHAPES[name] if h <= hmax)


def magnitudes(name, n):
    """the shape's harmonic magnitudes 1..n"""
    m = [0.0] * n
    for h, a, _ in SHAPES[name]:
        if h <= n:
            m[h - 1] += abs(a)
    return m
