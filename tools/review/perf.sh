#!/bin/zsh
# perf.sh RIG RUNS FRAMES [FLY_VAR=value ...]: the same scene with build/review/RIG/before and /after,
# alternating (before1 after1 before2 ...), no frames written. One line per run: the game's "frames" summary
# (mean, p99, max ms), the most particles if FLY_FXSTAT=1, and the hitches (frames over 100 ms).
# Drop runs with a hitch from both sides before comparing; report every run, not just the average.
RIG=${0:A:h:h:h}/build/review/$1; runs=$2 frames=$3; shift 3
mkdir -p $RIG/perf
for ((i=1; i<=runs; i++)); do for who in before after; do
  log=$RIG/perf/$who$i.log
  (cd $RIG/$who && env FLY_SCENE=1 "$@" build/flycoos --world $RIG/a.flyw --new-game --no-serve --frames $frames --time ${TIME:-0.42}) > $log 2>&1
  fx=$(grep fxstat $log | awk '{print $4}' | sort -n | tail -1)
  echo "$who$i $(grep '^frames' $log | sed 's/ | chunks.*//')${fx:+ | particles max $fx} | hitches $(grep -c '^hitch' $log)"
done; done
