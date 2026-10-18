# Building FM1-GM

## Host tests and the simulator

Any machine with a C compiler and Python 3 with Pillow (for the font header):

```
tests/run_tests.sh
```

This builds and runs the engine and sequencer tests (`tests/host/gm_test.c`), the flash store test, the platform tests kept from Felucca, X0X and FoMni, the installer test, and the scenarios in `tests/scenarios/` against the whole app in `build/host/gm_host`. Screenshots and audio go to `build/scenarios/`. `build/host/gm_test WAVDIR` also writes a WAV of every program and drum.

## The device package

Needs Linux x86-64, or Docker on macOS (each tool then runs in a linux/amd64 container; set `JIELI_DOCKER=1` to force it).

1. The JieLi toolchain (clang 4.0.1 for pi32v2):
   ```
   tools/get_toolchain.sh ~/.jieli
   export JIELI_TOOLCHAIN=~/.jieli/toolchain
   ```
2. The JieLi AC79 SDK, release AC79NN_SDK_V1.2.1_2023-12-13 (commit d179b448). The package builder takes the SPL and the config tool from it:
   ```
   git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/ac79-sdk
   export AC79_SDK=~/ac79-sdk
   ```
3. Build:
   ```
   ./build.sh                    # build/fm1gm.fwsc, identity FM-1_600
   ./build.sh --release 0.1      # build/fm1gm-0.1.fwsc, identity FM-1_6000100
   ```

The build checks the image before packaging: entry point, no calls from RAM code into flash, no soft-double routines (the FPU is single precision), the RAM and pool budgets, and that hardware registers are touched only from `firmware/hal/`.

GitHub Actions (`.github/workflows/build.yml`) does all of this on every push and uploads the package as an artifact.

## The web page

`web/` is a static site: `index.html`, `app.js` (the panel and the page), `emu.js` (runs the emulator), `worklet.js` (its audio out), `fm1ota.js` (the installer, a port of `tools/fm1_install.py`), and two built files kept in the repository so the site needs no build step to serve:

- `web/fm1gm.wasm`: `sh host/build_web.sh`. Needs clang and wasm-ld (LLVM 13 or later, `--target=wasm32`); no Emscripten and no libc.
- `web/fm1gm.fwsc`: the device package, copied from `build/fm1gm.fwsc` after `./build.sh`. Update `web/build.txt` with the commit it was built from.

`tests/web_install_test.mjs` (Node 18 or later, run by `tests/run_tests.sh`) tests the installer against a simulated FM-1.

## Layout

- `firmware/hal/`, `firmware/loader/`, `firmware/crt0.S`, `firmware/app.ld`: Felucca's FM-1 platform and update loader, unchanged.
- `firmware/src/main.c`: the unity build (everything that touches the HAL).
- `firmware/src/dsp/gm.c`: the GM engine, a sample player; `gm_wave.h` the wavetable's types, `gm_names.h` the GM names.
- `assets/soundfont/fm1gm.sf2`: the instruments and drums, one sample per program with its attack and a seamless loop. The build turns it into `build/gen/gm_wavetable.h` (`tools/gen_wavetable.py`, 4-bit ADPCM, Python standard library only). The SoundFont itself is made from FluidR3_GM.sf2 by `tools/sf2_reduce.py` (needs numpy and scipy): `python3 tools/sf2_reduce.py -v FluidR3_GM.sf2 assets/soundfont/fm1gm.sf2` (with `-v` it prints each program's sample length and loop, and how well the loop's seam matched, 1 being a perfect match). Its settings (the key whose sample is kept, attack and loop lengths, sample rates) are at the top of the script. The app image must stay within the 581,564-byte app slot; the wavetable takes about 420 KB of it.
- `firmware/src/dsp/seq.c`: the sequencer, run from the audio render; the UI talks to it through a command ring.
- `firmware/src/app/`: the platform layer, settings and song slots, and the UI. The song byte format is `song_stream` / `song_parse` in `firmware/src/dsp/seq.c`.
- `firmware/src/storage.c`: the A/B flash store for the settings and panel (FoMni's), and the song store: every other data sector, shared by the 64 slots, each save a new version in free sectors.
- `host/gm_host.c`: the app on the host, driven by scenario scripts.
- `host/gm_web.c`: the app as WebAssembly for the web page, driven by `web/emu.js`.
