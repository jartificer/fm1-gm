#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/fm1_rescue.py's write policy against a simulated UBOOT (no hardware, no pyusb): the chip and
flash checks, the boot area that is never written, which sectors are written, the final read.
Run from the repo root:
  python3 tests/rescue_test.py
It needs build/fm1gm.fwsc (its flash image stands in for stock V15). With FM1_STOCK_DIR set to a folder
holding M-VAVE's official FM-1.fwsc files, it also runs the real V16 (and V15) through it."""
import hashlib
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import fm1_rescue as R  # noqa: E402

failed = 0


def ok(cond, what):
    global failed
    print(f"{what:<64} {'ok' if cond else 'FAIL'}")
    if not cond:
        failed += 1


class FakeUboot(R.Uboot):
    """the WL82 UBOOT with JieLi's flash loader: commands on a simulated 1 MiB NOR"""

    def __init__(self, flash, fid=0x856014, key=0x980F):
        super().__init__(None)
        self.nor, self.fid, self.key = bytearray(flash), fid, key
        self.erased = []

    def inquiry(self):
        return "WL82", "UBOOT1.00"

    def start_loader(self, blob):
        pass

    def chip(self):
        return self.key, 3, self.fid

    def read(self, addr, n):
        return bytes(self.nor[addr:addr + n])

    def cmd(self, cmd, args=b""):
        if cmd == R.CMD_ERASE_SECTOR:
            a = struct.unpack(">I", args)[0]
            self.erased.append(a)
            self.nor[a:a + R.SECTOR] = b"\xFF" * R.SECTOR
        return b"\0" * 14

    def write_block(self, cmd, addr, data):
        assert cmd == R.CMD_WRITE_FLASH
        for i, b in enumerate(data):
            self.nor[addr + i] &= b


def flash_of(image, app=None):
    """a device flash: image at 0, 0xFF above it; app (a flash image) over [0x4000, its end)"""
    f = bytearray(b"\xFF" * R.FLASH_SIZE)
    f[0:len(image)] = image
    if app is not None:
        f[R.WRITE_MIN:len(app)] = app[R.WRITE_MIN:]
    return f


def run(dev, image, name, write=True):
    out = []
    try:
        done = R.rescue(dev, image, b"", write, str(TMP), say=lambda *a, **k: out.append(" ".join(map(str, a))),
                        name=name)
        return done, "\n".join(out)
    except R.Fail as e:
        return None, str(e)


def stock_cases(name, image, ours):
    """name's image back over FM1-GM (ours: its package's flash image)"""
    end = len(image)
    for fid in R.FLASH_IDS:
        dev = FakeUboot(flash_of(image, ours), fid=fid)
        before = bytes(dev.nor)
        done, out = run(dev, image, name)
        ok(done is True and bytes(dev.nor[:end]) == image and dev.nor[end:] == before[end:],
           f"{name}: back over FM1-GM, flash id {fid:06X} -> {name}, rest kept")
        ok(all(R.WRITE_MIN <= a < end for a in dev.erased), f"  only sectors in [0x4000, {end:#x}) erased")
    dev = FakeUboot(flash_of(image, ours))
    done, out = run(dev, image, name, write=False)
    ok(done is False and dev.erased == [] and "check passed" in out, f"{name}: without --write: nothing written")
    other = bytearray(image)
    other[0x1000] ^= 0xFF                                       # the device has another boot area
    dev = FakeUboot(flash_of(bytes(other), ours))
    done, out = run(dev, image, name)
    ok(done is None and dev.erased == [] and "not writing" in out and "other" in out,
       f"{name}: another boot area -> refused, nothing written")
    dev = FakeUboot(flash_of(image, ours), fid=0xEF4014)
    done, out = run(dev, image, name)
    ok(done is None and dev.erased == [] and "not the FM-1" in out, f"{name}: another flash part -> refused")
    dev = FakeUboot(flash_of(image, ours), key=0x1234)
    done, out = run(dev, image, name)
    ok(done is None and dev.erased == [], f"{name}: another chip key -> refused")
    try:
        FakeUboot(flash_of(image)).write_sector(end, b"\0" * R.SECTOR, end)
        ok(False, f"{name}: a sector past the image is refused")
    except R.Fail:
        ok(True, f"{name}: a sector past the image is refused")


TMP = Path(os.environ.get("TMPDIR", "/tmp")) / "fm1_rescue_test"
TMP.mkdir(parents=True, exist_ok=True)
pkg = ROOT / "build/fm1gm.fwsc"
if not pkg.exists():
    print("rescue tests: skipped (no build/fm1gm.fwsc: run ./build.sh)")
    sys.exit(0)
ours = R.package_flash_image(pkg.read_bytes())
ok(len(ours) == 0x93000, "FM1-GM's flash image is [0, 0x93000)")
stand_in = bytearray(ours)                                     # a V15 stand-in: same size, another app
stand_in[0x8000:0x9000] = bytes(range(256)) * 16
stock_cases("V15", bytes(stand_in), ours)
d = os.environ.get("FM1_STOCK_DIR")
for f in sorted(Path(d).glob("*.fwsc")) if d else []:
    raw = f.read_bytes()
    stock = R.STOCK.get(hashlib.sha256(raw).hexdigest())
    ok(stock is not None, f"{f.name}: a pinned official package")
    if stock:
        name, size = stock
        image = R.package_flash_image(raw)
        ok(len(image) == size, f"{f.name}: {name}, flash image {size:#x}")
        stock_cases(name, image, ours)
print("rescue tests failed" if failed else "rescue tests passed")
sys.exit(1 if failed else 0)
