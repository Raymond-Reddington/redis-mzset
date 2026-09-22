# mzset — a multi-field sortable Redis Module

`mzset` is a Redis Module that extends the classic `ZSET` idea to **multiple
sort fields**. Where a native ZSET orders members by a single `double` score,
`mzset` orders by an ordered tuple of typed fields (each independently
ASC/DESC), while keeping the O(log N) skiplist performance of ZSET.

## Why

A native ZSET score is one `double` (~52 bits of usable range). If you need
to rank items by `(score DESC, created_at ASC, priority DESC)` you either:

- pack fields into the score (bit-hacks, brittle, ≤52 bits total), or
- maintain multiple parallel ZSETs and reconcile at query time.

Neither scales cleanly. `mzset` gives you a first-class multi-field
comparator with the same complexity profile as ZSET.

## Requirements

- Linux or macOS (Redis Modules do not build on native Windows; use WSL/Docker)
- Redis 6.0+
- gcc/clang, GNU make

## Build

```bash
make            # Linux -> mzset.so
make macos      # macOS
```

## Load

```bash
redis-server --loadmodule /path/to/mzset.so
# or at runtime:
redis-cli MODULE LOAD /path/to/mzset.so
```

## Commands

| Command | Description |
|---|---|
| `MZ.CREATE key SCHEMA <type:dir> …` | Create a new mzset key with a fixed schema |
| `MZ.ALTER key ADD <type:dir> DEFAULT <v>` | Append one field; all existing members get `v` |
| `MZ.ADD key member v1 … vN [NX]` | Upsert a member; `NX` = fail if exists |
| `MZ.UPDATE key member idx v [idx v …]` | Partial field update |
| `MZ.INCRBY key member idx delta` | Numeric field self-increment |
| `MZ.REM key member [member …]` | Delete members |
| `MZ.SCORE key member` | Return the field vector |
| `MZ.RANK key member [REV]` | 0-based rank; nil if absent |
| `MZ.RANGE key start stop [REV] [WITHFIELDS]` | Range by rank (Redis-style negatives) |
| `MZ.CARD key` | Number of members |

### Types & directions

| Type | Meaning | Wire format |
|---|---|---|
| `i64` | signed 64-bit int | decimal string |
| `f64` | IEEE 754 double | decimal string (`NaN` rejected) |
| `ts` | unsigned 64-bit timestamp | decimal string, non-negative |
| `bool` | 0 / 1 | `"0"` or `"1"` |

Directions are `asc` or `desc` per field.

### Example: game leaderboard

Sort by `(score DESC, created_at ASC, vip_flag ASC)`.

```
redis> MZ.CREATE lb SCHEMA f64:desc ts:asc bool:asc
OK
redis> MZ.ADD lb alice 87.5 1700000000 1
(integer) 1
redis> MZ.ADD lb bob   92.0 1700000010 0
(integer) 1
redis> MZ.ADD lb carol 87.5 1699999900 1
(integer) 1

redis> MZ.RANGE lb 0 -1
1) "bob"
2) "carol"
3) "alice"

redis> MZ.RANK lb carol
(integer) 1

redis> MZ.SCORE lb carol
1) "87.5"
2) (integer) 1699999900
3) (integer) 1

redis> MZ.RANGE lb 0 0 WITHFIELDS
1) 1) "bob"
   2) 1) "92"
      2) (integer) 1700000010
      3) (integer) 0

redis> MZ.INCRBY lb alice 0 10        # bump alice.score by 10.0
"97.5"

redis> MZ.ALTER lb ADD i64:desc DEFAULT 0   # add a 4th field on the fly
OK
```

## Design notes

- **Composite key.** Each member's sort tuple is encoded into a single
  byte string that is byte-comparable. Sorting the skiplist is one `memcmp`.
- **Encodings.** `i64` = big-endian with sign flip; `f64` = IEEE 754
  order-preserving transform; `ts`/`bool` = raw big-endian. DESC fields are
  XOR'd with `0xFF` after normal encoding.
- **Layout.** A key holds `(schema, hashtable, skiplist)`. The hashtable maps
  `member_id → raw_fields`; the skiplist orders `composite_key → member_id`
  and enables `MZ.RANK` / `MZ.RANGE`.
- **RDB.** Only the schema + raw fields are persisted; the skiplist is rebuilt
  on load. Load time is `O(N log N)` — ~1-2 s for 1M.
- **AOF.** Mutations call `RedisModule_ReplicateVerbatim`; the rewrite
  callback emits `MZ.CREATE` + one `MZ.ADD` per member.
- **`MZ.ALTER ADD`.** Appending a field with a uniform default keeps relative
  order intact, so the skiplist is not re-sorted — only ckeys and raw blobs
  are rewritten in place. `O(N)`.
- **Not supported here.** Field range queries (`RANGEBYFIELDS`), non-tail field
  removal in `ALTER`, string-typed fields. These were deliberately scoped out
  — see the design discussion in `docs/` (or the commit history).

## Limits

| Setting | Value |
|---|---|
| Max fields per key | 16 |
| Max member_id length | 512 bytes |
| Max skiplist level | 24 (~16M members before degradation) |

## Performance baseline

Redis 6.0.16, WSL2 on Windows 11, 1M members, schema `(f64:desc, i64:asc)`:

| Workload | Throughput |
|---|---|
| Bulk `MZ.ADD` via `redis-cli --pipe` | **152K ops/sec** |
| `MZ.RANK` (50 clients, random member) | 48K ops/sec |
| `MZ.SCORE` (50 clients) | 45K ops/sec |
| `MZ.RANGE 0 99` (50 clients) | 26K ops/sec |
| `MZ.RANGE 0 99 WITHFIELDS` (50 clients) | 6K ops/sec |
| Memory | 121 MB @ 1M members (~127 B/member) |

Reproduce with `bash bench/bench.sh 1000000`.

## Development

```bash
make                      # build mzset.so
tests/test_core           # standalone unit tests (no Redis needed)
bash tests/test.sh        # end-to-end via redis-cli
bash tests/test_aof.sh    # AOF replay & BGREWRITEAOF verification
bash bench/bench.sh 1000000
```

Layout:

```
mzset.h         all public types + prototypes
encoding.c      per-field order-preserving encode/decode
hashtable.c     open-chained hashtable: member_id -> raw fields
skiplist.c      classic skiplist ordered by memcmp of composite key
mzset.c         high-level ops: add / update / incrby / rem / rank / range / alter
module.c        RedisModule glue: OnLoad, commands, RDB, AOF, digest
tests/          unit + integration tests
bench/          load/latency benchmarks
```

## License

MIT — see `LICENSE`.
