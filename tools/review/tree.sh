#!/bin/zsh
# tree.sh RIG WHO SOURCE: a hooked, built copy of the game in build/review/RIG/WHO (see CLAUDE.md, "Review films").
#   SOURCE is a git revision (before: usually HEAD) or a directory to copy (after: the repo or an agent's worktree).
#   Also copies worlds/arena.flyw to build/review/RIG/a.flyw (films never touch worlds/).
# Run it again to pick up new edits: it re-copies, re-hooks and rebuilds only what changed.
set -e
REPO=${0:A:h:h:h}; RIG=$REPO/build/review/$1; WHO=$2; SRC=$3
[[ -n $1 && -n $WHO && -n $SRC ]] || { echo "usage: tree.sh RIG WHO REVISION|DIR"; exit 2; }
T=$RIG/$WHO; mkdir -p $T
if [[ -d $SRC ]]; then
  for d in src shaders data prefabs tests assets cmake; do [[ -d $SRC/$d ]] && rsync -a --delete $SRC/$d/ $T/$d/; done
  cp $SRC/CMakeLists.txt $SRC/CMakePresets.json $SRC/sanitize-ignore.txt $T/ 2>/dev/null || true
else
  git -C $REPO rev-parse --verify -q "$SRC^{commit}" >/dev/null || { echo "not a revision or a directory: $SRC"; exit 2; }
  rm -rf $T/src; git -C $REPO archive "$SRC" | tar -x -C $T --exclude worlds --exclude docs
fi
python3 $REPO/tools/review/scene_hook.py $T
[[ -f $RIG/a.flyw ]] || cp $REPO/worlds/arena.flyw $RIG/a.flyw
[[ -f $T/build/build.ninja ]] || cmake -S $T -B $T/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DFETCHCONTENT_BASE_DIR=$REPO/build/release/_deps > $RIG/$WHO-cfg.log 2>&1
cmake --build $T/build --target flycoos shaders > $RIG/$WHO-build.log 2>&1 && echo "$WHO built: $T/build/flycoos" || { echo "$WHO build FAILED, see $RIG/$WHO-build.log"; exit 1; }
