#!/usr/bin/env bash
# Sweep mzset performance by number of sort fields: 1, 2, 4, 8, 16.
# All fields are f64:asc for simplicity; only field count varies.
# Same N members loaded per config, same read workloads.
#
# Usage: bash bench/bench_fields_sweep.sh [N]   # default 500_000

set -u
cd "$(dirname "$0")/.."

MODULE="$(pwd)/mzset.so"
PORT=6394
CLI="redis-cli -p $PORT"
BENCH="redis-benchmark -p $PORT -q -c 50"

N=${1:-500000}
FIELDS_LIST="1 2 4 8 16"

if [ ! -f "$MODULE" ]; then
    echo "mzset.so not found. Run: make" >&2
    exit 1
fi

redis-server --port $PORT --daemonize yes --save "" --appendonly no \
             --loadmodule "$MODULE" \
             --logfile /tmp/mzset-sweep.log \
             --pidfile /tmp/mzset-sweep.pid > /dev/null
for _ in $(seq 1 40); do
    $CLI PING > /dev/null 2>&1 && break
    sleep 0.05
done
trap '[ -f /tmp/mzset-sweep.pid ] && kill $(cat /tmp/mzset-sweep.pid) 2>/dev/null; rm -f /tmp/mzset-sweep.pid' EXIT

strip() { tr '\r' '\n' | grep -E "requests per second" | tail -1 | sed 's/.*: //; s/ .*//'; }

echo "=========================================================================="
echo " Field-count sweep  |  N = $N  |  Redis $($CLI INFO server | grep redis_version | tr -d '\r' | cut -d: -f2)"
echo "=========================================================================="
printf "\n%-8s %12s %12s %12s %12s %14s %10s\n" \
       "fields" "insert/s" "rank/s" "range100/s" "range+f/s" "mem_MB" "B/member"
printf "%s\n" "--------------------------------------------------------------------------"

for nf in $FIELDS_LIST; do
    $CLI FLUSHALL > /dev/null

    # build schema: nf * "f64:asc"
    schema=""
    for _ in $(seq 1 $nf); do schema="$schema f64:asc"; done
    $CLI MZ.CREATE lb SCHEMA $schema > /dev/null

    # bulk load via --pipe: MZ.ADD lb mI v1 v2 ... vNF
    t0=$(date +%s.%N)
    python3 -c "
import sys, random
random.seed(42)
N = $N; NF = $nf
out = sys.stdout.buffer
for i in range(N):
    parts = [b'MZ.ADD', b'lb', f'm{i}'.encode()]
    for _ in range(NF):
        parts.append(f'{random.random()*100:.4f}'.encode())
    out.write(b'*%d\r\n' % len(parts))
    for p in parts:
        out.write(b'\$%d\r\n%s\r\n' % (len(p), p))
" | $CLI --pipe > /dev/null
    t1=$(date +%s.%N)
    elapsed=$(awk "BEGIN{print $t1 - $t0}")
    ins_rps=$(awk "BEGIN{printf \"%.0f\", $N / $elapsed}")

    rank_rps=$($BENCH -n 50000 -r $N MZ.RANK lb m__rand_int__ 2>&1 | strip)
    range_rps=$($BENCH -n 5000 MZ.RANGE lb 0 99 2>&1 | strip)
    rangef_rps=$($BENCH -n 5000 MZ.RANGE lb 0 99 WITHFIELDS 2>&1 | strip)

    bytes=$($CLI MEMORY USAGE lb)
    mb=$(awk "BEGIN{printf \"%.1f\", $bytes / 1048576.0}")
    per=$(awk "BEGIN{printf \"%.1f\", $bytes / $N}")

    printf "%-8s %12s %12s %12s %12s %14s %10s\n" \
           "$nf" "$ins_rps" "$rank_rps" "$range_rps" "$rangef_rps" "$mb" "$per"
done

echo
echo "== done =="
