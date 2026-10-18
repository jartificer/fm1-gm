# SPDX-License-Identifier: GPL-3.0-only
"""SoundFont 2 reading and writing, as much as FM1-GM's wavetable tools need.

read(path) -> SoundFont: presets (bank, program, name, zones), instruments, samples (16-bit mono
PCM as bytes, rate, root key, pitch correction, loop). zones(sf, preset) flattens a preset into
the regions a synthesizer plays: key range, velocity range, sample and the summed generators.
write(path, sf) writes a SoundFont made of plain instrument zones (no modulators).
"""
import struct
from dataclasses import dataclass, field

# generator numbers (SoundFont 2.04, 8.1.2)
G = dict(startAddrsOffset=0, endAddrsOffset=1, startloopAddrsOffset=2, endloopAddrsOffset=3,
         startAddrsCoarseOffset=4, modLfoToPitch=5, vibLfoToPitch=6, modEnvToPitch=7,
         initialFilterFc=8, initialFilterQ=9, modLfoToFilterFc=10, modEnvToFilterFc=11,
         endAddrsCoarseOffset=12, modLfoToVolume=13, chorusEffectsSend=15, reverbEffectsSend=16,
         pan=17, delayModLFO=21, freqModLFO=22, delayVibLFO=23, freqVibLFO=24, delayModEnv=25,
         attackModEnv=26, holdModEnv=27, decayModEnv=28, sustainModEnv=29, releaseModEnv=30,
         keynumToModEnvHold=31, keynumToModEnvDecay=32, delayVolEnv=33, attackVolEnv=34,
         holdVolEnv=35, decayVolEnv=36, sustainVolEnv=37, releaseVolEnv=38, keynumToVolEnvHold=39,
         keynumToVolEnvDecay=40, instrument=41, keyRange=43, velRange=44,
         startloopAddrsCoarseOffset=45, keynum=46, velocity=47, initialAttenuation=48,
         endloopAddrsCoarseOffset=50, coarseTune=51, fineTune=52, sampleID=53, sampleModes=54,
         scaleTuning=56, exclusiveClass=57, overridingRootKey=58)
GN = {v: k for k, v in G.items()}
RANGE = (G["keyRange"], G["velRange"])
UNSIGNED = (G["sampleID"], G["instrument"], G["sampleModes"])
# generators that only an instrument zone may hold, and that do not add up (8.1.3)
NOT_ADDITIVE = {G[n] for n in ("keynum", "velocity", "sampleModes", "exclusiveClass", "overridingRootKey",
                               "startAddrsOffset", "endAddrsOffset", "startloopAddrsOffset",
                               "endloopAddrsOffset", "startAddrsCoarseOffset", "endAddrsCoarseOffset",
                               "startloopAddrsCoarseOffset", "endloopAddrsCoarseOffset", "sampleID")}
DEFAULTS = {G["initialFilterFc"]: 13500, G["delayModLFO"]: -12000, G["delayVibLFO"]: -12000,
            G["delayModEnv"]: -12000, G["attackModEnv"]: -12000, G["holdModEnv"]: -12000,
            G["decayModEnv"]: -12000, G["releaseModEnv"]: -12000, G["delayVolEnv"]: -12000,
            G["attackVolEnv"]: -12000, G["holdVolEnv"]: -12000, G["decayVolEnv"]: -12000,
            G["releaseVolEnv"]: -12000, G["scaleTuning"]: 100, G["overridingRootKey"]: -1,
            G["keynum"]: -1, G["velocity"]: -1}


@dataclass
class Sample:
    name: str
    pcm: bytes              # 16-bit little-endian mono
    rate: int
    root: int
    corr: int               # pitch correction, cents
    loop: tuple             # (start, end) in samples, relative to the sample's start
    stype: int = 1          # 1 mono, 2 right, 4 left, 8 linked
    link: int = 0


@dataclass
class Zone:
    gens: dict = field(default_factory=dict)      # generator -> value (ranges as (lo, hi))


@dataclass
class Instrument:
    name: str
    zones: list             # the first may be global (no sampleID)


@dataclass
class Preset:
    name: str
    program: int
    bank: int
    zones: list             # the first may be global (no instrument)


@dataclass
class SoundFont:
    info: dict
    presets: list
    instruments: list
    samples: list


def _chunks(b, off, end):
    while off + 8 <= end:
        cid, n = struct.unpack_from("<4sI", b, off)
        yield cid, off + 8, n
        off += 8 + n + (n & 1)


def _str(b):
    return b.split(b"\0", 1)[0].decode("latin-1").strip()


def _gen_value(op, amount):
    if op in RANGE:
        return (amount & 0xFF, amount >> 8)
    if op in UNSIGNED:
        return amount
    return amount - 0x10000 if amount >= 0x8000 else amount


def read(path):
    b = open(path, "rb").read()
    riff, n, form = struct.unpack_from("<4sI4s", b, 0)
    if riff != b"RIFF" or form != b"sfbk":
        raise ValueError(f"{path}: not a SoundFont")
    lists = {}
    for cid, off, n in _chunks(b, 12, len(b)):
        if cid == b"LIST":
            lists[b[off:off + 4]] = {c: (o, m) for c, o, m in _chunks(b, off + 4, off + n)}
    info = {c.decode(): _str(b[o:o + m]) for c, (o, m) in lists[b"INFO"].items() if c != b"ifil"}
    so, sn = lists[b"sdta"][b"smpl"]
    smpl = b[so:so + sn]
    pd = lists[b"pdta"]

    def recs(cid, fmt):
        o, m = pd[cid]
        sz = struct.calcsize(fmt)
        return [struct.unpack_from(fmt, b, o + i * sz) for i in range(m // sz)]
    phdr, pbag, pgen = recs(b"phdr", "<20sHHHIII"), recs(b"pbag", "<HH"), recs(b"pgen", "<HH")
    inst, ibag, igen = recs(b"inst", "<20sH"), recs(b"ibag", "<HH"), recs(b"igen", "<HH")
    shdr = recs(b"shdr", "<20sIIIIIbbHH")

    def zones(bags, gens, lo, hi):
        out = []
        for z in range(lo, hi):
            zone = Zone()
            for g in range(bags[z][0], bags[z + 1][0]):
                op, amount = gens[g]
                zone.gens[op] = _gen_value(op, amount)
            out.append(zone)
        return out
    samples = []
    for name, start, end, ls, le, rate, root, corr, link, stype in shdr[:-1]:
        samples.append(Sample(_str(name), smpl[2 * start:2 * end], rate, root, corr, (ls - start, le - start),
                              stype, link))
    instruments = [Instrument(_str(inst[i][0]), zones(ibag, igen, inst[i][1], inst[i + 1][1]))
                   for i in range(len(inst) - 1)]
    presets = [Preset(_str(phdr[i][0]), phdr[i][1], phdr[i][2], zones(pbag, pgen, phdr[i][3], phdr[i + 1][3]))
               for i in range(len(phdr) - 1)]
    return SoundFont(info, presets, instruments, samples)


def _split(zs, key):
    if zs and key not in zs[0].gens:
        return zs[0].gens, zs[1:]
    return {}, zs


def _isect(a, b):
    lo, hi = max(a[0], b[0]), min(a[1], b[1])
    return (lo, hi) if lo <= hi else None


def zones(sf, preset):
    """the preset as regions: dicts with key, vel (ranges), sample (index) and every generator the
    instrument and the preset set (preset values added to the instrument's, as 8.5 says)"""
    out = []
    pglob, pzones = _split(preset.zones, G["instrument"])
    for pz in pzones:
        pg = {**pglob, **pz.gens}
        ins = sf.instruments[pg[G["instrument"]]]
        iglob, izones = _split(ins.zones, G["sampleID"])
        for iz in izones:
            ig = {**iglob, **iz.gens}
            key = _isect(ig.get(G["keyRange"], (0, 127)), pg.get(G["keyRange"], (0, 127)))
            vel = _isect(ig.get(G["velRange"], (0, 127)), pg.get(G["velRange"], (0, 127)))
            if not key or not vel:
                continue
            g = dict(DEFAULTS)
            g.update({k: v for k, v in ig.items() if k not in RANGE})
            for k, v in pg.items():
                if k in RANGE or k == G["instrument"] or k in NOT_ADDITIVE:
                    continue
                g[k] = g.get(k, 0) + v
            out.append(dict(key=key, vel=vel, sample=ig[G["sampleID"]], gens=g, instrument=ins.name))
    return out


def write(path, sf):
    """a SoundFont of sf.presets -> sf.instruments -> sf.samples; each preset zone names one
    instrument, each instrument zone one sample, both with their generators (no modulators)"""
    def name20(s):
        return s.encode("latin-1")[:19].ljust(20, b"\0")

    def gen_bytes(gens):
        ordered = [k for k in RANGE if k in gens] + \
                  [k for k in gens if k not in RANGE and k not in (G["instrument"], G["sampleID"])] + \
                  [k for k in (G["instrument"], G["sampleID"]) if k in gens]
        out = b""
        for k in ordered:
            v = gens[k]
            amount = (v[0] | v[1] << 8) if k in RANGE else v & 0xFFFF
            out += struct.pack("<HH", k, amount)
        return out
    smpl = bytearray()
    shdr = b""
    for i, s in enumerate(sf.samples):
        start = len(smpl) // 2
        n = len(s.pcm) // 2
        smpl += s.pcm + b"\0" * 92                  # 46 zero samples after each (7.10)
        shdr += struct.pack("<20sIIIIIbbHH", name20(s.name), start, start + n, start + s.loop[0],
                            start + s.loop[1], s.rate, s.root, s.corr, s.link, s.stype)
    shdr += struct.pack("<20sIIIIIbbHH", name20("EOS"), 0, 0, 0, 0, 0, 0, 0, 0, 0)

    def hydra(items, zones_of, hdr):
        bags, gens, heads = b"", b"", b""
        nb = ng = 0
        for it in items:
            heads += hdr(it, nb)
            for z in zones_of(it):
                bags += struct.pack("<HH", ng, 0)
                gb = gen_bytes(z.gens)
                gens += gb
                ng += len(gb) // 4
                nb += 1
        return heads, bags + struct.pack("<HH", ng, 0), gens + b"\0\0\0\0", nb
    ph, pbag, pgen, npb = hydra(sorted(sf.presets, key=lambda p: (p.bank, p.program)), lambda p: p.zones,
                                lambda p, nb: struct.pack("<20sHHHIII", name20(p.name), p.program, p.bank, nb, 0, 0, 0))
    ph += struct.pack("<20sHHHIII", name20("EOP"), 0, 0, npb, 0, 0, 0)
    ih, ibag, igen, nib = hydra(sf.instruments, lambda i: i.zones,
                                lambda i, nb: struct.pack("<20sH", name20(i.name), nb))
    ih += struct.pack("<20sH", name20("EOI"), nib)
    mod_end = b"\0" * 10

    def chunk(cid, data):
        return cid + struct.pack("<I", len(data)) + data + (b"\0" if len(data) & 1 else b"")

    def zstr(s):
        d = s.encode("latin-1") + b"\0"
        return d + (b"\0" if len(d) & 1 else b"")
    info = chunk(b"ifil", struct.pack("<HH", 2, 1))
    for k in ("isng", "INAM", "ICRD", "IENG", "IPRD", "ICOP", "ICMT", "ISFT"):
        if k in sf.info:
            info += chunk(k.encode(), zstr(sf.info[k]))
    pdta = (chunk(b"phdr", ph) + chunk(b"pbag", pbag) + chunk(b"pmod", mod_end) + chunk(b"pgen", pgen) +
            chunk(b"inst", ih) + chunk(b"ibag", ibag) + chunk(b"imod", mod_end) + chunk(b"igen", igen) +
            chunk(b"shdr", shdr))
    body = (chunk(b"LIST", b"INFO" + info) + chunk(b"LIST", b"sdta" + chunk(b"smpl", bytes(smpl))) +
            chunk(b"LIST", b"pdta" + pdta))
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + len(body)) + b"sfbk" + body)
