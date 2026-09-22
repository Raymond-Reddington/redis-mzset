#!/usr/bin/env bash
# Side-by-side benchmark: native ZSET vs mzset.
# Both populated with the same members and the same primary score field.
# mzset additionally carries a secondary i64 field to demonstrate multi-field cost.
#
# Usage: bash bench/bench_vs_zset.sh [N]   # default 1_000_000

set -u
cd "$(dirname "$0")/.."

MODULE="$(pwd)/mzset.so"
PORT=6395
CLI="redis-cli -p $PORT"
BENCH="redis-benchmark -p $PORT -q -c 50"

N=${1:-1000000}
ZKEY=z
MKEY=m

if [ ! -f "$MODULE" ]; then
    echo "mzset.so not found. Run: make" >&2
    exit 1
fi

redis-server --port $PORT --daemonize yes --save "" --appendonly no \
             --loadmodule "$MODULE" \
             --logfile /tmp/mzset-vs.log \
             --pidfile /tmp/mzset-vs.pid > /dev/null
for _ in $(seq 1 40); do
    $CLI PING > /dev/null 2>&1 && break
    sleep 0.05
done
trap '[ -f /tmp/mzset-vs.pid ] && kill $(cat /tmp/mzset-vs.pid) 2>/dev/null; rm -f /tmp/mzset-vs.pid' EXIT

$CLI FLUSHALL > /dev/null
$CLI MZ.CREATE $MKEY SCHEMA f64:desc i64:asc > /dev/null

strip() { tr '\r' '\n' | grep -E "requests per second" | tail -1 | sed 's/.*: //; s/ .*//'; }

fmt() { awk "BEGIN{printf \"%'12.0f\", $1}"; }

# ---- 1. bulk insert via --pipe (single connection, same generator) ----

echo "==============================================================="
echo " ZSET vs MZSET  |  N = $N  |  Redis $($CLI INFO server | grep redis_version | tr -d '\r' | cut -d: -f2)"
echo "==============================================================="

echo
echo "[1] Bulk insert (single-connection --pipe)"

# ZSET load: ZADD z score member
t0=$(date +%s.%N)
python3 -c "
import sys, random
random.seed(42)
N = $N
out = sys.stdout.buffer
for i in range(N):
    member = f'm{i}'.encode()
    score  = f'{random.random()*100:.4f}'.encode()
    parts  = [b'ZADD', b'$ZKEY', score, member]
    out.write(b'*%d\r\n' % len(parts))
    for p in parts:
        out.write(b'\$%d\r\n%s\r\n' % (len(p), p))
" | $CLI --pipe > /dev/null
t1=$(date +%s.%N)
z_elapsed=$(awk "BEGIN{print $t1 - $t0}")
z_rps=$(awk "BEGIN{printf \"%.0f\", $N / $z_elapsed}")

# MZSET load: MZ.ADD m member score ts
t0=$(date +%s.%N)
python3 -c "
import sys, random
random.seed(42)
N = $N
out = sys.stdout.buffer
for i in range(N):
    member = f'm{i}'.encode()
    score  = f'{random.random()*100:.4f}'.encode()
    ts     = f'{1700000000 + random.randrange(1000000)}'.encode()
    parts  = [b'MZ.ADD', b'$MKEY', member, score, ts]
    out.write(b'*%d\r\n' % len(parts))
    for p in parts:
        out.write(b'\$%d\r\n%s\r\n' % (len(p), p))
" | $CLI --pipe > /dev/null
t1=$(date +%s.%N)
m_elapsed=$(awk "BEGIN{print $t1 - $t0}")
m_rps=$(awk "BEGIN{printf \"%.0f\", $N / $m_elapsed}")

ratio=$(awk "BEGIN{printf \"%.2fx\", $z_rps / $m_rps}")
printf "    ZSET  : %s ops/sec  (%.2fs)\n" "$(fmt $z_rps)" "$z_elapsed"
printf "    MZSET : %s ops/sec  (%.2fs)  -- ZSET %s\n" "$(fmt $m_rps)" "$m_elapsed" "$ratio"

# ---- 2. rank latency (50 clients) ----

echo
echo "[2] RANK by random member  (50 clients, 100k queries)"
z=$($BENCH -n 100000 -r $N ZRANK $ZKEY m__rand_int__ 2>&1 | strip)
m=$($BENCH -n 100000 -r $N MZ.RANK $MKEY m__rand_int__ 2>&1 | strip)
ratio=$(awk "BEGIN{printf \"%.2fx\", $z / $m}")
printf "    ZRANK    : %s ops/sec\n" "$(fmt $z)"
printf "    MZ.RANK  : %s ops/sec  -- ZSET %s\n" "$(fmt $m)" "$ratio"

# ---- 3. score/lookup ----

echo
echo "[3] SCORE by random member  (50 clients, 100k queries)"
z=$($BENCH -n 100000 -r $N ZSCORE $ZKEY m__rand_int__ 2>&1 | strip)
m=$($BENCH -n 100000 -r $N MZ.SCORE $MKEY m__rand_int__ 2>&1 | strip)
ratio=$(awk "BEGIN{printf \"%.2fx\", $z / $m}")
printf "    ZSCORE   : %s ops/sec\n" "$(fmt $z)"
printf "    MZ.SCORE : %s ops/sec  -- ZSET %s\n" "$(fmt $m)" "$ratio"

# ---- 4. range top-100 ----

echo
echo "[4] RANGE top-100  (50 clients, 10k queries)"
z=$($BENCH -n 10000 ZRANGE $ZKEY 0 99 2>&1 | strip)
m=$($BENCH -n 10000 MZ.RANGE $MKEY 0 99 2>&1 | strip)
ratio=$(awk "BEGIN{printf \"%.2fx\", $z / $m}")
printf "    ZRANGE   : %s ops/sec\n" "$(fmt $z)"
printf "    MZ.RANGE : %s ops/sec  -- ZSET %s\n" "$(fmt $m)" "$ratio"

# ---- 5. range top-100 with payload ----

echo
echo "[5] RANGE top-100 with payload  (50 clients, 10k queries)"
z=$($BENCH -n 10000 ZRANGE $ZKEY 0 99 WITHSCORES 2>&1 | strip)
m=$($BENCH -n 10000 MZ.RANGE $MKEY 0 99 WITHFIELDS 2>&1 | strip)
ratio=$(awk "BEGIN{printf \"%.2fx\", $z / $m}")
printf "    ZRANGE WITHSCORES  : %s ops/sec\n" "$(fmt $z)"
printf "    MZ.RANGE WITHFIELDS: %s ops/sec  -- ZSET %s\n" "$(fmt $m)" "$ratio"

# ---- 6. memory ----

echo
echo "[6] Memory footprint"
zbytes=$($CLI MEMORY USAGE $ZKEY)
mbytes=$($CLI MEMORY USAGE $MKEY)
zper=$(awk "BEGIN{printf \"%.1f\", $zbytes / $N}")
mper=$(awk "BEGIN{printf \"%.1f\", $mbytes / $N}")
ratio=$(awk "BEGIN{printf \"%.2fx\", $mbytes / $zbytes}")
printf "    ZSET   : %s bytes  (%.1f MB, %s B/member, 1 f64 field)\n" \
       "$(fmt $zbytes)" "$(awk "BEGIN{print $zbytes/1048576}")" "$zper"
printf "    MZSET  : %s bytes  (%.1f MB, %s B/member, 2 fields)  -- MZSET %s\n" \
       "$(fmt $mbytes)" "$(awk "BEGIN{print $mbytes/1048576}")" "$mper" "$ratio"

echo
echo "== done =="
