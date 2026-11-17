#!/bin/sh
# Regenerate the C code from the game image and rebuild everything.
set -e
cd "$(dirname "$0")"
# functions are discovered from the entry point (analyze/discover.py); per-game hints come from games/<title id>.toml
python3 recomp/armrecomp.py game/code_decompressed.bin - build/gen > build/recomp.log
cat build/gen/discovery.txt
head -5 build/recomp.log
make -j"$(nproc)" 2>&1 | grep -E "error|warning" || true
