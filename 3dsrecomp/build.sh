#!/bin/sh
# Regenerate the C code from the game image and rebuild everything.
set -e
cd "$(dirname "$0")"
# the title's function list (games/<title id>/functions_rea.txt; functions_extra.txt next to it is read too)
TITLE=$(python3 -c "import sys; sys.path.insert(0, 'recomp'); import layout; print(f'{layout.read_layout(\"game/exheader.bin\").title_id:016x}')")
python3 recomp/armrecomp.py game/code_decompressed.bin "games/$TITLE/functions_rea.txt" build/gen > build/recomp.log
head -5 build/recomp.log
make -j"$(nproc)" 2>&1 | grep -E "error|warning" || true
