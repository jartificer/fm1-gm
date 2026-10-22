#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# FM1-GM host tests: the maths and platform pieces kept from Felucca / X0X / FoMni, the GM engine and
# the sequencer, the flash store, and the whole firmware app in the simulator (tests/scenarios/*.gm:
# UI, recording, tracks, persistence), with screenshots and audio in build/scenarios/.
#   tests/run_tests.sh
set -u
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
FAIL=0
OUT=build/host
mkdir -p "$OUT" build/scenarios
run() {
    name="$1"; shift
    if "$@" > "$OUT/$name.log" 2>&1; then
        echo "  ok   $name"
    else
        echo "  FAIL $name (see $OUT/$name.log)"
        tail -15 "$OUT/$name.log" | sed 's/^/       /'
        FAIL=1
    fi
}
run fastmath sh -c "$CC -O2 -ffp-contract=off -Wall -Wextra -Werror -o $OUT/fastmath_test tests/host/fastmath_test.c -lm && $OUT/fastmath_test"
run encoder sh -c "$CC -O2 -w -Ifirmware/hal -o $OUT/encoder_test tests/host/encoder_test.c && $OUT/encoder_test"
run uac sh -c "$CC -O2 -w -Ifirmware/src -Ifirmware/hal -o $OUT/uac_test tests/host/uac_test.c -lm && $OUT/uac_test"
run trs sh -c "$CC -O2 -w -Ifirmware/src -Ifirmware/hal -o $OUT/trs_test tests/host/trs_test.c && $OUT/trs_test"
sh tools/gen_wavetable.sh
run gm sh -c "$CC -O2 -ffp-contract=off -std=c99 -Wall -Wextra -Wdouble-promotion -Werror -DOM_HOST -Ifirmware/src/dsp -Ibuild/gen \
    -o $OUT/gm_test tests/host/gm_test.c firmware/src/dsp/gm.c firmware/src/dsp/seq.c -lm && $OUT/gm_test"
# the engine again under AddressSanitizer: a read past a table is silent on the host, noise on the device
if echo 'int main(void){return 0;}' | $CC -fsanitize=address -x c -o $OUT/asan_probe - >/dev/null 2>&1; then
    run gm-asan sh -c "$CC -O1 -g -fsanitize=address -std=c99 -DOM_HOST -Ifirmware/src/dsp -Ibuild/gen \
        -o $OUT/gm_test_asan tests/host/gm_test.c firmware/src/dsp/gm.c firmware/src/dsp/seq.c -lm && $OUT/gm_test_asan"
else
    echo "  skip gm-asan (the compiler has no AddressSanitizer)"
fi
run storage sh -c "$CC -O2 -Wall -Werror -o $OUT/storage_test tests/storage_test.c && $OUT/storage_test"
# the update path, against the firmware package (Felucca's tests; needs ./build.sh)
if [ -f build/fm1gm.fwsc ]; then
    run ota-entry sh -c "$CC -o $OUT/ota_test tests/ota_test.c && $OUT/ota_test build/fm1gm.fwsc"
    if [ -z "${AC79_SDK:-}" ]; then
        echo "  skip update-loader (needs AC79_SDK, as the build)"
    else
    run update-loader sh -c "head -c 100000 build/fm1gm.bin > $OUT/old_app.bin && \
        python3 tools/fm1pkg_make.py $OUT/old_app.bin build/loader/ota.bin $OUT/old.fwsc >/dev/null && \
        $CC -o $OUT/ldr_test tests/ldr_test.c && $OUT/ldr_test $OUT/old.fwsc build/fm1gm.fwsc"
    fi
else
    echo "  skip update-path tests (no build/fm1gm.fwsc: run ./build.sh)"
fi
run installer python3 tests/install_test.py
if command -v node >/dev/null 2>&1; then
    run web-installer node tests/web_install_test.mjs
    run web-songs node tests/web_song_test.mjs
else
    echo "  skip web-installer (needs node)"
fi
run host-build sh host/build_host.sh
for s in tests/scenarios/*.gm; do
    n=$(basename "$s" .gm)
    mkdir -p "build/scenarios/$n"
    run "scenario-$n" build/host/gm_host "$s" "build/scenarios/$n"
done
[ $FAIL -eq 0 ] && echo "all tests passed" || echo "TESTS FAILED"
exit $FAIL
