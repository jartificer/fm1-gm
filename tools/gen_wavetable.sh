#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# build/gen/gm_wavetable.h from the SoundFont, when it is missing or older than its sources (host builds).
cd "$(dirname "$0")/.."
H=build/gen/gm_wavetable.h
for f in assets/soundfont/fm1gm.sf2 assets/soundfont/fm1gm_sustain.json tools/gen_wavetable.py tools/gm_shapes.py tools/sf2.py; do
    [ -f "$H" ] && [ "$H" -nt "$f" ] || { python3 tools/gen_wavetable.py "$H" >/dev/null; exit $?; }
done
