#!/usr/bin/env bash
# Verify AOF persistence: writes replay after kill; BGREWRITEAOF produces valid AOF.
# Requires: redis-server, redis-cli on PATH; mzset.so built.

set -u
cd "$(dirname "$0")/.."

MODULE="$(pwd)/mzset.so"
PORT=6398
DIR="$(mktemp -d)"
CLI="redis-cli -p $PORT"

start_redis() {
    redis-server --port $PORT --daemonize yes \
                 --dir "$DIR" \
                 --appendonly yes --appendfilename "appendonly.aof" \
                 --save "" \
                 --loadmodule "$MODULE" \
                 --logfile "$DIR/redis.log" \
                 --pidfile "$DIR/redis.pid"
    for _ in $(seq 1 40); do
        $CLI PING > /dev/null 2>&1 && return 0
        sleep 0.05
    done
    echo "redis didn't start; log:"
    cat "$DIR/redis.log"
    exit 1
}

hard_kill() {
    [ -f "$DIR/redis.pid" ] || return 0
    kill -9 "$(cat "$DIR/redis.pid")" 2>/dev/null || true
    rm -f "$DIR/redis.pid"
    sleep 0.2
}

soft_shutdown() {
    $CLI SHUTDOWN NOSAVE 2>/dev/null || true
    rm -f "$DIR/redis.pid"
    sleep 0.2
}

cleanup() {
    hard_kill
    rm -rf "$DIR"
}
trap cleanup EXIT

pass=0; fail=0
check() {
    if [ "$2" = "$3" ]; then
        printf "  PASS  %s\n" "$1"; pass=$((pass+1))
    else
        printf "  FAIL  %s\n        expected: %q\n        actual:   %q\n" "$1" "$2" "$3"
        fail=$((fail+1))
    fi
}

echo "== AOF persistence verification =="
echo "-- 1. start fresh --"
start_redis

echo "-- 2. write mixed workload --"
$CLI MZ.CREATE lb SCHEMA f64:desc i64:asc bool:asc > /dev/null
for i in $(seq 1 50); do
    $CLI MZ.ADD lb "m$i" "$(( (i * 37) % 100 )).5" "$(( 1700000000 + i ))" "$(( i % 2 ))" > /dev/null
done
$CLI MZ.UPDATE lb m10 0 200.0 > /dev/null    # bump m10 to the top
$CLI MZ.INCRBY lb m20 1 500 > /dev/null      # slide m20's ts
$CLI MZ.REM lb m3 m5 m7 > /dev/null

card_before=$($CLI MZ.CARD lb)
top_before=$($CLI MZ.RANGE lb 0 4)
score_m10_before=$($CLI MZ.SCORE lb m10)

echo "   state: card=$card_before  top5=$(echo "$top_before" | tr '\n' ' ')"

echo "-- 3. hard-kill redis (simulate crash) --"
hard_kill

echo "-- 4. restart, replay AOF --"
start_redis
check "card preserved"   "$card_before"        "$($CLI MZ.CARD lb)"
check "ordering preserved" "$top_before"       "$($CLI MZ.RANGE lb 0 4)"
check "score preserved"  "$score_m10_before"   "$($CLI MZ.SCORE lb m10)"

echo "-- 5. BGREWRITEAOF (exercises aof_rewrite callback) --"
$CLI BGREWRITEAOF > /dev/null
# wait for completion
for _ in $(seq 1 100); do
    stat=$($CLI INFO persistence | grep aof_rewrite_in_progress | tr -d '\r')
    case "$stat" in
        *:0) break ;;
    esac
    sleep 0.1
done
last=$($CLI INFO persistence | grep aof_last_bgrewrite_status | tr -d '\r' | cut -d: -f2)
check "rewrite success" "ok" "$last"

echo "-- 6. hard-kill + restart, verify rewritten AOF replays --"
hard_kill
start_redis
check "post-rewrite card"     "$card_before"       "$($CLI MZ.CARD lb)"
check "post-rewrite ordering" "$top_before"        "$($CLI MZ.RANGE lb 0 4)"
check "post-rewrite score"    "$score_m10_before"  "$($CLI MZ.SCORE lb m10)"

echo "-- 7. incremental writes after rewrite, replay again --"
$CLI MZ.ADD lb newmember 999.0 1700099999 1 > /dev/null
$CLI MZ.INCRBY lb newmember 0 1 > /dev/null    # 999 -> 1000
rank_after=$($CLI MZ.RANK lb newmember)
hard_kill
start_redis
check "incremental replay: rank preserved" "$rank_after" "$($CLI MZ.RANK lb newmember)"

soft_shutdown
echo
echo "== $pass passed, $fail failed =="
[ "$fail" = 0 ]
