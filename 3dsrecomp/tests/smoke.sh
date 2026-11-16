#!/bin/sh
# Scripted smoke test for one game: runs the [tests] input of games/<title id>.toml from a fresh save in
# build/testgame and writes the screenshots it names. Usage: tests/smoke.sh <title id>
cd "$(dirname "$0")/.." || exit 1
f="games/$1.toml"
[ -f "$f" ] || { echo "no $f"; exit 1; }
eval "$(python3 -c "
import tomllib, shlex, sys
t = tomllib.load(open(sys.argv[1], 'rb')).get('tests', {})
print('IN=' + shlex.quote(t.get('input', '')), 'SH=' + shlex.quote(t.get('shots', '')), 'FR=' + str(t.get('frames', 600)))" "$f")"
tests/play.sh "$IN" "$SH" "$FR"
