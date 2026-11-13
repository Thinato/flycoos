#!/usr/bin/env python3
"""Mutation check: each mutant alone must make core_test fail. Proves the new tests bite.

    python3 tools/review/mutate.py RIG MUTANTS.py [--dry] [--workers N --worker K] [--source DIR]

MUTANTS.py (keep it in build/review/RIG/, it's scratch) defines a list of
    M = [(path, old_text, new_text, name), ...]
where old_text must appear exactly once in path (relative to the repo) and new_text breaks one rule the
change added: a flipped comparison, a dropped clause, a removed line, a changed data value.

Each worker copies the source (default: this repo's working tree; --source an agent's worktree) into
build/review/RIG/mutK, builds core_test there (release) and checks the baseline passes first. Then it
applies one mutant at a time, rebuilds, runs core_test and restores the file. Output: one line per mutant,
"caught", "SURVIVED" or "BUILD FAIL" (a mutant that doesn't compile proves nothing: rewrite it), then
"X of Y caught". A survivor is either a missing test (add one) or an equivalent mutant (say why in the
report). --dry only checks every old_text is found once. ONLY="name|name" runs just those.
Split a long list across workers that run at the same time: --workers 3 --worker 0, 1, 2.
"""
import os
import runpy
import subprocess
import sys

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), '..', '..'))
args = sys.argv[1:]
if len(args) < 2:
    sys.exit(__doc__)
rig, mutants = REPO + '/build/review/' + args[0], args[1]
M = runpy.run_path(mutants)['M']
dry = '--dry' in args
workers = int(args[args.index('--workers') + 1]) if '--workers' in args else 1
worker = int(args[args.index('--worker') + 1]) if '--worker' in args else 0
source = args[args.index('--source') + 1] if '--source' in args else REPO
W = f'{rig}/mut{worker}'

os.makedirs(W, exist_ok=True)
for d in ('src', 'shaders', 'data', 'prefabs', 'tests', 'assets', 'cmake'):
    if os.path.isdir(f'{source}/{d}'):
        subprocess.run(['rsync', '-a', '--delete', f'{source}/{d}/', f'{W}/{d}/'], check=True)
for f in ('CMakeLists.txt', 'CMakePresets.json', 'sanitize-ignore.txt'):
    if os.path.exists(f'{source}/{f}'):
        subprocess.run(['cp', f'{source}/{f}', W], check=True)

bad = [(name, open(f'{W}/{path}').read().count(a)) for path, a, b, name in M if open(f'{W}/{path}').read().count(a) != 1]
for name, n in bad:
    print(f'BAD {name}: old_text found {n} times')
if dry or bad:
    print(len(M), 'mutants checked,', len(bad), 'bad')
    sys.exit(1 if bad else 0)

if not os.path.exists(f'{W}/build/build.ninja'):
    subprocess.run(['cmake', '-S', W, '-B', f'{W}/build', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                    '-DFETCHCONTENT_FULLY_DISCONNECTED=ON', f'-DFETCHCONTENT_BASE_DIR={REPO}/build/release/_deps'],
                   check=True, capture_output=True)


def build():
    return subprocess.run(['cmake', '--build', f'{W}/build', '--target', 'core_test'], capture_output=True, text=True)


def passes():
    try:
        t = subprocess.run([f'{W}/build/core_test'], capture_output=True, text=True, cwd=W, timeout=1200)
    except subprocess.TimeoutExpired:  # a hang counts as a failure
        return False
    return 'all checks passed' in t.stdout + t.stderr


assert build().returncode == 0 and passes(), 'the baseline must build and pass before mutating'
todo = [m for i, m in enumerate(M) if i % workers == worker]
if os.environ.get('ONLY'):
    todo = [m for m in M if m[3] in os.environ['ONLY'].split('|')]
caught = 0
for path, a, b, name in todo:
    p = f'{W}/{path}'
    src = open(p).read()
    open(p, 'w').write(src.replace(a, b))
    try:
        res = 'BUILD FAIL' if build().returncode != 0 else 'SURVIVED' if passes() else 'caught'
    finally:
        open(p, 'w').write(src)
    caught += res == 'caught'
    print(f'{res:10} {name}', flush=True)
print(f'{caught} of {len(todo)} caught')
