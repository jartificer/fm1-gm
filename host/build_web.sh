#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the FM1-GM emulator for the web page: the app (host/gm_web.c), the engine and the sequencer
# as one freestanding WebAssembly module, web/fm1gm.wasm. Needs clang and wasm-ld (LLVM 13 or later);
# no libc, no Emscripten. Same sources and -ffp-contract=off as the firmware.
set -e
cd "$(dirname "$0")/.."
CC="${CC:-clang}"
mkdir -p build/gen build/web
[ -f build/gen/felucca_font.h ] || python3 tools/gen_font.py build/gen/felucca_font.h >/dev/null
sh tools/gen_wavetable.sh
for f in host/gm_web.c firmware/src/dsp/gm.c firmware/src/dsp/seq.c; do
    $CC --target=wasm32 -O2 -ffp-contract=off -std=c99 -ffreestanding -nostdlib -fno-builtin-printf \
        -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
        -DOM_HOST -Ifirmware/src -Ifirmware/src/dsp -Ibuild/gen -c "$f" -o "build/web/$(basename "$f" .c).o"
done
$CC --target=wasm32 -nostdlib -Wl,--no-entry -Wl,--export-dynamic -Wl,--initial-memory=8388608 \
    -Wl,-z,stack-size=262144 -Wl,--strip-all -o web/fm1gm.wasm build/web/gm_web.o build/web/gm.o build/web/seq.o
echo "web: web/fm1gm.wasm ($(wc -c < web/fm1gm.wasm) bytes)"
