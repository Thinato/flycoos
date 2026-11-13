#!/bin/zsh
# shoot.sh RIG WHO NAME FRAMES [FLY_VAR=value ...]: films a scene in the arena copy with build/review/RIG/WHO
# into build/review/RIG/shots/WHO+NAME/fNNNN.bmp (every second frame, counted from the scene start, so even
# numbers; 60 frames = 1 s). The FLY_* variables are listed at the top of tools/review/scene_hook.py.
# Runs in the tree folder, so the game reads that tree's data/. Never serves MCP (--no-serve), so it can't touch the game on port 47800. TIME: time of day (default 0.42).
RIG=${0:A:h:h:h}/build/review/$1; who=$2 name=$3 frames=$4; shift 4
out=$RIG/shots/$who$name; rm -rf $out; mkdir -p $out
(cd $RIG/$who && env FLY_SCENE=1 FLY_SHOTS=$out "$@" build/flycoos --world $RIG/a.flyw --new-game --no-serve \
  --frames $frames --time ${TIME:-0.42}) > $out.log 2>&1
echo "$who$name: $(ls $out | wc -l | tr -d ' ') frames, log $out.log"
