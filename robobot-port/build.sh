#!/bin/sh
# Regenerate the C code from the game image and rebuild everything.
set -e
cd "$(dirname "$0")"
python3 recomp/armrecomp.py game/code_decompressed.bin recomp/functions_rea.txt build/gen > build/recomp.log
head -5 build/recomp.log
make -j"$(nproc)" 2>&1 | grep -E "error|warning" || true
