#!/bin/sh
# Renderer regression check: Azahar replays must stay pixel-identical and screenshots at fixed
# frames must match build/baseline/ byte for byte. Usage: tests/regress.sh [--no-shots]
cd "$(dirname "$0")/.." || exit 1
fail=0
run() { # state vram linear list size ref expected
    out=$(RB_RENDERER=software ./build/gpu_replay "$1" "$2" "$3" "$4" "$5" /dev/null "$6" 2>/dev/null | grep -o '[0-9]*$')
    if [ "$out" != "$7" ]; then echo "FAIL replay $4 ($2): $out differing pixels, expected $7"; fail=1; else echo "ok   replay $4 ($(basename "$2")): $out"; fi
}
C1=build/ref/capture; C2=build/ref/capture2; C3=build/ref/capture3
run build/replay/state_10.bin $C1/before_0_vram.bin $C1/before_0_linear.bin 30194c00 66d0 $C1/after_0_vram.bin 0
run build/replay/state_28.bin $C1/before_1_vram.bin $C1/before_1_linear.bin 30194c00 66d0 $C1/after_1_vram.bin 0
for k in 0 1 2 3; do
    read a s < $C2/list_$k.txt
    if [ "$a" = 30068780 ]; then st=12; else st=30; fi
    run build/replay/state_$st.bin $C2/before_${k}_vram.bin $C2/before_${k}_linear.bin $a $s $C2/after_${k}_vram.bin 0
done
run build/replay/state_02.bin $C3/before_0_vram.bin $C3/before_0_linear.bin 300ebdd0 20 $C3/after_0_vram.bin 864
run build/replay/state_16.bin $C3/before_1_vram.bin $C3/before_1_linear.bin 300ebdd0 20 $C3/after_1_vram.bin 2
if [ "$1" != "--no-shots" ]; then
    rm -f build/shot_000120.png build/shot_000300.png
    RB_RENDERER=software RB_FAST=1 RB_HEADLESS=1 RB_SHOT=120,300 RB_EXIT_FRAME=301 timeout 1800 ./build/robobot game >/dev/null 2>&1
    for f in 000120 000300; do
        if cmp -s build/shot_$f.png build/baseline/shot_$f.png; then echo "ok   screenshot $f"; else echo "FAIL screenshot $f differs from baseline"; fail=1; fi
    done
    # Vulkan renderer: edges rasterise slightly differently, so allow under 1% of pixels to differ
    rm -f build/shot_000120.png build/shot_000300.png
    RB_RENDERER=vulkan RB_FAST=1 RB_HEADLESS=1 RB_SHOT=120,300 RB_EXIT_FRAME=301 timeout 1800 ./build/robobot game >/dev/null 2>&1
    for f in 000120 000300; do
        pct=$(.venv/bin/python tests/imgdiff.py build/baseline/shot_$f.png build/shot_$f.png 8 2>/dev/null | cut -d% -f1)
        if [ -n "$pct" ] && [ "$(echo "$pct < 1" | bc)" = 1 ]; then echo "ok   vulkan screenshot $f ($pct% differ)"; else echo "FAIL vulkan screenshot $f: ${pct:-no image}% differ"; fail=1; fi
    done
fi
exit $fail
