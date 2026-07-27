#!/usr/bin/env bash
# Golden-output tests for the Group 7 ATM.
#
# Each case feeds a scripted stdin to one of the builds and compares the output
# against a saved "golden" file. Timestamps are the only non-deterministic part
# of the output, so we normalise them to <TIMESTAMP> before comparing.
#
#   ./run_tests.sh          run all cases, report PASS/FAIL, exit 1 on any fail
#   UPDATE=1 ./run_tests.sh regenerate the golden files from current output
#
# Persistent cases run in a throwaway directory so atm_data.dat never leaks
# between cases or into the repo.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CASES="$ROOT/tests/cases"
GOLDEN="$ROOT/tests/golden"
INMEM="$ROOT/in_memory/atm"
PERSIST="$ROOT/persistent/atm"

pass=0
fail=0

# strip the wall-clock timestamps so runs are comparable
normalize() {
    sed -E 's/[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}/<TIMESTAMP>/g'
}

# check <name> <actual-output-file>: diff against golden, or update it
check() {
    local name="$1" actual="$2"
    local want="$GOLDEN/$name.out"
    if [ "${UPDATE:-0}" = "1" ]; then
        cp "$actual" "$want"
        echo "UPDATED $name"
        return
    fi
    if [ ! -f "$want" ]; then
        echo "FAIL    $name (no golden file; run UPDATE=1 first)"
        fail=$((fail + 1))
        return
    fi
    if diff -u "$want" "$actual" > /tmp/atm_test_diff.$$ 2>&1; then
        echo "PASS    $name"
        pass=$((pass + 1))
    else
        echo "FAIL    $name"
        cat /tmp/atm_test_diff.$$
        fail=$((fail + 1))
    fi
    rm -f /tmp/atm_test_diff.$$
}

# single-run case: run_case <name> <binary>
run_case() {
    local name="$1" bin="$2"
    local work; work="$(mktemp -d)"
    ( cd "$work" && "$bin" ) < "$CASES/$name.in" 2>&1 | normalize > "$work/out"
    check "$name" "$work/out"
    rm -rf "$work"
}

# two-run persistence case: same data file, two scripted sessions back to back
run_reload() {
    local name="$1" bin="$2"
    local work; work="$(mktemp -d)"
    {
        ( cd "$work" && "$bin" ) < "$CASES/$name.1.in" 2>&1
        ( cd "$work" && "$bin" ) < "$CASES/$name.2.in" 2>&1
    } | normalize > "$work/out"
    check "$name" "$work/out"
    rm -rf "$work"
}

# build both variants up front
echo "Building..."
make -C "$ROOT/in_memory" >/dev/null || { echo "in-memory build failed"; exit 1; }
make -C "$ROOT/persistent" >/dev/null || { echo "persistent build failed"; exit 1; }

mkdir -p "$GOLDEN"

run_case   inmem_session  "$INMEM"
run_case   inmem_limits   "$INMEM"
run_case   inmem_lockout  "$INMEM"
run_case   inmem_unlock   "$INMEM"
run_case   inmem_pesewas  "$INMEM"
run_case   persist_admin  "$PERSIST"
run_reload persist_reload "$PERSIST"

echo
echo "== $pass passed, $fail failed =="
[ "$fail" -eq 0 ]
