#!/usr/bin/env bash
# Integration tests for the mzset module.
# Assumes: redis-server + redis-cli are on PATH.
# Loads mzset.so into a temporary redis-server on port 6399.

set -u
cd "$(dirname "$0")/.."

MODULE="$(pwd)/mzset.so"
PORT=6399
CLI="redis-cli -p $PORT"

if [ ! -f "$MODULE" ]; then
    echo "mzset.so not found. Run: make" >&2
    exit 1
fi

# start redis
redis-server --port $PORT --daemonize yes --save "" --appendonly no \
             --loadmodule "$MODULE" \
             --logfile /tmp/mzset-redis.log \
             --pidfile /tmp/mzset-redis.pid
sleep 0.3

cleanup() {
    if [ -f /tmp/mzset-redis.pid ]; then
        kill "$(cat /tmp/mzset-redis.pid)" 2>/dev/null || true
        rm -f /tmp/mzset-redis.pid
    fi
}
trap cleanup EXIT

pass=0
fail=0

check() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        printf "  PASS  %s\n" "$desc"
        pass=$((pass+1))
    else
        printf "  FAIL  %s\n        expected: %q\n        actual:   %q\n" \
               "$desc" "$expected" "$actual"
        fail=$((fail+1))
    fi
}

$CLI FLUSHALL > /dev/null

echo "-- schema / basic add --"
check "create ok"    "OK"    "$($CLI MZ.CREATE lb SCHEMA f64:desc i64:asc bool:asc)"
check "add alice"    "1"     "$($CLI MZ.ADD lb alice 87.5 1700000000 1)"
check "add bob"      "1"     "$($CLI MZ.ADD lb bob   92.0 1700000010 0)"
check "add carol"    "1"     "$($CLI MZ.ADD lb carol 87.5 1699999900 1)"
check "card=3"       "3"     "$($CLI MZ.CARD lb)"

echo "-- ordering (score desc, ts asc, bool asc) --"
# bob (92.0) < alice (87.5, ts=1700000000) < carol (87.5, ts=1699999900) ... wait
# score DESC:   bob first (92.0), then the 87.5 pair
# among 87.5:  ts ASC -> carol (1699999900) before alice (1700000000)
check "range 0 -1"   "bob
carol
alice" "$($CLI MZ.RANGE lb 0 -1)"

echo "-- rank --"
check "rank bob"    "0"  "$($CLI MZ.RANK lb bob)"
check "rank carol"  "1"  "$($CLI MZ.RANK lb carol)"
check "rank alice"  "2"  "$($CLI MZ.RANK lb alice)"
check "rank rev alice"  "0"  "$($CLI MZ.RANK lb alice REV)"

echo "-- score --"
check "score alice n" "3" "$($CLI MZ.SCORE lb alice | wc -l | tr -d ' ')"

echo "-- update: change alice's score to 95.0 (should now be top) --"
check "update ret"   "1"    "$($CLI MZ.UPDATE lb alice 0 95.0)"
check "alice now top" "alice" "$($CLI MZ.RANGE lb 0 0)"

echo "-- incrby --"
check "incrby bob ts" "1700000011" "$($CLI MZ.INCRBY lb bob 1 1)"   # bob.ts (idx=1) += 1
check "incrby bob f64" "94" "$($CLI MZ.INCRBY lb bob 0 2)"          # bob.score (idx=0) += 2.0
# after incrby, alice (95.0) still tops bob (94.0)
check "top still alice" "alice"  "$($CLI MZ.RANGE lb 0 0)"

echo "-- withfields --"
out=$($CLI MZ.RANGE lb 0 0 WITHFIELDS)
first_line=$(printf '%s\n' "$out" | head -1)
check "withfields member" "alice" "$first_line"

echo "-- rem --"
check "rem bob"      "1"    "$($CLI MZ.REM lb bob)"
check "card=2"       "2"    "$($CLI MZ.CARD lb)"

echo "-- NX on existing --"
check "add alice NX" "0"    "$($CLI MZ.ADD lb alice 1 2 0 NX)"

echo "-- wrong type --"
$CLI SET plain hello > /dev/null
out=$($CLI MZ.ADD plain m 1 2 3 2>&1 | head -1)
case "$out" in
    *WRONGTYPE*) printf "  PASS  wrong type errors\n"; pass=$((pass+1)) ;;
    *)           printf "  FAIL  wrong type errors, got: %s\n" "$out"; fail=$((fail+1)) ;;
esac

echo "-- rank on missing member --"
$CLI DEL lb > /dev/null
$CLI MZ.CREATE lb SCHEMA i64:asc > /dev/null
$CLI MZ.ADD lb x 1 > /dev/null
out=$($CLI MZ.RANK lb nobody)
check "rank missing = nil" "" "$out"

echo "-- rdb roundtrip --"
$CLI DEL lb > /dev/null
$CLI MZ.CREATE lb SCHEMA i64:desc f64:asc > /dev/null
$CLI MZ.ADD lb a 10 1.5 > /dev/null
$CLI MZ.ADD lb b 20 2.5 > /dev/null
$CLI MZ.ADD lb c 20 1.0 > /dev/null
before=$($CLI MZ.RANGE lb 0 -1)
$CLI DEBUG RELOAD > /dev/null
after=$($CLI MZ.RANGE lb 0 -1)
check "rdb roundtrip preserves order" "$before" "$after"

echo "-- MZ.ALTER: add a field, order preserved, RDB roundtrip --"
$CLI DEL lb > /dev/null
$CLI MZ.CREATE lb SCHEMA f64:desc i64:asc > /dev/null
$CLI MZ.ADD lb a 87.5 100 > /dev/null
$CLI MZ.ADD lb b 92.0 90  > /dev/null
$CLI MZ.ADD lb c 87.5 50  > /dev/null
order_before=$($CLI MZ.RANGE lb 0 -1)
check "alter ok"         "OK"            "$($CLI MZ.ALTER lb ADD bool:asc DEFAULT 1)"
check "order preserved"  "$order_before" "$($CLI MZ.RANGE lb 0 -1)"
check "card unchanged"   "3"             "$($CLI MZ.CARD lb)"
check "existing has 3 fields now" "3"    "$($CLI MZ.SCORE lb a | wc -l | tr -d ' ')"
check "default value = 1" "1"            "$($CLI MZ.SCORE lb a | tail -1)"
check "add now needs 3 vals; 2 fails" "ERR field count mismatch" \
                                          "$($CLI MZ.ADD lb d 1 2 2>&1 | head -1)"
check "add with 3 vals ok" "1"           "$($CLI MZ.ADD lb d 100 1 0)"
check "d (f64=100) is now top"  "d"      "$($CLI MZ.RANGE lb 0 0)"
$CLI DEBUG RELOAD > /dev/null
check "alter survives RDB reload" "3"    "$($CLI MZ.SCORE lb a | wc -l | tr -d ' ')"
check "post-reload top is d"      "d"    "$($CLI MZ.RANGE lb 0 0)"
# stack ALTER up to 16 fields
$CLI DEL big > /dev/null
$CLI MZ.CREATE big SCHEMA i64:asc > /dev/null
$CLI MZ.ADD big m 1 > /dev/null
for i in $(seq 1 15); do
    $CLI MZ.ALTER big ADD i64:asc DEFAULT 0 > /dev/null
done
out=$($CLI MZ.ALTER big ADD i64:asc DEFAULT 0 2>&1 | head -1)
case "$out" in
    ERR*max*) printf "  PASS  alter blocked at 16 fields\n"; pass=$((pass+1)) ;;
    *)        printf "  FAIL  alter cap: got %s\n" "$out"; fail=$((fail+1)) ;;
esac

echo
echo "== $pass passed, $fail failed =="
[ "$fail" = 0 ]
