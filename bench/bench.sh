#!/usr/bin/env bash
# Baseline performance bench for mzset.
# Measures: bulk ADD throughput, RANK/RANGE/SCORE latency, memory at 1M scale.
# Uses redis-cli --pipe for bulk load (redis-benchmark -P has a bug with custom
# module commands on Redis 6.0). Uses redis-benchmark (no pipelining) for reads.
#
# Usage: bash bench/bench.sh [N]     # N = target member count (default 1_000_000)

set -u
cd "$(dirname "$0")/.."

MODULE="$(pwd)/mzset.so"
PORT=6397
CLI="redis-cli -p $PORT"
BENCH_NP="redis-benchmark -p $PORT -q -c 50"       # no pipeline
BENCH_SINGLE="redis-benchmark -p $PORT -q -c 1"

N=${1:-1000000}
KEY=lb

if [ ! -f "$MODULE" ]; then
    echo "mzset.so not found. Run: make" >&2
    exit 1
fi

redis-server --port $PORT --daemonize yes --save "" --appendonly no \
             --loadmodule "$MODULE" \
             --logfile /tmp/mzset-bench.log \
             --pidfile /tmp/mzset-bench.pid \
             --maxmemory-policy noeviction > /dev/null
for _ in $(seq 1 40); do
    $CLI PING > /dev/null 2>&1 && break
    sleep 0.05
done

trap '[ -f /tmp/mzset-bench.pid ] && kill $(cat /tmp/mzset-bench.pid) 2>/dev/null; rm -f /tmp/mzset-bench.pid' EXIT

$CLI FLUSHALL > /dev/null

echo "======================================================================"
echo " mzset benchmark : target ${N} members, schema=(f64:desc, i64:asc)"
echo "======================================================================"
$CLI MZ.CREATE $KEY SCHEMA f64:desc i64:asc > /dev/null

# ---- 1. bulk load via --pipe ----
echo
echo "-- 1. bulk MZ.ADD via redis-cli --pipe --"
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
    parts  = [b'MZ.ADD', b'$KEY', member, score, ts]
    out.write(b'*%d\r\n' % len(parts))
    for p in parts:
        out.write(b'\$%d\r\n%s\r\n' % (len(p), p))
" | $CLI --pipe > /tmp/pipe_out.txt
t1=$(date +%s.%N)
elapsed=$(awk "BEGIN{print $t1 - $t0}")
inserted=$($CLI MZ.CARD $KEY)
rps=$(awk "BEGIN{printf \"%.0f\", $N / $elapsed}")
echo "   $inserted inserted in ${elapsed}s  =>  ${rps} ops/sec (single-threaded pipe)"
grep -E "errors|replies" /tmp/pipe_out.txt

# ---- 2. read latency benches ----
strip_progress() {
    # redis-benchmark -q updates progress with \r; keep only the final line.
    tr '\r' '\n' | grep -E "requests per second" | tail -1
}

echo
echo "-- 2. MZ.RANK (100k random members, 50 clients) --"
$BENCH_NP -n 100000 -r $N MZ.RANK $KEY m__rand_int__ 2>&1 | strip_progress

echo
echo "-- 3. MZ.SCORE (100k random members, 50 clients) --"
$BENCH_NP -n 100000 -r $N MZ.SCORE $KEY m__rand_int__ 2>&1 | strip_progress

echo
echo "-- 4. MZ.RANGE top-100 (10k queries, 50 clients) --"
$BENCH_NP -n 10000 MZ.RANGE $KEY 0 99 2>&1 | strip_progress

echo
echo "-- 5. MZ.RANGE top-100 WITHFIELDS (10k queries, 50 clients) --"
$BENCH_NP -n 10000 MZ.RANGE $KEY 0 99 WITHFIELDS 2>&1 | strip_progress

echo
echo "-- 6. single-thread p50/p95/p99 for MZ.RANK --"
$BENCH_SINGLE --precision 3 -n 20000 -r $N MZ.RANK $KEY m__rand_int__ 2>&1 | strip_progress

echo
echo "-- 7. single-thread p50/p95/p99 for MZ.ADD (mix of insert/update) --"
$BENCH_SINGLE --precision 3 -n 20000 -r $N MZ.ADD $KEY m__rand_int__ __rand_int__ __rand_int__ 2>&1 | strip_progress

# ---- 3. memory footprint ----
echo
echo "-- 8. Memory footprint --"
mem_bytes=$($CLI MEMORY USAGE $KEY)
mem_mb=$(awk "BEGIN{printf \"%.1f\", $mem_bytes / 1048576.0}")
if [ "$inserted" -gt 0 ]; then
    per_entry=$(awk "BEGIN{printf \"%.1f\", $mem_bytes / $inserted}")
else
    per_entry="?"
fi
echo "   ${mem_bytes} bytes  ->  ${mem_mb} MB total  |  ${per_entry} B / member"

echo
echo "== done =="
