#!/bin/zsh
# gates.sh [REPO]: every gate a commit needs (see CLAUDE.md). Run it in the background; takes minutes.
# Needs build/release and build/dev configured (cmake --preset release / dev). Pass = both builds
# "exit 0 warnings 0", both "all checks passed" (dev runs under ASan/UBSan), "all e2e checks passed".
cd ${1:-${0:A:h:h:h}}; L=${TMPDIR:-/tmp}
cmake --build build/release > $L/fly-rel-build.txt 2>&1; echo "REL-BUILD-DONE exit $? warnings $(grep -c 'warning:' $L/fly-rel-build.txt)"
./build/release/core_test 2>&1 | grep -E "^FAIL|check\(s\) failed|all checks passed"
cmake --build build/dev > $L/fly-dev-build.txt 2>&1; echo "DEV-BUILD-DONE exit $? warnings $(grep -c 'warning:' $L/fly-dev-build.txt)"
./build/dev/core_test 2>&1 | grep -E "^FAIL|check\(s\) failed|all checks passed|ERROR: AddressSanitizer|runtime error"
python3 tools/test_mcp.py 2>&1 | tail -3
echo GATES-EXIT
