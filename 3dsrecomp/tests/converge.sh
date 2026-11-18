#!/bin/sh
# Like 3dsrecomp run's restart loop, in the test copy: play a script; code the analysis missed (main program or
# modules) is noted; rebuild and play again, until a run finds nothing new (at most 8 rounds).
# Usage: tests/converge.sh <title id> "<RB_INPUT>" "<RB_SHOT>" <exit frame>
cd "$(dirname "$0")/.." || exit 1
id=$1; T=work/$id/testgame
for round in 1 2 3 4 5 6 7 8; do
    rm -f $T/missing_functions.txt $T/missing_modules.txt
    tests/play.sh "$2" "$3" "$4" "$id"
    new=0
    if [ -s $T/missing_functions.txt ]; then
        cut -d' ' -f1 $T/missing_functions.txt | sort -u | while read a; do grep -q "^$a" work/$id/extra_functions.txt 2>/dev/null || echo "$a # found at run time" >> work/$id/extra_functions.txt; done
        new=1
    fi
    [ -s $T/missing_modules.txt ] && new=1
    [ $new = 0 ] && { echo "converged after $round run(s)"; exit 0; }
    echo "round $round: missing code noted: $(cat $T/missing_functions.txt $T/missing_modules.txt 2>/dev/null | tr '\n' ' ')"
    ./3dsrecomp build "$id" | grep -E "fail|built"
done
